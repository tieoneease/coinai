"""Reference encoder for PROTOCOL.md v1. Writes protocol/vectors/v1.json.

Both implementations (firmware C, app Kotlin) must encode each `fields` entry to exactly `hex`
and decode `hex` back to the same fields. Run: python protocol/gen_vectors.py
"""
import json
import pathlib
import struct

SERVICE_UUID = "c0a10000-f569-41ae-8676-7f5a60d835fc"
CTRL_UUID = "c0a10001-f569-41ae-8676-7f5a60d835fc"
DATA_UUID = "c0a10002-f569-41ae-8676-7f5a60d835fc"

TYPES = {
    "HELLO": 0x01, "LIST": 0x02, "GET": 0x03, "ACK": 0x04, "WIFI_SET": 0x05, "CFG_SET": 0x06, "REPLY": 0x07,
    "STATUS": 0x81, "CLIP_STARTED": 0x82, "CLIP_ENDED": 0x83, "LIST_RESULT": 0x84, "ERROR": 0x85,
}
CFG_KEYS = {"reply_wait_s": (0x01, "<H"), "brightness": (0x02, "<B"), "idle_timeout_s": (0x03, "<H"), "off_after_h": (0x04, "<H")}
REPLY_STATES = {"transcribing": 0, "thinking": 1, "done": 2, "failed": 3}


def tlv(name, value: bytes) -> bytes:
    assert len(value) <= 241, "a TLV must fit one ATT operation (244 bytes at MTU 247)"
    return struct.pack("<BH", TYPES[name], len(value)) + value


def encode(name, f):
    if name == "HELLO":
        v = struct.pack("<BQ", f["version"], f["phone_utc_ms"])
    elif name == "LIST":
        v = b""
    elif name == "GET":
        v = struct.pack("<HI", f["clip"], f["offset"])
    elif name == "ACK":
        v = struct.pack("<HI", f["clip"], f["crc32"])
    elif name == "WIFI_SET":
        ssid, pw = f["ssid"].encode(), f["password"].encode()
        assert len(ssid) <= 32 and len(pw) <= 64
        v = bytes([len(ssid)]) + ssid + bytes([len(pw)]) + pw
    elif name == "CFG_SET":
        key, fmt = CFG_KEYS[f["key"]]
        v = bytes([key]) + struct.pack(fmt, f["value"])
    elif name == "REPLY":
        text = f["text"].encode()
        assert len(text) <= 200
        v = struct.pack("<HB", f["clip"], REPLY_STATES[f["state"]]) + text
    elif name == "STATUS":
        flags = (f["charging"] << 0) | (f["usb_power"] << 1) | (f["recording"] << 2)
        v = struct.pack("<BBH", f["battery_pct"], flags, f["pending"]) + f["fw_version"].encode()
    elif name == "CLIP_STARTED":
        v = struct.pack("<HBIQ", f["clip"], f["codec"], f["sample_rate"], f["start_utc_ms"])
    elif name == "CLIP_ENDED":
        v = struct.pack("<HIII", f["clip"], f["size"], f["duration_ms"], f["crc32"])
    elif name == "LIST_RESULT":
        v = bytes([1 if f["more"] else 0]) + b"".join(
            struct.pack("<HIB", e["clip"], e["size"], 1 if e["complete"] else 0) for e in f["entries"])
    elif name == "ERROR":
        v = struct.pack("<BBH", TYPES[f["request"]], f["code"], f["clip"])
    else:
        raise KeyError(name)
    return tlv(name, v)


def data_packet(clip, offset, payload: bytes) -> bytes:
    assert len(payload) <= 238
    return struct.pack("<HI", clip, offset) + payload


T = 1760000000000  # 2025-10-09T08:53:20Z in ms
CASES = [
    ("app_to_device", "HELLO", {"version": 1, "phone_utc_ms": T}),
    ("app_to_device", "LIST", {}),
    ("app_to_device", "GET", {"clip": 7, "offset": 4096}),
    ("app_to_device", "ACK", {"clip": 7, "crc32": 0xDEADBEEF}),
    ("app_to_device", "WIFI_SET", {"ssid": "home", "password": "secret pass"}),
    ("app_to_device", "CFG_SET", {"key": "reply_wait_s", "value": 45}),
    ("app_to_device", "CFG_SET", {"key": "brightness", "value": 200}),
    ("app_to_device", "REPLY", {"clip": 7, "state": "thinking", "text": ""}),
    ("app_to_device", "REPLY", {"clip": 7, "state": "done", "text": "Got it \u2014 added to your list."}),
    ("device_to_app", "STATUS", {"battery_pct": 87, "charging": 1, "usb_power": 1, "recording": 0, "pending": 2, "fw_version": "0.1.0"}),
    ("device_to_app", "STATUS", {"battery_pct": 255, "charging": 0, "usb_power": 0, "recording": 1, "pending": 0, "fw_version": "0.1.0"}),
    ("device_to_app", "CLIP_STARTED", {"clip": 7, "codec": 1, "sample_rate": 16000, "start_utc_ms": T}),
    ("device_to_app", "CLIP_ENDED", {"clip": 7, "size": 18234, "duration_ms": 6020, "crc32": 0x1234ABCD}),
    ("device_to_app", "LIST_RESULT", {"more": False, "entries": [
        {"clip": 6, "size": 9000, "complete": True},
        {"clip": 7, "size": 18234, "complete": True},
        {"clip": 8, "size": 512, "complete": False}]}),
    ("device_to_app", "ERROR", {"request": "GET", "code": 1, "clip": 9}),
]


def main():
    out = {
        "protocol_version": 1,
        "service_uuid": SERVICE_UUID,
        "ctrl_uuid": CTRL_UUID,
        "data_uuid": DATA_UUID,
        "ctrl": [{"direction": d, "type": n, "type_code": TYPES[n], "fields": f, "hex": encode(n, f).hex()} for d, n, f in CASES],
        "data": [{"clip": 7, "offset": 238, "payload_hex": bytes(range(10)).hex(), "hex": data_packet(7, 238, bytes(range(10))).hex()}],
    }
    path = pathlib.Path(__file__).parent / "vectors" / "v1.json"
    path.parent.mkdir(exist_ok=True)
    path.write_text(json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {path} ({len(out['ctrl'])} ctrl vectors)")


if __name__ == "__main__":
    main()
