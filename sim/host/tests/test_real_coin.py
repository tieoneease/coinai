"""The real firmware (sim/coin/build/coinsim.exe) under the host, over tools/vlink.py and tools/central.py.

fake_coin.py stands in for the coin everywhere else; this test is what checks the IPC packing and timing of the actual
C side: boot, a PWR recording of speech, pairing, GET and ACK, Standby and the PTT wake with RTC memory and the
clock, and a mouse-speed tap on the touch screen. Skipped when coinsim.exe has not been built (sim/README.md)."""

import json
import re
import subprocess
import time
import zlib

import pytest

from test_ble_e2e import HOST, PY, central, free_port, get, post, wait

COIN = HOST.parent / 'coin' / 'build' / 'coinsim.exe'
SAMPLE = 'samples/remind-me.ogg'

pytestmark = pytest.mark.skipif(not COIN.exists(), reason='sim/coin/build/coinsim.exe not built')


def console_since(api, seq):
    return [e['text'] for e in get(f'{api}/api/console?since={seq}')['entries']]


def console_seq(api):
    return get(f'{api}/api/console')['seq']


def wait_line(api, seq, pattern, timeout=15):
    found = []

    def match():
        found[:] = [m for m in (re.search(pattern, t) for t in console_since(api, seq)) if m]
        return bool(found)

    wait(match, timeout, what=pattern)
    return found[0]


@pytest.fixture
def rig(tmp_path):
    coin_port, phone_port, http_port = free_port(), free_port(), free_port()
    procs = []
    try:
        vlink = subprocess.Popen([PY, '-u', str(HOST / 'tools' / 'vlink.py'), '--coin-port', str(coin_port),
                                  '--phone-port', str(phone_port)], stdout=subprocess.PIPE, text=True)
        procs.append(vlink)
        for line in vlink.stdout:
            if 'ready' in line:
                break
        host = subprocess.Popen([PY, '-u', str(HOST / 'coinsim.py'), '--coin', str(COIN),
                                 '--ble', f'tcp-client:127.0.0.1:{coin_port}', '--http-port', str(http_port),
                                 '--data', str(tmp_path / 'data'), '--power-on'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(host)
        api = f'http://127.0.0.1:{http_port}'
        wait(lambda: get(f'{api}/api/state')['ble']['advertising'], what='advertising')
        yield phone_port, api, tmp_path
    finally:
        for p in reversed(procs):
            p.terminate()
            p.wait(10)


def record(api, hold_ms):
    """Hold PWR with the speech sample playing into the mics; returns (clip id, voiced ms)."""
    seq = console_seq(api)
    post(f'{api}/api/input', {'mic': {'source': 'file', 'file': SAMPLE, 'loop': False}, 'hold': {'pwr': hold_ms}})
    m = wait_line(api, seq, r'clip (\d+): (\d+) bytes, (\d+) ms \(voiced (\d+)\)')
    return int(m.group(1)), int(m.group(4))


def test_real_coin(rig):
    phone_port, api, tmp = rig
    st = get(f'{api}/api/state')
    assert st['power']['state'] == 'active' and st['power']['fw_version'].endswith('-sim')
    assert st['panel']['frames'] > 0
    keys = tmp / 'phone_keys.json'

    # A PWR recording of speech passes the speech gate.
    clip, voiced = record(api, 4500)
    assert voiced > 1500

    # Pairing, LIST and GET. Pairing must not stall (Bumble's controller once held it 10 s; see vlink.py).
    t0 = time.monotonic()
    r, out = central(phone_port, api, 'hello', 'list', f'get {clip} 0', 'wait 0.3', keys=keys)
    assert 'error' not in r, out
    assert r['paired'] and r['encrypted'] and r['authenticated']
    assert time.monotonic() - t0 < 8, out
    ended = next(i for i in r['indications'] if i.startswith(f'CLIP_ENDED clip={clip} '))
    size = int(re.search(r'size=(\d+)', ended).group(1))
    crc = re.search(r'crc32=([0-9a-f]{8})', ended).group(1)
    stored = (tmp / 'data' / 'littlefs' / f'{clip:05d}.ogg').read_bytes()
    assert size == len(stored) and int(crc, 16) == zlib.crc32(stored)
    assert r['data'][str(clip)] == {'first': 0, 'end': size, 'packets': r['data'][str(clip)]['packets'],
                                    'bytes': size, 'in_order': True}

    # ACK deletes it; the bond re-encrypts without pairing.
    r, out = central(phone_port, api, 'hello', f'ack {clip} {crc}', 'list', keys=keys)
    assert 'error' not in r, out
    assert not r['paired'] and r['encrypted']
    assert 'LIST_RESULT []' in r['indications']
    assert not (tmp / 'data' / 'littlefs' / f'{clip:05d}.ogg').exists()

    # Standby, then PWR: the host relaunches the coin as a deep-sleep wake and the clip records at once.
    seq = console_seq(api)
    post(f'{api}/api/console', {'line': 'sleep'})
    wait(lambda: get(f'{api}/api/state')['power']['state'] == 'standby', what='standby')
    clip2, voiced2 = record(api, 4500)
    st = get(f'{api}/api/state')['power']
    assert st['reset_reason'] == 8 and st['wake'] == 8
    assert any('wake: ptt' in t for t in console_since(api, seq))
    assert voiced2 > 1500

    # The clock the phone set before Standby survived it (RTC memory).
    r, out = central(phone_port, api, 'hello', f'get {clip2} 0', 'wait 0.3', keys=keys)
    ended = next(i for i in r['indications'] if i.startswith(f'CLIP_ENDED clip={clip2} '))
    start_ms = int(re.search(r'start_utc_ms=(\d+)', ended).group(1))
    assert abs(start_ms / 1000 - time.time()) < 60, ended

    # A tap shorter than LVGL's 30 ms input poll still reaches the UI.
    seq = console_seq(api)
    post(f'{api}/api/input', {'touch': {'pressed': True, 'x': 233, 'y': 350}})
    post(f'{api}/api/input', {'touch': {'pressed': False, 'x': 233, 'y': 350}})
    wait_line(api, seq, r'touch down at 233,350')
