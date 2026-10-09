# coinai BLE protocol v1 (draft)

This is the contract between `firmware/` (ESP32-S3, NimBLE peripheral) and `android/` (Kotlin, GATT central). Both sides implement it and share the test vectors in `protocol/vectors/`, which will be added with the first implementation. Status: proposed, 2026-10-09.

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
- **Access:** service discovery is open. Characteristic values require authenticated encryption, and a read or write before pairing fails with "Insufficient Authentication", which starts pairing.
- **Addresses:** the device uses resolvable private addresses once bonded. Android's companion presence detection needs a bonded device for that.
- **Dev builds:** a build flag relaxes security for bench testing. Release builds never ship with it.

## Link

- Data length update to 251 first, then MTU 247.
- Request 2M PHY.
- During transfer: connection interval 15 ms (min = max), latency 0, supervision timeout 6 s.
- Idle: let the central choose the parameters.

## GATT

### Service `coinai`

UUIDs are generated at implementation.

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
| device → app | `CLIP_ENDED` | clip id, size in bytes, duration ms, CRC-32 |
| device → app | `LIST_RESULT` | clip ids, sizes, complete/in-progress flags |

**`data`** (notify):
- Packet: `[clip id u16 LE][offset u32 LE][bytes…]`, up to 238 payload bytes per notification at MTU 247.
- The device streams from the offset given in `GET`, including while the clip is still being recorded, and finishes with `CLIP_ENDED`.

### Standard services

Device Information (0x180A) and Battery (0x180F, with level notifications).

## Rules

1. **Clips are Ogg Opus files** (RFC 7845): 16 kHz mono, about 24 kbps. The app can store, play or upload them unchanged.
2. **The device deletes a clip only after an `ACK` whose CRC matches.** Unacknowledged clips survive power loss and reboots, and they are never deleted automatically.
3. **Transfers are idempotent and resumable.** After any disconnect the app sends `GET` with the last byte offset it stored.
4. **Time.** There is no RTC chip, so `HELLO` sets the device clock. Clips recorded from Off carry an estimated start time; the app corrects it using receive time minus duration.
5. **Versioning.** `HELLO` carries the protocol version. Unknown TLV types are ignored, so either side can add messages without breaking the other.
6. **One message per ATT operation.** A TLV fits in one write or indication: at most 244 bytes at MTU 247. Longer results, such as a long `LIST_RESULT`, are split across several messages.
7. **Reply window.**
   - After the `ACK` for the last pending clip, the device stays awake and connected for up to `reply_wait_s` seconds. This is a `CFG_SET` key; the default is 45.
   - During the window the device shows progress from `REPLY`.
   - A `REPLY` in state `done` or `failed` shows its text and starts the normal idle timeout.
   - When the window closes, the device goes to Standby. A reply that arrives later is only on the phone.
   - The app removes markdown, and any characters the coin's font lacks, before sending.
