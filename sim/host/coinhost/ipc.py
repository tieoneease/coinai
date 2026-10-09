"""Framed coin <-> host protocol (sim/IPC.md): codec and message constants."""

from __future__ import annotations

import asyncio
import struct
from dataclasses import dataclass

import numpy as np

IPC_VERSION = 1
PANEL_SIZE = 466
MAX_FRAME_LEN = 16 * 1024 * 1024

HDR = struct.Struct('<BI')

# coin -> host
HELLO = 0x01
I2C = 0x03
GPIO_OUT = 0x04
FRAME = 0x05
PANEL = 0x06
SPEAKER = 0x07
MIC_STATE = 0x08
SLEEP = 0x09
RESET = 0x0A
BLE_ADV = 0x10
BLE_INDICATE = 0x11
BLE_NOTIFY = 0x12
BLE_WRITE_RESULT = 0x13
BLE_PASSKEY = 0x14
BLE_DISCONNECT = 0x15
BLE_FORGET = 0x16
BLE_CONN_PARAMS = 0x17
BLE_LINK_SETUP = 0x18
BLE_TX_POWER = 0x19
BLE_BATTERY = 0x1A
BLE_READY_ACK = 0x1B

# host -> coin
I2C_RESULT = 0x83
GPIO_IN = 0x84
TOUCH = 0x85
MIC = 0x86
BLE_CONNECTED = 0x90
BLE_IND_DONE = 0x91
BLE_CREDIT = 0x92
BLE_DISCONNECTED = 0x93
BLE_SECURITY = 0x94
BLE_MTU = 0x95
BLE_SUBSCRIBE = 0x96
BLE_WRITE = 0x97
BLE_PASSKEY_REQ = 0x98
BLE_RSSI = 0x99
BLE_READY = 0x9A
BLE_ADV_DONE = 0x9B

NAMES = {v: k for k, v in globals().items() if k.isupper() and isinstance(v, int) and k not in (
    'IPC_VERSION', 'PANEL_SIZE', 'MAX_FRAME_LEN')}

# esp_reset_reason_t
RST_POWERON = 1
RST_SW = 3
RST_PANIC = 4
RST_TASK_WDT = 6
RST_DEEPSLEEP = 8
RESET_NAMES = {0: 'unknown', 1: 'power-on', 2: 'external', 3: 'software', 4: 'panic', 5: 'int wdt',
               6: 'task wdt', 7: 'wdt', 8: 'deep sleep', 9: 'brownout', 10: 'sdio'}

# COINSIM_WAKE bits (1 << esp_sleep_source_t)
WAKE_EXT1 = 1 << 3
WAKE_TIMER = 1 << 4


class ProtocolError(Exception):
    pass


def encode(msg_type: int, payload: bytes = b'') -> bytes:
    return HDR.pack(msg_type, len(payload)) + payload


async def read_frame(reader: asyncio.StreamReader) -> tuple[int, bytes]:
    """Read one frame. Raises asyncio.IncompleteReadError at EOF."""
    hdr = await reader.readexactly(HDR.size)
    msg_type, length = HDR.unpack(hdr)
    if length > MAX_FRAME_LEN:
        raise ProtocolError(f'frame 0x{msg_type:02x} too long: {length}')
    payload = await reader.readexactly(length) if length else b''
    return msg_type, payload


class FrameParser:
    """Incremental parser for a byte stream (used by tests and fake_coin)."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self._buf += data
        out = []
        while len(self._buf) >= HDR.size:
            msg_type, length = HDR.unpack_from(self._buf)
            if length > MAX_FRAME_LEN:
                raise ProtocolError(f'frame 0x{msg_type:02x} too long: {length}')
            if len(self._buf) < HDR.size + length:
                break
            out.append((msg_type, bytes(self._buf[HDR.size:HDR.size + length])))
            del self._buf[:HDR.size + length]
        return out


# ---------------------------------------------------------------------------- coin -> host decoders

def decode_hello(p: bytes) -> tuple[int, str]:
    if len(p) < 4:
        raise ProtocolError('HELLO too short')
    return struct.unpack_from('<I', p)[0], p[4:].decode('utf-8', 'replace')


@dataclass
class I2cRequest:
    seq: int
    addr: int
    wbytes: bytes
    rlen: int


def decode_i2c(p: bytes) -> I2cRequest:
    if len(p) < 7:
        raise ProtocolError('I2C too short')
    seq, addr, wlen, rlen = struct.unpack_from('<HBHH', p)
    wbytes = p[7:7 + wlen]
    if len(wbytes) != wlen:
        raise ProtocolError('I2C wbytes truncated')
    return I2cRequest(seq, addr, bytes(wbytes), rlen)


@dataclass
class Frame:
    x: int
    y: int
    w: int
    h: int
    pixels: bytes  # RGB565 LE, row-major, w*h*2 bytes


def decode_frame(p: bytes) -> Frame:
    if len(p) < 8:
        raise ProtocolError('FRAME too short')
    x, y, w, h = struct.unpack_from('<HHHH', p)
    n = w * h * 2
    if len(p) - 8 != n:
        raise ProtocolError(f'FRAME {w}x{h} needs {n} pixel bytes, got {len(p) - 8}')
    if x + w > PANEL_SIZE or y + h > PANEL_SIZE:
        raise ProtocolError(f'FRAME {x},{y} {w}x{h} outside the {PANEL_SIZE}x{PANEL_SIZE} panel')
    return Frame(x, y, w, h, p[8:])


def encode_frame(x: int, y: int, w: int, h: int, pixels: bytes) -> bytes:
    return encode(FRAME, struct.pack('<HHHH', x, y, w, h) + pixels)


@dataclass
class Sleep:
    timer_us: int
    ext1_mask: int
    ext1_mode: int
    rtc: bytes


def decode_sleep(p: bytes) -> Sleep:
    if len(p) < 21:
        raise ProtocolError('SLEEP too short')
    timer_us, mask, mode, rtc_len = struct.unpack_from('<QQBI', p)
    rtc = p[21:21 + rtc_len]
    if len(rtc) != rtc_len:
        raise ProtocolError('SLEEP rtc truncated')
    return Sleep(timer_us, mask, mode, bytes(rtc))


def encode_sleep(timer_us: int, ext1_mask: int, ext1_mode: int, rtc: bytes) -> bytes:
    return encode(SLEEP, struct.pack('<QQBI', timer_us, ext1_mask, ext1_mode, len(rtc)) + rtc)


@dataclass
class Adv:
    on: bool
    interval: int  # 0.625 ms units
    duration_ms: int

    @property
    def interval_ms(self) -> float:
        return self.interval * 0.625


def decode_adv(p: bytes) -> Adv:
    on, itvl, dur = struct.unpack_from('<BHi', p)
    return Adv(bool(on), itvl, dur)


# ---------------------------------------------------------------------------- host -> coin encoders

def i2c_result(seq: int, ack: bool, rbytes: bytes = b'') -> bytes:
    return encode(I2C_RESULT, struct.pack('<HB', seq, 0 if ack else 1) + rbytes)


def gpio_in(pin: int, level: int) -> bytes:
    return encode(GPIO_IN, bytes([pin, 1 if level else 0]))


def touch(pressed: bool, x: int, y: int) -> bytes:
    x = max(0, min(PANEL_SIZE - 1, int(x)))
    y = max(0, min(PANEL_SIZE - 1, int(y)))
    return encode(TOUCH, struct.pack('<BHH', 1 if pressed else 0, x, y))


def mic(pcm_stereo_s16: bytes) -> bytes:
    return encode(MIC, pcm_stereo_s16)


def ble_connected(conn: int, addr_type: int, addr_lsb_first: bytes) -> bytes:
    return encode(BLE_CONNECTED, struct.pack('<HB', conn, addr_type) + addr_lsb_first[:6])


def ble_ind_done(seq: int, confirmed: bool) -> bytes:
    return encode(BLE_IND_DONE, struct.pack('<HB', seq, 1 if confirmed else 0))


def ble_credit(n: int) -> bytes:
    return encode(BLE_CREDIT, struct.pack('<H', n))


def ble_disconnected(reason: int) -> bytes:
    return encode(BLE_DISCONNECTED, bytes([reason & 0xFF]))


def ble_security(encrypted: bool, authenticated: bool, bonded: bool) -> bytes:
    return encode(BLE_SECURITY, bytes([int(encrypted), int(authenticated), int(bonded)]))


def ble_mtu(mtu: int) -> bytes:
    return encode(BLE_MTU, struct.pack('<H', mtu))


def ble_subscribe(which: int, enabled: bool) -> bytes:
    return encode(BLE_SUBSCRIBE, bytes([which, int(enabled)]))


def ble_write(seq: int, value: bytes) -> bytes:
    return encode(BLE_WRITE, struct.pack('<H', seq) + value)


def ble_rssi(rssi: int) -> bytes:
    return encode(BLE_RSSI, struct.pack('<b', max(-128, min(127, int(rssi)))))


# ---------------------------------------------------------------------------- pixels

def rgb565_to_rgba(pixels: bytes, w: int, h: int) -> np.ndarray:
    """RGB565 LE -> (h, w, 4) uint8 RGBA."""
    v = np.frombuffer(pixels, dtype='<u2').reshape(h, w).astype(np.uint32)
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    out = np.empty((h, w, 4), dtype=np.uint8)
    out[..., 0] = (r * 527 + 23) >> 6
    out[..., 1] = (g * 259 + 33) >> 6
    out[..., 2] = (b * 527 + 23) >> 6
    out[..., 3] = 255
    return out


def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
