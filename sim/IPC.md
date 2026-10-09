# Coin simulator: coin process ↔ host

The simulator has two programs:
- **`coinsim.exe`** is the coin's firmware (`firmware/main/*.c`), compiled for Windows. Hardware access goes through shims. Built from `sim/coin/`.
- **The host** (`sim/host/`, Python) stands in for everything outside the ESP32-S3:
  - the AXP2101 PMIC (including power on and off) and the QMI8658 IMU
  - the buttons, touch panel, microphones, speaker and display
  - the Bluetooth radio, through Bumble
  - the browser UI

The host starts `coinsim.exe` for each boot. The process exits on deep sleep, restart, crash and power-off; the host starts it again when the coin would wake or power on.

## Transport

- The host listens on TCP `127.0.0.1:<port>` and passes the port in `COINSIM_IPC`. The coin connects once at start-up, before `app_main`, and keeps the connection until it exits.
- Every message is a frame: `type u8`, `length u32 LE`, then `length` payload bytes. All integers are little-endian.
- Types `0x01–0x7F` go coin → host; types `0x80–0xFF` go host → coin.
- The host never blocks the coin: a request that needs an answer carries a `seq u16`, and the host answers with the same `seq`.
- stdout and stderr of `coinsim.exe` are the coin's USB serial console: ESP log lines and the console REPL. The host captures them and writes the user's console input to stdin.

## Launch environment

| Variable | Meaning |
|---|---|
| `COINSIM_IPC` | TCP port of the host |
| `COINSIM_DATA` | Data directory. The coin uses `<dir>/littlefs/` for its LittleFS partition and `<dir>/nvs.txt` for NVS (one `namespace key type hex` line per entry; the host does not read it). It survives power-off |
| `COINSIM_RESET` | ESP-IDF `esp_reset_reason_t` value: 1 POWERON, 3 SW, 4 PANIC, 6 TASK_WDT, 8 DEEPSLEEP |
| `COINSIM_WAKE` | Bitmask of `1 << esp_sleep_source_t`, as returned by `esp_sleep_get_wakeup_causes()`: EXT1 = 3 → `0x8`, TIMER = 4 → `0x10`. 0 or absent: not a wake from sleep |
| `COINSIM_EXT1` | GPIO mask that caused an EXT1 wake (`esp_sleep_get_ext1_wakeup_status`) |
| `COINSIM_RTC` | Path to the RTC memory blob from the last SLEEP message. Absent after power-off. The coin loads it only when `COINSIM_RESET` is 8 (DEEPSLEEP): like the ESP-IDF bootloader, every other boot starts `RTC_DATA_ATTR` variables from their initial values |
| `COINSIM_RTC_US` | The RTC clock in µs at launch. It counts from power-on and keeps counting through deep sleep (`rtc_time_get`). It restarts at 0 after Off |
| `COINSIM_GPIO` | Input pin levels at launch, e.g. `0=1,3=1,11=1,21=0`. GPIO3 = PWR (1 = pressed), GPIO0 = BOOT (0 = pressed) |

## Coin → host

| Type | Name | Payload | Notes |
|---|---|---|---|
| 0x01 | HELLO | `ipc_version u32` (1), `fw_version` (rest, UTF-8) | First frame after connect |
| 0x03 | I2C | `seq u16, addr u8, wlen u16, rlen u16, wbytes[wlen]` | One I2C transaction (write then optional read, repeated start). Answer: I2C_RESULT |
| 0x04 | GPIO_OUT | `pin u8, level u8` | Output pins: 46 speaker amplifier enable, 1 LCD reset, 2 touch reset |
| 0x05 | FRAME | `x u16, y u16, w u16, h u16, pixels[w*h] RGB565 u16 LE`, row-major | An LVGL flush of a 466×466 panel |
| 0x06 | PANEL | `on u8, brightness_pct u8` | `on` = 0: panel asleep (black). Before the first PANEL of a boot the host shows frames at full brightness |
| 0x07 | SPEAKER | `pcm s16[]`, mono 16 kHz | Audible only while GPIO46 is 1 |
| 0x08 | MIC_STATE | `open u8` | 1 while the coin's microphone capture runs |
| 0x09 | SLEEP | `timer_us u64` (0 = no timer), `ext1_mask u64`, `ext1_mode u8` (0 any low, 1 any high), `rtc_len u32, rtc[rtc_len]` | Deep sleep. The process exits right after. The host saves `rtc` and wakes the coin by starting it again |
| 0x0A | RESET | `reason u8` (`esp_reset_reason_t`) | `esp_restart` (3), a task watchdog (6) or a panic (4). The process exits right after; the host starts it again with that reason. If the process exits without SLEEP or RESET, the host treats it as a panic |
| 0x10 | BLE_ADV | `on u8, interval u16` (0.625 ms units), `duration_ms i32` (−1 = until changed) | Advertising as PROTOCOL.md describes: flags, the complete list of 128-bit service UUIDs `c0a10000-f569-41ae-8676-7f5a60d835fc`, complete name `coinai`. Connectable, general discoverable. When `duration_ms` ends the host sends BLE_ADV_DONE |
| 0x11 | BLE_INDICATE | `seq u16, value[]` | Indication on `ctrl`. Answer: BLE_IND_DONE |
| 0x12 | BLE_NOTIFY | `value[]` | Notification on `data`. Uses one credit (see BLE_CREDIT) |
| 0x13 | BLE_WRITE_RESULT | `seq u16, result u8` | Answer to BLE_WRITE. 0 = accepted, 1 = insufficient authentication, 2 = invalid length, 3 = queue full, 4 = unlikely error. Mapped to ATT errors 0x05, 0x0D, 0x09, 0x0E |
| 0x14 | BLE_PASSKEY | `passkey u32` | Answer to BLE_PASSKEY_REQ: the 6 digits the coin shows |
| 0x15 | BLE_DISCONNECT | – | Terminate the connection (remote user terminated, 0x13) |
| 0x16 | BLE_FORGET | – | Delete all bonds |
| 0x17 | BLE_CONN_PARAMS | `itvl_min u16, itvl_max u16, latency u16, timeout u16` | A connection parameter update request (1.25 ms / 10 ms units) |
| 0x18 | BLE_LINK_SETUP | – | Data length 251 and 2M PHY wanted. The host may ignore it, but must not let the request hold up other HCI commands (see `tools/vlink.py`) |
| 0x19 | BLE_TX_POWER | `dbm i8` | |
| 0x1A | BLE_BATTERY | `pct u8` | Battery Service level |

## Host → coin

| Type | Name | Payload | Notes |
|---|---|---|---|
| 0x83 | I2C_RESULT | `seq u16, status u8` (0 ACK, 1 NACK), `rbytes[rlen]` | |
| 0x84 | GPIO_IN | `pin u8, level u8` | Sent on every change of an input pin: 3 PWR, 0 BOOT, 11 touch INT, 21 IMU INT1 |
| 0x85 | TOUCH | `pressed u8, x u16, y u16` | Panel coordinates 0–465. The host also holds GPIO11 (touch INT) low while pressed. A press released before LVGL's next input read (30 ms) still counts once, so a mouse click is a tap |
| 0x86 | MIC | `pcm s16[]`, interleaved stereo 16 kHz | The two microphones, as the ES7210 delivers them. Send roughly in real time; the coin keeps up to 1 s and plays silence when it runs dry |
| 0x90 | BLE_CONNECTED | `conn u16, peer_addr_type u8, peer_addr[6]` (LSB first) | The over-the-air address, as NimBLE's `peer_ota_addr`: type 0 public, 1 random |
| 0x91 | BLE_IND_DONE | `seq u16, confirmed u8` | Confirmed by the peer, or not (timeout or disconnect) |
| 0x92 | BLE_CREDIT | `n u16` | Notification credits returned. The coin starts every connection with 12 credits. A notification without a credit fails with "out of buffers", as NimBLE's ENOMEM does on the device |
| 0x93 | BLE_DISCONNECTED | `reason u8` | HCI reason code. "Out of range" in the UI gives the coin 0x08 (supervision timeout); the phone sees 0x13, because a Bumble host can only terminate a link |
| 0x94 | BLE_SECURITY | `encrypted u8, authenticated u8, bonded u8` | After pairing or after re-encryption with a stored bond |
| 0x95 | BLE_MTU | `mtu u16` | |
| 0x96 | BLE_SUBSCRIBE | `which u8` (1 ctrl indications, 2 data notifications), `enabled u8` | |
| 0x97 | BLE_WRITE | `seq u16, value[]` | A write to `ctrl`. The host holds the ATT response until BLE_WRITE_RESULT |
| 0x98 | BLE_PASSKEY_REQ | – | Pairing needs the passkey to display. Answer: BLE_PASSKEY |
| 0x99 | BLE_RSSI | `rssi i8` | The connection's RSSI, sent on change |
| 0x9A | BLE_READY | – | Sent once right after HELLO, even while the host's radio bridge is still down (the coin's own radio is always up). The coin then starts advertising as its state requires; the host applies it when the bridge comes up |
| 0x9B | BLE_ADV_DONE | – | A timed advertising phase ended |

## Devices on the I2C bus

These answer ACK; every other address NACKs.

| Addr | Device | Modelled |
|---|---|---|
| 0x34 | AXP2101 PMIC | Registers, as in "PMIC model" |
| 0x6B | QMI8658 IMU | `WHO_AM_I` 0x00 = 0x05, revision 0x01 = 0x7C, CTRL1–9 (0x02–0x0A) store writes, data block 0x33–0x40 (temperature, accel XYZ, gyro XYZ, s16 LE), reset 0x60 ← 0xB0. Scale from CTRL2/CTRL3: the firmware sets ±4 g (8192 LSB/g) and ±512 dps (64 LSB/dps). With CTRL7 = 0 (sensors off) the data registers hold their last values |
| 0x18 | ES8311 codec | ACK only; reads 0 |
| 0x40 | ES7210 ADC | ACK only; reads 0 |
| 0x5A | CST9217 touch | ACK only; touch arrives as TOUCH frames |

## PMIC model (AXP2101)

Initial registers are the real coin's (2026-10-09, USB powered):
```
00=28 01=14 03=4a 04=00 05=00 06=00 07=00 10=34 12=08 13=03 14=65 15=06 16=04 17=00 18=0a 19=06
20=04 21=01 22=06 23=3f 24=00 25=18 26=08 27=14 30=1f 40=ff 41=fc 42=5f 50=12 61=05 62=04 63=15
64=03 68=01 69=01 80=0f 81=00 82=12 83=28 84=46 85=64 86=00 90=ff 91=01 92=1c 93=1c 94=19 95=0d
96=07 97=17 98=0e 99=00 9a=00
```
Unlisted registers read 0. Writes are stored, and survive a software power-off and deep sleep; the table above returns only when the PMIC itself loses power (no battery and no USB). The following registers are live:

| Reg | Behaviour |
|---|---|
| 0x00 | Bit 5 VBUS good (USB cable), bit 3 battery present |
| 0x01 | Bits 7:5: 001 charging, 010 discharging, 000 standby. Bits 2:0 charger state, as the chip and `pmic.c` number them: 0 trickle, 1 precharge, 2 constant current, 3 constant voltage, 4 done, 5 not charging. Charging means VBUS and a battery below 100 % |
| 0x10 | Writing bit 0 = 1 powers the system off: the host kills the coin process; the state becomes Off |
| 0x22 | Bit 1: PWR long press powers off |
| 0x27 | Bits 1:0 PWR "on" time (128 ms, 512 ms, 1 s, 2 s). Bits 3:2 long-press "off" time (4, 6, 8, 10 s) |
| 0x34/0x35 | VBAT in mV, 14 bits (`(H & 0x3F) << 8 \| L`) |
| 0x38/0x39 | VBUS mV (5000 with USB, else 0) |
| 0x3A/0x3B | VSYS mV (≈ max(VBUS − 100, VBAT)) |
| 0x3C/0x3D | Die temperature: `raw = 7274 − (°C − 22) × 20` |
| 0xA4 | Battery percent |

Power behaviour, modelled after the hardware:
- **Off → on:** PWR held for the 0x27 "on" time powers the coin on: `COINSIM_RESET=1`, PWR still pressed in `COINSIM_GPIO`. On the board, plugging in USB does not power it on; the simulator keeps that unconfirmed behaviour.
- **Long press:** with 0x22 bit 1 set, PWR held for the "off" time powers off from any state.
- **Battery:** with no battery and no USB, or with the battery below 3.0 V and no USB, the coin is Off.
- **Standby:** in deep sleep, with `ext1_mask` bit 3 and mode "any high", pressing PWR starts the coin with `COINSIM_WAKE=0x8`, `COINSIM_EXT1=0x8` and PWR pressed. The timer starts it with `COINSIM_WAKE=0x10`.

## Bluetooth address

The coin advertises with its public address (D8). The host cannot give a controller it does not own a chosen public
address: netsim assigns its own, and HCI has no standard command to change it. So by default the host advertises
`--address` (C0:1A:51:00:00:01) as a **static random** address, and the phone sees type random. `tools/vlink.py
--coin-address C0:1A:51:00:00:01` makes the controller's own public address the coin's, to exercise public-address
advertising.
