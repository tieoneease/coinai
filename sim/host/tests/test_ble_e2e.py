"""Bluetooth end to end without the emulator: tools/vlink.py (two Bumble controllers on a LocalLink) <-> coinsim.py
(fake_coin + Bumble peripheral) <-> tools/central.py (a phone-like Bumble central).

Covers: advertising content and interval, MTU 247, DIS/GAP/Battery, ATT 0x05 before pairing, LE Secure Connections
passkey pairing with the passkey read from GET /api/state, indications, credit-limited notifications, bonded
re-encryption on reconnect, WIFI_SET never logged, out-of-range."""

import json
import pathlib
import socket
import subprocess
import sys
import time
import urllib.request

import pytest

HOST = pathlib.Path(__file__).resolve().parent.parent
PY = sys.executable


def free_port() -> int:
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def get(url):
    with urllib.request.urlopen(url, timeout=3) as r:
        return json.loads(r.read())


def post(url, body):
    req = urllib.request.Request(url, json.dumps(body).encode(), {'content-type': 'application/json'})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def wait(cond, timeout=20, what=''):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            if cond():
                return
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.1)
    raise AssertionError(f'timed out: {what}')


def central(phone_port, api, *commands, keys=None, extra=()):
    cmd = [PY, str(HOST / 'tools' / 'central.py'), '--transport', f'tcp-client:127.0.0.1:{phone_port}',
           '--passkey-from', f'{api}/api/state', *extra]
    if keys:
        cmd += ['--keys', str(keys)]
    out = subprocess.run(cmd + list(commands), capture_output=True, text=True, timeout=90)
    lines = [ln for ln in out.stdout.splitlines() if ln.startswith('RESULT ')]
    assert lines, out.stdout + out.stderr
    return json.loads(lines[-1][7:]), out.stdout


@pytest.fixture(params=['random-static', 'public'])
def rig(request, tmp_path):
    coin_port, phone_port, http_port = free_port(), free_port(), free_port()
    ctrl_addr = 'C0:1A:51:00:00:01' if request.param == 'public' else 'F4:00:00:00:00:01'
    procs = []
    try:
        vlink = subprocess.Popen([PY, '-u', str(HOST / 'tools' / 'vlink.py'), '--coin-port', str(coin_port),
                                  '--phone-port', str(phone_port), '--coin-address', ctrl_addr],
                                 stdout=subprocess.PIPE, text=True)
        procs.append(vlink)
        for line in vlink.stdout:
            if 'ready' in line:
                break
        host = subprocess.Popen([PY, '-u', str(HOST / 'coinsim.py'), '--coin', str(HOST / 'fake_coin.py'),
                                 '--ble', f'tcp-client:127.0.0.1:{coin_port}', '--http-port', str(http_port),
                                 '--data', str(tmp_path / 'data'), '--power-on'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(host)
        api = f'http://127.0.0.1:{http_port}'
        wait(lambda: get(f'{api}/api/state')['ble']['advertising'], what='advertising')
        yield request.param, phone_port, api, tmp_path
    finally:
        for p in reversed(procs):
            p.terminate()
            p.wait(10)


def test_phone_flow(rig):
    mode, phone_port, api, tmp = rig
    st = get(f'{api}/api/state')['ble']
    assert st['status'] == 'up' and st['address_type'] == ('public' if mode == 'public' else 'random static')
    keys = tmp / 'phone_keys.json'

    r, out = central(phone_port, api, 'read-dis', 'read-battery', 'hello', 'list', 'get 7 0', 'wifi HomeNet s3cretpass',
                     keys=keys, extra=('--measure-adv', '2.5'))
    assert 'error' not in r, out
    adv = r['adv']
    assert adv['name'] == 'coinai' and adv['connectable'] and adv['flags'] == 6
    assert adv['data_hex'] == '0201061107fc35d8605a7f7686ae4169f50000a1c00709636f696e6169'
    assert adv['address'] == ('C0:1A:51:00:00:01/P' if mode == 'public' else 'C0:1A:51:00:00:01')
    assert 900 < adv['measured_interval_ms'] < 1200  # 1022.5 ms requested by the coin
    assert r['mtu'] == 247
    assert r['dis'] == {'manufacturer': 'coinai', 'model': 'ESP32-S3-Touch-AMOLED-1.75C', 'firmware': 'fake-0.1',
                        'gap_name': 'coinai'}
    assert r['battery'] == 100
    assert r['att_errors'] == [5] and r['paired'] and r['passkey_entered'] == '123456'
    assert r['encrypted'] and r['authenticated']
    assert r['indications'][0].startswith('STATUS battery=100%')
    assert r['indications'][1] == 'LIST_RESULT [7:5000]'
    assert r['indications'][2].startswith('CLIP_ENDED clip=7 size=5000')
    assert r['data'] == {'7': {'first': 0, 'end': 5000, 'packets': 22, 'bytes': 5000, 'in_order': True}}

    log = get(f'{api}/api/ble_log')['entries']
    texts = [e['text'] for e in log]
    assert 'write refused: result 1 -> ATT error 0x05' in texts
    assert 'security: encrypted=1 authenticated=1 bonded=1' in texts
    assert any(t.startswith('DATA clip 7 bytes 0..5000 (22 pkts') for t in texts)
    assert any(t.startswith('WIFI_SET (') for t in texts)
    assert not any('s3cret' in t or 'HomeNet' in t for t in texts)
    assert get(f'{api}/api/state')['ble']['bonds'] == 1
    assert get(f'{api}/api/state')['ble']['credits_in_flight'] == 0

    # Reconnect: the bond re-encrypts the link; no ATT error, no pairing.
    r2, out2 = central(phone_port, api, 'hello', keys=keys)
    assert 'error' not in r2, out2
    assert r2['att_errors'] == [] and not r2['paired'] and r2['encrypted']
    assert r2['indications'][0].startswith('STATUS')
    assert get(f'{api}/api/ble_log')['entries'][-1]['text'].startswith('advertising on')

    # Out of range: not visible; back in range: visible again.
    post(f'{api}/api/input', {'ble': {'out_of_range': True}})
    r3, _ = central(phone_port, api, 'hello', extra=('--scan-timeout', '3'))
    assert 'TimeoutError' in r3.get('error', '')
    post(f'{api}/api/input', {'ble': {'out_of_range': False, 'rssi': -80}})
    r4, out4 = central(phone_port, api, 'hello', keys=keys)
    assert 'error' not in r4, out4
    console = '\n'.join(e['text'] for e in get(f'{api}/api/console')['entries'])
    assert 'rssi -80 dBm' in console
