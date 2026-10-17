# coinai plan

**Status:** 2026-10-09. Revision 3: phone (OPPO Find N6, Android 16 / ColorOS 16) and processing (SamBot) are settled.
- Phase 0 is done except `adb devices`, which needs your phone.
- Phase 1 is done except the steps that need you (screen, touch, buttons) and IMU INT1.
- Evidence is in HARDWARE.md ("Factory state") and `evidence/`.

**Two parts:**
- Firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.75C. The battery is installed. MAC A0:F2:62:E3:0A:C0, COM9 over native USB.
- A native Android app.

**Related files:**
- [HARDWARE.md](HARDWARE.md): board facts.
- [PROTOCOL.md](PROTOCOL.md): the device ↔ app contract.
- [design/ptt-flow.png](design/ptt-flow.png): flow diagram.

## Contract

**Objective:**
1. **Push-to-talk.** Holding the PWR button records voice, whether the device is Active, in Standby (deep sleep) or Off. Each clip reaches the Android app over BLE without the app being opened.
2. **Every board function works:**
   - Custom high-performance UI on the touch AMOLED.
   - Both buttons.
   - Both microphones.
   - Speaker.
   - IMU.
   - Battery, charging and power management.
   - Wi-Fi (clip delivery straight to SamBot, time sync, firmware updates).
   - BLE.
3. **Android app.**
   - Pairs with the device.
   - Receives clips in the background.
   - Sends each clip to SamBot as one more client of its `web` conversation. The reply appears as a notification, as a short version on the coin's screen, and in full in SamBot's existing web client.
   - Configures the device (Wi-Fi, settings) and can push firmware updates.

**Done means.** Each phase below passes its evidence on this device and your phone. The end-to-end check: a clip recorded from Off reaches SamBot while the app is swiped away and the phone is idle, and the reply comes back as a notification, on the coin, and in the web client.

**Constraints:**
- **Firmware:** ESP-IDF v5.5.5 in C, Waveshare BSP `waveshare/esp32_s3_touch_amoled_1_75c ^3`, LVGL 9.5.
- **App:** Kotlin with Jetpack Compose, minSdk and targetSdk 36 (Android 16), the version your OPPO Find N6 runs (ColorOS 16). Sideloaded for personal use, not published on the Play Store.
- The factory image is backed up before the first flash.
- Nothing irreversible without an explicit go-ahead: no eFuse burns, flash encryption or secure boot.

**Repo layout:**

```
coinai/
  firmware/    ESP-IDF project
  android/     Gradle project (Kotlin, Compose)
  protocol/    shared test vectors for PROTOCOL.md
  PROTOCOL.md  HARDWARE.md  PLAN.md
```

## Device behaviour

### Power states

| State | Powered | Drain (estimates, to measure) | Left by |
|---|---|---|---|
| Active | everything, screen on | tens of mA | idle timeout or a PWR tap → Standby |
| Standby | PMIC 3V3 on; ESP32 in deep sleep; panel sleep-in; audio rail (ALDO1) off; BLE off | ~0.4–0.6 mA, of which ~0.33 mA is R9 on SYS_OUT | PWR press (GPIO3 deep-sleep wake), timer |
| Off | PMIC shut down | µA | PWR held for the PMIC ON-time (128 ms–2 s) |

Standby falls through to Off after a configurable number of unused hours, or on low battery.

### Buttons

- **PWR is the mic button.** It is the only key that can power the board on from Off, and its state reaches the ESP32 on GPIO3.
  - Capture starts on press. A release within 200 ms counts as a tap: the audio is discarded and the screen toggles.
  - From Standby or Off, if PWR is still held once firmware is ready, recording runs until release.
- **BOOT** is an in-app secondary button (cancel, back). It is not a wake source: PWR already covers waking, and this avoids strapping-pin edge cases.
  - Holding BOOT while powering on from Off is expected to enter ROM download mode, which would be a recovery path (**verify**). esptool's USB reset already reaches download mode without it.
- **Power off:** from the UI menu, or by holding BOOT and then PWR for 3 s. The PMIC's hardware long-press shutdown is disabled so that long dictation can't cut power. Task, interrupt and RTC watchdogs cover hangs, and so does a UI heartbeat: if the UI stops ticking for 6 s outside a recording, the coin saves a core dump and restarts (D20).
- **Touch** is for UI navigation only.

### Recording

- **Pipeline:** ES7210 MIC1+MIC2 at 16 kHz/16-bit → mono mix + high-pass → Opus (VoIP mode, 24 kbps, 20 ms frames) → Ogg Opus file on LittleFS.
- **Clip size:** about 3 KB/s, so a minute is about 180 KB, and the partition holds about 1.9 h of undelivered audio.
- **Durability:** data is flushed at least once per second, and LittleFS is power-loss safe.
- **Latency targets** (press → first captured sample): Active < 50 ms, Standby ≤ 400 ms, Off ≤ 700 ms after the PMIC ON-time.
- **Boot fast path:**
  - Disable the PSRAM memtest (about 2 s saved here).
  - Turn bootloader and app logs down.
  - `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`.
  - Initialize audio before the display and BLE.

### Delivery

Store and forward over BLE, as specified in [PROTOCOL.md](PROTOCOL.md):
- Every clip is durable on flash first.
- Transfer begins as soon as the phone connects, including mid-recording.
- It resumes by byte offset after a disconnect.
- The device deletes its copy only after the app acknowledges a CRC-matched copy.
- After a clip the coin waits 10 s for the phone to connect, then up to 60 s for a connected phone to take the clips (D19).

## Android app

**Pairing** (once):
- The system Companion Device picker shows devices advertising the coinai service UUID.
- After you pick one, the phone bonds: you type the passkey shown on the coin.
- The app then registers for device presence.

**Background wake path:**
1. The coin advertises after a press.
2. Android detects it and binds the app's `CompanionDeviceService`. The app does not need to be running.
3. That service starts `SyncService`, a foreground service of type `connectedDevice`. Its notification shows only while syncing.
4. `SyncService` connects and runs `HELLO`, `LIST` and `GET`, saves the `.ogg`, checks the CRC, sends `ACK`, queues processing, and stops.

**Network:**
- Companion apps holding `REQUEST_COMPANION_RUN_IN_BACKGROUND` are exempted from battery optimization, and exempt apps keep network access in Doze.
- Processing runs as a WorkManager job with a network constraint, so it waits out offline periods and retries.

**Processing: SamBot.** The app is one more SamBot client, like the desktop palette and the Telegram bridge. It uses routes SamBot already has, so the server does not change:
1. `POST /voice/transcribe` (multipart, field `audio`, type `audio/ogg`). The clip goes in unchanged and Groq Whisper text comes back. Telegram voice notes are also Ogg Opus and already use this route.
2. Open `GET /events?lane=web` (SSE). Discard frames until `host_stream_live`, the boundary between replayed history and live events.
3. `POST /prompt` with `{text, lane: "web"}`. If a turn is already running, SamBot steers the text into it.
4. Collect assistant text from `message_end` frames until `agent_settled`. After a deadline, keep the partial reply.

The reference implementation is `~/.sambot/telegram/src/sambot-client.ts` on the server; the app mirrors it.

**Where replies show:**
- **In full:** SamBot's web client, installed from Chrome as a PWA (it already ships a manifest and service worker). The coin's prompts land in the same `web` conversation, so the web client renders them with everything it already supports. The app builds no chat UI.
- **Notification:** the reply text with markdown removed. Tapping it opens the web client.
- **On the coin:** a short reply sent with `REPLY` (see PROTOCOL.md), if it arrives while the coin is still awake.

**Reaching SamBot:** the HTTPS proxy at `sambot.exe.xyz` is private; it redirects unauthenticated requests to the exe.dev login. The app sends an exe.dev VM token in `X-Exedev-Authorization: Bearer …`, created with `ssh exe.dev ssh-key generate-api-key --vm=sambot` and stored encrypted on the phone. The web client keeps using your normal exe.dev login in Chrome.

**Duplicates:** `/prompt` has no idempotency key.
- Transcription is retried freely.
- Each clip is prompted at most once.
- If the outcome of a `/prompt` call is unknown, such as a timeout after sending, the clip is marked uncertain and offers a manual resend.
- **Over Wi-Fi from the coin** (D10): the coin writes a tombstone just before `POST /prompt` and deletes the clip once SamBot accepts it. A reset or power loss between the two deletes the clip at the next boot, and an unknown outcome deletes it with "Not sure SamBot got it." on the screen: the coin has no resend, so such a clip is lost rather than sent twice.
- **Known gap (coin and phone):** if the phone has received a whole clip and the link drops before its `ACK`, the coin keeps that clip from Wi-Fi only until it reboots. After a reboot Wi-Fi may send it while the phone sends its copy too. Rare (a lost link, a reboot and a pending phone copy together); the fix would be the phone confirming a prompted clip by id at the next connection.

**UI (Compose):**
- Status: battery, last sync, pending clips.
- Clip list: playback (Media3), transcript, delivery state, resend.
- Device settings: Wi-Fi, brightness, sleep timers, reply wait.
- SamBot: token, connection test.
- Pairing.
- Debug log.

**Libraries:** Nordic Android-BLE-Library 2.x (+ `ble-ktx`), Media3 ExoPlayer, WorkManager, Room.

**Unverified on your phone:**
- How quickly presence is detected.
- Behaviour after Settings → Force stop. Expected: nothing until the app is reopened. On the emulator (stock Android 16) a presence event still started the force-stopped app and the visit completed, but that event was simulated from the shell (android/README.md); a real one may differ.
- ColorOS battery management. ColorOS has its own per-app background and auto-launch switches, separate from Android's battery-optimization exemption, and their names vary by ColorOS version. The app points to them, and phase 5 tests with them on and off.

**Phase 5 phone settings.** The menu paths below come from OPPO's support pages for ColorOS 12 and later and from third-party guides, and have not been checked on ColorOS 16. Each phase 5 run records which of these settings are on.

| Setting | Path (ColorOS 12+) |
|---|---|
| Allow background activity | Settings → Apps → App management → coinai → Battery usage |
| Auto launch (start after reboot) | Settings → Apps → App management → coinai → Battery usage, or Settings → App management → Auto-launch apps |
| Android battery optimization | Settings → Battery → More battery settings → Optimize battery use → coinai → Don't optimize. The app also asks through `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` |
| Lock in recents | Recent apps → the card's menu → Lock |
| Notifications | Allowed, including the "clip delivered" and reply channels |

The matrix runs each condition with the settings at their defaults and with all of them on:
- screen off for 10 minutes or more
- after a phone reboot
- after swiping the app away from recents
- after Force stop; delivery is expected to stop until the app is opened

**Fallback** if presence detection is slow or unreliable: a persistent `connectedDevice` service holding an auto-connect link. The cost is a permanent notification.

## Firmware architecture

| Component | Responsibility |
|---|---|
| `board` | BSP wrapper, I2C bus, rail sequencing (ALDO1 for audio), pins held through sleep |
| `pmic` | AXP2101 driver ported to C from Waveshare's register code: rails, charger, fuel gauge, PWR events by polling, shutdown |
| `input` | PWR (GPIO3) and BOOT (GPIO0) debouncing and gestures; wake-reason handling |
| `power` | Active/Standby/Off state machine, wake sources, display sleep, boot fast path |
| `audio` | ES7210 capture, mono/high-pass, Opus via `esp_audio_codec`, Ogg page writer, speaker playback |
| `clipstore` | LittleFS clip queue: write, flush, CRC, delete on ACK |
| `ble` | NimBLE peripheral per PROTOCOL.md |
| `net` | Wi-Fi STA (networks from the app, the USB console, or the coin's own Settings > Wi-Fi), clip delivery to SamBot (`sambot.c`), SNTP; HTTPS OTA with rollback to come |
| `ui` | LVGL 9 screens around the eyes bot (the default face): recording, send/think/reply, Recent, Controls, brightness, settings, Wi-Fi setup, pairing passkey, power off; flows and springs; IMU tilt, shake and face-down |
| `kbd` | A reusable on-screen keyboard for the round screen (`kbd.h`): all printable ASCII on four pages, keys that commit on release (slide to correct) with a magnified preview, backspace repeat, password show/hide |
| `eyes` | The eyes bot: one custom-drawn LVGL object (eyes, lids, rim progress/orbit, ripple, "z"s) that invalidates only what moved |
| `imu` | QMI8658 (`waveshare/qmi8658`); raise-to-wake later |
| `diag` | USB console: `i2cscan`, `pmic`, `rec`, `clips`, `clip get`, `ble`, `wifi`, `ota`, `sleep`, `fbdump` (frame → PNG over USB), boot timing |

Cores: the radio and audio capture/encode run on core 0; LVGL rendering runs on core 1.

**Display path, for the highest-performance UI.** The BSP default (PSRAM buffers, no tearing sync, 40 MHz) is replaced with:
- DMA buffers in internal RAM. *Done in phase 3 (D16):* `display.c` sets up the panel and LVGL itself, with two 24-line draw buffers in internal DMA RAM.
- TE-synced flushing on GPIO13.
- A trial of 80 MHz QSPI.
- LVGL rendering on its own core. *Done:* LVGL runs on core 1.
- FPS counters.

A full 466×466 frame is 434 KB, so the bus caps full-screen redraws at about 46 fps (40 MHz) or about 92 fps (80 MHz).

**Flash layout** (32 MB, confirm in phase 0):

```
# Name,   Type, SubType,  Offset,   Size
nvs,      data, nvs,      0x9000,   0x6000
otadata,  data, ota,      0xF000,   0x2000
phy_init, data, phy,      0x11000,  0x1000
ota_0,    app,  ota_0,    0x20000,  0x600000
ota_1,    app,  ota_1,    0x620000, 0x600000
coredump, data, coredump, 0xC20000, 0x10000
storage,  data, littlefs, 0xC30000, 0x13D0000
```

## Phases

Each phase ends with evidence on the device or your phone. **You** marks the steps that need you there.

**Status (2026-10-10):**

| Phase | State | Still open |
|---|---|---|
| 0 | done | **you:** USB debugging on, then `adb devices` |
| 1 | done | **you:** screen, touch and button counters; IMU INT1 pin |
| 2 | done: clips decode, loopback tone 65 dB above control | **you:** listen to a clip |
| 3 | standby → first audio frame **208–229 ms** over 10 unattended trials (target 400 ms) | **you:** real PWR presses from Standby and Off; BOOT+PWR power-off; then disable the PMIC long-press (D5) |
| 4 | firmware GATT server built: advertises (seen by the PC), RAM budget fixed (93 KB internal free), recording unaffected by the radio. Protocol layer passes 11/11 edge-case checks over the USB console link, including resume and GET-while-recording (`evidence/phase4-selftest-serial.txt`). App slice 3 merged (89 unit tests: sends the UTC offset after HELLO, drops clips discarded mid-stream, sends the transcript with REPLY thinking). **No over-the-air exchange with the device yet:** the PC's adapter is too deaf to hold a connection | **you:** USB debugging on the phone (installs the app and runs the link tests), or attach the PC's Wi-Fi/BT antenna; pairing passkey entry on the phone |
| 8 (UI) | the eyes-bot UI from the signed-off motion study runs on the coin and in the simulator. In the simulator every flow and screen was checked frame by frame (talk, fail, away, pair, no-speech, discard, power, sleep, low battery; Controls, brightness, Settings, About, Recent), including a real REPLY from a central, the clock from `utc_offset_min` and the BOOT-hold discard. On the coin the idle face renders 13 changed frames/s at 3.3 ms each (4% busy); flows hitched to 43–55 ms frames, traced to whole-ring rim redraws and whole-paragraph text fades. The fix (the rim sweeps in and drains, and text fades in line by line) is built and checked in the simulator, **not yet re-measured on the coin** | **you:** look at it on the coin; face-down → screen off, tilt direction, BOOT held 1 s while recording discards |
| 7 (Wi-Fi delivery) | built (`net.c`, `sambot.c`, `plaintext.c`; D10 revised): up to 4 networks, delivery mode `CFG_SET` 0x06, the SamBot token on the USB console. On the coin: Wi-Fi starts and stops with BLE and the UI running (about 30 KB internal RAM left while it is on), the scan, the no-network fallback to the phone, and the token stored without being echoed. `plaintext.c` matches the app's `PlainText.forCoin` on its test cases. On-coin setup (D18) on the coin: the scan list, the keyboard (one 42 ms frame as it opens, then frames of 14 ms at most), and a join to a network that is not there ("Network not found", nothing saved, no RAM lost over 8 keyboard openings; `evidence/wifi-setup-coin.png`). In the simulator, `test_wifi_setup_by_touch` drives the whole setup by touch: keyboard pages, slide-to-correct, backspace repeat, a wrong password and a retry that keeps the text, joining, and hold-to-forget (`evidence/wifi-setup-sim.png`). **On a real network (2026-10-13):** clips reach SamBot and the reply shows on the coin (PROTOCOL.md "Wi-Fi delivery", firmware status); the coin polls SamBot's transcript for the reply instead of holding the live stream, which carried megabytes of token deltas. Internal RAM: about 31.5 KB free while Wi-Fi and TLS are up, but the largest free block after Wi-Fi is off is 12.8 KB against 31.7 KB before (watch: fragmentation) | **you:** what to do with the clips recorded before Wi-Fi delivery (they would go to SamBot oldest first once delivery is Wi-Fi first) |
| sim | desktop simulator ([sim/README.md](sim/README.md)): the firmware runs as `coinsim.exe` with a browser UI. Over virtual BLE (Bumble, and netsim as the emulator uses) the full PROTOCOL.md exchange works with pairing and bonds, and a recorded sentence reaches SamBot exactly. It also exercised paths not yet tried on the device: cut-off clip recovery, low-battery off, idle Standby. **The app on the emulator (2026-10-17):** pairing, background visits with the app killed, force-stopped or in Doze, Delivery settings, and a voiced clip failing cleanly without a token (android/README.md). It found and fixed four app bugs: the first sync started during bonding and never retried; `ensureBond()` removed the bond; a failed bond left the association; unusable SamBot settings left the coin without a final reply, and a refused token reached it as `transcribe: transcribe: HTTP 401`. CDM presence has to be simulated there | **you:** OK to put the coin's SamBot token into the emulator app for the full SamBot round trip |

0. **Safety net and toolchains.**
   - ESP-IDF v5.5.5 via Espressif's EIM.
   - Android SDK command-line tools and a JDK 21, installed per user with no admin rights. Builds run from Gradle; Android Studio is optional for you.
   - **You** enable USB debugging on the phone.
   - Back up the full 32 MB flash and run the eFuse check.
   - *Evidence:* a 33,554,432-byte backup and its hash, the eFuse report, and your phone listed by `adb devices`.
1. **Board bring-up.**
   - Waveshare's `02_lvgl_demo_v9` as a toolchain proof (**you** confirm screen and touch).
   - coinai skeleton and diag console.
   - PMIC: log the factory configuration, then set the rails, disable the long-press shutdown, set the ON-time to 128 ms, and **cap charging at 100 mA** until the battery capacity is known.
   - Buttons, IMU.
   - *Evidence:* I2C scan, PMIC readings, button and IMU logs, `fbdump`.
2. **Audio and clips.**
   - Two-mic capture → Opus → Ogg on flash; pull clips over USB; speaker playback.
   - *Evidence:* the `.ogg` decodes with the right duration (**you** listen).
3. **Push-to-talk from Standby and Off.**
   - Deep-sleep wake on GPIO3, PMIC shutdown, boot fast path, timing instrumentation.
   - *Evidence:* a latency table with 10 trials per state, and speech spoken at the moment of pressing is in the clip.
4. **BLE link and foreground sync.**
   - Firmware GATT service per PROTOCOL.md.
   - App protocol module (unit-tested against the shared vectors), pairing, and a sync screen. nRF Connect is used for early probing.
   - *Evidence:* clips arrive on the phone with matching CRCs and play; transfer resumes after a forced disconnect; measured throughput.
5. **Background delivery.**
   - Companion presence → foreground service → sync.
   - Matrix on your phone:
     - app in background;
     - app swiped away;
     - phone idle (Doze forced via `adb shell dumpsys deviceidle force-idle`);
     - after a reboot;
     - out of range, then back;
     - offline, then network back.
   - *Evidence:* pass or fail for each case, from logcat.
6. **SamBot and replies.**
   - **You:**
     - Create the VM token with `ssh exe.dev ssh-key generate-api-key --vm=sambot --label=coinai-phone` and paste it into the app.
     - Install the web client from Chrome: ⋮ → Add to home screen → Install.
   - App: the SamBot client (transcribe, wait for live events, prompt, collect), the notification, `REPLY` to the coin, and at-most-once prompting.
   - Firmware: the reply wait and the reply screen.
   - *Evidence:*
     - A clip recorded with the app closed appears in the web client as a prompt with SamBot's reply.
     - The same reply appears as a notification and on the coin.
     - Server-side timings come from SamBot's `voice_transcribe_*` log entries.
7. **Wi-Fi delivery, time and OTA.**
   - Clips straight to SamBot over Wi-Fi (D10, PROTOCOL.md "Wi-Fi delivery"), the phone as the fallback; networks from the app; SNTP; OTA from the app over BLE and/or Wi-Fi HTTPS; rollback.
   - *Evidence:* a clip recorded away from the phone gets its reply on the coin over Wi-Fi, with join, upload, transcribe and reply times; a clip recorded with no known network reaches the phone; an update from v1 to v2, and a bad image that rolls back.
8. **Custom device UI and display performance; app UI polish.**
   - Mockups first, then build.
   - *Evidence:* FPS and flush-time numbers, tear-free motion (**you** check), `fbdump` captures.
9. **Power polish and battery size.**
   - Measure the drain in each state. There is no current sensor, so the firmware logs battery voltage, fuel gauge and state on flash (`battlog`: every minute on battery, at standby entry and wake), and `scripts/battlog.py` turns that into drain rates per state.
   - Estimate the capacity by timing a full charge at a known current, then set the final charge current.
   - *Evidence:* measured drain, projected standby time, and the capacity estimate.

## Decisions (proposed)

These move to `CONTEXT.md` or `docs/decisions/` once accepted.

- **D1 ESP-IDF 5.5 + C + Waveshare BSP.**
  - *Rejected:* Arduino, which lacks the codec stack and makes OTA rollback and NimBLE tuning harder.
  - *Rejected:* PlatformIO, which lags IDF releases.
  - *Rejected:* raw panel drivers, which add work for no gain.
- **D2 PWR is the mic button via GPIO3.**
  - *Rejected:* BOOT. It is a strapping pin and can't power the board on from Off.
  - *Rejected:* touch. Too many accidental presses in a pocket.
- **D3 Standby drops the BLE link and reconnects after a press.**
  - *Rejected:* keeping a connection through light sleep. The board has no 32 kHz crystal, so the 40 MHz crystal would stay on.
- **D4 Clips are Ogg Opus, stored and forwarded, resumable, deleted on a CRC-matched ACK.**
  - *Rejected:* raw PCM, which is about 10× larger.
  - *Rejected:* live-only streaming, which loses audio whenever the phone is away.
- **D5 The PMIC's long-press shutdown is disabled;** power off from the UI or with BOOT+PWR. It stays enabled (6 s, factory setting) until phase 3 proves the firmware's own power-off path and watchdogs; until then it is the recovery path.
  - *Rejected:* a 10 s hardware off, which would cut long dictations.
- **D6 Two OTA slots with rollback.**
  - *Rejected:* a single factory app. Changing the layout later needs USB and wipes stored clips.
- **D7 Wi-Fi credentials go from the app through an encrypted characteristic.**
  - *Rejected:* Espressif's protocomm provisioning, which adds a second GATT service and its SDK to the app.
- **D8 LE Secure Connections, bonding, a passkey on the device screen, and private addresses.**
- **D9 Native Kotlin with Compose for the app.**
  - *Rejected:* Flutter. The Android background pieces (`CompanionDeviceService`, foreground service, GATT) are native Kotlin either way; the open-source omi Flutter app implements them in Kotlin. Flutter would add a Dart bridge without removing that work. Its advantage, a shared UI with iOS, is out of scope.
  - *Rejected:* React Native, for the same reason.
- **D10 (revised 2026-10-12) Wi-Fi first, the phone as the fallback.** On a known network the coin sends the clip to SamBot itself and shows the reply; otherwise the clip goes to the phone over BLE, which talks to any server. You chose this order; `CFG_SET` key 0x06 switches to phone first or phone only.
  - *Why:* the clip and the reply no longer depend on the phone's background limits (ColorOS) or on the phone being near. A missing network shows within a scan (about 5 s with BLE on), while a missing phone shows only by timeout, so trying Wi-Fi first costs little when it fails.
  - *Cost:* about 0.15–0.5 mAh more per clip (join, TLS, upload; estimated, to measure with `battlog` flag W). The TX peaks (about 350 mA) are left to the phone below 3.6 V on battery. The original "far more battery than BLE" below was overstated.
  - *Credentials:* an exe.dev API token scoped to the SamBot VM (`--exp=never`), stored in NVS (not encrypted) and set over USB, never readable back. Anyone holding the coin with a USB cable could extract it; it can be revoked with `ssh exe.dev ssh-key`, and SamBot can restrict it through the token's ctx.
  - *Rejected:* Tailscale or tailcat to reach the VM directly. tailcat has no ESP32 port, and the C Tailscale client (MicroLink) is third-party and needs RAM the coin does not have.
  - *Original decision (2026-10-09), superseded:* the device sends to the phone over BLE; the phone talks to any server.
  - *Rejected:* device → server over Wi-Fi as the primary path. It only works on known networks, joining Wi-Fi and setting up TLS costs far more battery per clip than BLE, and server credentials would live on the device.
  - Device Wi-Fi stays for firmware updates and time, and possibly as an at-home fallback later.
- **D11 Nordic Android-BLE-Library 2.x.**
  - *Rejected:* raw `BluetoothGatt`, which has no operation queue and well-known pitfalls.
  - *Rejected:* Nordic Kotlin-BLE-Library 2.0, which is still in beta.
- **D12 Companion presence plus a short-lived foreground service by default;** a persistent foreground service is the fallback.
- **D13 The app is a SamBot client on the `web` lane and uses existing routes; full replies render in SamBot's own web client.**
  - *Rejected:* a new `coin` lane. SamBot's lane allowlist and web UI would need changes, and you asked for another client of the same conversation.
  - *Rejected:* sending the `.ogg` as a `/prompt` attachment. SamBot saves non-text attachments with a path note and does not transcribe them.
  - *Rejected:* a native chat renderer. It would duplicate the web client.
  - *Rejected:* the web client in a WebView inside the app. A WebView has its own cookie jar, so you would log in to exe.dev again inside the app, and embedded login flows are fragile. The installed PWA shares Chrome's login.
  - *Rejected:* making the whole app a web page. On Android, Web Bluetooth works only in a foreground tab after a tap, so it can't receive clips in the background.
- **D14 The app reaches SamBot through the exe.dev proxy with a VM token.**
  - *Rejected:* an SSH tunnel inside the app, as the palette uses. It would be fragile on mobile networks and would put an SSH key on the phone.
  - *Rejected:* making the proxy public. That would expose SamBot's API to the internet.
- **D15 minSdk 36.** Only Android 16's presence API is implemented, because it is the only one we can test. Lower minSdk if another phone appears.
- **D16 The firmware sets up the display itself, with internal-RAM DMA draw buffers, and LVGL code stays out of IRAM.**
  - The BSP's 50-line PSRAM draw buffers made the SPI driver allocate a 46 KB internal bounce buffer on every flush. Once audio was in, it failed with `ESP_ERR_NO_MEM` and the screen stopped updating.
  - *Rejected:* DMA straight from PSRAM (`psram_dma_direct`). It underflowed at 40 MHz QSPI (`DMA TX underflow`) while the CPU also fetched code from PSRAM.
  - *Rejected:* `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` (Waveshare's setting). It placed 105 KB of LVGL code in internal SRAM. Without it the internal heap has 116 KB free (low-water 83 KB) instead of 44 KB (8 KB); BLE needs that room. Phase 8 measures the render cost.
- **D18 (2026-10-13) Wi-Fi networks can be added on the coin itself:** Settings > Wi-Fi scans, lists the networks in range, and takes a password on an on-screen keyboard (`kbd.c`). A network is saved only once it joins; the coin then asks SamBot's `/health` whether the token works. The app's `WIFI_SET` stays.
  - *Why:* without it the coin depends on the phone app or a USB console to get online, which defeats Wi-Fi as the fallback for a missing phone.
  - *Rejected:* a SoftAP with a captive page (an access point and a web server need internal RAM the coin does not have with BLE and the display running), WPS (phone hotspots rarely offer it), a QR code (no camera), LVGL's `lv_keyboard` (its button matrix drops a press that slides to another key and has no magnified preview).
- **D19 (2026-10-16) The coin waits 10 s for a phone that has not connected, not 60 s.** A connected phone still gets 60 s to take the clips.
  - *Why:* the coin's battery log showed every wake held awake for exactly 60 s, screen on, for a phone that had never paired: 90 wakes, 72 minutes awake, the battery from 48% to 7% and then the low-battery cut. Pending clips that no phone collects made every wake pay the full window.
  - *Cost:* a phone that needs more than 10 s to reconnect gets the clips on the next wake, unless the coin is awake anyway (10 s linger after a recording from Standby, the idle timeout otherwise). Experiment 2 measures the real reconnect time.
- **D20 (2026-10-16) One lock serializes everything sent to the panel, and a stalled UI restarts the coin.**
  - *Why:* the screen froze for good while the coin kept recording. A core dump showed the LVGL task waiting forever in `spi_device_acquire_bus` inside a flush. ESP-IDF's SPI master is not thread-safe for one device used from several tasks: brightness changes from the control loop or the BLE worker raced LVGL's flushes, and the losing task was never woken. Reproduced with `bright 80 5000` during `ui bench` (froze within 5,000 writes); with the lock, 120,000 writes in 3 runs and no freeze.
  - *How:* `display.c` takes one mutex around its own commands and around the panel's `draw_bitmap`, which it wraps, because LVGL's flushes go through it. The control loop restarts the coin through `esp_system_abort` (core dump to flash) when the UI tick is 6 s old and nothing is recording.
- **D17 The device measures speech per clip (`voiced_ms` in `CLIP_ENDED`), and the app does not prompt SamBot below a threshold.** SamBot's transcriber turned a silent clip into "Thank you."
  - *Rejected:* app-side detection. It would need Opus decoding on the phone, or Whisper's `no_speech_prob`, which SamBot's `/voice/transcribe` does not return.

## Risks and unverified items

- The ~0.33 mA Standby floor from R9 is inferred from the schematic, not measured.
- Whether the PMIC keeps its ON-time and OFF-time settings across Off is unknown. It sets the from-Off latency. (PWROK delay is 8 ms, register 0x25 = 0x18.)
- A cold-started capture begins with about 40 ms of digital silence while the ES7210 settles, followed by a faint −59 dBFS click. It is counted in the latency figures.
- CO5300 at 80 MHz QSPI is untested.
- IMU INT1 on GPIO21 conflicts with Waveshare's pin table.
- Android presence-detection latency, force-stop behaviour, and your phone maker's battery manager are unverified on a phone. The emulator cannot test presence: its controller never reports it.
- Battery capacity is unknown. The 100 mA charge cap stays until it is estimated or read from the cell.
- SSE through the exe.dev proxy with token auth is untested. The web client already uses SSE through the proxy with cookie auth.
- How long exe.dev VM tokens stay valid is unknown.
- ColorOS may stop the app in ways that stock Android does not.
- The battery log can't measure Standby drain yet. Its `rtc_s` jumps by up to about ±50 minutes across deep-sleep wakes, probably because each boot rescales the whole RTC count with a fresh slow-clock calibration.

## Open questions

Answered on 2026-10-09:
- **Phone:** OPPO Find N6, Android 16 / ColorOS 16 (D15).
- **Processing:** SamBot (D13, D14).

Still open:
1. **Battery.** The capacity is printed on the cell, or on the order page if you bought it online; otherwise phase 9 estimates it.

## If iOS is ever wanted

The protocol already follows Apple's accessory guidelines. An iOS app would be a separate Swift app, because Core Bluetooth background handling has to be native.

Apple's rules (TN3115):
- The app is relaunched after system termination or a reboot, not after a user force-quit, unless it pairs through AccessorySetupKit on iOS 26+.
- Each background wake gives about 10 s of run time, so uploads go through a background `URLSession`.
