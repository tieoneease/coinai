"""Grab the coin's current LVGL screen over the diag console and save it as a PNG.

Usage: python scripts/fbdump.py COM9 out.png
"""
import struct
import sys
import zlib

from coinlink import fetch_dump


def write_png(path, w, h, rgb):
    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    port, out = sys.argv[1], sys.argv[2]
    header, data, _ = fetch_dump(port, "fbdump", "FBDUMP-BEGIN", "FBDUMP-END", timeout=60)
    w, h, stride = int(header[0]), int(header[1]), int(header[2])
    rgb = bytearray(w * h * 3)
    for y in range(h):
        row = data[y * stride:y * stride + w * 2]
        for x, (px,) in enumerate(struct.iter_unpack("<H", row)):
            i = (y * w + x) * 3
            rgb[i] = ((px >> 11) & 0x1F) * 255 // 31
            rgb[i + 1] = ((px >> 5) & 0x3F) * 255 // 63
            rgb[i + 2] = (px & 0x1F) * 255 // 31
    write_png(out, w, h, bytes(rgb))
    print(f"{out}: {w}x{h}, {len(data)} bytes, CRC ok, {' '.join(header[3:])}")


if __name__ == "__main__":
    main()
