"""Capture (and optionally drive) the coin's USB-Serial/JTAG console without resetting it on open.

Usage: python scripts/serlog.py COM9 SECONDS [--reset] [--send "line"]...

--reset   pulse the virtual RTS line first (chip reset into the app), then follow the
          port through USB re-enumeration.
--send    write a console line once the port is open (repeatable, sent in order).
"""
import sys
import time

import serial


def open_port(name):
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 0.1
    s.dtr = False  # set before open() so opening never resets or straps the chip
    s.rts = False
    s.open()
    return s


def main():
    port, seconds = sys.argv[1], float(sys.argv[2])
    rest = sys.argv[3:]
    reset = "--reset" in rest
    sends = [rest[i + 1] for i, a in enumerate(rest) if a == "--send" and i + 1 < len(rest)]
    out = sys.stdout.buffer

    if reset:
        s = open_port(port)
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        s.close()
        time.sleep(0.3)

    deadline = time.time() + seconds
    s = None
    while time.time() < deadline:
        if s is None:
            try:
                s = open_port(port)
            except (serial.SerialException, OSError):
                time.sleep(0.05)
                continue
            for line in sends:
                time.sleep(0.3)
                s.write(line.encode() + b"\r\n")
            sends = []
        try:
            data = s.read(4096)
        except (serial.SerialException, OSError):
            s.close()
            s = None
            continue
        if data:
            out.write(data)
            out.flush()
    if s is not None:
        s.close()


if __name__ == "__main__":
    main()
