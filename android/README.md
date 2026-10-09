# coinai Android app

Kotlin + Jetpack Compose app that will receive clips from the coin over BLE and forward them to SamBot (see `../PLAN.md`, "Android app"). This first slice has no BLE yet:

- `dev.coinai.protocol`: pure-Kotlin codec for `../PROTOCOL.md` v1 (every `ctrl` TLV message, `data` packets). Unknown TLV types decode to `CtrlMessage.Unknown`.
- `dev.coinai.sambot`: `SambotClient` (`health`, `transcribe`, `ask`), mirroring SamBot's reference client `~/.sambot/telegram/src/sambot-client.ts`.
- `MainActivity`: a status placeholder and SamBot settings (base URL, token, "Test connection" against `GET /health`), stored with DataStore. The token is stored in plain text for now; encrypting it is a later task (`TODO(token-encryption)` in `settings/SambotSettings.kt`).

## Build and test (Git Bash)

Needs JDK 21 and the Android SDK (platform `android-36`, build-tools `36.0.0`). `local.properties` is git-ignored; create it once:

```bash
cd android
echo 'sdk.dir=C:/Users/<you>/AppData/Local/Android/Sdk' > local.properties
export JAVA_HOME=/c/Users/<you>/tools/jdk-21   # this shell only

./gradlew testDebugUnitTest   # unit tests: protocol vectors, codec, SambotClient against MockWebServer
./gradlew assembleDebug       # app/build/outputs/apk/debug/app-debug.apk
```

The first run downloads Gradle 9.8.1 from `services.gradle.org`, which redirects to GitHub releases. If that is too slow, download `gradle-9.8.1-bin.zip` from any mirror, check it against `distributionSha256Sum` in `gradle/wrapper/gradle-wrapper.properties`, and place it in `~/.gradle/wrapper/dists/gradle-9.8.1-bin/<hash>/` (run `./gradlew --version` once to create that directory, stop it, then copy the zip in). The wrapper verifies the checksum before unpacking.

The protocol tests read `../protocol/vectors/v1.json` in place: `app/build.gradle.kts` adds `protocol/vectors` as a test resource directory. Regenerate the vectors with `python protocol/gen_vectors.py`, never edit them here.

## Versions

| Component | Version | Note |
|---|---|---|
| Android Gradle plugin | 9.4.1 | Built-in Kotlin (no `kotlin-android` plugin); supports up to API 37, needs Gradle ≥ 9.6.0 |
| Gradle | 9.8.1 | wrapper, `bin` distribution, SHA-256 pinned |
| Kotlin | 2.4.21 | via the Compose compiler and serialization plugins, which lift AGP's KGP 2.2.10 floor |
| Compose BOM | 2026.06.01 | Compose 1.11.4, Material 3 1.4.0 |
| activity-compose | 1.13.0 | |
| DataStore Preferences | 1.2.1 | |
| kotlinx.coroutines | 1.11.0 | |
| kotlinx.serialization | 1.11.0 | |
| OkHttp / MockWebServer 3 | 5.4.0 | |
| JUnit | 4.13.2 | |

compileSdk, targetSdk and minSdk are all 36 (PLAN.md D15). Compose BOM 2026.08.00+ (Compose 1.12) and OkHttp 5.5.0 declare `minCompileSdk=37` in their AAR metadata, so they need compileSdk 37 and the `android-37` platform. Raising compileSdk alone (targetSdk and minSdk can stay 36) unlocks them.
