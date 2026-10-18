# coinai Android app

Kotlin + Jetpack Compose app that receives clips from the coin over BLE and forwards them to SamBot (see `../PLAN.md`, "Android app", and `../PROTOCOL.md`).

## Layout

| Package | Android? | What |
|---|---|---|
| `dev.coinai.protocol` | no | Codec for PROTOCOL.md v1 (every `ctrl` TLV message, `data` packets); `Scene.split` cuts a scene's JSON into 1–8 `SCENE` parts of ≤ 237 bytes. Unknown TLV types decode to `CtrlMessage.Unknown`. |
| `dev.coinai.sync` | no | `CoinLink` (the coin as a message link), `SyncEngine` (one session: `HELLO`/`STATUS`, `CFG_SET utc_offset_min` and `delivery`, queued `WIFI_SET`s, `LIST`, resumable `GET`, reassembly, size + CRC-32 check, durable commit, then `ACK`; a clip the coin discards mid-stream is dropped; a clip the coin is sending over Wi-Fi (`ERROR` 4) is skipped until it is offered back with `CLIP_STARTED`), `CoinClock` (rule 8: the phone's UTC offset), `CoinSettings` (delivery order, `WIFI_SET` limits in UTF-8 bytes), `ClipStore`, `ReplyWindow` (rule 7: forward `REPLY`, the transcript while `thinking`, while the coin waits; a scene as its `SCENE` parts, or its summary as `REPLY` `done` when the coin refuses it with `ERROR` 5 or lacks `STATUS` bit 3). |
| `dev.coinai.clips` | no | `ClipProcessor` (transcribe, voiced/blank gate, scene or hand-off, at-most-once prompt, reply), clip states, `SceneSummary` (a card as plain text), `PlainText` (markdown removal; folding to the coin's character set, printable ASCII and newline, with the coin's own mappings). |
| `dev.coinai.sambot` | no | `SambotClient` (`health`, `transcribe`, `ask`, `scene` with an 8 s call timeout) after SamBot's reference client. |
| `dev.coinai.data` | yes | Room 3 clip table, `ClipRepository` (implements `ClipStore` and `ProcessingStore`; audio in `filesDir/clips/<device>/`). |
| `dev.coinai.settings` | yes | DataStore settings; the SamBot token as AES-256-GCM ciphertext + IV, key in the Android Keystore (`KeystoreSecretBox`). A plain token from the first version is encrypted on first read. The coin's Wi-Fi networks (passwords sealed the same way, own key) with their state per coin, and the delivery order. |
| `dev.coinai.ble` | yes | `CoinBleManager` (Nordic Android-BLE-Library), CDM pairing and presence (`CoinPairing`, `CoinPresenceService`), `SyncService` (connectedDevice FGS for one visit). |
| `dev.coinai.app` | yes | `CoinaiApp` (process-wide objects), `ClipWorker` (WorkManager), notifications. |

Clip states: `RECEIVING → RECEIVED → TRANSCRIBED → PROMPTED → REPLIED`, side exits `NO_SPEECH`, `UNCERTAIN`, `FAILED`. A clip is prompted at most once: `TRANSCRIBED → PROMPTED` commits before `POST /prompt`, and a prompt whose outcome is unknown becomes `UNCERTAIN` and is only sent again by the user ("Resend").

Scenes (PROTOCOL.md "Scenes", PLAN.md D22), for a coin whose last `STATUS` has bit 3 set: after the transcript the coin gets `thinking` with it, and the app calls `POST /coin/scene` (at_ms = the clip's start, utc_offset_min = the phone's offset then). The scene goes to the coin in place of the final `REPLY` and is kept with the clip (Room version 2: `scene`, `handoff`).
- No `handoff`: the clip is `REPLIED` with the card's summary as its reply; no prompt, no notification. The row says "Answered on the coin".
- A `handoff` (a `later` card): it is kept on the `TRANSCRIBED` clip and prompted instead of the transcript, claimed at most once as above. Nothing more goes to the coin. The row says "Handed to SamBot" until SamBot's reply arrives (notification as usual); a hand-off that ends `FAILED` or `UNCERTAIN` posts a notification instead of a coin line. A retry reuses the kept scene: the route is not called again.
- The route fails (network, 8 s timeout, non-200, malformed body): logged, and the clip takes the plain way (prompt the transcript, `REPLY`).

Open protocol questions are marked `// PROTOCOL-Q:` in the code. One: the protocol gives no time bound for the `ERROR` 5 that refuses a `SCENE`, so `ReplyWindow` waits 2 s after the last part (also past the last clip's answer) before taking silence as accepted.

Settings on the main screen: SamBot, **Delivery** (Wi-Fi first, the default / phone first / phone only; `CFG_SET` key `0x06` after `HELLO` in every session and at once when changed during a visit), **Coin Wi-Fi** (add a network or an open one, list, remove from the app's list; a new network waits for the next session, goes out with `WIFI_SET` after `STATUS`, and shows as sent or refused; the coin keeps the 4 newest and cannot forget one over BLE), Processing.

`WIFI_SET` has no answer when accepted, and its `ERROR` names no clip, so the engine sends one at a time with a `LIST` after each as the barrier (the coin handles requests in order). Unfinished transfers that `LIST` leaves out are asked for at the end of a session, so one the coin delivered over Wi-Fi gets `ERROR` 1 and is dropped.

`SyncService` sends `utc_offset_min` again when the time zone changes during a visit (a context-registered `ACTION_TIMEZONE_CHANGED` receiver); between visits the change reaches the coin with the next session's `HELLO`.

Pairing: the chooser creates the association, then `CoinPairing` bonds and starts the first sync once Android reports the bond (CDM reports the coin present during the pairing link already, and a sync started then would time out on the passkey dialog). A bond that fails or is cancelled removes the association again, and the main screen says so. `CoinBleManager` never calls Nordic's `ensureBond()`: in 2.11 it removes an existing bond and pairs again.

A clip that cannot reach SamBot because the app's SamBot settings are unusable (no token, a bad URL) or because the proxy refuses the token (401, 403, a redirect to its login) ends `FAILED` with a short line for the coin: "Set up SamBot in the coinai app" or "SamBot didn't accept the app's token".

### Run on the Android emulator (2026-10-17)

Against the simulated coin over netsim (sim/README.md, "With the Android emulator"), Android 16 on the emulator:
- **Pairing:** the chooser, the passkey from the coin's screen, the bond, and the first sync. A wrong passkey leaves no association and the retry note; pairing again then works.
- **Visits:** Standby, a PWR hold, and the clip on the phone 1.75–2.7 s after release, with the stored bond and no prompt. This held with the app process killed, after `am force-stop`, and in forced Doze (`dumpsys deviceidle force-idle`).
- **Settings:** Delivery reaches the coin and is stored there.
- **Processing without a token:** a voiced clip of 4060 ms fails and the coin shows "Didn't reach SamBot / Set up SamBot in the coinai app" about 0.2 s after the clip ends.
- **SamBot with the token (2026-10-18):** `sim/samples/remind-me.ogg` was spoken into the coin.
  - The coin showed `transcribing` 0.5 s after the clip reached the phone, then `thinking` with SamBot's exact transcript 0.8 s later.
  - The prompt and the `/events` stream worked through the exe.dev proxy with the token.
  - SamBot's answer took 91 s: it set the reminder, added to the list, and corrected its own date.
  - By then the coin's 45 s reply window (PROTOCOL.md rule 7) had closed. The coin went back to its face and then to Standby. The answer is on the phone only: the clip row and a notification.

The emulator's Bluetooth controller never reports the advertiser tracking that CDM's presence scan uses: 0 results, whatever the address type. So each visit stands in for that one report with `adb shell cmd companiondevice simulate-device-event <association id> 0`, and with event 1 before the next press. Everything after it is the real path: CDM binds `CoinPresenceService`, `SyncService` connects, and so on.

**Not run yet:**
- the real phone's presence detection and its latency;
- whether a real presence report also starts a force-stopped app;
- ColorOS's battery manager;
- a SamBot answer inside the reply window, shown on the coin (the coin's own Wi-Fi path has done this);
- OTA;
- scenes (`SCENE`, `/coin/scene`, hand-offs): unit-tested against `FakeCoin` and MockWebServer only.

`KeystoreSecretBox` needs a real Keystore; its tests use the same AES-GCM with an in-memory key.

## Build and test (Git Bash)

Needs JDK 21 and the Android SDK (platform `android-36`, build-tools `36.0.0`). `local.properties` is git-ignored; create it once:

```bash
cd android
echo 'sdk.dir=C\:/Users/<you>/AppData/Local/Android/Sdk' > local.properties
export JAVA_HOME=/c/Users/<you>/tools/jdk-21   # this shell only

./gradlew testDebugUnitTest   # unit tests (JVM; the Room test runs on Robolectric)
./gradlew assembleDebug       # app/build/outputs/apk/debug/app-debug.apk
./gradlew lintDebug
```

The first run downloads Gradle 9.8.1 from `services.gradle.org`, which redirects to GitHub releases. If that is too slow, download `gradle-9.8.1-bin.zip` from any mirror, check it against `distributionSha256Sum` in `gradle/wrapper/gradle-wrapper.properties`, and place it in `~/.gradle/wrapper/dists/gradle-9.8.1-bin/<hash>/` (run `./gradlew --version` once to create that directory, stop it, then copy the zip in). The wrapper verifies the checksum before unpacking.

The protocol tests read `../protocol/vectors/v1.json` in place: `app/build.gradle.kts` adds `protocol/vectors` as a test resource directory. Regenerate the vectors with `python protocol/gen_vectors.py`, never edit them here.

The sync tests drive `SyncEngine` against `FakeCoin` (`src/test/.../testing/FakeCoin.kt`), a device simulator that speaks PROTOCOL.md through the real codec and can disconnect, stall, duplicate, reorder, drop and corrupt packets.

## Versions

| Component | Version | Note |
|---|---|---|
| Android Gradle plugin | 9.4.1 | Built-in Kotlin (no `kotlin-android` plugin); supports up to API 37, needs Gradle ≥ 9.6.0 |
| Gradle | 9.8.1 | wrapper, `bin` distribution, SHA-256 pinned |
| Kotlin | 2.4.21 | via the Compose compiler and serialization plugins, which lift AGP's KGP 2.2.10 floor |
| KSP | 2.3.12 | Room's annotation processor |
| Compose BOM | 2026.06.01 | Compose 1.11.4, Material 3 1.4.0 |
| activity-compose | 1.13.0 | |
| DataStore Preferences | 1.2.1 | |
| Room 3 | 3.0.3 | `androidx.room3`, with `androidx.sqlite:sqlite-framework` 2.7.1 (`AndroidSQLiteDriver`) |
| WorkManager | 2.12.0 | |
| Media3 ExoPlayer | 1.11.1 | clip playback |
| Nordic Android-BLE-Library | 2.11.0 | `ble` + `ble-ktx` |
| kotlinx.coroutines | 1.11.0 | |
| kotlinx.serialization | 1.11.0 | |
| OkHttp / MockWebServer 3 | 5.4.0 | |
| JUnit | 4.13.2 | |
| Robolectric | 4.17 | with `androidx.test:core` 1.7.0 |

compileSdk, targetSdk and minSdk are all 36 (PLAN.md D15). Compose BOM 2026.08.00+ (Compose 1.12), OkHttp 5.5.0 and androidx.core 1.19 declare `minCompileSdk=37` in their AAR metadata, so they need compileSdk 37.
