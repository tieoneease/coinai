"""The simulated board: power supervisor for the coin process, IPC server, device models and signal state."""

from __future__ import annotations

import asyncio
import codecs
import collections
import logging
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import time

import numpy as np

from . import audio, ipc, tlv
from .ble import BleBridge
from .imu import ADDR as IMU_ADDR, Motion, Qmi8658
from .pmic import ADDR as PMIC_ADDR, Axp2101
from .winjob import JobObject

log = logging.getLogger('coinsim')

ACK_ONLY_ADDRS = {0x18: 'ES8311', 0x40: 'ES7210', 0x5A: 'CST9217'}
INPUT_PINS = (0, 3, 11, 21)
OUTPUT_PINS = (46, 1, 2)
PIN_PWR, PIN_BOOT, PIN_TP_INT, PIN_IMU_INT1 = 3, 0, 11, 21
PIN_AMP, PIN_LCD_RST, PIN_TP_RST = 46, 1, 2

LOAD_MA = {'off': 0.02, 'standby': 0.3, 'booting': 90.0, 'active': 90.0, 'error': 0.02}
ANSI_RE = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub('', s)


class Sim:
    def __init__(self, coin_cmd: list[str], data_dir: pathlib.Path, ble_spec: str = 'none',
                 address: str = 'C0:1A:51:00:00:01', evidence_dir: pathlib.Path | None = None) -> None:
        self.coin_cmd = coin_cmd
        self.data_dir = data_dir
        self.evidence_dir = evidence_dir
        self.uploads_dir = data_dir / 'uploads'
        self.rtc_path = data_dir / 'rtc.bin'
        self.pmic = Axp2101()
        self.motion = Motion()
        self.imu = Qmi8658(self.motion)
        self.mic = audio.MicSource()
        self.ble = BleBridge(self, ble_spec, address, data_dir)
        self.hub = None  # web.Hub, set by the web layer
        self.job = JobObject()

        self.state = 'off'
        self.error: str | None = None
        self.fw_version: str | None = None
        self.ipc_version: int | None = None
        self.reset_reason: int | None = None
        self.wake_bits = 0
        self.boot_count = 0
        self.launch_time: float | None = None
        self.power_on_time: float | None = None  # RTC epoch: counts from power-on through sleep
        self.sleep: ipc.Sleep | None = None
        self.sleep_time: float | None = None
        self.off_reason: str | None = 'initial'
        self.levels = {0: 1, 3: 0, 11: 1, 21: 0}
        self.outputs: dict[int, int | None] = {46: None, 1: None, 2: None}
        self.touch = {'pressed': False, 'x': 0, 'y': 0}
        self.fb = np.zeros((ipc.PANEL_SIZE, ipc.PANEL_SIZE), dtype='<u2')
        self.panel_on: bool | None = None
        self.brightness = 100
        self.frames = 0
        self.mic_open = False
        self.mic_level = -120.0
        self.speaker_level = -120.0
        self._speaker_t = 0.0
        self.speaker_muted = False
        self.speaker_volume = 0.8
        self.sim_speed = 0.0  # battery simulation speed (0 = frozen)

        self.console: collections.deque = collections.deque(maxlen=4000)
        self.console_seq = 0
        self.ble_log: collections.deque = collections.deque(maxlen=600)
        self.ble_log_seq = 0
        self._data_run: dict | None = None

        self.port = 0
        self._server: asyncio.base_events.Server | None = None
        self.proc: asyncio.subprocess.Process | None = None
        self._proc_gen = 0
        self.exit_intent: tuple | None = None
        self.writer: asyncio.StreamWriter | None = None
        self._ipc_closed: asyncio.Event | None = None
        self._launch_levels: dict[int, int] = {}
        self._tasks: set[asyncio.Task] = set()
        self._standby_task: asyncio.Task | None = None
        self._mic_task: asyncio.Task | None = None
        self._pwr_press_id = 0
        self._crashes: collections.deque = collections.deque(maxlen=5)
        self._partial = ''
        self._partial_task: asyncio.Task | None = None
        self._closing = False
        self.dirty = asyncio.Event()

    # ================================================================ helpers
    def _spawn(self, coro, name: str | None = None) -> asyncio.Task:
        t = asyncio.create_task(coro, name=name)
        self._tasks.add(t)
        t.add_done_callback(self._tasks.discard)
        return t

    def mark_dirty(self) -> None:
        self.dirty.set()

    @property
    def coin_connected(self) -> bool:
        return self.writer is not None and not self.writer.is_closing()

    def send(self, frame: bytes) -> bool:
        w = self.writer
        if w is None or w.is_closing():
            return False
        w.write(frame)
        return True

    def sys_line(self, text: str) -> None:
        self._console_add('sys', text)

    def _console_add(self, kind: str, text: str) -> None:
        self.console_seq += 1
        entry = {'seq': self.console_seq, 'kind': kind, 'text': text}
        self.console.append(entry)
        if self.hub:
            self.hub.console(entry)

    def log_ble(self, kind: str, text: str, raw_len: int | None = None) -> None:
        self.ble_log_seq += 1
        entry = {'id': self.ble_log_seq, 't': time.strftime('%H:%M:%S') + f'.{int(time.time() * 1000) % 1000:03d}',
                 'kind': kind, 'text': text}
        if raw_len is not None:
            entry['len'] = raw_len
        self.ble_log.append(entry)
        if kind != 'data':
            self._data_run = None
        if self.hub:
            self.hub.ble_log(entry)

    def log_data(self, value: bytes) -> None:
        parsed = tlv.parse_data(value)
        if parsed is None:
            self.log_ble('data', f'data packet too short ({len(value)} bytes)')
            return
        clip, off, n = parsed
        now = time.monotonic()
        run = self._data_run
        if run is None or run['clip'] != clip or now - run['last'] > 1.0 or self.ble_log[-1] is not run['entry']:
            self.log_ble('data', '')
            run = self._data_run = {'clip': clip, 'first': off, 'end': off + n, 'pkts': 0, 'bytes': 0,
                                    't0': now, 'last': now, 'entry': self.ble_log[-1], 'pushed': 0.0}
        run['pkts'] += 1
        run['bytes'] += n
        run['end'] = max(run['end'], off + n)
        run['last'] = now
        dt = now - run['t0']
        rate = f', {run["bytes"] / dt / 1000:.1f} kB/s' if dt > 0.05 else ''
        run['entry']['text'] = (f'DATA clip {clip} bytes {run["first"]}..{run["end"]} '
                                f'({run["pkts"]} pkts, {run["bytes"]} B{rate})')
        if self.hub and (now - run['pushed'] > 0.25 or run['pkts'] == 1):
            run['pushed'] = now
            self.hub.ble_log(run['entry'])

    # ================================================================ start / stop
    async def start(self, power_on: bool = False) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)
        # Run state (bonds, uploads, audio) is never source: the dir ignores itself, as a venv does. This keeps
        # sim/.data/ out of git without a .gitignore outside sim/host/.
        ignore = self.data_dir / '.gitignore'
        if not ignore.exists():
            ignore.write_text('# created by coinsim: simulator run state, not source\n*\n')
        self.uploads_dir.mkdir(parents=True, exist_ok=True)
        if self.rtc_path.exists():
            self.rtc_path.unlink()  # the host starts with the coin Off: RTC memory is gone
        self._server = await asyncio.start_server(self._on_ipc_client, '127.0.0.1', 0)
        self.port = self._server.sockets[0].getsockname()[1]
        log.info('IPC on 127.0.0.1:%d', self.port)
        self.ble.start()
        self._spawn(self._tick_loop(), 'tick')
        self.sys_line(f'host ready; IPC port {self.port}; coin: {" ".join(self.coin_cmd)}')
        if power_on:
            await self.power_on('--power-on')

    async def close(self) -> None:
        self._closing = True
        await self._kill_proc()
        if self._server:
            self._server.close()
        if self.writer:
            self.writer.close()
        await self.ble.close()
        watchers = [t for t in self._tasks if t.get_name().startswith('coin-')]
        if watchers:  # let them read the killed process's stdout to EOF, so its pipe closes inside the loop
            await asyncio.wait(watchers, timeout=2)
        for t in list(self._tasks):
            t.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)

    async def _kill_proc(self) -> None:
        proc = self.proc
        if proc is None:
            return
        self._proc_gen += 1  # its exit is no longer interesting
        self.proc = None
        if proc.returncode is None:
            try:
                proc.kill()
            except ProcessLookupError:
                pass
            try:
                await asyncio.wait_for(proc.wait(), 5)
            except asyncio.TimeoutError:
                log.warning('coin process %s did not exit', proc.pid)
        self._close_stdin(proc)
        self._close_ipc()

    @staticmethod
    def _close_stdin(proc) -> None:
        if proc.stdin is not None:
            try:
                proc.stdin.close()
            except Exception:  # noqa: BLE001
                pass

    def _close_ipc(self) -> None:
        if self.writer is not None:
            try:
                self.writer.close()
            except Exception:  # noqa: BLE001
                pass
            self.writer = None

    # ================================================================ power state machine
    async def power_on(self, why: str) -> None:
        if self.state not in ('off', 'error'):
            return
        if not self.pmic.power_ok:
            self.sys_line(f'power on refused ({why}): no USB and no usable battery')
            return
        self.power_on_time = time.monotonic()
        if self.rtc_path.exists():
            self.rtc_path.unlink()
        self.sleep = None
        self.sys_line(f'=== power on ({why}) ===')
        await self._launch(ipc.RST_POWERON)

    async def power_off(self, why: str) -> None:
        if self.state == 'off':
            return
        self.sys_line(f'=== power off: {why} ===')
        self.exit_intent = ('off',)
        await self._kill_proc()
        self._coin_stopped()
        self._cancel_standby()
        self.state = 'off'
        self.off_reason = why
        self.error = None
        self.sleep = None
        self.power_on_time = None
        self.launch_time = None
        if self.rtc_path.exists():
            self.rtc_path.unlink()
        self.outputs = {46: 0, 1: 0, 2: 0}
        self.mark_dirty()

    def _cancel_standby(self) -> None:
        if self._standby_task:
            self._standby_task.cancel()
            self._standby_task = None

    def _coin_stopped(self) -> None:
        """Common clean-up when the coin process is gone for any reason."""
        self._stop_mic()
        self.panel_on = False
        self.ble.on_coin_gone()
        self.touch['pressed'] = False
        self._flush_partial()
        self.mark_dirty()

    def gpio_env(self) -> str:
        return ','.join(f'{p}={self.levels[p]}' for p in INPUT_PINS)

    async def _launch(self, reset_reason: int, wake: int = 0, ext1: int = 0) -> None:
        self.boot_count += 1
        self.reset_reason = reset_reason
        self.wake_bits = wake
        self.exit_intent = None
        self.fw_version = None
        self.state = 'booting'
        self.error = None
        self.launch_time = time.monotonic()
        self.fb[:] = 0
        # until the first PANEL message the panel state is unknown (None) and FRAMEs are shown at full
        # brightness, so a coin that never sends PANEL still shows its UI.
        self.panel_on = None
        self.frames = 0
        if self.hub:
            self.hub.frame(0, 0, ipc.PANEL_SIZE, ipc.PANEL_SIZE)
        rtc_us = int((time.monotonic() - (self.power_on_time or time.monotonic())) * 1e6)
        env = dict(os.environ)
        env.update({
            'COINSIM_IPC': str(self.port),
            'COINSIM_DATA': str(self.data_dir),
            'COINSIM_RESET': str(reset_reason),
            'COINSIM_WAKE': str(wake),
            'COINSIM_EXT1': str(ext1),
            'COINSIM_RTC_US': str(rtc_us),
            'COINSIM_GPIO': self.gpio_env(),
            'PYTHONUNBUFFERED': '1',
        })
        # COINSIM_RTC is passed on every launch since the last SLEEP; the coin loads it only on a deep-sleep wake
        # (COINSIM_RESET 8), as the ESP-IDF bootloader keeps RTC_DATA_ATTR only then (IPC.md).
        if self.rtc_path.exists():
            env['COINSIM_RTC'] = str(self.rtc_path)
        else:
            env.pop('COINSIM_RTC', None)
        self._launch_levels = dict(self.levels)
        what = ipc.RESET_NAMES.get(reset_reason, str(reset_reason))
        wake_s = f', wake 0x{wake:x}' + (f' ext1 0x{ext1:x}' if ext1 else '') if wake else ''
        self.sys_line(f'--- boot #{self.boot_count}: reset {reset_reason} ({what}){wake_s}, '
                      f'rtc {rtc_us / 1e6:.3f} s, gpio {self.gpio_env()} ---')
        flags = 0
        if sys.platform == 'win32':
            flags = subprocess.CREATE_NEW_PROCESS_GROUP  # Ctrl+C in the host console must not reach the coin
        try:
            proc = await asyncio.create_subprocess_exec(
                *self.coin_cmd, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT, env=env, creationflags=flags)
        except OSError as e:
            self.state = 'error'
            self.error = f'cannot start the coin ({" ".join(self.coin_cmd)}): {e}'
            self.sys_line(self.error)
            self.mark_dirty()
            return
        self.job.assign(proc.pid)
        self.proc = proc
        self._proc_gen += 1
        gen = self._proc_gen
        self._spawn(self._watch(proc, gen), f'coin-{gen}')
        self.mark_dirty()

    async def _watch(self, proc: asyncio.subprocess.Process, gen: int) -> None:
        decoder = codecs.getincrementaldecoder('utf-8')('replace')
        try:
            while True:
                chunk = await proc.stdout.read(65536)
                if not chunk:
                    break
                self._console_out(decoder.decode(chunk))
        except Exception as e:  # noqa: BLE001
            log.debug('stdout: %s', e)
        rc = await proc.wait()
        self._close_stdin(proc)
        self._console_out(decoder.decode(b'', final=True))
        self._flush_partial()
        if gen != self._proc_gen or self._closing:
            return
        # The IPC socket may still hold the SLEEP/RESET frame: let the reader drain it first.
        closed = self._ipc_closed
        if closed is not None and self.writer is not None:
            try:
                await asyncio.wait_for(closed.wait(), 2.0)
            except asyncio.TimeoutError:
                pass
        if gen != self._proc_gen:
            return
        self.proc = None
        self._close_ipc()
        self._coin_stopped()
        await self._on_exit(rc)

    async def _on_exit(self, rc: int) -> None:
        intent = self.exit_intent
        self.exit_intent = None
        if intent and intent[0] == 'sleep':
            self._enter_standby(intent[1])
        elif intent and intent[0] == 'reset':
            reason = intent[1]
            self.sys_line(f'--- restart: reset reason {reason} ({ipc.RESET_NAMES.get(reason, "?")}) ---')
            await self._launch(reason)
        elif intent and intent[0] == 'off':
            pass
        else:
            now = time.monotonic()
            self._crashes.append(now)
            self.sys_line(f'--- coin process exited (code {rc}) without SLEEP or RESET: panic ---')
            if len(self._crashes) >= 3 and now - self._crashes[-3] < 10:
                self.sys_line('--- crash loop: waiting 2 s before the next boot ---')
                self.state = 'booting'
                self.mark_dirty()
                await asyncio.sleep(2.0)
                if self.state != 'booting' or self.proc is not None:
                    return
            await self._launch(ipc.RST_PANIC)

    def _enter_standby(self, sl: ipc.Sleep) -> None:
        self.state = 'standby'
        self.sleep = sl
        self.sleep_time = time.monotonic()
        mode = 'any high' if sl.ext1_mode else 'any low'
        timer = f'timer {sl.timer_us / 1e6:g} s' if sl.timer_us else 'no timer'
        self.sys_line(f'--- deep sleep: {timer}, ext1 mask 0x{sl.ext1_mask:x} ({mode}), rtc {len(sl.rtc)} bytes ---')
        self._cancel_standby()
        if sl.timer_us:
            self._standby_task = self._spawn(self._standby_timer(sl), 'standby-timer')
        self.mark_dirty()
        self._check_ext1()

    async def _standby_timer(self, sl: ipc.Sleep) -> None:
        await asyncio.sleep(sl.timer_us / 1e6)
        if self.state == 'standby' and self.sleep is sl:
            self._standby_task = None
            await self._wake(ipc.WAKE_TIMER, 0)

    def _check_ext1(self) -> None:
        sl = self.sleep
        if self.state != 'standby' or sl is None or not sl.ext1_mask:
            return
        # the ext1 wake status (COINSIM_EXT1) holds the masked pins that satisfy the mode at wake time, which is
        # what esp_sleep_get_ext1_wakeup_status() reports on the S3. Level-triggered: a pin already at the wake level
        # when SLEEP arrives wakes the coin at once, as on hardware.
        hits = 0
        for pin in INPUT_PINS:
            if sl.ext1_mask & (1 << pin):
                level = self.levels[pin]
                if (sl.ext1_mode and level) or (not sl.ext1_mode and not level):
                    hits |= 1 << pin
        if hits:
            self._spawn(self._wake(ipc.WAKE_EXT1, hits), 'ext1-wake')

    async def _wake(self, wake: int, ext1: int) -> None:
        if self.state != 'standby':
            return
        self._cancel_standby()
        self.state = 'booting'
        await self._launch(ipc.RST_DEEPSLEEP, wake, ext1)

    # ================================================================ inputs
    def set_level(self, pin: int, level: int) -> None:
        level = 1 if level else 0
        if self.levels.get(pin) == level:
            return
        self.levels[pin] = level
        self.send(ipc.gpio_in(pin, level))
        self._check_ext1()
        self.mark_dirty()

    def set_button(self, name: str, pressed: bool) -> None:
        if name == 'pwr':
            before = self.levels[PIN_PWR]
            self.set_level(PIN_PWR, 1 if pressed else 0)
            if pressed and not before:
                self._pwr_press_id += 1
                self._spawn(self._pwr_hold(self._pwr_press_id, time.monotonic()), 'pwr-hold')
        elif name == 'boot':
            self.set_level(PIN_BOOT, 0 if pressed else 1)
        else:
            raise ValueError(f'unknown button {name}')

    @property
    def pwr_pressed(self) -> bool:
        return self.levels[PIN_PWR] == 1

    @property
    def boot_pressed(self) -> bool:
        return self.levels[PIN_BOOT] == 0

    async def _pwr_hold(self, press_id: int, t0: float) -> None:
        """The AXP2101 PWR key: on-time powers on from Off; with 0x22 bit 1 the off-time powers off."""
        def held() -> bool:
            return press_id == self._pwr_press_id and self.pwr_pressed

        if self.state in ('off', 'error'):
            await asyncio.sleep(self.pmic.on_time_s)
            if held() and self.state in ('off', 'error'):
                await self.power_on(f'PWR held {int(self.pmic.on_time_s * 1000)} ms')
        off_s = self.pmic.off_time_s
        remaining = t0 + off_s - time.monotonic()
        if remaining > 0:
            await asyncio.sleep(remaining)
        if held() and self.pmic.long_press_off_enabled and self.state != 'off':
            await self.power_off(f'PWR long press {off_s:g} s')

    async def hold_button(self, name: str, ms: int) -> None:
        self.set_button(name, True)
        await asyncio.sleep(ms / 1000)
        self.set_button(name, False)

    def set_touch(self, pressed: bool, x: int, y: int) -> None:
        x = max(0, min(ipc.PANEL_SIZE - 1, int(x)))
        y = max(0, min(ipc.PANEL_SIZE - 1, int(y)))
        self.touch = {'pressed': bool(pressed), 'x': x, 'y': y}
        self.send(ipc.touch(pressed, x, y))
        # the CST9217 INT line (GPIO11) is modelled as low while a finger is down, high otherwise. GPIO21
        # (IMU INT1) stays low: the firmware enables no QMI8658 interrupt.
        self.set_level(PIN_TP_INT, 0 if pressed else 1)
        self.mark_dirty()

    async def set_power_inputs(self, battery_present: bool | None = None, usb: bool | None = None,
                               volts: float | None = None, pct: float | None = None) -> None:
        p = self.pmic
        if battery_present is not None:
            p.battery_present = bool(battery_present)
        if usb is not None:
            p.usb = bool(usb)
        if volts is not None:
            p.set_battery_volts(float(volts))
        if pct is not None:
            p.battery_pct = max(0.0, min(100.0, float(pct)))
        if not p.battery_present and not p.usb:
            # PMIC registers keep their written values through a soft power-off and sleep; they return to
            # the table in IPC.md only when the PMIC itself loses power (no battery and no USB).
            p.reset_registers()
        await self._check_supply()
        self.mark_dirty()

    async def _check_supply(self) -> None:
        if self.state != 'off' and not self.pmic.power_ok:
            p = self.pmic
            why = 'power cut' if not p.battery_present and not p.usb else f'battery empty ({p.vbat_mv} mV, no USB)'
            await self.power_off(why)

    async def cut_power(self) -> None:
        await self.set_power_inputs(battery_present=False, usb=False)

    # ================================================================ console
    def _console_out(self, text: str) -> None:
        if not text:
            return
        text = self._partial + text.replace('\r\n', '\n')
        lines = text.split('\n')
        self._partial = lines.pop()
        for line in lines:
            self._console_add('out', line.rstrip('\r'))
        if self._partial:
            if self._partial_task is None or self._partial_task.done():
                self._partial_task = self._spawn(self._partial_flush_later(), 'partial')

    async def _partial_flush_later(self) -> None:
        await asyncio.sleep(0.15)
        self._flush_partial()

    def _flush_partial(self) -> None:
        if self._partial:
            self._console_add('out', self._partial.rstrip('\r'))
            self._partial = ''

    async def console_input(self, line: str) -> bool:
        self._console_add('in', line)
        proc = self.proc
        if proc is None or proc.stdin is None:
            self.sys_line('(console input dropped: the coin is not running)')
            return False
        try:
            proc.stdin.write((line + '\n').encode())
            await proc.stdin.drain()
        except (ConnectionError, OSError) as e:
            self.sys_line(f'(console input failed: {e})')
            return False
        return True

    # ================================================================ IPC
    async def _on_ipc_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        sock = writer.get_extra_info('socket')
        if sock is not None:
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
        if self.writer is not None:
            log.warning('a second coin connected; dropping the previous IPC connection')
            self._close_ipc()
        self.writer = writer
        closed = self._ipc_closed = asyncio.Event()
        # Inputs that changed between launch and connect.
        for pin in INPUT_PINS:
            if self._launch_levels.get(pin, self.levels[pin]) != self.levels[pin]:
                self.send(ipc.gpio_in(pin, self.levels[pin]))
        self.mark_dirty()
        try:
            while True:
                t, p = await ipc.read_frame(reader)
                try:
                    await self._dispatch(t, p)
                except ipc.ProtocolError as e:
                    log.warning('IPC %s: %s', ipc.NAMES.get(t, hex(t)), e)
                    self.sys_line(f'(IPC error in {ipc.NAMES.get(t, hex(t))}: {e})')
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        except ipc.ProtocolError as e:
            self.sys_line(f'(IPC stream error: {e})')
        finally:
            closed.set()
            if self.writer is writer:
                self.writer = None
                self.mark_dirty()
            try:
                writer.close()
            except Exception:  # noqa: BLE001
                pass

    async def _dispatch(self, t: int, p: bytes) -> None:
        if t == ipc.I2C:
            self._i2c(ipc.decode_i2c(p))
        elif t == ipc.FRAME:
            f = ipc.decode_frame(p)
            self.fb[f.y:f.y + f.h, f.x:f.x + f.w] = np.frombuffer(f.pixels, dtype='<u2').reshape(f.h, f.w)
            self.frames += 1
            if self.hub:
                self.hub.frame(f.x, f.y, f.w, f.h)
        elif t == ipc.SPEAKER:
            pcm = np.frombuffer(p[:len(p) // 2 * 2], dtype='<i2')
            self.speaker_level = audio.dbfs(pcm)
            self._speaker_t = time.monotonic()
            if self.outputs.get(PIN_AMP) == 1 and self.hub and not self.speaker_muted:
                self.hub.speaker(p[:len(p) // 2 * 2])
        elif t == ipc.HELLO:
            self.ipc_version, self.fw_version = ipc.decode_hello(p)
            if self.ipc_version != ipc.IPC_VERSION:
                self.sys_line(f'(coin speaks IPC version {self.ipc_version}; host is {ipc.IPC_VERSION})')
            self.state = 'active'
            self.ble.on_coin_hello(self.fw_version)
            self.mark_dirty()
        elif t == ipc.GPIO_OUT:
            pin, level = p[0], p[1]
            self.outputs[pin] = level
            self.mark_dirty()
        elif t == ipc.PANEL:
            self.panel_on = bool(p[0])
            self.brightness = p[1]
            self.mark_dirty()
        elif t == ipc.MIC_STATE:
            self._set_mic_open(bool(p[0]))
        elif t == ipc.SLEEP:
            sl = ipc.decode_sleep(p)
            self.rtc_path.write_bytes(sl.rtc)
            self.exit_intent = ('sleep', sl)
        elif t == ipc.RESET:
            self.exit_intent = ('reset', p[0] if p else ipc.RST_SW)
        elif 0x10 <= t <= 0x1B:
            self.ble.on_coin_message(t, p)
        else:
            log.debug('unknown IPC type 0x%02x (%d bytes)', t, len(p))

    def _i2c(self, req: ipc.I2cRequest) -> None:
        if req.addr == PMIC_ADDR:
            rbytes, events = self.pmic.i2c(req.wbytes, req.rlen)
            self.send(ipc.i2c_result(req.seq, True, rbytes))
            if 'poweroff' in events:
                self._spawn(self.power_off('PMIC software power-off (0x10 bit 0)'), 'pmic-off')
            elif events:
                self.mark_dirty()
        elif req.addr == IMU_ADDR:
            self.send(ipc.i2c_result(req.seq, True, self.imu.i2c(req.wbytes, req.rlen)))
        elif req.addr in ACK_ONLY_ADDRS:
            self.send(ipc.i2c_result(req.seq, True, bytes(req.rlen)))
        else:
            self.send(ipc.i2c_result(req.seq, False, bytes(req.rlen)))

    # ================================================================ microphone
    def _set_mic_open(self, open_: bool) -> None:
        if open_ == self.mic_open:
            return
        self.mic_open = open_
        if open_:
            self.mic.rewind()
            self._mic_task = self._spawn(self._mic_loop(), 'mic')
        else:
            self._stop_mic()
        self.mark_dirty()

    def _stop_mic(self) -> None:
        self.mic_open = False
        if self._mic_task:
            self._mic_task.cancel()
            self._mic_task = None
        self.mic_level = -120.0

    async def _mic_loop(self) -> None:
        period = audio.CHUNK / audio.RATE
        t0 = time.monotonic() - 2 * period  # start 40 ms ahead so the coin's buffer never starts empty
        n = 0
        while self.mic_open:
            due = t0 + n * period
            delay = due - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            elif delay < -0.25:
                t0 = time.monotonic() - n * period  # fell behind (debugger, sleep): resync, don't burst
            chunk = self.mic.read(audio.CHUNK)
            self.mic_level = audio.dbfs(chunk)
            if not self.send(ipc.mic(audio.to_stereo_bytes(chunk))):
                break
            n += 1

    async def select_mic(self, source: str, file: str | None = None, tone_hz: float | None = None,
                         tone_dbfs: float | None = None, loop: bool | None = None) -> None:
        if source not in audio.SOURCES:
            raise ValueError(f'mic source must be one of {audio.SOURCES}')
        if tone_hz is not None:
            self.mic.tone_hz = max(20.0, min(7900.0, float(tone_hz)))
        if tone_dbfs is not None:
            self.mic.tone_dbfs = max(-90.0, min(0.0, float(tone_dbfs)))
        if loop is not None:
            self.mic.loop = bool(loop)
        if source == 'file':
            if file:
                path = self.resolve_audio(file)
                pcm = await asyncio.to_thread(audio.decode_file, path)
                self.mic.set_file(str(path), file, pcm)
            elif self.mic.file_pcm is None:
                raise ValueError('no file selected')
        self.mic.kind = source
        self.mark_dirty()

    def _audio_dirs(self) -> dict[str, pathlib.Path | None]:
        # sim/samples: speech for the microphone, committed with the simulator.
        samples = self.evidence_dir.parent / 'sim' / 'samples' if self.evidence_dir else None
        return {'samples': samples, 'evidence': self.evidence_dir, 'uploads': self.uploads_dir}

    def audio_files(self) -> list[dict]:
        out = []
        for label, base in self._audio_dirs().items():
            if base is None or not base.is_dir():
                continue
            for f in sorted(base.iterdir()):
                if f.is_file() and f.suffix.lower() in audio.AUDIO_EXTS:
                    out.append({'name': f'{label}/{f.name}', 'size': f.stat().st_size})
        return out

    def resolve_audio(self, name: str) -> pathlib.Path:
        label, _, fname = name.replace('\\', '/').partition('/')
        base = self._audio_dirs().get(label)
        if base is None or not fname or '/' in fname or fname.startswith('.'):
            raise ValueError(f'unknown audio file {name!r}')
        path = (base / fname).resolve()
        if path.parent != base.resolve() or not path.is_file():
            raise ValueError(f'unknown audio file {name!r}')
        return path

    # ================================================================ periodic
    async def _tick_loop(self) -> None:
        last = time.monotonic()
        acc = 0.0
        while True:
            await asyncio.sleep(0.01)
            now = time.monotonic()
            dt = now - last
            last = now
            self.motion.step(dt)
            if self.speaker_level > -120 and now - self._speaker_t > 0.25:
                self.speaker_level = -120.0
                self.mark_dirty()
            acc += dt
            if acc >= 0.1:
                if self.sim_speed > 0:
                    load = LOAD_MA.get(self.state, 0.0) + (10.0 if self.mic_open else 0.0)
                    before = round(self.pmic.battery_pct, 1)
                    self.pmic.step(acc * self.sim_speed, load)
                    if round(self.pmic.battery_pct, 1) != before:
                        self.mark_dirty()
                    await self._check_supply()
                acc = 0.0
                if self.state in ('active', 'booting', 'standby') or self.mic_open:
                    self.mark_dirty()

    # ================================================================ snapshot
    def snapshot(self) -> dict:
        now = time.monotonic()
        sl = self.sleep if self.state == 'standby' else None
        standby = None
        if sl is not None:
            left = None
            if sl.timer_us:
                left = max(0.0, sl.timer_us / 1e6 - (now - (self.sleep_time or now)))
            standby = {'timer_s': sl.timer_us / 1e6 if sl.timer_us else None, 'remaining_s': left,
                       'ext1_mask': sl.ext1_mask, 'ext1_mode': 'any high' if sl.ext1_mode else 'any low',
                       'pwr_wakes': bool(sl.ext1_mask & (1 << PIN_PWR)) and sl.ext1_mode == 1,
                       'rtc_bytes': len(sl.rtc)}
        p = self.pmic
        return {
            'power': {
                'state': self.state,
                'error': self.error,
                'reset_reason': self.reset_reason,
                'reset_name': ipc.RESET_NAMES.get(self.reset_reason) if self.reset_reason else None,
                'wake': self.wake_bits,
                'boot_count': self.boot_count,
                'uptime_s': round(now - self.launch_time, 1) if self.launch_time and self.state in ('booting', 'active') else None,
                'rtc_s': round(now - self.power_on_time, 1) if self.power_on_time else None,
                'standby': standby,
                'off_reason': self.off_reason if self.state == 'off' else None,
                'fw_version': self.fw_version,
                'ipc_connected': self.coin_connected,
                'pid': self.proc.pid if self.proc else None,
            },
            'buttons': {'pwr': self.pwr_pressed, 'boot': self.boot_pressed},
            'pins': {
                'gpio3_pwr': self.levels[PIN_PWR], 'gpio0_boot': self.levels[PIN_BOOT],
                'gpio11_tp_int': self.levels[PIN_TP_INT], 'gpio21_imu_int1': self.levels[PIN_IMU_INT1],
                'gpio46_amp': self.outputs.get(PIN_AMP), 'gpio1_lcd_rst': self.outputs.get(PIN_LCD_RST),
                'gpio2_tp_rst': self.outputs.get(PIN_TP_RST),
                'dcdc1': int(p.dcdc1_on) if self.state != 'off' else 0,
                'aldo1': int(p.aldo1_on) if self.state != 'off' else 0,
                'mic': int(self.mic_open),
            },
            'touch': self.touch,
            'panel': {'on': self.panel_on, 'brightness': self.brightness, 'frames': self.frames},
            'pmic': p.snapshot(),
            'imu': {'motion': self.motion.snapshot(), 'regs': self.imu.data_values()},
            'audio': {**self.mic.snapshot(), 'mic_open': self.mic_open, 'mic_level_dbfs': round(self.mic_level, 1),
                      'speaker_level_dbfs': round(self.speaker_level, 1), 'amp': self.outputs.get(PIN_AMP) == 1,
                      'speaker_muted': self.speaker_muted, 'speaker_volume': self.speaker_volume},
            'ble': self.ble.snapshot(),
            'sim': {'speed': self.sim_speed, 'coin_cmd': self.coin_cmd, 'data_dir': str(self.data_dir),
                    'ipc_port': self.port},
        }
