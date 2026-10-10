"""Browser UI (aiohttp), the WebSocket that drives it, and the JSON automation API.

WebSocket /ws
  server -> browser, text JSON:  {"t":"state", ...Sim.snapshot()}  (<= 15 Hz, when something changed)
                                 {"t":"console", "entries":[{seq, kind: out|in|sys, text}]}
                                 {"t":"blelog", "entries":[{id, t, kind: rx|tx|event|radio|data, text}]}
                                 (an entry whose id is already shown replaces it: data runs update in place)
  server -> browser, binary:     0x01 FRAME  x u16, y u16, w u16, h u16, RGB565 LE pixels (row-major)
                                 0x02 SPEAKER  s16 mono 16 kHz (only while GPIO46 = 1 and not muted)
  browser -> server, text JSON:  {"t":"input", ...same body as POST /api/input}
                                 {"t":"console", "line": "..."}
  browser -> server, binary:     0x10 MIC  s16 mono 16 kHz from getUserMedia
"""

from __future__ import annotations

import asyncio
import collections
import json
import logging
import pathlib
import struct
import time
import zlib

import numpy as np
from aiohttp import WSMsgType, web

from . import ipc
from .supervisor import Sim, strip_ansi

log = logging.getLogger('coinsim.web')
WEB_DIR = pathlib.Path(__file__).resolve().parent.parent / 'web'
STATE_HZ = 15
MAX_UPLOAD = 50 * 1024 * 1024


def png_bytes(rgba: np.ndarray) -> bytes:
    h, w, _ = rgba.shape
    raw = b''.join(b'\x00' + rgba[y].tobytes() for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


class Client:
    def __init__(self, ws: web.WebSocketResponse) -> None:
        self.ws = ws
        self.wake = asyncio.Event()
        self.rect: list[int] | None = None  # x0, y0, x1, y1 (exclusive)
        self.console: list[dict] = []
        self.blelog: dict[int, dict] = {}
        self.audio: collections.deque = collections.deque(maxlen=50)
        self.state_dirty = True

    def add_rect(self, x: int, y: int, w: int, h: int) -> None:
        if self.rect is None:
            self.rect = [x, y, x + w, y + h]
        else:
            r = self.rect
            r[0], r[1], r[2], r[3] = min(r[0], x), min(r[1], y), max(r[2], x + w), max(r[3], y + h)
        self.wake.set()


class Hub:
    def __init__(self, sim: Sim) -> None:
        self.sim = sim
        self.clients: set[Client] = set()
        sim.hub = self

    # ------------------------------------------------------------ called by Sim
    def frame(self, x: int, y: int, w: int, h: int) -> None:
        for c in self.clients:
            c.add_rect(x, y, w, h)

    def console(self, entry: dict) -> None:
        for c in self.clients:
            c.console.append(entry)
            if len(c.console) > 2000:
                del c.console[:500]
            c.wake.set()

    def ble_log(self, entry: dict) -> None:
        for c in self.clients:
            c.blelog[entry['id']] = dict(entry)
            c.wake.set()

    def speaker(self, pcm: bytes) -> None:
        for c in self.clients:
            c.audio.append(b'\x02' + pcm)
            c.wake.set()

    # ------------------------------------------------------------ loops
    async def state_loop(self) -> None:
        while True:
            await self.sim.dirty.wait()
            self.sim.dirty.clear()
            for c in self.clients:
                c.state_dirty = True
                c.wake.set()
            await asyncio.sleep(1 / STATE_HZ)

    async def sender(self, c: Client) -> None:
        ws = c.ws
        while not ws.closed:
            await c.wake.wait()
            c.wake.clear()
            if c.state_dirty:
                c.state_dirty = False
                await ws.send_str(json.dumps({'t': 'state', **self.sim.snapshot()}, separators=(',', ':')))
            if c.console:
                entries, c.console = c.console, []
                await ws.send_str(json.dumps({'t': 'console', 'entries': entries}))
            if c.blelog:
                entries, c.blelog = list(c.blelog.values()), {}
                await ws.send_str(json.dumps({'t': 'blelog', 'entries': entries}))
            while c.audio:
                await ws.send_bytes(c.audio.popleft())
            if c.rect is not None:
                x0, y0, x1, y1 = c.rect
                c.rect = None
                px = np.ascontiguousarray(self.sim.fb[y0:y1, x0:x1]).tobytes()
                await ws.send_bytes(b'\x01' + struct.pack('<HHHH', x0, y0, x1 - x0, y1 - y0) + px)


# ---------------------------------------------------------------- input handling
def _bool(v) -> bool:
    if isinstance(v, str):
        return v.lower() in ('1', 'true', 'on', 'yes', 'press', 'pressed', 'down')
    return bool(v)


async def apply_input(sim: Sim, body: dict) -> dict:
    if not isinstance(body, dict):
        raise ValueError('body must be a JSON object')
    waits = []
    for key, val in body.items():
        if key in ('t',):
            continue
        if key in ('pwr', 'boot'):
            sim.set_button(key, _bool(val))
        elif key == 'hold':
            for name, ms in val.items():
                waits.append(sim.hold_button(name, max(0, min(60000, int(ms)))))
        elif key == 'touch':
            sim.set_touch(_bool(val.get('pressed', True)), int(val.get('x', 0)), int(val.get('y', 0)))
        elif key == 'usb':
            await sim.set_power_inputs(usb=_bool(val))
        elif key == 'battery':
            if isinstance(val, dict):
                await sim.set_power_inputs(battery_present=val.get('present'), volts=val.get('volts'),
                                           pct=val.get('pct'))
            else:
                await sim.set_power_inputs(battery_present=_bool(val))
        elif key == 'cut_power':
            if _bool(val):
                await sim.cut_power()
        elif key == 'die_temp_c':
            sim.pmic.die_temp_c = float(val)
        elif key == 'imu':
            m = sim.motion
            if 'stick' in val:
                m.set_stick(float(val['stick'][0]), float(val['stick'][1]))
            if 'pitch' in val or 'roll' in val:
                m.set_tilt(float(val.get('pitch', m.target_pitch)), float(val.get('roll', m.target_roll)))
            if 'yaw_rate' in val:
                m.yaw_rate = max(-500.0, min(500.0, float(val['yaw_rate'])))
            if _bool(val.get('tap', False)):
                m.tap()
            if _bool(val.get('shake', False)):
                m.shake()
        elif key == 'mic':
            await sim.select_mic(val.get('source', sim.mic.kind), val.get('file'), val.get('tone_hz'),
                                 val.get('tone_dbfs'), val.get('loop'))
        elif key == 'speaker':
            if 'muted' in val:
                sim.speaker_muted = _bool(val['muted'])
            if 'volume' in val:
                sim.speaker_volume = max(0.0, min(1.0, float(val['volume'])))
        elif key == 'ble':
            if 'rssi' in val:
                sim.ble.set_rssi(int(val['rssi']))
            if 'out_of_range' in val:
                sim.ble.set_out_of_range(_bool(val['out_of_range']))
            if _bool(val.get('forget', False)):
                await sim.ble.forget_bonds()
        elif key == 'sim':
            if 'speed' in val:
                sim.sim_speed = max(0.0, min(100000.0, float(val['speed'])))
            if 'capacity_mah' in val:
                sim.pmic.capacity_mah = max(10.0, float(val['capacity_mah']))
        else:
            raise ValueError(f'unknown input {key!r}')
    sim.mark_dirty()
    if waits:
        await asyncio.gather(*waits)
    return {'ok': True}


# ---------------------------------------------------------------- app
def make_app(sim: Sim) -> web.Application:
    hub = Hub(sim)
    app = web.Application(client_max_size=MAX_UPLOAD)

    # The page's scripts change with the simulator: revalidate them (ETag) instead of running a cached copy.
    async def no_cache_static(request, response):
        if request.path.startswith('/static/'):
            response.headers['Cache-Control'] = 'no-cache'

    app.on_response_prepare.append(no_cache_static)
    app['hub'] = hub

    async def on_startup(app):
        app['state_task'] = asyncio.create_task(hub.state_loop())

    async def on_cleanup(app):
        app['state_task'].cancel()
        for c in list(hub.clients):
            await c.ws.close()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)

    async def index(request):
        return web.FileResponse(WEB_DIR / 'index.html', headers={'Cache-Control': 'no-cache'})

    async def ws_handler(request):
        ws = web.WebSocketResponse(heartbeat=20, max_msg_size=4 * 1024 * 1024)
        await ws.prepare(request)
        c = Client(ws)
        c.add_rect(0, 0, ipc.PANEL_SIZE, ipc.PANEL_SIZE)
        c.console = list(sim.console)[-1500:]
        c.blelog = {e['id']: dict(e) for e in list(sim.ble_log)}
        hub.clients.add(c)
        sender = asyncio.create_task(hub.sender(c))
        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    try:
                        data = json.loads(msg.data)
                        if data.get('t') == 'input':
                            await apply_input(sim, data)
                        elif data.get('t') == 'console':
                            await sim.console_input(str(data.get('line', '')))
                    except Exception as e:  # noqa: BLE001
                        await ws.send_str(json.dumps({'t': 'error', 'error': str(e)}))
                elif msg.type == WSMsgType.BINARY:
                    if msg.data[:1] == b'\x10':
                        n = (len(msg.data) - 1) // 2
                        pcm = np.frombuffer(msg.data, dtype='<i2', count=n, offset=1).copy()
                        sim.mic.push_browser(pcm, time.monotonic())
        finally:
            hub.clients.discard(c)
            sender.cancel()
        return ws

    def json_error(e: Exception, status: int = 400):
        return web.json_response({'ok': False, 'error': str(e)}, status=status)

    async def api_state(request):
        snap = sim.snapshot()
        snap['console_seq'] = sim.console_seq
        snap['ble_log_seq'] = sim.ble_log_seq
        return web.json_response(snap)

    async def api_input(request):
        try:
            body = await request.json()
            return web.json_response(await apply_input(sim, body))
        except (ValueError, TypeError, KeyError, AttributeError) as e:
            return json_error(e)

    async def api_console_post(request):
        try:
            body = await request.json()
            line = body['line'] if isinstance(body, dict) else str(body)
        except Exception as e:  # noqa: BLE001
            return json_error(e)
        ok = await sim.console_input(str(line))
        return web.json_response({'ok': ok})

    async def api_console_get(request):
        since = int(request.query.get('since', 0))
        plain = request.query.get('plain', '1') != '0'
        entries = [dict(e, text=strip_ansi(e['text'])) if plain else e for e in sim.console if e['seq'] > since]
        return web.json_response({'seq': sim.console_seq, 'entries': entries})

    async def api_ble_log(request):
        since = int(request.query.get('since', 0))
        return web.json_response({'seq': sim.ble_log_seq, 'entries': [e for e in sim.ble_log if e['id'] > since]})

    async def api_screen(request):
        rgba = ipc.rgb565_to_rgba(sim.fb.tobytes(), ipc.PANEL_SIZE, ipc.PANEL_SIZE)
        if request.query.get('raw') != '1' and sim.panel_on is False:
            rgba[..., :3] = 0
        return web.Response(body=png_bytes(rgba), content_type='image/png')

    async def api_audio_files(request):
        return web.json_response({'files': sim.audio_files()})

    async def api_upload(request):
        reader = await request.multipart()
        saved = []
        async for part in reader:
            if part.filename is None:
                continue
            name = pathlib.Path(part.filename).name
            if not name or name.startswith('.') or pathlib.Path(name).suffix.lower() not in \
                    {'.ogg', '.opus', '.wav', '.mp3', '.flac', '.m4a', '.aac', '.webm'}:
                return json_error(ValueError(f'unsupported file {part.filename!r}'))
            dest = sim.uploads_dir / name
            size = 0
            with open(dest, 'wb') as f:
                while chunk := await part.read_chunk(65536):
                    size += len(chunk)
                    f.write(chunk)
            saved.append({'name': f'uploads/{name}', 'size': size})
        return web.json_response({'ok': True, 'saved': saved})

    app.router.add_get('/', index)
    app.router.add_get('/ws', ws_handler)
    app.router.add_get('/api/state', api_state)
    app.router.add_post('/api/input', api_input)
    app.router.add_post('/api/console', api_console_post)
    app.router.add_get('/api/console', api_console_get)
    app.router.add_get('/api/ble_log', api_ble_log)
    app.router.add_get('/api/screen.png', api_screen)
    app.router.add_get('/api/audio_files', api_audio_files)
    app.router.add_post('/api/upload', api_upload)
    app.router.add_static('/static/', WEB_DIR, append_version=False)
    return app
