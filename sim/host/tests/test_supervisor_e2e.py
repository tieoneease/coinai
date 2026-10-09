"""End-to-end supervisor tests against fake_coin.py: boot, PWR wake, timer wake, PMIC power-off, long press,
BOOT+PWR gesture, power-on from Off, crash/watchdog/restart relaunch, power cut, console, mic and touch."""

import asyncio
import pathlib
import sys
import time

import pytest

from coinhost import ipc
from coinhost.supervisor import Sim, strip_ansi

HOST = pathlib.Path(__file__).resolve().parent.parent
FAKE = [sys.executable, '-u', str(HOST / 'fake_coin.py')]


async def until(cond, timeout=10.0, what=''):
    t0 = time.monotonic()
    while not cond():
        if time.monotonic() - t0 > timeout:
            raise AssertionError(f'timed out waiting for {what or cond}')
        await asyncio.sleep(0.02)


def console_text(sim):
    return '\n'.join(strip_ansi(e['text']) for e in sim.console)


def run(coro_fn, tmp_path):
    async def main():
        sim = Sim(FAKE, tmp_path / 'data', 'none')
        await sim.start()
        try:
            await coro_fn(sim)
        finally:
            await sim.close()
    asyncio.run(main())


async def boot(sim):
    await sim.hold_button('pwr', 200)  # 0x27 on-time 128 ms
    await until(lambda: sim.state == 'active', what='HELLO')
    await until(lambda: sim.frames >= 7, what='test pattern')


def test_boot_and_device_models(tmp_path):
    async def body(sim):
        assert sim.state == 'off'
        await sim.hold_button('pwr', 60)  # shorter than the on-time: stays off
        await asyncio.sleep(0.2)
        assert sim.state == 'off' and sim.boot_count == 0
        await boot(sim)
        assert sim.reset_reason == ipc.RST_POWERON and sim.boot_count == 1
        assert sim.fw_version == 'fake-0.1'
        assert sim.panel_on is True and sim.brightness == 100
        assert sim.outputs[1] == 1 and sim.outputs[2] == 1
        assert sim.fb[0, 0] == 0xFFFF and sim.fb[400, 233] == ipc.rgb_to_rgb565(0, 160, 255)
        await until(lambda: 'probe 0x51: NACK' in console_text(sim), what='I2C log')
        txt = console_text(sim)
        assert 'IC type 0x4a' in txt and 'VBAT 4200 mV, 100 %' in txt and 'USB yes' in txt
        assert 'WHO_AM_I 0x05 rev 0x7c; accel' in txt
        az = float(txt.split('WHO_AM_I 0x05 rev 0x7c; accel ')[1].split(' g')[0].split()[2])
        assert az == pytest.approx(1.0, abs=0.02)  # flat, with sensor noise
        assert 'boots since power-on 1' in txt
        # GPIO_IN reaches the coin
        sim.set_button('boot', True)
        await until(lambda: 'BOOT pressed (GPIO0=0)' in console_text(sim))
        sim.set_button('boot', False)
        # touch -> TOUCH + INT low, draws a dot
        sim.set_touch(True, 100, 100)
        assert sim.levels[11] == 0
        await until(lambda: sim.fb[100, 100] == 0xFFFF and sim.fb[300, 100] != 0xFFFF or sim.fb[100, 100] == 0xFFFF)
        sim.set_touch(False, 100, 100)
        assert sim.levels[11] == 1
        # mic frames while MIC_STATE open
        await sim.console_input('mic on')
        await until(lambda: sim.mic_open)
        await asyncio.sleep(0.5)
        await sim.console_input('mic off')
        await until(lambda: not sim.mic_open)
        await sim.console_input('status')
        await until(lambda: 'mic_bytes' in console_text(sim))
        mic_bytes = int(console_text(sim).split('mic_bytes ')[-1].split()[0])
        assert 0.4 * 64000 < mic_bytes < 0.9 * 64000, mic_bytes  # ~real time, stereo s16 16 kHz = 64 kB/s
    run(body, tmp_path)


def test_sleep_wake_paths(tmp_path):
    async def body(sim):
        await boot(sim)
        # deep sleep, PWR wake (ext1)
        await sim.console_input('sleep')
        await until(lambda: sim.state == 'standby', what='standby')
        assert sim.sleep.ext1_mask == 0x8 and sim.sleep.ext1_mode == 1 and sim.rtc_path.exists()
        assert sim.panel_on is False
        sim.set_button('pwr', True)
        await until(lambda: sim.state == 'active', what='ext1 wake')
        sim.set_button('pwr', False)
        assert sim.reset_reason == ipc.RST_DEEPSLEEP and sim.wake_bits == ipc.WAKE_EXT1
        await until(lambda: 'boots since power-on 2' in console_text(sim))
        txt = console_text(sim)
        assert 'wake 0x8, ext1 0x8' in txt and "gpio {0: 1, 3: 1, 11: 1, 21: 0}" in txt
        # deep sleep with a timer
        await sim.console_input('sleep 0.5')
        await until(lambda: sim.state == 'standby')
        t0 = time.monotonic()
        await until(lambda: sim.state == 'active', what='timer wake')
        assert time.monotonic() - t0 >= 0.45
        assert sim.wake_bits == ipc.WAKE_TIMER and sim.boot_count == 3
        await until(lambda: 'boots since power-on 3' in console_text(sim))
        rtc_line = [line for line in console_text(sim).splitlines() if 'boot #3' in line][0]
        assert float(rtc_line.split('rtc ')[1].split(' s')[0]) > 0.5  # RTC clock kept counting
        # PMIC power-off clears RTC memory; PWR powers on again with reset 1
        await sim.console_input('poweroff')
        await until(lambda: sim.state == 'off', what='PMIC off')
        assert 'PMIC' in sim.off_reason and not sim.rtc_path.exists()
        await boot(sim)
        assert sim.reset_reason == ipc.RST_POWERON
        await until(lambda: console_text(sim).count('boots since power-on 1') == 2)
    run(body, tmp_path)


def test_crash_watchdog_restart(tmp_path):
    async def body(sim):
        await boot(sim)
        await sim.console_input('panic')
        await until(lambda: sim.boot_count == 2 and sim.state == 'active', what='panic relaunch')
        assert sim.reset_reason == ipc.RST_PANIC
        await sim.console_input('wdt')
        await until(lambda: sim.boot_count == 3 and sim.state == 'active')
        assert sim.reset_reason == ipc.RST_TASK_WDT
        await sim.console_input('restart')
        await until(lambda: sim.boot_count == 4 and sim.state == 'active')
        assert sim.reset_reason == ipc.RST_SW
        txt = console_text(sim)
        assert 'without SLEEP or RESET: panic' in txt and "Guru Meditation" in txt
    run(body, tmp_path)


def test_long_press_and_gesture_and_power_cut(tmp_path):
    async def body(sim):
        await boot(sim)
        # BOOT+PWR 3 s: the (fake) firmware powers off through the PMIC
        await asyncio.gather(sim.hold_button('boot', 3400), sim.hold_button('pwr', 3400))
        await until(lambda: sim.state == 'off', what='gesture power-off')
        assert 'PMIC' in sim.off_reason
        # PMIC long press (0x22 bit 1 set, 0x27 off-time 6 s) powers off from Active
        await boot(sim)
        await sim.hold_button('pwr', 6300)
        await until(lambda: sim.state == 'off', timeout=2)
        assert 'long press' in sim.off_reason
        # power cut while active; no power-on without supply; USB alone does not power on
        await boot(sim)
        await sim.cut_power()
        assert sim.state == 'off' and sim.off_reason == 'power cut'
        await sim.hold_button('pwr', 300)
        assert sim.state == 'off'
        await sim.set_power_inputs(usb=True)
        await asyncio.sleep(0.2)
        assert sim.state == 'off'
        # battery below 3.0 V without USB: Off
        await sim.set_power_inputs(battery_present=True, pct=50, usb=False)
        await boot(sim)
        await sim.set_power_inputs(volts=2.9)
        await asyncio.sleep(0.05)
        assert sim.state == 'off' and 'battery empty' in sim.off_reason
    run(body, tmp_path)


def test_missing_coin_executable(tmp_path):
    async def body(sim):
        await sim.hold_button('pwr', 200)
        await until(lambda: sim.state == 'error')
        assert 'cannot start the coin' in sim.error
    async def main():
        sim = Sim([str(tmp_path / 'nope.exe')], tmp_path / 'data', 'none')
        await sim.start()
        try:
            await body(sim)
        finally:
            await sim.close()
    asyncio.run(main())
