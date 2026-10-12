# coinai BLE protocol v1 (draft)

This is the contract between `firmware/` (ESP32-S3, NimBLE peripheral) and `android/` (Kotlin, GATT central). Both sides implement it and must pass the test vectors in `protocol/vectors/v1.json`. `protocol/gen_vectors.py` is the reference encoder that writes them. Status: wire format fixed 2026-10-09.

## Advertising

- **PDU:** legacy `ADV_IND` on all three channels.
- **Contents:** flags, the complete 128-bit coinai service UUID, and the name `coinai`. The Android Companion Device association filter matches on the service UUID.
- **After a recording** (a clip is pending):
  - every 20 ms for 30 s;
  - then every 152.5 ms until the delivery window closes;
  - then stop and go to Standby.
- **Pending clips** are retried on the next wake and on a periodic timer wake.
- **Active with nothing pending:** every 1022.5 ms, so the app can reach settings and status.

The intervals follow Apple's Accessory Design Guidelines §58.5. They are harmless on Android and keep iOS possible later.

## Security

- **Pairing:** LE Secure Connections with bonding and MITM protection. The device shows a 6-digit passkey on its screen (IO capability DisplayOnly), and the user types it on the phone.
- **Access:** service discovery is open. A write to `ctrl` before the link is encrypted with an authenticated (MITM) key fails with "Insufficient Authentication", which starts pairing, and the device sends nothing on `ctrl` or `data` to such a link. The CCCDs can be written before pairing.
- **Addresses:** the device uses resolvable private addresses once bonded. Android's companion presence detection needs a bonded device for that. *Firmware status: the first build advertises with its public identity address; RPAs wait until companion presence detection has been tested with them on the phone.*
- **Bench mode:** `ble bench` on the USB console lets an unpaired, unencrypted link use the protocol until the next reboot, for PC test clients that cannot enter a passkey. `STATUS` then reports `fw_version` with a `-bench` suffix. `WIFI_SET` is refused (`ERROR` code 5) on any link that is not encrypted and authenticated, bench mode included.

## Link

- Data length update to 251 first, then MTU 247. The device starts the data length update and the 2M PHY request after connecting; the phone, as GATT client, requests the 247-byte MTU.
- During transfer: connection interval 15 ms (min = max), latency 0, supervision timeout 6 s.
- Idle: let the central choose the parameters.

## GATT

### Service `coinai`

| Item | UUID |
|---|---|
| Service `coinai` | `c0a10000-f569-41ae-8676-7f5a60d835fc` |
| `ctrl` characteristic | `c0a10001-f569-41ae-8676-7f5a60d835fc` |
| `data` characteristic | `c0a10002-f569-41ae-8676-7f5a60d835fc` |

**`ctrl`** (write with response + indicate). Messages are TLV: `type u8, len u16 LE, value`.

| Direction | Message | Fields |
|---|---|---|
| app → device | `HELLO` | protocol version, phone UTC ms |
| app → device | `LIST` | (none) |
| app → device | `GET` | clip id, byte offset |
| app → device | `ACK` | clip id, CRC-32 of the whole file |
| app → device | `WIFI_SET` | SSID, password |
| app → device | `CFG_SET` | key, value |
| app → device | `REPLY` | clip id, state (`transcribing`, `thinking`, `done`, `failed`), text: UTF-8 plain text of at most 200 bytes, cut at a character boundary |
| device → app | `STATUS` | battery %, charging, pending clip count, recording flag, firmware version |
| device → app | `CLIP_STARTED` | clip id, codec (`ogg-opus`), sample rate, estimated start UTC ms |
| device → app | `CLIP_ENDED` | clip id, size in bytes, duration ms, CRC-32, voiced ms, start UTC ms |
| device → app | `LIST_RESULT` | clip ids, sizes, complete/in-progress flags |

**`data`** (notify):
- Packet: `[clip id u16 LE][offset u32 LE][bytes…]`, up to 238 payload bytes per notification at MTU 247.
- The device streams from the offset given in `GET`, including while the clip is still being recorded, and finishes with `CLIP_ENDED`.

### Wire format v1

All integers are little-endian. Strings are UTF-8 with no terminator, and a string field takes the rest of the value unless a length byte precedes it. Message types `0x01`–`0x3F` go app → device; `0x80`–`0xBF` go device → app.

| Code | Message | Value layout |
|---|---|---|
| `0x01` | `HELLO` | `version u8` (1), `phone_utc_ms u64` |
| `0x02` | `LIST` | empty |
| `0x03` | `GET` | `clip u16`, `offset u32` |
| `0x04` | `ACK` | `clip u16`, `crc32 u32` |
| `0x05` | `WIFI_SET` | `ssid_len u8`, `ssid` (1–32 bytes), `pass_len u8`, `password` (0 bytes for an open network, else 8–64). Adds the network to the coin's list of up to 4 for Wi-Fi delivery (the newest first, the oldest dropped; the same SSID again replaces its password). Invalid → `ERROR` code 5 |
| `0x06` | `CFG_SET` | `key u8`, then the value: `0x01` reply_wait_s `u16` (0–600, default 45), `0x02` brightness `u8` (percent, 1–100, default 100), `0x03` idle_timeout_s `u16` (5–3600, default 30), `0x04` off_after_h `u16` (0–720, 0 = never, default 0), `0x05` utc_offset_min `i16` (local time minus UTC in minutes, −720 to 840; unset until sent, and the coin shows no clock until then), `0x06` delivery `u16` (0 Wi-Fi first, 1 phone first, 2 phone only; default 0; see "Wi-Fi delivery"). Stored on the device; out of range → `ERROR` code 5 |
| `0x07` | `REPLY` | `clip u16`, `state u8` (0 transcribing, 1 thinking, 2 done, 3 failed), `text` (\≤ 200 bytes) |
| `0x81` | `STATUS` | `battery_pct u8` (255 = unknown), `flags u8` (bit 0 charging, bit 1 USB power, bit 2 recording), `pending u16`, `fw_version` (\≤ 32 bytes) |
| `0x82` | `CLIP_STARTED` | `clip u16`, `codec u8` (1 = Ogg Opus), `sample_rate u32`, `start_utc_ms u64` (0 = unknown) |
| `0x83` | `CLIP_ENDED` | `clip u16`, `size u32`, `duration_ms u32`, `crc32 u32`, `voiced_ms u32` (time with speech-level audio; the app does not prompt SamBot for a clip below its threshold; `0xFFFFFFFF` = unknown, for a clip recovered after power loss), `start_utc_ms u64` (0 = unknown) |
| `0x84` | `LIST_RESULT` | `flags u8` (bit 0 = more messages follow), then entries of `clip u16`, `size u32`, `flags u8` (bit 0 complete), up to 34 per message |
| `0x85` | `ERROR` | `request_type u8`, `code u8` (1 unknown clip, 2 bad offset, 3 bad length, 4 busy, 5 invalid value), `clip u16` (`0xFFFF` if none) |

- **CRC-32:** IEEE 802.3, the same as zlib's `crc32`, taken over the whole `.ogg` file.
- **Which messages are unsolicited:** `STATUS` (also whenever battery, charging, recording or the pending count changes) and `CLIP_STARTED` (a recording began during the connection, or Wi-Fi gave a clip back; the app may `GET` it at once, and a recording one arrives as it is recorded). Everything else answers a request.
- **Answers:** `HELLO` → `STATUS`. `LIST` → one or more `LIST_RESULT`. `GET` → data packets, then `CLIP_ENDED`. A matching `ACK` → `STATUS` with the new pending count; a refused one → `ERROR`. A malformed or invalid request → `ERROR`.
- **Requests are handled in order,** one at a time: answers (and `ERROR`s) come back in the order of the requests they answer. `WIFI_SET` and `CFG_SET` get no answer when accepted, so the app follows one with `LIST` and reads an `ERROR` for it that arrives before the `LIST_RESULT` as its refusal.
- **A new `GET` replaces a running one;** the replaced transfer gets no `CLIP_ENDED`.
- **A clip discarded while it streams** (the user held BOOT during the recording) is deleted on the device. The transfer stops with `ERROR` code 1 for that clip instead of `CLIP_ENDED`, and the clip leaves the pending count; the app drops what it received.
- **Clip ids:** `u16`, increasing, kept across reboots in NVS. They wrap after 65535.
- **A clip the coin is sending over Wi-Fi** is not the phone's: `LIST_RESULT` and the pending count leave it out, `CLIP_STARTED` is not sent for it, and a `GET` or `ACK` of it gets `ERROR` code 4 (busy). It then either disappears (delivered: a later `GET` gets code 1) or comes back with `CLIP_STARTED` (Wi-Fi failed before SamBot was prompted).
- **On connect:** the device answers `HELLO` with `STATUS`.

### Standard services

Device Information (0x180A) and Battery (0x180F, with level notifications).

## Rules

1. **Clips are Ogg Opus files** (RFC 7845): 16 kHz mono, about 24 kbps. The app can store, play or upload them unchanged.
2. **The device deletes a clip only after an `ACK` whose CRC matches.** Unacknowledged clips survive power loss and reboots, and they are never deleted automatically.
3. **Transfers are idempotent and resumable.** After any disconnect the app sends `GET` with the last byte offset it stored.
4. **Time.** There is no RTC chip, so `HELLO` sets the device clock, and it survives Standby but not Off. `CLIP_ENDED` carries the clip's start time when the device knew the time while recording; otherwise it is 0 and the app estimates it as receive time minus duration.
5. **Versioning.** `HELLO` carries the protocol version. Unknown TLV types are ignored, so either side can add messages without breaking the other.
6. **One message per ATT operation.** A TLV fits in one write or indication: at most 244 bytes at MTU 247. Longer results, such as a long `LIST_RESULT`, are split across several messages.
7. **Reply window.**
   - After the `ACK` for the last pending clip, the device stays awake and connected for up to `reply_wait_s` seconds. This is a `CFG_SET` key; the default is 45.
   - During the window the device shows progress from `REPLY`.
   - A `REPLY` in state `done` or `failed` shows its text and starts the normal idle timeout.
   - When the window closes, the device goes to Standby. A reply that arrives later is only on the phone.
   - The app removes markdown, and any characters the coin's font lacks, before sending. The font covers printable ASCII (0x20–0x7E) and newline. The coin itself maps ‘ ’ ′ to `'`, “ ” ″ to `"`, … to `...`, – — − • · to `-`, no-break and thin spaces to a space, ° to ` deg` and × to `x`, and drops anything else.
   - In state `thinking` the text may carry what SamBot heard (the transcript); the coin shows it while it waits.
8. **Local time.** The coin shows a clock only after the app has sent `utc_offset_min` (`CFG_SET` key `0x05`). The app sends it after `HELLO` and whenever the phone's time zone changes.

## Wi-Fi delivery

With a known network (`WIFI_SET`) and a SamBot token stored, the coin can send clips to SamBot itself (PLAN.md D10). It makes the same calls as the app: `POST /voice/transcribe`, then `GET /events?lane=web&tail=1` until `host_stream_live`, `POST /prompt`, and the assistant's text until `agent_settled`, through the exe.dev proxy with the token in `X-Exedev-Authorization`. The coin shows the progress and reply as if the app had sent `REPLY`. The prompt goes out at most once: the coin marks the clip on flash before `POST /prompt`, and a clip so marked is deleted at the next boot instead of being sent again.

- **`delivery` (`CFG_SET` key `0x06`):** 0 = Wi-Fi first: a new recording is the coin's from its first byte, and goes to the phone only if no known network answers or SamBot is not reached. 1 = phone first: Wi-Fi only when no phone is connected when the clip ends. 2 = phone only. Clips left from earlier go over Wi-Fi in the background in modes 0 and 1.
- **Left to the phone:** clips below the voiced threshold (the app keeps them as "no speech"), clips the phone received in full but has not `ACK`ed yet, and everything while the battery is below 3.6 V off USB power.
- **The token** is set over the USB console (`net token`), never sent back. It is an exe.dev API token scoped to the SamBot VM.
- **Firmware status:** built; the network calls have not yet run against a real network.
