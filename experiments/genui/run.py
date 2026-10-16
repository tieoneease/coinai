"""Generative UI, experiment 1: can a fast model fill the coin's card catalog well enough?

    python run.py compose <run dir> [model,model,...]  # ask the models from the SamBot VM (ssh host "sambot");
                                                       # GENUI_URL: an OpenAI-style chat completions URL the VM reaches
    python run.py render  <run dir>                    # show every scene on the simulator: verdict + screenshot
    python run.py report  <run dir>                    # report.html

compose runs on the VM because Groq is reached through the VM's exe.dev integration. The
models see catalog.md as the system prompt and one request from questions.txt at a time. render
needs the simulator running and SIMPORT set; the firmware lays each card out and reports what
did not fit (scene.c), so the verdicts are the coin's, not a re-implementation."""
import html
import json
import os
import pathlib
import re
import statistics
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
MODELS = ['qwen/qwen3.8-27b', 'openai/gpt-oss-120b', 'openai/gpt-oss-20b']
CONTEXT = 'Local time: Friday 16 October 2026, 10:45 PM.'

VM_SCRIPT = r'''
import json, time, urllib.request, urllib.error
SYSTEM = %(system)s
QUESTIONS = %(questions)s
MODELS = %(models)s
CONTEXT = %(context)s
EXTRA = {"openai/gpt-oss-20b": {"reasoning_effort": "low"}, "openai/gpt-oss-120b": {"reasoning_effort": "low"}}
URL = %(url)s

def ask(model, q):
    body = {"model": model, "temperature": 0.3, "max_completion_tokens": 700,
            "response_format": {"type": "json_object"},
            "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": CONTEXT + "\nHeard: " + q}]}
    body.update(EXTRA.get(model, {}))
    for attempt in range(5):
        req = urllib.request.Request(URL, json.dumps(body).encode(),
                                     {"Content-Type": "application/json", "User-Agent": "coinai-genui"})
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=40) as r:
                d = json.load(r)
            return {"latency": round(time.time() - t0, 3), "out": d["choices"][0]["message"]["content"],
                    "usage": d.get("usage", {})}
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(3 + attempt * 4)
                continue
            return {"latency": round(time.time() - t0, 3), "error": "HTTP %%d: %%s" %% (e.code, e.read()[:300].decode(errors="replace"))}
        except Exception as e:
            return {"latency": round(time.time() - t0, 3), "error": str(e)[:300]}
    return {"error": "rate limited"}

for model in MODELS:
    for i, (cls, q) in enumerate(QUESTIONS):
        r = ask(model, q)
        r.update(model=model, i=i, q=q, cls=cls)
        print(json.dumps(r), flush=True)
        time.sleep(0.4)
'''


def questions():
    out = []
    for line in (HERE / 'questions.txt').read_text(encoding='utf-8').splitlines():
        if line.strip() and not line.startswith('#'):
            cls, q = line.split('|', 1)
            out.append((cls, q))
    return out


def compose(run: pathlib.Path, models):
    run.mkdir(parents=True, exist_ok=True)
    script = VM_SCRIPT % {'system': json.dumps((HERE / 'catalog.md').read_text(encoding='utf-8')),
                          'questions': json.dumps(questions()), 'models': json.dumps(models),
                          'context': json.dumps(CONTEXT), 'url': json.dumps(os.environ['GENUI_URL'])}
    with open(run / 'raw.jsonl', 'w', encoding='utf-8') as f:
        p = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=12', 'sambot', 'python3 -'],
                           input=script, capture_output=True, text=True, timeout=1800)
        f.write(p.stdout)
    n = len(p.stdout.splitlines())
    print(f'{n} answers in {run / "raw.jsonl"}', p.stderr[-500:] if p.returncode else '')


# Claims of an action done, which this composer can't do: a card must never show one.
CLAIM = re.compile(r"\b(i('ve| have| just)? (sent|texted|messaged|emailed|called|booked|ordered|added|scheduled|"
                   r"reminded|turned|played|deleted|set (a|an|the|your) (reminder|alarm))|"
                   r"(message|text|email|reminder|order|booking|appointment) (sent|set|placed|added|booked|scheduled)|"
                   r"added to your|turned (off|on) the|now playing|deleted|"
                   # an action implied without a hand-off: only a later card reaches SamBot's tools
                   r"i('ll| will| can) (send|text|message|email|call|book|order|add|schedule|remind|turn|play|delete|draft)|"
                   r"coming (right )?up)\b", re.I)


def claim_of(r):
    """The first phrase of a shown scene that claims an action only SamBot could take."""
    if r.get('verdict') != 'shown' or r.get('kind') in ('later', 'timer'):
        return ''
    m = CLAIM.search(scene_text(r['scene']).replace('\u2019', "'"))
    return m.group(0) if m else ''


def scene_text(scene):
    parts = [scene.get('say', '')]
    for v in (scene.get('card') or {}).values():
        if isinstance(v, str):
            parts.append(v)
        elif isinstance(v, list):
            parts += [x for x in v if isinstance(x, str)]
    return ' | '.join(parts)


def render(run: pathlib.Path):
    sys.path.insert(0, str(HERE))
    from coinscene import Sim
    sim = Sim()
    shots = run / 'shots'
    shots.mkdir(exist_ok=True)
    rows = [json.loads(l) for l in (run / 'raw.jsonl').read_text(encoding='utf-8').splitlines() if l.strip()]
    with open(run / 'scenes.jsonl', 'w', encoding='utf-8') as f:
        for r in rows:
            r['verdict'] = 'error' if 'error' in r else None
            if not r['verdict']:
                try:
                    scene = json.loads(r['out'])
                    r['scene'] = scene
                except ValueError:
                    r['verdict'] = 'not JSON'
            if not r['verdict']:
                line = sim.show(json.dumps(scene))
                r['firmware'] = line
                m = re.match(r'SCENE (\w+) kind=(\S+) lines=\d+ issues=(\d+)(?:: (.*))?', line)
                r['verdict'] = 'shown' if m and m.group(1) == 'shown' else 'rejected'
                r['kind'] = m.group(2) if m else '?'
                r['issues'] = int(m.group(3)) if m else -1
                r['issue_text'] = (m.group(4) or '') if m else line
                time.sleep(1.5)
                name = f"{r['model'].split('/')[-1]}-{r['i']:02d}.png"
                sim.shot(shots / name)
                r['shot'] = f'shots/{name}'
            f.write(json.dumps(r) + '\n')
            print(r['model'].split('/')[-1], r['i'], r['verdict'], r.get('kind', ''), r.get('issue_text', '')[:80])


def expected_ok(r):
    """Did the model route the request the way its class asks?"""
    k, cls = r.get('kind'), r['cls']
    if cls in ('act', 'live'):
        return k in ('later', 'confirm')
    if cls == 'timer':
        return k == 'timer'
    if cls == 'local':
        return k not in ('later', None)
    return True  # vague: judged by eye


def report(run: pathlib.Path):
    rows = [json.loads(l) for l in (run / 'scenes.jsonl').read_text(encoding='utf-8').splitlines() if l.strip()]
    for r in rows:
        r['claim'] = claim_of(r)
    models = list(dict.fromkeys(r['model'] for r in rows))
    qs = questions()
    summary = []
    for m in models:
        rs = [r for r in rows if r['model'] == m]
        lat = sorted(r['latency'] for r in rs if 'latency' in r and 'error' not in r)
        shown = [r for r in rs if r['verdict'] == 'shown']
        clean = [r for r in shown if r['issues'] == 0]
        routed = [r for r in shown if r['cls'] != 'vague']
        summary.append({
            'model': m, 'n': len(rs), 'errors': sum(r['verdict'] in ('error', 'not JSON') for r in rs),
            'shown': len(shown), 'clean': len(clean),
            'routing': f"{sum(expected_ok(r) for r in routed)}/{len(routed)}",
            'claims': sum(bool(r.get('claim')) for r in rs),
            'p50': statistics.median(lat) if lat else 0, 'p90': lat[int(len(lat) * 0.9) - 1] if lat else 0,
            'kinds': ', '.join(f'{k} {n}' for k, n in sorted(
                {k: sum(r.get('kind') == k for r in shown) for k in {r.get('kind') for r in shown}}.items())),
        })
    by = {(r['model'], r['i']): r for r in rows}
    e = html.escape
    out = ['<!doctype html><meta charset=utf-8><title>coin scenes</title><style>',
           'body{background:#111;color:#ddd;font:14px system-ui,sans-serif;margin:24px}',
           'table{border-collapse:collapse}td,th{border:1px solid #333;padding:6px 8px;vertical-align:top}',
           'th{background:#1c1c1c;text-align:left}.q{max-width:180px;font-weight:600}',
           '.cell{width:250px}.cell img{width:233px;height:233px;border-radius:50%;display:block;margin-bottom:6px}',
           '.say{color:#f4ecdf;font-style:italic}.bad{color:#ff6b5e}.ok{color:#8fd18f}.dim{color:#888}',
           '.cls{font-size:12px;color:#ffb547}</style>',
           f'<h1>coin/1 scenes: {len(qs)} requests x {len(models)} models</h1>',
           f'<p class=dim>System prompt: catalog.md. Context: "{e(CONTEXT)}". Latency is VM to Groq and back '
           '(no coin hop). Verdicts come from the firmware\'s own layout (scene.c) in the simulator. '
           '"Routing" counts requests whose card kind matches the request class (act/live: later or '
           'confirm; timer: timer; local: anything but later). "Claims" flags text that says an action '
           'was done outside a later or timer card.</p><table><tr><th>model<th>answers<th>errors<th>shown'
           '<th>no issues<th>routing<th>claims<th>p50 s<th>p90 s<th>cards</tr>']
    for s in summary:
        out.append(f"<tr><td>{e(s['model'])}<td>{s['n']}<td>{s['errors']}<td>{s['shown']}<td>{s['clean']}"
                   f"<td>{s['routing']}<td>{s['claims']}<td>{s['p50']:.2f}<td>{s['p90']:.2f}<td>{e(s['kinds'])}</tr>")
    out.append('</table><h2>Every request</h2><table><tr><th>request')
    out += [f'<th>{e(m)}' for m in models]
    out.append('</tr>')
    for i, (cls, q) in enumerate(qs):
        out.append(f'<tr><td class=q>{e(q)}<div class=cls>{cls}</div>')
        for m in models:
            r = by.get((m, i))
            if not r:
                out.append('<td class=cell>-')
                continue
            cell = []
            if r.get('shot'):
                cell.append(f'<img src="{e(r["shot"])}" loading=lazy>')
            sc = r.get('scene') or {}
            if sc.get('say'):
                cell.append(f'<div class=say>"{e(str(sc["say"]))}"</div>')
            meta = f"{r.get('kind', r['verdict'])} &middot; {r.get('latency', 0):.2f} s"
            ok = expected_ok(r) if r['verdict'] == 'shown' else False
            cell.append(f'<div class={"ok" if ok else "bad"}>{meta}</div>')
            if r.get('issue_text'):
                cell.append(f'<div class=bad>{e(r["issue_text"])}</div>')
            if r.get('claim'):
                cell.append(f'<div class=bad>claims: "{e(r["claim"])}"</div>')
            if r['verdict'] in ('error', 'not JSON'):
                cell.append(f'<div class=bad>{e(r.get("error") or r.get("out", "")[:200])}</div>')
            out.append('<td class=cell>' + ''.join(cell))
        out.append('</tr>')
    out.append('</table>')
    (run / 'report.html').write_text('\n'.join(out), encoding='utf-8')
    (run / 'summary.json').write_text(json.dumps(summary, indent=1), encoding='utf-8')
    for s in summary:
        print(s)


def main():
    cmd, run = sys.argv[1], pathlib.Path(sys.argv[2])
    if cmd == 'compose':
        compose(run, sys.argv[3].split(',') if len(sys.argv) > 3 else MODELS)
    elif cmd == 'render':
        render(run)
    elif cmd == 'report':
        report(run)


if __name__ == '__main__':
    main()
