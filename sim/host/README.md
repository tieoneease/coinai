# coinsim host

The host side of the desktop simulator for the coin. It implements the host end of the contract in `../IPC.md`:

- It supervises `coinsim.exe`.
- It models the board around it: the AXP2101 PMIC, the QMI8658 IMU, the I2C bus map, the GPIOs, the mic and the speaker.
- It bridges the coin's NimBLE-shaped `BLE_*` messages onto a real Bumble peripheral.
- It serves the instrument-panel UI: at https://coinsim.localhost/ when started through [portless](https://github.com/vercel-labs/portless) (it listens on portless's `$PORT`), otherwise at http://127.0.0.1:8740/ (`--http-port`).

The Python is `C:/Users/Tanker/tools/coinsim-venv` (3.13, with bumble, aiohttp, numpy, av, grpcio and pytest).

```sh
PY=/c/Users/Tanker/tools/coinsim-venv/Scripts/python.exe

# The real coin (default --coin sim/coin/build/coinsim.exe) on the Android emulator's netsim radio:
portless coinsim $PY sim/host/coinsim.py

# Without the coin build: the Python stand-in (see fake_coin.py for its stdin commands)
portless coinsim $PY sim/host/coinsim.py --coin sim/host/fake_coin.py --power-on
```

To power the coin on, hold **PWR** (Space). **BOOT** is B. Holding both for 3 s powers the coin off.

Ctrl+C or Ctrl+Break stops the host. The coin runs in a kill-on-close Job Object, so it also dies if the host is killed.

## Bluetooth

| `--ble` | Radio |
|---|---|
| `android-netsim` (default) | The emulator's netsim. With no emulator running, the UI shows **No radio** and retries every 5 s. |
| `tcp-client:127.0.0.1:9101` | The virtual air of `tools/vlink.py`: two Bumble controllers on one LocalLink. |
| `none` | No radio. The coin still gets `BLE_READY`. |

Testing with no emulator or phone:

```sh
$PY sim/host/tools/vlink.py                                       # coin side :9101, phone side :9102
portless coinsim $PY sim/host/coinsim.py --coin sim/host/fake_coin.py --ble tcp-client:127.0.0.1:9101 --power-on
$PY sim/host/tools/central.py --transport tcp-client:127.0.0.1:9102 \
    --passkey-from https://coinsim.localhost/api/state --keys /tmp/phone_keys.json hello list "get 7 0"
```

`central.py` behaves like the phone:

1. Connects, sets MTU 247 and subscribes.
2. Writes. On ATT 0x05 it pairs, typing the passkey that the coin shows (read from `/api/state`), then retries.
3. With `--keys`, it re-encrypts with the stored bond on later runs.

`--passkey-from` accepts the portless URL: `central.py` resolves `*.localhost` names to 127.0.0.1 itself (RFC 6761), because Windows' resolver does not and portless would otherwise need an admin hosts-file entry. The certificate is still checked against the real name.

## Layout

| Path | What |
|---|---|
| `coinsim.py` | Entry point: `--coin --data --http-port --ble --address --power-on --battery --no-usb` |
| `coinhost/ipc.py` | Frame codec and message encoders/decoders for IPC.md |
| `coinhost/supervisor.py` | Power states, launch/relaunch, sleep/wake, RTC blob, PWR/BOOT, I2C map, GPIOs, mic loop, console |
| `coinhost/pmic.py`, `imu.py` | AXP2101 and QMI8658 register models; battery charge/drain model; motion model |
| `coinhost/ble.py` | Bumble bridge: GAP/GATT/DIS/BAS, SC+MITM pairing with the coin's passkey, JSON bonds, credits, radio controls |
| `coinhost/tlv.py` | PROTOCOL.md TLV decoding for the traffic log (WIFI_SET contents are never logged) |
| `coinhost/audio.py` | Mic sources: silence (-65 dBFS), tone, file (decoded with `av`), and the browser |
| `coinhost/web.py`, `web/` | aiohttp server, WebSocket (frames, speaker, mic) and JSON API; plain HTML/CSS/JS with no CDN |
| `fake_coin.py` | IPC stand-in for coinsim.exe, used by the tests |
| `tools/vlink.py`, `tools/central.py` | Virtual BLE air; phone-like central |

Run state lives in `--data` (default `sim/.data/default/`):

- `rtc.bin`
- `ble_keys.json`
- `uploads/`

Each data dir contains a `*` `.gitignore`, so it is ignored by git.

## API

- `GET /api/state`: the full snapshot, including `ble.passkey` while pairing.
- `POST /api/input`: one JSON object. Its keys are:
  - `pwr`, `boot`
  - `hold:{pwr|boot: ms}`
  - `touch:{pressed,x,y}`
  - `usb`
  - `battery:{present,volts,pct}`
  - `cut_power`
  - `die_temp_c`
  - `imu:{stick,pitch,roll,yaw_rate,tap,shake}`
  - `mic:{source,file,tone_hz,tone_dbfs,loop}`
  - `speaker:{muted,volume}`
  - `ble:{rssi,out_of_range,forget}`
  - `sim:{speed,capacity_mah}`
- `POST /api/console`: sends a line (`{"line": "..."}`) to the coin's stdin.
- `GET /api/console?since=N[&plain=1]`: reads console output.
- `GET /api/ble_log?since=N`: the Bluetooth traffic log.
- `GET /api/screen.png`: the current screen.
- `GET /api/audio_files`: the list of mic audio files.
- `POST /api/upload`: uploads a mic audio file.

## Tests

```sh
cd sim/host && $PY -m pytest tests -q
```

| File | Covers |
|---|---|
| `test_models.py` | Frame codec, PMIC and IMU unit tests |
| `test_supervisor_e2e.py` | `fake_coin.py` through the supervisor: boot, sleep/wake, crash/WDT/restart, long-press, power cut, brownout, missing exe |
| `test_ble_e2e.py` | vlink + host + central, with both random-static and public advertising |
