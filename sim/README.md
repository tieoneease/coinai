# Desktop simulator

The coin and the app, both running on this PC, talking to each other over virtual Bluetooth:

```
 browser UI (127.0.0.1:8740)          Android emulator (the real app)
   buttons, touch, joystick,                     │  Bluetooth: netsim
   battery, mic, speaker                         │
            │                                    │
   sim/host/coinsim.py ── Bumble peripheral ─────┘
   PMIC, IMU, GPIO, audio models
            │  TCP, sim/IPC.md
   sim/coin/build/coinsim.exe = firmware/main/*.c compiled for Windows
```

- **The coin** is the real firmware: `app_main.c`, `ble.c`, `audio.c`, `ui.c` and the rest are compiled unchanged. Only the layer below them is replaced: FreeRTOS, ESP-IDF services, the I2C/GPIO/I2S drivers, the display, and the radio (`firmware/main/ble_link.h` is the seam; `ble_nimble.c` on the coin, `sim/coin/src/ble_link_sim.c` here). LVGL renders the real UI and Opus encodes with the coin's settings. LittleFS and NVS live in `sim/.data/<name>/`.
- **The host** (`sim/host/`) is everything around the ESP32-S3: the AXP2101 and QMI8658 register models, the buttons, the panel, the mics and speaker, and a Bumble GATT peripheral that carries the coin's BLE. Every boot is one `coinsim.exe` process: deep sleep, a restart, a watchdog or a crash ends it, and the host starts the next boot with the right reset reason, wake cause and RTC memory. Details: [host/README.md](host/README.md).
- **The contract** between the two is [IPC.md](IPC.md).

## Build the coin

Needs a firmware build first (`scripts/idf firmware build`): the simulator takes its `sdkconfig.h` and managed components (LVGL, codec headers). The compiler is llvm-mingw (clang 23, per user, in `C:\Users\Tanker\tools`); CMake and Ninja are ESP-IDF's.

```bash
export PATH=/c/Users/Tanker/tools/llvm-mingw-20261006-ucrt-x86_64/bin:/c/Espressif/tools/cmake/3.30.2/bin:/c/Espressif/tools/ninja/1.12.1:$PATH
cmake -G Ninja -S sim/coin -B sim/coin/build -DCMAKE_C_COMPILER=clang.exe   # once; add -DOPUS_SOURCE=<opus-1.5.2.tar.gz> to skip the download
cmake --build sim/coin/build
```

Rebuild after changing firmware code. Windows locks a running exe: power the simulated coin off first (console `poweroff`, or **Cut power**).

## Run

```bash
PY=/c/Users/Tanker/tools/coinsim-venv/Scripts/python.exe

# Without the emulator: two Bumble controllers on a virtual link, and a scripted phone
$PY sim/host/tools/vlink.py
$PY sim/host/coinsim.py --ble tcp-client:127.0.0.1:9101 --power-on      # then open http://127.0.0.1:8740/
$PY sim/host/tools/central.py --passkey-from http://127.0.0.1:8740/api/state --keys /tmp/phone.json hello list "get 1 0"

# With the Android emulator: the coin on the emulator's Bluetooth (netsim, which the emulator starts)
$SDK/emulator/emulator.exe -avd coinai_api36
$PY sim/host/coinsim.py --power-on
cd android && ./gradlew installDebug
```

On the page: hold **PWR** (Space) to record, **BOOT** is B, both for 3 s power off, holding PWR 6 s is the PMIC's own long-press off. Click the round screen to touch it. The joystick tilts the IMU (with yaw, tap and shake beside it). The battery panel unplugs USB, drains or swaps the battery and can run time 60× or 600×. The microphone can be silence, a tone, the browser's microphone, or a file: `sim/samples/remind-me.ogg` is a spoken reminder that SamBot transcribes word for word. The console under the coin is its USB serial console, with every command in the top-level README.

### The Android emulator needs a hypervisor

The emulator runs only with Windows Hypervisor Platform (WHPX) enabled. That needs admin rights and a reboot, once:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All   # admin PowerShell, then reboot
```

The AVD `coinai_api36` (Android 16, Google APIs, x86_64, Pixel 7) is already created. Bluetooth in the emulator is netsim; `coinsim.py`'s default `--ble android-netsim` joins it, and the app finds the coin as `coinai` at C0:1A:51:00:00:01 (static random; see IPC.md, "Bluetooth address"). Pairing asks for the passkey shown on the simulated coin's screen and on the page. netsim itself is verified without the emulator: with `netsimd.exe --no-shutdown` running standalone, `coinsim.py` (default `--ble`) and `central.py --transport android-netsim --random` paired, re-encrypted with the bond, and moved a clip at 35 kB/s with the 15 ms interval applied. **Not yet run:** Android itself, meaning the app's companion-device pairing, background delivery and the emulator's Bluetooth stack. That waits for WHPX.

## Tests

```bash
cd sim/host && $PY -m pytest tests -q
```

`test_real_coin.py` runs the real `coinsim.exe` (skipped if it is not built): boot, a recorded sentence passing the speech gate, pairing, GET with size and CRC checked against the stored file, ACK and deletion, Standby and the PTT wake with RTC memory and the phone-set clock, and a tap faster than LVGL's input poll. The other tests use `fake_coin.py`, a Python stand-in for the coin.

## How faithful it is

Verified against the hardware's behaviour (2026-10-10): the boot log, the UI, PTT from Off and from Standby (reset reason 8, wake 0x8, RTC memory kept), tap vs hold, the cut-off clip recovery after a power loss mid-recording, the PMIC 6 s long press, BOOT+PWR power-off, the low-battery power-off (3 s below 3450 mV off USB), Standby after the delivery window on battery, the task watchdog (reset reason 6 after 5 s), the speaker amplifier, and the full PROTOCOL.md exchange with pairing and bonding.

Not the hardware:
- **Timing.** Windows threads on a desktop CPU: FreeRTOS priorities and core pinning are ignored, and latencies (`ptttest`, encode times, frame rates) say nothing about the coin. Measure those on the device.
- **Memory.** `stats` and stack high-water marks report the coin's measured figures and half of each requested stack, not this process. x86-64 stacks are reserved 8× larger.
- **Radio.** Bumble's controller or netsim, not the ESP32-S3's: no RF, RSSI comes from the slider, and the link-setup HCI timeout seen on the coin (HARDWARE.md, "BLE bring-up") does not happen here. The address is static random, not public.
- **Audio.** The host delivers mic audio at the coin's level; the ES7210 gain and ES8311 registers are not modelled.
- **Display.** LVGL's flushes go to the page; QSPI timing, tearing and the TE line are not modelled.
- **Power loss.** Cutting power kills the process: a clip loses what was in the C library's buffer, which tests the recovery, but not every way a flash write can tear.
- **Clock.** Time of day survives deep sleep but not a restart or watchdog reset; the chip keeps it through both.
- **Not simulated yet:** Wi-Fi and OTA (firmware phase 7).
