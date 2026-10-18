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

from coinhost import tlv
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


def test_scenes(rig):
    """PROTOCOL.md "Scenes": STATUS says the coin shows scenes; a two-part SCENE is shown and ends the reply window; one
    the coin cannot show (no card) gets ERROR 5 and leaves the screen alone."""
    phone_port, api, tmp = rig
    vectors = json.loads((HOST.parent.parent / 'protocol' / 'vectors' / 'v1.json').read_text())
    split = vectors['scene_split'][0]
    keys = tmp / 'phone_keys.json'

    def reply_window():
        seq = console_seq(api)
        post(f'{api}/api/console', {'line': 'ble'})
        return wait_line(api, seq, r'reply window (\w+)').group(1)

    # The ACK of a clip opens the reply window (rule 7), and the phone says it is thinking.
    clip, _ = record(api, 3000)
    r, out = central(phone_port, api, 'hello', f'get {clip} 0', 'wait 0.3', f'ack {clip} last',
                     f'reply {clip} 1 How do I reset my router?', 'wait 0.3', keys=keys)
    assert 'error' not in r, out
    assert 'scenes' in r['indications'][0], r['indications']
    assert reply_window() == 'open'

    # Its answer comes as a scene in two parts.
    seq = console_seq(api)
    r, out = central(phone_port, api, 'hello', f'scene {clip} {split["json"]}', 'wait 0.8', keys=keys)
    assert 'error' not in r, out
    shown = wait_line(api, seq, rf'SCENE clip {clip}: (\d+) bytes, shown')
    assert shown.group(1) == str(len(split['json'].encode()))
    assert not any(i.startswith('ERROR') for i in r['indications']), r['indications']
    assert ui_mode(api) == 'scene'
    assert reply_window() == 'closed'

    # One without a card cannot be shown: ERROR 5, and the card stays.
    r, out = central(phone_port, api, 'hello', 'scene 10 {"say":"hi","mood":"happy"}', 'wait 0.8', keys=keys)
    assert 'ERROR request=SCENE code=5 (invalid value) clip=10' in r['indications'], r['indications']
    assert ui_mode(api) == 'scene'


def test_scene_timer_through_standby(rig):
    """A timer card keeps running through Standby: the coin sleeps on its idle timeout with a timer wake, wakes before
    the end and shows the timer run out (about 75 s)."""
    phone_port, api, tmp = rig
    post(f'{api}/api/input', {'usb': False})
    post(f'{api}/api/console', {'line': 'ble inject 060300030500'})  # CFG_SET idle_timeout_s 5
    timer = json.dumps({'say': 'On it.', 'mood': 'focus', 'card': {'kind': 'timer', 'label': 'Pasta', 'seconds': 72}})
    seq = console_seq(api)
    t0 = time.monotonic()
    post(f'{api}/api/console', {'line': 'ble inject ' + tlv.scene(5, timer)[0].hex()})
    wait_line(api, seq, r'SCENE clip 5: \d+ bytes, shown')
    sleep_s = int(wait_line(api, seq, r'standby: timer (\d+) s', timeout=20).group(1))
    assert 60 <= sleep_s <= 68, sleep_s  # 72 s less the idle timeout and the early wake
    wait_line(api, seq, r'wake: timer', timeout=80)
    wait_line(api, seq, r'SCENE-ACTION timer done', timeout=10)
    assert 71 < time.monotonic() - t0 < 76
    assert ui_mode(api) == 'scene'


def test_scene_timer_with_the_screen_off_on_usb(rig):
    """On USB power the idle timeout turns the screen off instead; the timer still goes off and turns it back on, and
    once unplugged the coin goes to Standby (a missed alarm once kept it awake for good). About 80 s."""
    phone_port, api, tmp = rig
    post(f'{api}/api/input', {'usb': True})
    post(f'{api}/api/console', {'line': 'ble inject 060300030500'})  # CFG_SET idle_timeout_s 5
    timer = json.dumps({'say': 'On it.', 'mood': 'focus', 'card': {'kind': 'timer', 'label': 'Pasta', 'seconds': 68}})
    seq = console_seq(api)
    t0 = time.monotonic()
    post(f'{api}/api/console', {'line': 'ble inject ' + tlv.scene(5, timer)[0].hex()})
    wait_line(api, seq, r'SCENE clip 5: \d+ bytes, shown')
    wait_line(api, seq, r'idle on USB power: screen off', timeout=20)
    wait(lambda: get(f'{api}/api/state')['panel']['brightness'] == 0, 5, what='panel dark')
    wait_line(api, seq, r'SCENE-ACTION timer done', timeout=80)
    assert 67 < time.monotonic() - t0 < 72
    wait(lambda: get(f'{api}/api/state')['panel']['brightness'] > 0, 5, what='panel lit again')
    assert ui_mode(api) == 'scene'

    post(f'{api}/api/input', {'usb': False})
    wait_line(api, seq, r'standby: timer 0 s', timeout=20)


# The on-screen keyboard's keys (firmware/main/kbd.c): row centres and the first key's left edge.
KEY_ROWS = {0: (190, 20, 'qwertyuiop', '1234567890'), 1: (240, 42, 'asdfghjkl', '-/:;()$&@"'),
            2: (290, 85, 'zxcvbnm', '.,?!\'')}
PAGE_KEY, BKSP, DONE = (93, 340), (411, 290), (356, 340)


def key_xy(ch):
    """Centre of a letter key (letters page) or a digit/symbol key (the 123 page), and which page."""
    for row, (y, x0, letters, digits) in KEY_ROWS.items():
        if ch in letters:
            return x0 + letters.index(ch) * 43 + 19, y, 'abc'
        if ch in digits:
            x0 = 20 if row < 2 else 85 + (297 - 5 * 56 - 16) // 2
            pitch = 43 if row < 2 else 60
            w = 39 if row < 2 else 56
            return x0 + digits.index(ch) * pitch + w // 2, y, '123'
    raise KeyError(ch)


def touch(api, x, y, pressed=True):
    post(f'{api}/api/input', {'touch': {'pressed': pressed, 'x': x, 'y': y}})
    time.sleep(0.07)  # longer than LVGL's 30 ms input poll


def tap(api, x, y):
    touch(api, x, y)
    touch(api, x, y, False)


def ui_mode(api):
    seq = console_seq(api)
    post(f'{api}/api/console', {'line': 'ui state'})
    return wait_line(api, seq, r'ui: mode (\w+)').group(1)


def test_wifi_setup_by_touch(rig):
    """Settings > Wi-Fi > Add network on the touch screen alone: the scan list, the keyboard (pages, backspace,
    slide-to-correct), a wrong password and a retry that keeps the text, joining, and hold-to-forget."""
    phone_port, api, tmp = rig
    post(f'{api}/api/console', {'line': 'ui show settings'})
    wait(lambda: ui_mode(api) == 'settings', what='settings')
    tap(api, 233, 176)  # Wi-Fi
    wait(lambda: ui_mode(api) == 'wifi', what='Wi-Fi screen')
    tap(api, 233, 340)  # Add network (no network saved yet)
    seq = console_seq(api)
    wait(lambda: ui_mode(api) == 'scan', what='scan')
    time.sleep(2.6)  # the simulated scan takes 2 s
    tap(api, 233, 176)  # the strongest network: "Home"
    wait(lambda: ui_mode(api) == 'kbd', what='keyboard')
    time.sleep(0.4)

    def type_text(text, page='abc'):
        for ch in text:
            x, y, want = key_xy(ch)
            if want != page:
                tap(api, *PAGE_KEY)
                page = want
            tap(api, x, y)
        return page

    # A wrong password first: Done, then "Password not accepted"; a tap brings the keyboard back with the text.
    type_text('wrongpass')
    tap(api, *DONE)
    wait_line(api, seq, r'setup join "Home": result 1', timeout=10)
    wait(lambda: ui_mode(api) == 'join', what='result screen')
    tap(api, 233, 240)
    wait(lambda: ui_mode(api) == 'kbd', what='keyboard again')
    time.sleep(0.4)
    post(f'{api}/api/input', {'touch': {'pressed': True, 'x': BKSP[0], 'y': BKSP[1]}})
    time.sleep(2.0)  # held: repeats until the field is empty
    post(f'{api}/api/input', {'touch': {'pressed': False, 'x': BKSP[0], 'y': BKSP[1]}})
    time.sleep(0.1)

    # "coinai-sim", with "-" from the 123 page, an extra letter taken back, and "s" reached by sliding from "a".
    page = type_text('coinai')
    page = type_text('-', page)
    tap(api, *PAGE_KEY)  # abc
    x, y, _ = key_xy('a')
    touch(api, x, y)
    touch(api, x + 22, y)
    touch(api, x + 43, y)  # now on "s"
    touch(api, x + 43, y, False)
    type_text('iqm')
    tap(api, *BKSP)  # the "m"
    tap(api, *BKSP)  # the "q"
    type_text('m')
    tap(api, *DONE)
    wait_line(api, seq, r'setup join "Home": result 0', timeout=10)
    wait(lambda: ui_mode(api) == 'wifi', timeout=8, what='back to the Wi-Fi screen')

    # The network is saved; holding its row's screen for 2 s forgets it.
    seq = console_seq(api)
    post(f'{api}/api/console', {'line': 'wifi'})
    wait_line(api, seq, r'network 0: "Home", connected')
    tap(api, 233, 176)
    wait(lambda: ui_mode(api) == 'netforget', what='forget screen')
    post(f'{api}/api/input', {'touch': {'pressed': True, 'x': 233, 'y': 300}})
    time.sleep(2.4)
    post(f'{api}/api/input', {'touch': {'pressed': False, 'x': 233, 'y': 300}})
    wait(lambda: ui_mode(api) == 'wifi', timeout=5, what='Wi-Fi screen after forgetting')
    seq = console_seq(api)
    post(f'{api}/api/console', {'line': 'wifi'})
    wait_line(api, seq, r'0 networks stored')
