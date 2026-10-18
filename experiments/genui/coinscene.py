"""Show a coin/1 scene on the simulator or on the coin and report what the firmware made of it.

    python coinscene.py sim  scene.json [shot.png]   # the running simulator (sim/README.md)
    python coinscene.py COM9 scene.json [shot.png]   # the coin over USB (shot via scripts/fbdump.py)

The firmware is the validator: it lays the card out with its own fonts and prints
"SCENE shown kind=... lines=... issues=N: ..." (scene.c)."""
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.request

REPO = pathlib.Path(__file__).resolve().parents[2]
CHUNK = 400  # hex characters per console line (the coin's console takes 640)


def scene_lines(scene_json: str):
    h = scene_json.encode('utf-8').hex()
    yield 'ui scene begin'
    for i in range(0, len(h), CHUNK):
        yield 'ui scene + ' + h[i:i + CHUNK]
    yield 'ui scene end'


class Sim:
    def __init__(self, port=None):
        port = port or os.environ.get('SIMPORT')
        if not port:
            raise SystemExit('set SIMPORT to the simulator\'s HTTP port')
        self.api = f'http://127.0.0.1:{port}/api'

    def call(self, path, body=None):
        req = urllib.request.Request(self.api + path, data=json.dumps(body).encode() if body is not None else None,
                                     headers={'content-type': 'application/json'})
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)

    def show(self, scene_json: str, wait=6.0) -> str:
        since = self.call('/console')['seq']
        for line in scene_lines(scene_json):
            self.call('/console', {'line': line})
        t0 = time.time()
        while time.time() - t0 < wait:
            for e in self.call(f'/console?since={since}')['entries']:
                k = e['text'].find('SCENE ')
                if k >= 0:
                    return e['text'][k:].strip()
            time.sleep(0.1)
        return 'SCENE timeout'

    def shot(self, path):
        with urllib.request.urlopen(self.api + '/screen.png', timeout=30) as r, open(path, 'wb') as f:
            f.write(r.read())

    def tap(self, x, y):
        self.call('/input', {'touch': {'pressed': True, 'x': x, 'y': y}})
        time.sleep(0.08)
        self.call('/input', {'touch': {'pressed': False, 'x': x, 'y': y}})


class Coin:
    def __init__(self, port):
        import serial
        self.port = port
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.1
        self.s.dtr = self.s.rts = False  # opening with DTR/RTS asserted resets the board
        self.s.open()

    def show(self, scene_json: str, wait=6.0) -> str:
        self.s.reset_input_buffer()
        for line in scene_lines(scene_json):
            data = (line + '\r\n').encode()
            for i in range(0, len(data), 64):  # the USB console's receive buffer is small
                self.s.write(data[i:i + 64])
                time.sleep(0.004)
            time.sleep(0.03)
        buf, t0 = b'', time.time()
        while time.time() - t0 < wait:
            buf += self.s.read(4096)
            for line in buf.decode(errors='replace').splitlines():
                k = line.find('SCENE ')
                if k >= 0 and 'ui scene' not in line:
                    return line[k:].strip()
        return 'SCENE timeout'

    def shot(self, path):
        self.s.close()
        py = sys.executable
        for _ in range(5):
            r = subprocess.run([py, str(REPO / 'scripts' / 'fbdump.py'), self.port, str(path)], capture_output=True,
                               text=True, timeout=120)
            if r.returncode == 0:
                break
        self.s.open()


def main():
    target, src = sys.argv[1], sys.argv[2]
    scene = pathlib.Path(src).read_text(encoding='utf-8')
    dev = Sim() if target == 'sim' else Coin(target)
    print(dev.show(scene))
    if len(sys.argv) > 3:
        time.sleep(1.5)
        dev.shot(sys.argv[3])


if __name__ == '__main__':
    main()
