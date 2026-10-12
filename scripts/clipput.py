"""Store an Ogg Opus file on the coin as a new clip, over the USB console without a reset:
    clipput.py COM9 file.ogg
Prints the new clip id. The coin computes size, CRC and duration itself (voiced: unknown)."""
import re
import sys
import time

import serial


def command(s, line, want, timeout=3.0):
    data = (line + '\r\n').encode()
    for i in range(0, len(data), 64):  # the console's receive buffer is small: no long bursts
        s.write(data[i:i + 64])
        time.sleep(0.004)
    buf = b''
    end = time.time() + timeout
    while time.time() < end:
        buf += s.read(4096)
        m = re.search(want, buf.decode(errors='replace'))
        if m:
            return m
    raise SystemExit(f'no answer to {line[:40]!r}: {buf[-200:]!r}')


def main():
    port, path = sys.argv[1], sys.argv[2]
    data = open(path, 'rb').read()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0.05, False, False  # no reset
    s.open()
    time.sleep(0.2)
    s.read(65536)
    clip = int(command(s, 'clip put begin', r'clip put (\d+)').group(1))
    for i in range(0, len(data), 300):
        command(s, 'clip put ' + data[i:i + 300].hex(), r'\bok\b|failed')
    command(s, 'clip put end', rf'clip {clip} stored')
    s.close()
    print(f'clip {clip}: {len(data)} bytes')


if __name__ == '__main__':
    main()
