"""Copy a clip off the coin over the diag console and check it.

Usage: python scripts/clipget.py COM9 <clip id> out.ogg
Verifies the transfer CRC, then walks the Ogg pages: page CRCs, BOS/EOS flags, OpusHead fields,
and the duration from the last granule position.
"""
import struct
import sys

from coinlink import fetch_dump


def ogg_crc(data):
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else (crc << 1)
            crc &= 0xFFFFFFFF
    return crc


def check_ogg(data):
    pos, pages, granule, flags_seen, head = 0, 0, 0, [], None
    while pos < len(data):
        if data[pos:pos + 4] != b"OggS":
            raise ValueError(f"no page at byte {pos}")
        htype = data[pos + 5]
        granule = struct.unpack_from("<q", data, pos + 6)[0]
        crc = struct.unpack_from("<I", data, pos + 22)[0]
        nseg = data[pos + 26]
        segs = data[pos + 27:pos + 27 + nseg]
        end = pos + 27 + nseg + sum(segs)
        page = bytearray(data[pos:end])
        page[22:26] = b"\0\0\0\0"
        if ogg_crc(page) != crc:
            raise ValueError(f"page {pages} CRC mismatch")
        if pages == 0:
            body = data[pos + 27 + nseg:end]
            if body[:8] != b"OpusHead":
                raise ValueError("first packet is not OpusHead")
            head = struct.unpack_from("<BBHIhB", body, 8)
        flags_seen.append(htype)
        pages += 1
        pos = end
    if not flags_seen[0] & 0x02 or not flags_seen[-1] & 0x04:
        raise ValueError("missing BOS or EOS flag")
    version, channels, pre_skip, rate, gain, family = head
    seconds = (granule - pre_skip) / 48000
    return f"{pages} pages, Opus v{version} {channels} ch, input {rate} Hz, pre-skip {pre_skip}, {seconds:.2f} s"


def main():
    port, clip, out = sys.argv[1], sys.argv[2], sys.argv[3]
    _, data, tail = fetch_dump(port, f"clip get {clip}", "CLIP-BEGIN", "CLIP-END")
    with open(out, "wb") as f:
        f.write(data)
    print(f"{out}: {len(data)} bytes, transfer CRC {tail[0]} ok; {check_ogg(data)}")


if __name__ == "__main__":
    main()
