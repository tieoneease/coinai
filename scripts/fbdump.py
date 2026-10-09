"""Grab the coin's current LVGL screen over the diag console and save it as a PNG.

Usage: python scripts/fbdump.py COM9 out.png
Sends `fbdump`, decodes the base64 RGB565 frame between FBDUMP-BEGIN/END and checks its CRC-32.
"""
import base64
import re
import struct
import sys
import time
import zlib

import serial


# The firmware emits 57-byte chunks: 76 base64 characters, the last line possibly shorter and padded.
B64_LINE = re.compile(r"^[A-Za-z0-9+/]{4,76}={0,2}$")


def open_port(name):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = name, 115200, 0.2
    s.dtr = s.rts = False  # never reset or strap the chip on open
    s.open()
    return s


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
    s = open_port(port)
    s.reset_input_buffer()
    s.write(b"\r\nfbdump\r\n")
    buf, header, lines = b"", None, []
    deadline = time.time() + 60
    while time.time() < deadline:
        buf += s.read(65536)
        *complete, buf = buf.split(b"\n")
        for raw in complete:
            line = raw.strip().decode(errors="replace")
            if line.startswith("FBDUMP-BEGIN"):
                header, lines = line.split(), []
            elif line.startswith("FBDUMP-END") and header:
                s.close()
                return finish(header, lines, line.split()[1], out)
            elif header and B64_LINE.match(line):
                lines.append(line)
    sys.exit("timed out waiting for FBDUMP-END")


def finish(header, lines, crc_hex, out):
    w, h, stride = int(header[1]), int(header[2]), int(header[3])
    data = base64.b64decode("".join(lines))
    crc_ok = (zlib.crc32(data) & 0xFFFFFFFF) == int(crc_hex, 16)
    rgb = bytearray(w * h * 3)
    for y in range(h):
        row = data[y * stride:y * stride + w * 2]
        for x, (px,) in enumerate(struct.iter_unpack("<H", row)):
            i = (y * w + x) * 3
            rgb[i] = ((px >> 11) & 0x1F) * 255 // 31
            rgb[i + 1] = ((px >> 5) & 0x3F) * 255 // 63
            rgb[i + 2] = (px & 0x1F) * 255 // 31
    write_png(out, w, h, bytes(rgb))
    print(f"{out}: {w}x{h}, {len(data)} bytes, CRC {'ok' if crc_ok else 'MISMATCH'}, {' '.join(header[4:])}")
    if not crc_ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
