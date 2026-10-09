'use strict';
// coinsim browser UI. Protocol: see coinhost/web.py.

const $ = (id) => document.getElementById(id);
const PANEL = 466;
let ws = null;
let S = null; // last state

// ------------------------------------------------------------------ small DOM helpers
const cache = new WeakMap();
function setText(el, v) { v = String(v); if (cache.get(el) !== v) { cache.set(el, v); el.textContent = v; } }
function setAttr(el, k, v) { v = String(v); if (el.getAttribute(k) !== v) el.setAttribute(k, v); }
function show(el, on) { if (el.hidden === on) el.hidden = !on; }
function fmt(v, d = 0, unit = '') { return v === null || v === undefined ? '–' : Number(v).toFixed(d) + unit; }
function rangeFill(el) { const p = (el.value - el.min) / (el.max - el.min) * 100; el.style.setProperty('--p', p + '%'); }
function hex2(n) { return n === null || n === undefined ? '–' : '0x' + n.toString(16).padStart(2, '0'); }
let editing = null; // an input the user is dragging; state updates do not fight it
function track(el) {
  el.addEventListener('pointerdown', () => { editing = el; });
  el.addEventListener('pointerup', () => { setTimeout(() => { if (editing === el) editing = null; }, 400); });
  el.addEventListener('blur', () => { if (editing === el) editing = null; });
}

// ------------------------------------------------------------------ sending
function send(body) {
  if (ws && ws.readyState === 1) ws.send(JSON.stringify({ t: 'input', ...body }));
  else fetch('/api/input', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
}

// ------------------------------------------------------------------ display
const canvas = $('screen');
const ctx2d = canvas.getContext('2d', { alpha: false });
ctx2d.fillStyle = '#000'; ctx2d.fillRect(0, 0, PANEL, PANEL);
const LUT = new Uint32Array(65536);
(function buildLut() {
  const le = new Uint8Array(new Uint32Array([1]).buffer)[0] === 1;
  for (let v = 0; v < 65536; v++) {
    const r = ((v >> 11) & 31) * 527 + 23 >> 6, g = ((v >> 5) & 63) * 259 + 33 >> 6, b = (v & 31) * 527 + 23 >> 6;
    LUT[v] = le ? (255 << 24 | b << 16 | g << 8 | r) >>> 0 : (r << 24 | g << 16 | b << 8 | 255) >>> 0;
  }
})();
function drawFrame(buf) {
  const dv = new DataView(buf);
  const x = dv.getUint16(1, true), y = dv.getUint16(3, true), w = dv.getUint16(5, true), h = dv.getUint16(7, true);
  if (!w || !h) return;
  const src = new Uint16Array(buf.slice(9, 9 + w * h * 2));
  const img = ctx2d.createImageData(w, h);
  const dst = new Uint32Array(img.data.buffer);
  for (let i = 0, n = w * h; i < n; i++) dst[i] = LUT[src[i]];
  ctx2d.putImageData(img, x, y);
}

// touch: inside the round panel only, ~60 Hz
let touching = false, lastTouchSent = 0, touchPending = null;
function panelXY(e) {
  const r = canvas.getBoundingClientRect();
  return [Math.round((e.clientX - r.left) / r.width * PANEL - 0.5), Math.round((e.clientY - r.top) / r.height * PANEL - 0.5)];
}
function inside([x, y]) { const dx = x - 232.5, dy = y - 232.5; return dx * dx + dy * dy <= 233 * 233; }
function sendTouch(pressed, xy) {
  send({ touch: { pressed, x: Math.max(0, Math.min(465, xy[0])), y: Math.max(0, Math.min(465, xy[1])) } });
  lastTouchSent = performance.now();
  const dot = $('touch-dot');
  dot.classList.toggle('on', pressed);
  if (pressed) { const r = canvas.getBoundingClientRect(); dot.style.left = (xy[0] + .5) / PANEL * r.width + 'px'; dot.style.top = (xy[1] + .5) / PANEL * r.height + 'px'; }
}
canvas.addEventListener('pointerdown', (e) => {
  const xy = panelXY(e);
  if (!inside(xy)) return;
  canvas.setPointerCapture(e.pointerId);
  touching = xy; sendTouch(true, xy);
});
canvas.addEventListener('pointermove', (e) => {
  if (!touching) return;
  const xy = panelXY(e);
  if (!inside(xy)) { touching = false; sendTouch(false, xy); return; }
  touching = xy;
  const due = 16 - (performance.now() - lastTouchSent);
  if (due <= 0) sendTouch(true, xy);
  else if (!touchPending) touchPending = setTimeout(() => { touchPending = null; if (touching) sendTouch(true, touching); }, due);
});
function endTouch(e) { if (!touching) return; const xy = touching; touching = false; sendTouch(false, xy); }
canvas.addEventListener('pointerup', endTouch);
canvas.addEventListener('pointercancel', endTouch);

// zoom
document.querySelectorAll('.zoom button').forEach((b) => b.addEventListener('click', () => {
  document.querySelectorAll('.zoom button').forEach((o) => setAttr(o, 'aria-pressed', o === b));
  document.documentElement.style.setProperty('--z', b.dataset.zoom);
  localStorage.setItem('coinsim.zoom', b.dataset.zoom);
}));
{ const z = localStorage.getItem('coinsim.zoom'); const b = z && document.querySelector(`.zoom button[data-zoom="${z}"]`); if (b) b.click(); }

// ------------------------------------------------------------------ buttons
const held = { pwr: false, boot: false };
function press(name, down) {
  if (held[name] === down) return;
  held[name] = down;
  $('btn-' + name).classList.toggle('down', down);
  send({ [name]: down });
}
for (const name of ['pwr', 'boot']) {
  const el = $('btn-' + name);
  el.addEventListener('pointerdown', (e) => { el.setPointerCapture(e.pointerId); press(name, true); e.preventDefault(); });
  el.addEventListener('pointerup', () => press(name, false));
  el.addEventListener('pointercancel', () => press(name, false));
  el.addEventListener('lostpointercapture', () => press(name, false));
  el.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); if (!e.repeat) press(name, true); } });
  el.addEventListener('keyup', (e) => { if (e.key === 'Enter') press(name, false); });
}
function typing(e) { const t = e.target; return t.closest && t.closest('input, textarea, select, [contenteditable]'); }
window.addEventListener('keydown', (e) => {
  if (typing(e) || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.code === 'Space') { e.preventDefault(); if (!e.repeat) press('pwr', true); }
  else if (e.code === 'KeyB') { e.preventDefault(); if (!e.repeat) press('boot', true); }
});
window.addEventListener('keyup', (e) => {
  if (e.code === 'Space') { if (held.pwr) { e.preventDefault(); press('pwr', false); } }
  else if (e.code === 'KeyB') press('boot', false);
});
window.addEventListener('blur', () => { press('pwr', false); press('boot', false); });
// A mouse click leaves focus on the control; drop it so Space means PWR again (keyboard focus is kept).
document.addEventListener('pointerup', (e) => {
  if (e.pointerType && e.target.closest) { const c = e.target.closest('button'); if (c && document.activeElement === c) c.blur(); }
});
$('cut-power').addEventListener('click', () => send({ cut_power: true }));

// ------------------------------------------------------------------ battery
function switchEl(el, onChange) {
  el.addEventListener('click', () => { const v = el.getAttribute('aria-checked') !== 'true'; setAttr(el, 'aria-checked', v); onChange(v); });
}
switchEl($('sw-battery'), (v) => send({ battery: { present: v } }));
switchEl($('sw-usb'), (v) => send({ usb: v }));
const batV = $('bat-volts'); track(batV);
batV.addEventListener('input', () => { rangeFill(batV); send({ battery: { volts: Number(batV.value) } }); });
function segEl(el, onPick) {
  el.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
    el.querySelectorAll('button').forEach((o) => setAttr(o, 'aria-pressed', o === b));
    onPick(b.dataset.v);
  }));
}
function segSet(el, v) { el.querySelectorAll('button').forEach((o) => setAttr(o, 'aria-pressed', o.dataset.v === String(v))); }
segEl($('sim-speed'), (v) => send({ sim: { speed: Number(v) } }));

// ------------------------------------------------------------------ motion
const joy = $('joy'), knob = $('joy-knob');
let joyActive = false, joyLast = 0, joyXY = [0, 0], joyTimer = null;
function joySend() { joyLast = performance.now(); send({ imu: { stick: joyXY } }); }
function joyMove(e) {
  const r = joy.getBoundingClientRect(), R = r.width / 2 - 24;
  let dx = e.clientX - (r.left + r.width / 2), dy = e.clientY - (r.top + r.height / 2);
  const d = Math.hypot(dx, dy); if (d > R) { dx *= R / d; dy *= R / d; }
  knob.style.transform = `translate(${dx}px, ${dy}px)`;
  joyXY = [dx / R, dy / R];
  const due = 33 - (performance.now() - joyLast);
  if (due <= 0) joySend(); else if (!joyTimer) joyTimer = setTimeout(() => { joyTimer = null; joySend(); }, due);
}
joy.addEventListener('pointerdown', (e) => { joy.setPointerCapture(e.pointerId); joyActive = true; knob.classList.remove('spring'); joyMove(e); });
joy.addEventListener('pointermove', (e) => { if (joyActive) joyMove(e); });
function joyRelease() {
  if (!joyActive) return; joyActive = false;
  knob.classList.add('spring'); knob.style.transform = 'translate(0px, 0px)';
  clearTimeout(joyTimer); joyTimer = null; joyXY = [0, 0]; joySend();
}
joy.addEventListener('pointerup', joyRelease);
joy.addEventListener('pointercancel', joyRelease);
joy.addEventListener('keydown', (e) => {
  const step = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[e.key];
  if (!step) return; e.preventDefault();
  joyXY = [Math.max(-1, Math.min(1, joyXY[0] + step[0] * .25)), Math.max(-1, Math.min(1, joyXY[1] + step[1] * .25))];
  const R = joy.clientWidth / 2 - 24; knob.classList.add('spring');
  knob.style.transform = `translate(${joyXY[0] * R}px, ${joyXY[1] * R}px)`; joySend();
});
const yaw = $('yaw'); track(yaw); rangeFill(yaw);
yaw.addEventListener('input', () => { rangeFill(yaw); setText($('yaw-out'), yaw.value + ' °/s'); send({ imu: { yaw_rate: Number(yaw.value) } }); });
yaw.addEventListener('dblclick', () => { yaw.value = 0; yaw.dispatchEvent(new Event('input')); });
$('tap').addEventListener('click', () => send({ imu: { tap: true } }));
$('shake').addEventListener('click', () => send({ imu: { shake: true } }));
$('level').addEventListener('click', () => {
  joyXY = [0, 0]; knob.classList.add('spring'); knob.style.transform = 'translate(0px, 0px)';
  yaw.value = 0; rangeFill(yaw); setText($('yaw-out'), '0 °/s');
  send({ imu: { stick: [0, 0], yaw_rate: 0 } });
});

// ------------------------------------------------------------------ audio
let actx = null, gain = null, playAt = 0, micNode = null, micStream = null;
function audioCtx() {
  if (!actx) {
    actx = new AudioContext();
    gain = actx.createGain(); gain.connect(actx.destination);
    gain.gain.value = Number($('vol').value);
  }
  if (actx.state === 'suspended') actx.resume().catch(() => {});
  return actx;
}
function updateAudioEnable() { show($('audio-enable'), !!actx && actx.state === 'suspended'); }
document.addEventListener('pointerdown', () => { if (actx && actx.state === 'suspended') actx.resume().then(updateAudioEnable); }, { capture: true });
$('audio-enable').addEventListener('click', () => { audioCtx(); setTimeout(updateAudioEnable, 100); });
function playPcm(buf) {
  const a = audioCtx(); updateAudioEnable();
  const pcm = new Int16Array(buf.slice(1));
  if (!pcm.length) return;
  const ab = a.createBuffer(1, pcm.length, 16000);
  const ch = ab.getChannelData(0);
  for (let i = 0; i < pcm.length; i++) ch[i] = pcm[i] / 32768;
  const src = a.createBufferSource(); src.buffer = ab; src.connect(gain);
  const now = a.currentTime;
  if (playAt < now + 0.03 || playAt > now + 1.0) playAt = now + 0.06;
  src.start(playAt); playAt += ab.duration;
}
const vol = $('vol'); track(vol); rangeFill(vol);
vol.addEventListener('input', () => { rangeFill(vol); if (gain) gain.gain.value = Number(vol.value); setText($('vol-out'), Math.round(vol.value * 100) + ' %'); send({ speaker: { volume: Number(vol.value) } }); });
switchEl($('sw-mute'), (v) => send({ speaker: { muted: v } }));

async function startBrowserMic() {
  const note = $('browser-mic-note');
  try {
    const a = audioCtx();
    micStream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 1 } });
    await a.audioWorklet.addModule('/static/mic-worklet.js');
    const src = a.createMediaStreamSource(micStream);
    micNode = new AudioWorkletNode(a, 'mic-down');
    micNode.port.onmessage = (ev) => {
      if (!ws || ws.readyState !== 1) return;
      const pcm = ev.data; const out = new Uint8Array(1 + pcm.byteLength);
      out[0] = 0x10; out.set(new Uint8Array(pcm.buffer), 1); ws.send(out);
    };
    src.connect(micNode);
    const mute = a.createGain(); mute.gain.value = 0; micNode.connect(mute); mute.connect(a.destination);
    note.textContent = `Streaming this browser's microphone (${a.sampleRate} Hz → 16 kHz).`; show(note, true);
  } catch (err) {
    note.textContent = 'Microphone unavailable: ' + err.message + '. Allow microphone access for 127.0.0.1 and pick the source again.';
    show(note, true);
  }
}
function stopBrowserMic() {
  if (micNode) { micNode.disconnect(); micNode = null; }
  if (micStream) { micStream.getTracks().forEach((t) => t.stop()); micStream = null; }
}
segEl($('mic-src'), (v) => {
  if (v === 'browser') startBrowserMic(); else { stopBrowserMic(); show($('browser-mic-note'), false); }
  $('file-row').dataset.keep = v === 'file' ? '1' : '';
  if (v === 'file') {
    const f = $('file-sel').value;
    if (!f) { show($('file-row'), true); return; }
    send({ mic: { source: 'file', file: f, loop: $('file-loop').checked } });
  } else send({ mic: { source: v, tone_hz: Number($('tone-hz').value), tone_dbfs: Number($('tone-db').value) } });
});
$('tone-hz').addEventListener('change', () => send({ mic: { source: 'tone', tone_hz: Number($('tone-hz').value) } }));
$('tone-db').addEventListener('change', () => send({ mic: { source: 'tone', tone_dbfs: Number($('tone-db').value) } }));
$('file-sel').addEventListener('change', () => { segSet($('mic-src'), 'file'); send({ mic: { source: 'file', file: $('file-sel').value, loop: $('file-loop').checked } }); });
$('file-loop').addEventListener('change', () => send({ mic: { source: S ? S.audio.source : 'file', loop: $('file-loop').checked } }));
async function loadFiles(select) {
  const r = await fetch('/api/audio_files'); const { files } = await r.json();
  const sel = $('file-sel'); const cur = select || sel.value || (S && S.audio.file);
  sel.innerHTML = '';
  if (!files.length) sel.append(new Option('no audio files', ''));
  for (const f of files) sel.append(new Option(`${f.name} (${(f.size / 1024).toFixed(0)} kB)`, f.name));
  if (cur && files.some((f) => f.name === cur)) sel.value = cur;
}
$('file-up').addEventListener('change', async () => {
  const f = $('file-up').files[0]; if (!f) return;
  const fd = new FormData(); fd.append('file', f, f.name);
  const r = await fetch('/api/upload', { method: 'POST', body: fd }); const j = await r.json();
  if (j.ok && j.saved.length) { await loadFiles(j.saved[0].name); $('file-sel').dispatchEvent(new Event('change')); }
  $('file-up').value = '';
});

// ------------------------------------------------------------------ bluetooth
const rssi = $('rssi'); track(rssi); rangeFill(rssi);
rssi.addEventListener('input', () => { rangeFill(rssi); setText($('rssi-out'), rssi.value + ' dBm'); send({ ble: { rssi: Number(rssi.value) } }); });
switchEl($('sw-oor'), (v) => send({ ble: { out_of_range: v } }));
$('ble-forget').addEventListener('click', () => send({ ble: { forget: true } }));
const blelog = $('blelog'); const logItems = new Map();
const KIND = { rx: '→', tx: '←', data: '⇠', event: '·', radio: '◦' };
function addBleLog(entries) {
  const atBottom = blelog.scrollHeight - blelog.scrollTop - blelog.clientHeight < 30;
  for (const e of entries) {
    let li = logItems.get(e.id);
    if (!li) {
      li = document.createElement('li'); li.dataset.k = e.kind;
      li.innerHTML = '<time></time><span class="k"></span><span class="t"></span>';
      li.title = { rx: 'phone → coin (ctrl write)', tx: 'coin → phone (ctrl indication)', data: 'coin → phone (data notifications)', event: 'link event', radio: 'radio' }[e.kind] || '';
      blelog.append(li); logItems.set(e.id, li);
      while (logItems.size > 600) { const [k, old] = logItems.entries().next().value; old.remove(); logItems.delete(k); }
    }
    li.children[0].textContent = e.t; li.children[1].textContent = KIND[e.kind] || '·';
    li.children[2].textContent = e.text + (e.len !== undefined && e.kind !== 'data' ? `  [${e.len} B]` : '');
  }
  if (atBottom) blelog.scrollTop = blelog.scrollHeight;
}
$('log-clear').addEventListener('click', () => { blelog.innerHTML = ''; logItems.clear(); });
$('log-data').addEventListener('change', () => blelog.classList.toggle('nodata', !$('log-data').checked));

// ------------------------------------------------------------------ console
const con = $('console');
let conLines = 0;
const ANSI = /\x1b\[([0-9;]*)m/g;
function ansiFrag(text) {
  const frag = document.createDocumentFragment();
  let cls = [], last = 0, m;
  const push = (s) => { if (!s) return; if (cls.length) { const sp = document.createElement('span'); sp.className = cls.join(' '); sp.textContent = s; frag.append(sp); } else frag.append(s); };
  ANSI.lastIndex = 0;
  while ((m = ANSI.exec(text))) {
    push(text.slice(last, m.index)); last = ANSI.lastIndex;
    for (const c of (m[1] || '0').split(';').map(Number)) {
      if (c === 0) cls = [];
      else if (c === 1) cls.push('a-b');
      else if (c === 2) cls.push('a-d');
      else if ((c >= 30 && c <= 37) || (c >= 90 && c <= 97)) { cls = cls.filter((x) => !/^a-[39]\d$/.test(x)); cls.push('a-' + c); }
      else if (c === 39) cls = cls.filter((x) => !/^a-[39]\d$/.test(x));
    }
  }
  push(text.slice(last).replace(/\x1b\[[0-9;]*[A-Za-z]/g, ''));
  return frag;
}
function addConsole(entries) {
  const atBottom = con.scrollHeight - con.scrollTop - con.clientHeight < 30;
  const frag = document.createDocumentFragment();
  for (const e of entries) {
    const div = document.createElement('div');
    if (e.kind !== 'out') div.className = e.kind;
    if (e.kind === 'in') div.textContent = '› ' + e.text; else div.append(ansiFrag(e.text || ' '));
    frag.append(div); conLines++;
  }
  con.append(frag);
  while (conLines > 4000) { con.firstChild.remove(); conLines--; }
  if (atBottom) con.scrollTop = con.scrollHeight;
}
$('con-clear').addEventListener('click', () => { con.innerHTML = ''; conLines = 0; });
const hist = JSON.parse(localStorage.getItem('coinsim.history') || '[]'); let hpos = hist.length;
$('con-form').addEventListener('submit', (e) => {
  e.preventDefault();
  const inp = $('con-input'); const line = inp.value;
  if (ws && ws.readyState === 1) ws.send(JSON.stringify({ t: 'console', line }));
  if (line && hist[hist.length - 1] !== line) { hist.push(line); if (hist.length > 200) hist.shift(); localStorage.setItem('coinsim.history', JSON.stringify(hist)); }
  hpos = hist.length; inp.value = '';
});
$('con-input').addEventListener('keydown', (e) => {
  if (e.key === 'ArrowUp' && hpos > 0) { hpos--; e.target.value = hist[hpos]; e.preventDefault(); }
  else if (e.key === 'ArrowDown') { hpos = Math.min(hist.length, hpos + 1); e.target.value = hist[hpos] || ''; e.preventDefault(); }
});

// ------------------------------------------------------------------ render state
const STATE_NAMES = { off: 'Off', booting: 'Booting', active: 'Active', standby: 'Standby', error: 'Error' };
function dur(s) {
  if (s === null || s === undefined) return '–';
  if (s < 60) return s.toFixed(1) + ' s';
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), ss = Math.floor(s % 60);
  return (h ? h + ' h ' : '') + m + ' min ' + ss + ' s';
}
function render(s) {
  S = s;
  const p = s.power;
  // header
  const tone = { active: 'ok', booting: 'warn', standby: 'blue', off: 'off', error: 'bad' }[p.state];
  setAttr($('pill-power'), 'data-tone', tone); setText($('pill-power-text'), STATE_NAMES[p.state] || p.state);
  const b = s.ble;
  const bleTone = !b.enabled ? 'off' : b.status !== 'up' ? 'bad' : b.connected ? 'ok' : b.advertising ? 'blue' : 'warn';
  setAttr($('pill-ble'), 'data-tone', bleTone);
  setText($('pill-ble-text'), !b.enabled ? 'Bluetooth off' : b.status !== 'up' ? 'No radio' : b.connected ? 'Connected' : b.advertising ? 'Advertising' : 'Radio idle');
  // signals
  for (const el of document.querySelectorAll('.sig')) {
    const v = s.pins[el.dataset.sig]; const lvl = el.querySelector('.lvl');
    setText(lvl, v === null || v === undefined ? '–' : v ? 'H' : 'L'); setAttr(lvl, 'data-v', v === null || v === undefined ? '' : v);
  }
  // power
  const ps = $('power-state'); setAttr(ps, 'data-s', p.state);
  let label = STATE_NAMES[p.state] || p.state;
  if (p.state === 'off' && p.off_reason && p.off_reason !== 'initial') label += ' · ' + p.off_reason;
  setText(ps, label);
  setText($('pw-reset'), p.reset_reason ? `${p.reset_reason} (${p.reset_name})${p.wake ? ', wake 0x' + p.wake.toString(16) : ''}` : '–');
  setText($('pw-boots'), p.boot_count);
  setText($('pw-uptime'), dur(p.uptime_s));
  setText($('pw-rtc'), dur(p.rtc_s));
  setText($('pw-fw'), p.fw_version || '–');
  setText($('pw-proc'), p.pid ? `pid ${p.pid}, IPC ${p.ipc_connected ? 'connected' : 'waiting'}` : '–');
  show($('pw-error'), !!p.error); if (p.error) setText($('pw-error'), p.error);
  const sb = p.standby; show($('standby'), !!sb);
  if (sb) {
    const t = sb.timer_s ? `wakes in ${sb.remaining_s.toFixed(1)} s (timer ${sb.timer_s} s)` : 'no wake timer';
    setText($('standby-text'), `${t}; ${sb.pwr_wakes ? 'PWR wakes it' : 'ext1 mask 0x' + sb.ext1_mask.toString(16) + ' ' + sb.ext1_mode}; RTC memory ${sb.rtc_bytes} B`);
    $('standby-bar').style.width = sb.timer_s ? (100 * (1 - sb.remaining_s / sb.timer_s)).toFixed(1) + '%' : '0';
  }
  // panel
  const on = s.panel.on !== false && p.state !== 'off';
  $('screen-off').style.opacity = on ? Math.max(0, 1 - Math.max(s.panel.brightness, 4) / 100).toFixed(2) : '1';
  if (!s.touch.pressed && !touching) $('touch-dot').classList.remove('on');
  // battery
  const m = s.pmic;
  setAttr($('sw-battery'), 'aria-checked', m.battery_present); setAttr($('sw-usb'), 'aria-checked', m.usb);
  const chip = $('chg-chip');
  setText(chip, m.charging ? 'charging · ' + m.charger_state : m.usb ? (m.battery_present ? m.charger_state : 'USB, no battery') : m.battery_present ? 'on battery' : 'no power');
  setAttr(chip, 'data-tone', m.charging ? 'warn' : m.power_ok ? 'ok' : 'bad');
  if (editing !== batV) { batV.value = (m.vbat_mv || 2900) / 1000; rangeFill(batV); }
  batV.disabled = !m.battery_present;
  setText($('bat-out'), m.battery_present ? `${(m.vbat_mv / 1000).toFixed(2)} V · ${m.battery_pct.toFixed(m.battery_pct < 10 ? 1 : 0)} %` : 'no battery');
  segSet($('sim-speed'), s.sim.speed);
  setText($('pm-vbat'), m.vbat_mv + ' mV'); setText($('pm-vbus'), m.vbus_mv + ' mV'); setText($('pm-vsys'), m.vsys_mv + ' mV');
  setText($('pm-temp'), m.die_temp_c.toFixed(1) + ' °C'); setText($('pm-status'), hex2(m.status1) + ' ' + hex2(m.status2));
  setText($('pm-pct'), m.percent_reg + ' %');
  setText($('pm-dcdc1'), `${m.dcdc1_on ? 'on' : 'off'} ${m.dcdc1_mv} mV`); setText($('pm-aldo1'), `${m.aldo1_on ? 'on' : 'off'} ${m.aldo1_mv} mV`);
  setText($('pm-icc'), m.charge_current_ma + ' mA');
  setText($('pm-key'), `on ${m.on_time_ms} ms · off ${m.off_time_s} s${m.long_press_off ? '' : ' (disabled)'}`);
  // motion
  const mo = s.imu.motion, rg = s.imu.regs;
  setText($('tilt'), `pitch ${mo.pitch.toFixed(0)}° · roll ${mo.roll.toFixed(0)}°`);
  if (editing !== yaw && Number(yaw.value) !== mo.yaw_rate) { yaw.value = mo.yaw_rate; rangeFill(yaw); setText($('yaw-out'), mo.yaw_rate + ' °/s'); }
  setText($('imu-acc-state'), rg.accel_on ? '' : 'off'); setText($('imu-gyr-state'), rg.gyro_on ? '' : 'off');
  for (let i = 0; i < 3; i++) {
    axis('a' + i, rg.accel_g[i], 2, 3, ' g');
    axis('g' + i, rg.gyro_dps[i], 500, 1, '');
  }
  // audio
  const au = s.audio;
  segSet($('mic-src'), au.source);
  show($('tone-row'), au.source === 'tone'); show($('file-row'), au.source === 'file' || $('file-row').dataset.keep === '1');
  if (document.activeElement !== $('tone-hz')) $('tone-hz').value = au.tone_hz;
  if (document.activeElement !== $('tone-db')) $('tone-db').value = au.tone_dbfs;
  $('file-loop').checked = au.loop;
  $('mic-lamp').classList.toggle('on', au.mic_open);
  meter('mic', au.mic_open ? au.mic_level_dbfs : null);
  $('amp-lamp').classList.toggle('on', au.amp);
  meter('spk', au.amp ? au.speaker_level_dbfs : null);
  setAttr($('sw-mute'), 'aria-checked', au.speaker_muted);
  if (editing !== vol) { vol.value = au.speaker_volume; rangeFill(vol); setText($('vol-out'), Math.round(au.speaker_volume * 100) + ' %'); if (gain) gain.gain.value = au.speaker_volume; }
  // bluetooth
  const rl = $('radio-lamp'); rl.className = 'lamp ' + (b.status === 'up' ? 'on' : b.enabled ? 'bad' : '');
  let rt;
  if (!b.enabled) rt = 'Bluetooth disabled (--ble none)';
  else if (b.status === 'up') rt = `Radio up via ${b.transport}`;
  else if (b.status === 'connecting') rt = `Connecting to ${b.transport}…`;
  else rt = `No radio (${b.transport}): ${b.error || 'unreachable'}${b.retry_in_s !== null ? ` · retry in ${b.retry_in_s.toFixed(0)} s` : ''}`;
  setText($('radio-text'), rt);
  setText($('ble-addr'), `${b.address} (${b.address_type})${b.tx_power_dbm !== null ? ` · TX ${b.tx_power_dbm} dBm` : ''}`);
  setText($('ble-adv'), b.advertising ? `on · ${b.adv_interval_ms} ms${b.adv_remaining_ms !== null ? ` · ${(b.adv_remaining_ms / 1000).toFixed(1)} s left` : ''}` : b.adv_requested ? (b.out_of_range ? 'requested (out of range)' : 'requested (no radio)') : 'off');
  setText($('ble-conn'), b.connected ? `${b.peer} · MTU ${b.mtu} · credits out ${b.credits_in_flight}` : 'none');
  setText($('ble-sec'), b.connected ? `${b.encrypted ? 'encrypted' : 'not encrypted'}${b.authenticated ? ', authenticated' : ''}${b.bonded ? ', bonded' : ''}${b.pairing ? ' · pairing…' : ''}` : '–');
  setText($('ble-sub'), b.connected ? `ctrl ${b.subscribed_ctrl ? 'indicate' : '–'} · data ${b.subscribed_data ? 'notify' : '–'}` : '–');
  setText($('ble-bonds'), b.bonds);
  show($('passkey'), !!b.passkey); if (b.passkey) setText($('passkey-num'), b.passkey);
  if (editing !== rssi) { rssi.value = b.rssi; rangeFill(rssi); setText($('rssi-out'), b.rssi + ' dBm'); }
  setAttr($('sw-oor'), 'aria-checked', b.out_of_range);
}
function axis(id, v, full, d, unit) {
  const el = document.querySelector(`.axis[data-ax="${id}"]`); const f = el.querySelector('.af');
  const p = Math.max(-1, Math.min(1, v / full)) * 50;
  f.style.left = (p < 0 ? 50 + p : 50) + '%'; f.style.width = Math.abs(p) + '%';
  setText(el.querySelector('span'), v.toFixed(d) + unit);
}
function meter(id, db) {
  const pct = db === null ? 0 : Math.max(0, Math.min(100, (db + 80) / 80 * 100));
  const el = $(id + '-meter'); el.style.width = pct + '%'; el.style.setProperty('--mw', pct > 0 ? (10000 / pct) + '%' : '100%');
  setText($(id + '-db'), db === null ? '–' : db.toFixed(0) + ' dBFS');
}

// ------------------------------------------------------------------ websocket
let wsRetry = 500;
function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { wsRetry = 500; setAttr($('pill-ws'), 'data-tone', 'ok'); setText($('pill-ws-text'), 'host connected'); con.innerHTML = ''; conLines = 0; blelog.innerHTML = ''; logItems.clear(); };
  ws.onclose = () => {
    setAttr($('pill-ws'), 'data-tone', 'bad'); setText($('pill-ws-text'), 'host offline · reconnecting');
    setTimeout(connect, wsRetry); wsRetry = Math.min(5000, wsRetry * 2);
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data !== 'string') {
      const t = new Uint8Array(ev.data, 0, 1)[0];
      if (t === 1) drawFrame(ev.data);
      else if (t === 2) { if (!S || !S.audio.speaker_muted) playPcm(ev.data); }
      return;
    }
    const msg = JSON.parse(ev.data);
    if (msg.t === 'state') render(msg);
    else if (msg.t === 'console') addConsole(msg.entries);
    else if (msg.t === 'blelog') addBleLog(msg.entries);
    else if (msg.t === 'error') addConsole([{ kind: 'sys', text: '(UI input error: ' + msg.error + ')' }]);
  };
}
connect();
loadFiles();
