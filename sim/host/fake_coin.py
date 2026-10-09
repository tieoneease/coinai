"""A stand-in for coinsim.exe that speaks sim/IPC.md, for testing the host without the firmware.

It says HELLO, reads the PMIC and IMU over I2C, draws a test pattern, advertises, answers BLE_PASSKEY_REQ and
BLE_WRITE (HELLO -> STATUS indication, LIST -> LIST_RESULT, GET -> data notifications + CLIP_ENDED), keeps a boot
counter in RTC memory, and powers off on BOOT+PWR held 3 s. Console commands on stdin:

  sleep [timer_s]   deep sleep, ext1 on GPIO3 any-high (+ timer)
  restart           esp_restart (RESET 3)        wdt     task watchdog (RESET 6)
  panic             exit without SLEEP/RESET     poweroff  PMIC 0x10 bit 0
  mic on|off        MIC_STATE                     beep    speaker tone (GPIO46 on)
  adv on|off        advertising                   status  print state
"""

from __future__ import annotations

import asyncio
import math
import os
import pathlib
import struct
import sys
import threading
import time
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from coinhost import ipc, tlv  # noqa: E402

FW = 'fake-0.1'
T0 = time.monotonic()
GREEN, YELLOW, RED, RESET_C = '\x1b[0;32m', '\x1b[0;33m', '\x1b[0;31m', '\x1b[0m'


def logi(tag: str, msg: str, color: str = GREEN, level: str = 'I') -> None:
    ms = int((time.monotonic() - T0) * 1000)
    print(f'{color}{level} ({ms}) {tag}: {msg}{RESET_C}', flush=True)


def logw(tag, msg):
    logi(tag, msg, YELLOW, 'W')


class FakeCoin:
    def __init__(self) -> None:
        self.reader = self.writer = None
        self.seq = 0
        self.pending: dict[int, asyncio.Future] = {}
        self.levels = {}
        for kv in os.environ.get('COINSIM_GPIO', '').split(','):
            if '=' in kv:
                k, v = kv.split('=')
                self.levels[int(k)] = int(v)
        self.reset = int(os.environ.get('COINSIM_RESET', '1'))
        self.wake = int(os.environ.get('COINSIM_WAKE', '0'))
        self.ext1 = int(os.environ.get('COINSIM_EXT1', '0'))
        self.rtc_us = int(os.environ.get('COINSIM_RTC_US', '0'))
        self.rtc_boots = 0
        rtc_path = os.environ.get('COINSIM_RTC')
        if rtc_path and self.wake and os.path.exists(rtc_path):
            data = pathlib.Path(rtc_path).read_bytes()
            if len(data) >= 4:
                self.rtc_boots = struct.unpack_from('<I', data)[0]
        self.rtc_boots += 1
        self.credits = 12
        self.conn = False
        self.sec = (0, 0, 0)
        self.ind_seq = 0
        self.ind_waiters: dict[int, asyncio.Future] = {}
        self.battery = 255
        self.usb = False
        self.mic_bytes = 0
        self.both_task = None
        self.get_task = None
        self.clip = bytes((i * 7) & 0xFF for i in range(5000))

    # ------------------------------------------------------------ IPC
    def send(self, t: int, payload: bytes = b'') -> None:
        self.writer.write(ipc.encode(t, payload))

    async def i2c(self, addr: int, wbytes: bytes, rlen: int) -> tuple[bool, bytes]:
        self.seq = (self.seq + 1) & 0xFFFF
        fut = asyncio.get_running_loop().create_future()
        self.pending[self.seq] = fut
        self.send(ipc.I2C, struct.pack('<HBHH', self.seq, addr, len(wbytes), rlen) + wbytes)
        return await fut

    async def reg(self, addr: int, reg: int, n: int = 1) -> bytes:
        ok, data = await self.i2c(addr, bytes([reg]), n)
        if not ok:
            raise IOError(f'NACK from 0x{addr:02x}')
        return data

    async def wreg(self, addr: int, reg: int, val: int) -> None:
        await self.i2c(addr, bytes([reg, val]), 0)

    async def reader_loop(self) -> None:
        while True:
            try:
                t, p = await ipc.read_frame(self.reader)
            except (asyncio.IncompleteReadError, ConnectionError):
                logw('ipc', 'host closed the connection')
                os._exit(0)
            self.on_frame(t, p)

    def on_frame(self, t: int, p: bytes) -> None:
        if t == ipc.I2C_RESULT:
            seq, status = struct.unpack_from('<HB', p)
            fut = self.pending.pop(seq, None)
            if fut and not fut.done():
                fut.set_result((status == 0, p[3:]))
        elif t == ipc.GPIO_IN:
            pin, level = p[0], p[1]
            self.levels[pin] = level
            names = {3: 'PWR', 0: 'BOOT'}
            if pin in names:
                pressed = level == 1 if pin == 3 else level == 0
                logi('input', f'{names[pin]} {"pressed" if pressed else "released"} (GPIO{pin}={level})')
                self.check_gesture()
        elif t == ipc.TOUCH:
            pressed, x, y = struct.unpack_from('<BHH', p)
            if pressed:
                self.fill(max(0, x - 4), max(0, y - 4), 8, 8, 0xFFFF)
        elif t == ipc.MIC:
            self.mic_bytes += len(p)
        elif t == ipc.BLE_READY:
            logi('ble', 'radio ready; advertising every 1022.5 ms')
            self.send(ipc.BLE_TX_POWER, struct.pack('<b', 9))
            self.send(ipc.BLE_BATTERY, bytes([self.battery if self.battery <= 100 else 100]))
            self.adv(True)
        elif t == ipc.BLE_CONNECTED:
            conn, at = struct.unpack_from('<HB', p)
            addr = ':'.join(f'{b:02X}' for b in reversed(p[3:9]))
            self.conn, self.credits, self.sec = True, 12, (0, 0, 0)
            logi('ble', f'connected: handle {conn}, peer {addr} (type {at})')
        elif t == ipc.BLE_DISCONNECTED:
            self.conn = False
            if self.get_task:
                self.get_task.cancel()
            logi('ble', f'disconnected: reason 0x{p[0]:02x}')
            self.adv(True)
        elif t == ipc.BLE_SECURITY:
            self.sec = tuple(p[:3])
            logi('ble', f'security: encrypted={p[0]} authenticated={p[1]} bonded={p[2]}')
        elif t == ipc.BLE_MTU:
            logi('ble', f'MTU {struct.unpack_from("<H", p)[0]}')
        elif t == ipc.BLE_SUBSCRIBE:
            logi('ble', f'subscribe {"ctrl" if p[0] == 1 else "data"} {p[1]}')
        elif t == ipc.BLE_PASSKEY_REQ:
            passkey = int(os.environ.get('FAKE_PASSKEY', '123456'))
            logi('ble', 'pairing: passkey shown on screen')
            self.send(ipc.BLE_PASSKEY, struct.pack('<I', passkey))
        elif t == ipc.BLE_WRITE:
            seq = struct.unpack_from('<H', p)[0]
            self.on_write(seq, p[2:])
        elif t == ipc.BLE_IND_DONE:
            seq, ok = struct.unpack_from('<HB', p)
            fut = self.ind_waiters.pop(seq, None)
            if fut and not fut.done():
                fut.set_result(bool(ok))
        elif t == ipc.BLE_CREDIT:
            self.credits += struct.unpack_from('<H', p)[0]
        elif t == ipc.BLE_RSSI:
            logi('ble', f'rssi {struct.unpack_from("<b", p)[0]} dBm')
        elif t == ipc.BLE_ADV_DONE:
            logi('ble', 'advertising phase done')

    # ------------------------------------------------------------ BLE app
    def adv(self, on: bool, itvl: int = 1636, duration: int = -1) -> None:
        self.send(ipc.BLE_ADV, struct.pack('<BHi', int(on), itvl, duration))

    def indicate(self, value: bytes) -> asyncio.Future:
        self.ind_seq = (self.ind_seq + 1) & 0xFFFF
        fut = asyncio.get_running_loop().create_future()
        self.ind_waiters[self.ind_seq] = fut
        self.send(ipc.BLE_INDICATE, struct.pack('<H', self.ind_seq) + value)
        return fut

    def status_tlv(self) -> bytes:
        flags = (1 if self.usb and self.battery < 100 else 0) | (2 if self.usb else 0)
        return tlv.status(self.battery, flags, 1, FW)

    def on_write(self, seq: int, value: bytes) -> None:
        if not (self.sec[0] and self.sec[1]):
            logw('ble', 'write before authentication: insufficient authentication')
            self.send(ipc.BLE_WRITE_RESULT, struct.pack('<HB', seq, 1))
            return
        self.send(ipc.BLE_WRITE_RESULT, struct.pack('<HB', seq, 0))
        for t, v in tlv.split(value):
            logi('proto', tlv.describe_one(t, v))
            if t == tlv.HELLO:
                self.indicate(self.status_tlv())
            elif t == tlv.LIST:
                self.indicate(tlv.tlv(tlv.LIST_RESULT, bytes([0]) + struct.pack('<HIB', 7, len(self.clip), 1)))
            elif t == tlv.GET:
                clip, off = struct.unpack_from('<HI', v)
                if self.get_task:
                    self.get_task.cancel()
                self.get_task = asyncio.create_task(self.stream(clip, off))

    async def stream(self, clip: int, off: int) -> None:
        if clip != 7:
            self.indicate(tlv.error(tlv.GET, 1, clip))
            return
        chunk = 238
        while off < len(self.clip):
            while self.credits <= 0:
                await asyncio.sleep(0.002)
            self.credits -= 1
            self.send(ipc.BLE_NOTIFY, tlv.data_packet(clip, off, self.clip[off:off + chunk]))
            off += chunk
            await asyncio.sleep(0)
        crc = zlib.crc32(self.clip) & 0xFFFFFFFF
        self.indicate(tlv.tlv(tlv.CLIP_ENDED, struct.pack('<HIIIIQ', clip, len(self.clip), 1000, crc, 800, 0)))

    # ------------------------------------------------------------ display
    def fill(self, x: int, y: int, w: int, h: int, color: int) -> None:
        w = min(w, ipc.PANEL_SIZE - x)
        h = min(h, ipc.PANEL_SIZE - y)
        if w > 0 and h > 0:
            self.send(ipc.FRAME, struct.pack('<HHHH', x, y, w, h) + struct.pack('<H', color) * (w * h))

    def draw_pattern(self) -> None:
        bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0),
                (0, 0, 255), (40, 40, 40)]
        row = b''.join(struct.pack('<H', ipc.rgb_to_rgb565(*bars[x * len(bars) // ipc.PANEL_SIZE]))
                       for x in range(ipc.PANEL_SIZE))
        for y in range(0, 300, 50):  # LVGL-like partial flushes
            self.send(ipc.FRAME, struct.pack('<HHHH', 0, y, ipc.PANEL_SIZE, 50) + row * 50)
        band = {1: (0, 160, 255), 3: (0, 200, 0), 4: (255, 40, 40), 6: (255, 140, 0), 8: (160, 0, 255)}
        self.fill(0, 300, ipc.PANEL_SIZE, 166, ipc.rgb_to_rgb565(*band.get(self.reset, (90, 90, 90))))
        for i in range(min(self.rtc_boots, 20)):  # one white square per boot since power-on (RTC memory)
            self.fill(83 + i * 15, 360, 10, 10, 0xFFFF)

    # ------------------------------------------------------------ power
    def check_gesture(self) -> None:
        both = self.levels.get(3) == 1 and self.levels.get(0) == 0
        if both and self.both_task is None:
            self.both_task = asyncio.create_task(self.gesture())
        elif not both and self.both_task is not None:
            self.both_task.cancel()
            self.both_task = None

    async def gesture(self) -> None:
        await asyncio.sleep(3.0)
        logw('power', 'BOOT+PWR held 3 s: power off')
        await self.poweroff()

    async def poweroff(self) -> None:
        v = (await self.reg(0x34, 0x10))[0]
        await self.wreg(0x34, 0x10, v | 0x01)
        await asyncio.sleep(2.0)
        logi('power', 'still running 2 s after PMIC power-off', RED, 'E')

    async def deep_sleep(self, timer_s: float) -> None:
        while self.levels.get(3) == 1:  # the firmware waits for PWR release
            await asyncio.sleep(0.01)
        logi('power', f'standby: timer {timer_s:g} s')
        rtc = struct.pack('<I', self.rtc_boots)
        self.send(ipc.SLEEP, struct.pack('<QQBI', int(timer_s * 1e6), 1 << 3, 1, len(rtc)) + rtc)
        await self.writer.drain()
        os._exit(0)

    async def reset_with(self, reason: int) -> None:
        self.send(ipc.RESET, bytes([reason]))
        await self.writer.drain()
        os._exit(0)

    # ------------------------------------------------------------ console
    async def command(self, line: str) -> None:
        parts = line.split()
        if not parts:
            return
        cmd, args = parts[0], parts[1:]
        if cmd == 'sleep':
            await self.deep_sleep(float(args[0]) if args else 0)
        elif cmd == 'restart':
            await self.reset_with(3)
        elif cmd == 'wdt':
            logi('task_wdt', 'Task watchdog got triggered', RED, 'E')
            await self.reset_with(6)
        elif cmd == 'panic':
            print("Guru Meditation Error: Core 0 panic'ed (LoadProhibited)", flush=True)
            os._exit(3)
        elif cmd == 'poweroff':
            await self.poweroff()
        elif cmd == 'mic':
            on = bool(args) and args[0] == 'on'
            self.send(ipc.MIC_STATE, bytes([int(on)]))
            logi('audio', f'mic {"open" if on else "closed"}; {self.mic_bytes} bytes so far')
        elif cmd == 'beep':
            self.send(ipc.GPIO_OUT, bytes([46, 1]))
            pcm = b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * 880 * i / 16000))) for i in range(8000))
            self.send(ipc.SPEAKER, pcm)
        elif cmd == 'adv':
            self.adv(bool(args) and args[0] == 'on')
        elif cmd == 'status':
            print(f'boot reason {self.reset} wake 0x{self.wake:x} ext1 0x{self.ext1:x} rtc_us {self.rtc_us} '
                  f'rtc_boots {self.rtc_boots} credits {self.credits} sec {self.sec} mic_bytes {self.mic_bytes}',
                  flush=True)
        else:
            print(f'Unrecognized command: {cmd}', flush=True)
        print('coin> ', end='', flush=True)

    def stdin_thread(self, loop: asyncio.AbstractEventLoop) -> None:
        for line in sys.stdin:
            asyncio.run_coroutine_threadsafe(self.command(line.strip()), loop)

    # ------------------------------------------------------------ main
    async def main(self) -> None:
        port = int(os.environ['COINSIM_IPC'])
        self.reader, self.writer = await asyncio.open_connection('127.0.0.1', port)
        self.send(ipc.HELLO, struct.pack('<I', ipc.IPC_VERSION) + FW.encode())
        asyncio.create_task(self.reader_loop())
        logi('boot', f'fake coin {FW}: reset {self.reset}, wake 0x{self.wake:x}, ext1 0x{self.ext1:x}, '
                     f'rtc {self.rtc_us / 1e6:.3f} s, boots since power-on {self.rtc_boots}, gpio {self.levels}')

        ic = (await self.reg(0x34, 0x03))[0]
        st = await self.reg(0x34, 0x00, 2)
        vb = await self.reg(0x34, 0x34, 2)
        pct = (await self.reg(0x34, 0xA4))[0]
        vbat = ((vb[0] & 0x3F) << 8) | vb[1]
        self.usb = bool(st[0] & 0x20)
        self.battery = pct if st[0] & 0x08 else 255
        logi('pmic', f'IC type 0x{ic:02x}, status 0x{st[0]:02x} 0x{st[1]:02x}, VBAT {vbat} mV, {pct} %, '
                     f'USB {"yes" if self.usb else "no"}')
        await self.wreg(0x34, 0x90, (await self.reg(0x34, 0x90))[0] | 0x01)  # ALDO1 on

        who = await self.reg(0x6B, 0x00, 2)
        await self.wreg(0x6B, 0x60, 0xB0)
        for r, v in ((0x02, 0x40), (0x03, 0x16), (0x04, 0x56), (0x08, 0x03)):
            await self.wreg(0x6B, r, v)
        await asyncio.sleep(0.02)
        raw = struct.unpack('<7h', await self.reg(0x6B, 0x33, 14))
        logi('imu', f'WHO_AM_I 0x{who[0]:02x} rev 0x{who[1]:02x}; accel '
                    f'{raw[1] / 8192:.3f} {raw[2] / 8192:.3f} {raw[3] / 8192:.3f} g, gyro '
                    f'{raw[4] / 64:.1f} {raw[5] / 64:.1f} {raw[6] / 64:.1f} dps, {raw[0] / 256:.1f} C')
        ok, _ = await self.i2c(0x51, b'\x00', 1)
        logi('i2c', f'probe 0x51: {"ACK" if ok else "NACK"}')

        self.send(ipc.GPIO_OUT, bytes([1, 1]))
        self.send(ipc.GPIO_OUT, bytes([2, 1]))
        self.send(ipc.PANEL, bytes([1, 100]))
        self.draw_pattern()
        self.check_gesture()
        print('coin> ', end='', flush=True)
        threading.Thread(target=self.stdin_thread, args=(asyncio.get_running_loop(),), daemon=True).start()
        while True:
            await asyncio.sleep(1.0)
            raw = struct.unpack('<7h', await self.reg(0x6B, 0x33, 14))
            self.imu = raw


if __name__ == '__main__':
    asyncio.run(FakeCoin().main())
