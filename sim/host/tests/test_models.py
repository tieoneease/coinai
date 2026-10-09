"""Unit tests: AXP2101 and QMI8658 models, IPC frame codec, PROTOCOL.md TLV decoding."""

import asyncio
import struct

import numpy as np
import pytest

from coinhost import ipc, tlv
from coinhost.ble import adv_data
from coinhost.imu import Motion, Qmi8658
from coinhost.pmic import Axp2101, pct_to_volts, volts_to_pct


# ---------------------------------------------------------------- PMIC
def r(p, reg, n=1):
    return p.i2c(bytes([reg]), n)[0]


def adc(p, reg):
    h, l = r(p, reg, 2)
    return ((h & 0x3F) << 8) | l


def test_pmic_initial_registers():
    p = Axp2101()
    assert r(p, 0x03)[0] == 0x4A
    assert r(p, 0x27)[0] == 0x14 and r(p, 0x22)[0] == 0x06
    assert r(p, 0x80)[0] == 0x0F and r(p, 0x90)[0] == 0xFF
    assert r(p, 0x55)[0] == 0  # unlisted
    # USB + full battery = the real dump: 0x00 = 0x28, 0x01 = 0x14 (standby, charge done)
    assert r(p, 0x00)[0] == 0x28
    assert r(p, 0x01)[0] == 0x14


def test_pmic_status_bits_follow_supply():
    p = Axp2101()
    p.usb, p.battery_pct = False, 50
    assert r(p, 0x00)[0] & 0x28 == 0x08
    assert r(p, 0x01)[0] >> 5 == 0b010 and r(p, 0x01)[0] & 7 == 5  # discharging, not charging
    p.usb = True
    s2 = r(p, 0x01)[0]
    assert s2 >> 5 == 0b001 and s2 & 7 == 2  # charging, constant current
    p.battery_pct = 95
    assert r(p, 0x01)[0] & 7 == 3  # constant voltage above 4.1 V
    p.battery_present = False
    assert r(p, 0x00)[0] & 0x28 == 0x20
    assert r(p, 0x01)[0] >> 5 == 0
    assert adc(p, 0x34) == 0 and r(p, 0xA4)[0] == 0


def test_pmic_adc_registers():
    p = Axp2101()
    p.set_battery_volts(3.83)
    assert adc(p, 0x34) == 3830
    assert r(p, 0xA4)[0] == 50
    assert adc(p, 0x38) == 5000
    assert adc(p, 0x3A) == 4900  # max(VBUS - 100, VBAT)
    p.usb = False
    assert adc(p, 0x38) == 0 and adc(p, 0x3A) == 3830
    p.die_temp_c = 42.0
    raw = adc(p, 0x3C)
    assert raw == 7274 - 20 * 20
    assert 22 + (7274 - raw) / 20 == pytest.approx(42.0)


def test_pmic_writes_and_poweroff():
    p = Axp2101()
    _, ev = p.i2c(bytes([0x62, 0x09]), 0)
    assert r(p, 0x62)[0] == 0x09 and 'reg:62' in ev
    _, ev = p.i2c(bytes([0x10, 0x35]), 0)
    assert 'poweroff' in ev
    assert r(p, 0x10)[0] == 0x34  # bit 0 self-clears
    # multi-byte write and auto-increment read
    p.i2c(bytes([0x92, 0x11, 0x12]), 0)
    assert r(p, 0x92, 2) == bytes([0x11, 0x12])
    p.i2c(bytes([0x90, 0xFE]), 0)
    assert not p.aldo1_on
    p.i2c(bytes([0x22, 0x04]), 0)
    assert not p.long_press_off_enabled
    p.i2c(bytes([0x27, 0x0F]), 0)
    assert p.on_time_s == 2.0 and p.off_time_s == 10.0


def test_pmic_power_ok_and_battery_curve():
    p = Axp2101()
    p.usb = False
    p.battery_pct = 0
    assert p.vbat_mv == 3000 and p.power_ok
    p.set_battery_volts(2.9)
    p.battery_pct = 0
    p.battery_present = False
    assert not p.power_ok
    p.usb = True
    assert p.power_ok
    for pct in (0, 7, 33, 50, 88, 100):
        assert volts_to_pct(pct_to_volts(pct)) == pytest.approx(pct, abs=1e-6)
    assert pct_to_volts(0) == 3.0 and pct_to_volts(100) == 4.2


def test_pmic_charge_and_drain_simulation():
    p = Axp2101()
    p.battery_pct = 50
    p.usb = False
    p.step(3600, 40)  # 40 mAh from 400 mAh
    assert p.battery_pct == pytest.approx(40)
    p.usb = True
    p.step(600, 0)  # 0x62 = 0x04 -> 100 mA for 10 min = 16.7 mAh = 4.2 %
    assert p.battery_pct == pytest.approx(40 + 100 / 6 / 400 * 100, abs=0.01)


# ---------------------------------------------------------------- IMU
def imu_init(m=None):
    q = Qmi8658(m or Motion(noise=False))
    q.i2c(bytes([0x60, 0xB0]), 0)
    for reg, val in ((0x02, 0x40), (0x03, 0x16), (0x04, 0x56), (0x08, 0x03)):
        q.i2c(bytes([reg, val]), 0)
    return q


def imu_read(q):
    return struct.unpack('<7h', q.i2c(bytes([0x33]), 14))


def test_imu_identity_and_ctrl():
    q = Qmi8658()
    assert q.i2c(b'\x00', 2) == bytes([0x05, 0x7C])
    q = imu_init()
    assert q.i2c(b'\x02', 9) == bytes([0x40, 0x16, 0x56, 0, 0, 0, 0x03, 0, 0])
    assert q.accel_lsb_per_g == 8192 and q.gyro_lsb_per_dps == 64


def test_imu_flat_reads_one_g():
    q = imu_init()
    t, ax, ay, az, gx, gy, gz = imu_read(q)
    assert (ax, ay, az) == (0, 0, 8192)
    assert (gx, gy, gz) == (0, 0, 0)
    assert t == round(27.5 * 256)


def test_imu_tilt_and_gyro():
    m = Motion(noise=False)
    q = imu_init(m)
    m.set_stick(1.0, 0.0)  # roll +60
    rates = []
    for _ in range(200):
        m.step(0.005)
        rates.append(m.gyro[1])
    assert m.roll == pytest.approx(60, abs=0.01)
    assert max(rates) > 100  # deg/s while tilting
    assert abs(rates[-1]) < 1  # at rest again
    _, ax, ay, az, *_ = imu_read(q)
    assert ax / 8192 == pytest.approx(-np.sin(np.radians(60)), abs=1e-3)
    assert az / 8192 == pytest.approx(0.5, abs=1e-3)
    m.yaw_rate = 90
    m.step(0.01)
    assert imu_read(q)[6] == 90 * 64


def test_imu_sensors_off_hold_and_reset():
    m = Motion(noise=False)
    q = imu_init(m)
    before = imu_read(q)
    q.i2c(bytes([0x08, 0x00]), 0)  # CTRL7 = 0
    m.set_tilt(30, 0)
    for _ in range(100):
        m.step(0.01)
    assert imu_read(q) == before
    q.i2c(bytes([0x08, 0x03]), 0)
    assert imu_read(q) != before
    q.i2c(bytes([0x03, 0x26]), 0)  # +-8 g
    assert q.accel_lsb_per_g == 4096
    q.i2c(bytes([0x60, 0xB0]), 0)
    assert q.i2c(b'\x02', 9) == bytes(9)
    assert q.i2c(b'\x00', 1) == b'\x05'


def test_imu_tap_and_shake():
    m = Motion(noise=False)
    m.tap()
    peak = 0
    for _ in range(10):
        m.step(0.005)
        peak = max(peak, m.accel[2])
    assert peak > 2.5
    m.shake()
    assert m.shaking
    for _ in range(110):
        m.step(0.01)
    assert not m.shaking


# ---------------------------------------------------------------- IPC codec
def test_frame_codec_roundtrip():
    frames = ipc.encode(ipc.HELLO, struct.pack('<I', 1) + b'fw-1') + ipc.i2c_result(7, True, b'\x4a') + ipc.encode(0x9A)
    parser = ipc.FrameParser()
    out = []
    for i in range(0, len(frames), 3):  # dribble in
        out += parser.feed(frames[i:i + 3])
    assert [t for t, _ in out] == [ipc.HELLO, ipc.I2C_RESULT, ipc.BLE_READY]
    assert ipc.decode_hello(out[0][1]) == (1, 'fw-1')
    assert out[1][1] == b'\x07\x00\x00\x4a'

    async def read_all():
        reader = asyncio.StreamReader()
        reader.feed_data(frames)
        reader.feed_eof()
        got = []
        while True:
            try:
                got.append(await ipc.read_frame(reader))
            except asyncio.IncompleteReadError:
                return got
    assert asyncio.run(read_all()) == out


def test_decoders():
    req = ipc.decode_i2c(struct.pack('<HBHH', 5, 0x34, 2, 3) + b'\x10\x01')
    assert (req.seq, req.addr, req.wbytes, req.rlen) == (5, 0x34, b'\x10\x01', 3)
    with pytest.raises(ipc.ProtocolError):
        ipc.decode_i2c(struct.pack('<HBHH', 5, 0x34, 4, 0) + b'\x10')
    sl = ipc.decode_sleep(ipc.encode_sleep(5_000_000, 8, 1, b'abc')[5:])
    assert (sl.timer_us, sl.ext1_mask, sl.ext1_mode, sl.rtc) == (5_000_000, 8, 1, b'abc')
    adv = ipc.decode_adv(struct.pack('<BHi', 1, 32, 30000))
    assert adv.on and adv.interval_ms == 20.0 and adv.duration_ms == 30000
    assert ipc.touch(True, 500, -3) == ipc.encode(ipc.TOUCH, struct.pack('<BHH', 1, 465, 0))


def test_frame_pixels():
    px = struct.pack('<4H', 0xF800, 0x07E0, 0x001F, 0xFFFF)
    f = ipc.decode_frame(ipc.encode_frame(10, 20, 2, 2, px)[5:])
    assert (f.x, f.y, f.w, f.h) == (10, 20, 2, 2)
    rgba = ipc.rgb565_to_rgba(f.pixels, 2, 2)
    assert rgba[0, 0].tolist() == [255, 0, 0, 255]
    assert rgba[0, 1].tolist() == [0, 255, 0, 255]
    assert rgba[1, 0].tolist() == [0, 0, 255, 255]
    assert rgba[1, 1].tolist() == [255, 255, 255, 255]
    with pytest.raises(ipc.ProtocolError):
        ipc.decode_frame(struct.pack('<HHHH', 0, 0, 2, 2) + b'\x00' * 6)
    with pytest.raises(ipc.ProtocolError):
        ipc.decode_frame(struct.pack('<HHHH', 465, 0, 2, 1) + b'\x00' * 4)


# ---------------------------------------------------------------- TLV / advertising
def test_tlv_describe():
    assert tlv.describe(tlv.hello(1234)) == 'HELLO v1 phone_utc_ms=1234'
    assert tlv.describe(tlv.get(3, 476)) == 'GET clip=3 offset=476'
    s = tlv.describe(tlv.status(87, 3, 2, 'fw-1'))
    assert s == 'STATUS battery=87% flags=[charging,usb] pending=2 fw="fw-1"'
    assert 'brightness=40' in tlv.describe(tlv.cfg_set(2, 40))
    assert 'reply_wait_s=45' in tlv.describe(tlv.cfg_set(1, 45))
    ended = tlv.tlv(tlv.CLIP_ENDED, struct.pack('<HIIIIQ', 4, 1000, 2000, 0xDEADBEEF, 0xFFFFFFFF, 0))
    assert 'voiced_ms=unknown' in tlv.describe(ended) and 'crc32=deadbeef' in tlv.describe(ended)
    assert 'unknown clip' in tlv.describe(tlv.error(tlv.GET, 1, 9))
    two = tlv.list_() + tlv.ack(4, 1)
    assert tlv.describe(two) == 'LIST; ACK clip=4 crc32=00000001'


def test_wifi_set_never_logged():
    w = tlv.wifi_set(b'HomeNet', b'hunter2-secret')
    d = tlv.describe(w)
    assert 'hunter2' not in d and 'HomeNet' not in d and w.hex() not in d
    d = tlv.describe(w[:-3])  # truncated
    assert 'hunter2' not in d and 'secret'.encode().hex() not in d


def test_adv_data_layout():
    d = adv_data()
    assert len(d) <= 31
    assert d[:3] == bytes([2, 1, 6])
    assert d[3:5] == bytes([17, 7])
    assert d[5:21] == bytes.fromhex('fc35d8605a7f7686ae4169f50000a1c0')
    assert d[21:] == bytes([7, 9]) + b'coinai'
