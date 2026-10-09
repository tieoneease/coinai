"""Shared helpers for talking to the coin's diag console over USB-Serial/JTAG."""
import base64
import re
import time
import zlib

import serial

# Firmware dumps use 57-byte chunks: 76 base64 characters per line, the last one shorter and padded.
# One base64 line: 4-char groups; the last group may be padded ("XX==" encodes a single byte).
B64_LINE = re.compile(r"^(?:[A-Za-z0-9+/]{4}){0,18}(?:[A-Za-z0-9+/]{4}|[A-Za-z0-9+/]{3}=|[A-Za-z0-9+/]{2}==)$")


def open_port(name):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = name, 115200, 0.2
    s.dtr = s.rts = False  # never reset or strap the chip on open
    s.open()
    return s


def fetch_dump(port, command, begin, end, timeout=120):
    """Send `command`, return (begin_fields, payload_bytes, end_fields) for one BEGIN/END block.

    Raises RuntimeError on timeout or when the CRC-32 in the END line does not match.
    """
    s = open_port(port)
    try:
        s.reset_input_buffer()
        s.write(b"\r\n" + command.encode() + b"\r\n")
        buf, header, lines = b"", None, []
        deadline = time.time() + timeout
        while time.time() < deadline:
            buf += s.read(65536)
            *complete, buf = buf.split(b"\n")
            for raw in complete:
                line = raw.strip().decode(errors="replace")
                if line.startswith(begin):
                    header, lines = line.split()[1:], []
                elif line.startswith(end) and header is not None:
                    tail = line.split()[1:]
                    data = base64.b64decode("".join(lines))
                    if (zlib.crc32(data) & 0xFFFFFFFF) != int(tail[0], 16):
                        raise RuntimeError(f"CRC mismatch: got {len(data)} bytes")
                    return header, data, tail
                elif header is not None and B64_LINE.match(line):
                    lines.append(line)
        raise RuntimeError(f"timed out waiting for {end}")
    finally:
        s.close()
