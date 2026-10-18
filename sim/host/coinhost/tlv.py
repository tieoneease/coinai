"""PROTOCOL.md v1 TLV messages on `ctrl` and packets on `data`: encoders and a human-readable decoder.

WIFI_SET contents (the network password) are never decoded into text."""

from __future__ import annotations

import struct

SERVICE_UUID = 'c0a10000-f569-41ae-8676-7f5a60d835fc'
CTRL_UUID = 'c0a10001-f569-41ae-8676-7f5a60d835fc'
DATA_UUID = 'c0a10002-f569-41ae-8676-7f5a60d835fc'

HELLO, LIST, GET, ACK, WIFI_SET, CFG_SET, REPLY, SCENE = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
SCENE_PART = 237  # JSON bytes per SCENE message (PROTOCOL.md "Scenes")
STATUS, CLIP_STARTED, CLIP_ENDED, LIST_RESULT, ERROR = 0x81, 0x82, 0x83, 0x84, 0x85

NAMES = {
    HELLO: 'HELLO', LIST: 'LIST', GET: 'GET', ACK: 'ACK', WIFI_SET: 'WIFI_SET', CFG_SET: 'CFG_SET',
    REPLY: 'REPLY', SCENE: 'SCENE', STATUS: 'STATUS', CLIP_STARTED: 'CLIP_STARTED', CLIP_ENDED: 'CLIP_ENDED',
    LIST_RESULT: 'LIST_RESULT', ERROR: 'ERROR',
}
CFG_KEYS = {1: 'reply_wait_s', 2: 'brightness', 3: 'idle_timeout_s', 4: 'off_after_h'}
REPLY_STATES = {0: 'transcribing', 1: 'thinking', 2: 'done', 3: 'failed'}
ERROR_CODES = {1: 'unknown clip', 2: 'bad offset', 3: 'bad length', 4: 'busy', 5: 'invalid value'}


def tlv(msg_type: int, value: bytes = b'') -> bytes:
    return struct.pack('<BH', msg_type, len(value)) + value


def split(buf: bytes) -> list[tuple[int, bytes]]:
    """Split a buffer into (type, value) TLVs. A truncated tail is returned with type -1."""
    out = []
    i = 0
    while i + 3 <= len(buf):
        t, n = struct.unpack_from('<BH', buf, i)
        v = buf[i + 3:i + 3 + n]
        if len(v) != n:
            out.append((-1, bytes(buf[i:])))
            return out
        out.append((t, bytes(v)))
        i += 3 + n
    if i < len(buf):
        out.append((-1, bytes(buf[i:])))
    return out


# ---------------------------------------------------------------- app -> device encoders
def hello(phone_utc_ms: int, version: int = 1) -> bytes:
    return tlv(HELLO, struct.pack('<BQ', version, phone_utc_ms))


def list_() -> bytes:
    return tlv(LIST)


def get(clip: int, offset: int = 0) -> bytes:
    return tlv(GET, struct.pack('<HI', clip, offset))


def ack(clip: int, crc32: int) -> bytes:
    return tlv(ACK, struct.pack('<HI', clip, crc32))


def wifi_set(ssid: bytes, password: bytes) -> bytes:
    return tlv(WIFI_SET, bytes([len(ssid)]) + ssid + bytes([len(password)]) + password)


def cfg_set(key: int, value: int) -> bytes:
    fmt = '<B' if key == 2 else '<H'
    return tlv(CFG_SET, bytes([key]) + struct.pack(fmt, value & 0xFFFF))  # key 5 is signed


def reply(clip: int, state: int, text: str) -> bytes:
    return tlv(REPLY, struct.pack('<HB', clip, state) + text.encode('utf-8')[:200])


def scene(clip: int, json_text: str) -> list[bytes]:
    """A scene as its SCENE messages, in order."""
    b = json_text.encode('utf-8')
    chunks = [b[i:i + SCENE_PART] for i in range(0, len(b), SCENE_PART)] or [b'']
    return [tlv(SCENE, struct.pack('<HBB', clip, i, len(chunks)) + c) for i, c in enumerate(chunks)]


# ---------------------------------------------------------------- device -> app encoders (fake coin, tests)
def status(battery_pct: int, flags: int, pending: int, fw_version: str) -> bytes:
    return tlv(STATUS, struct.pack('<BBH', battery_pct, flags, pending) + fw_version.encode()[:32])


def error(request_type: int, code: int, clip: int = 0xFFFF) -> bytes:
    return tlv(ERROR, struct.pack('<BBH', request_type, code, clip))


def data_packet(clip: int, offset: int, payload: bytes) -> bytes:
    return struct.pack('<HI', clip, offset) + payload


# ---------------------------------------------------------------- decoding
def _fields(fmt: str, v: bytes):
    n = struct.calcsize(fmt)
    if len(v) < n:
        raise ValueError(f'needs {n} bytes, has {len(v)}')
    return struct.unpack_from(fmt, v), v[n:]


def describe_one(t: int, v: bytes) -> str:
    if t < 0:
        return f'TRUNCATED ({len(v)} bytes)'  # never hex: it could be part of a WIFI_SET
    name = NAMES.get(t, f'0x{t:02x}')
    try:
        if t == HELLO:
            (ver, utc), _ = _fields('<BQ', v)
            return f'HELLO v{ver} phone_utc_ms={utc}'
        if t == LIST:
            return 'LIST'
        if t == GET:
            (clip, off), _ = _fields('<HI', v)
            return f'GET clip={clip} offset={off}'
        if t == ACK:
            (clip, crc), _ = _fields('<HI', v)
            return f'ACK clip={clip} crc32={crc:08x}'
        if t == WIFI_SET:
            return f'WIFI_SET ({len(v)} bytes, contents not logged)'
        if t == CFG_SET:
            if not v:
                raise ValueError('empty')
            key = v[0]
            rest = v[1:]
            val = rest[0] if key == 2 and len(rest) >= 1 else (struct.unpack_from('<H', rest)[0] if len(rest) >= 2 else rest.hex())
            return f'CFG_SET {CFG_KEYS.get(key, f"key {key}")}={val}'
        if t == REPLY:
            (clip, st), text = _fields('<HB', v)
            return f'REPLY clip={clip} {REPLY_STATES.get(st, st)} "{text.decode("utf-8", "replace")}"'
        if t == SCENE:
            (clip, part, parts), body = _fields('<HBB', v)
            return f'SCENE clip={clip} part {part + 1}/{parts} ({len(body)} bytes)'
        if t == STATUS:
            (pct, flags, pending), fw = _fields('<BBH', v)
            bits = [n for b, n in ((1, 'charging'), (2, 'usb'), (4, 'recording'), (8, 'scenes')) if flags & b]
            pct_s = 'unknown' if pct == 255 else f'{pct}%'
            return f'STATUS battery={pct_s} flags=[{",".join(bits)}] pending={pending} fw="{fw.decode("utf-8", "replace")}"'
        if t == CLIP_STARTED:
            (clip, codec, rate, utc), _ = _fields('<HBIQ', v)
            return f'CLIP_STARTED clip={clip} codec={"ogg-opus" if codec == 1 else codec} rate={rate} start_utc_ms={utc}'
        if t == CLIP_ENDED:
            (clip, size, dur, crc, voiced, utc), _ = _fields('<HIIIIQ', v)
            voiced_s = 'unknown' if voiced == 0xFFFFFFFF else voiced
            return (f'CLIP_ENDED clip={clip} size={size} duration_ms={dur} crc32={crc:08x} '
                    f'voiced_ms={voiced_s} start_utc_ms={utc}')
        if t == LIST_RESULT:
            (flags,), rest = _fields('<B', v)
            entries = []
            while len(rest) >= 7:
                clip, size, ef = struct.unpack_from('<HIB', rest)
                entries.append(f'{clip}:{size}{"" if ef & 1 else "*"}')
                rest = rest[7:]
            more = ' more' if flags & 1 else ''
            return f'LIST_RESULT [{" ".join(entries)}]{more}'
        if t == ERROR:
            (req, code, clip), _ = _fields('<BBH', v)
            clip_s = '-' if clip == 0xFFFF else clip
            return f'ERROR request={NAMES.get(req, hex(req))} code={code} ({ERROR_CODES.get(code, "?")}) clip={clip_s}'
    except (ValueError, struct.error) as e:
        return f'{name} malformed ({e}): {v.hex()}'
    return f'{name} {v.hex()}'


def describe(buf: bytes) -> str:
    """Describe a ctrl value (one or more TLVs)."""
    parts = [describe_one(t, v) for t, v in split(buf)]
    return '; '.join(parts) if parts else f'(empty {len(buf)} bytes)'


def parse_data(buf: bytes) -> tuple[int, int, int] | None:
    """data packet -> (clip, offset, payload length)."""
    if len(buf) < 6:
        return None
    clip, off = struct.unpack_from('<HI', buf)
    return clip, off, len(buf) - 6
