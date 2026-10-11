"""Reads the coin's battery log (console `battlog`) and estimates drain and battery life.

    python scripts/battlog.py COM9              # read from the coin (no reset)
    python scripts/battlog.py --file log.csv    # or analyse a saved log

The AXP2101 has no battery-current sensor, so drain comes from how fast the fuel gauge (soc_pct)
and the battery voltage fall: on battery while awake, and across standby (an S record followed by
the next B). A charge on USB at the firmware's 100 mA constant current gives a rough capacity.
"""

from __future__ import annotations

import argparse
import sys
import time

CHARGE_MA = 100  # pmic chg setting; the constant-current phase only

# Resting Li-ion open-circuit voltage -> state of charge (generic curve, +-10 %).
OCV = [(3300, 0), (3500, 3), (3600, 8), (3680, 15), (3730, 25), (3770, 35), (3800, 45), (3840, 55),
       (3890, 65), (3950, 75), (4020, 85), (4100, 93), (4190, 100)]


def ocv_soc(mv: float) -> float:
    if mv <= OCV[0][0]:
        return 0.0
    for (v0, s0), (v1, s1) in zip(OCV, OCV[1:]):
        if mv <= v1:
            return s0 + (s1 - s0) * (mv - v0) / (v1 - v0)
    return 100.0


def read_serial(port: str) -> str:
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False  # opening with DTR/RTS asserted resets the board
    s.open()
    s.reset_input_buffer()
    s.write(b'battlog\r\n')
    out, t0 = b'', time.time()
    while time.time() - t0 < 20 and b'battlog end' not in out:
        out += s.read(4096)
    s.close()
    text = out.decode('utf-8', 'replace')
    return text[text.find('rtc_s,event'):text.find('battlog end')]


def parse(text: str) -> list[dict]:
    rows = []
    for line in text.splitlines():
        parts = line.strip().split(',')
        if len(parts) != 5 or not parts[0].isdigit():
            continue
        rows.append({'t': int(parts[0]), 'ev': parts[1], 'mv': int(parts[2]), 'soc': int(parts[3]),
                     'f': parts[4]})
    return rows


def analyse(rows: list[dict]) -> None:
    awake = {'h': 0.0, 'soc': 0.0, 'mv': 0.0, 'screen_h': 0.0}
    sleep = {'h': 0.0, 'soc': 0.0, 'mv': 0.0, 'n': 0}
    charge = []  # (hours of charging, soc gained)
    run = None
    for a, b in zip(rows, rows[1:]):
        dt = (b['t'] - a['t']) / 3600
        if dt <= 0 or b['ev'] == 'B' and a['ev'] not in 'S':
            run = None  # a reset or power cycle in between: the RTC restarted or the gap is unknown
            if dt <= 0:
                continue
        on_battery = 'U' not in a['f'] and 'U' not in b['f']
        if on_battery and a['ev'] == 'S' and b['ev'] == 'B':
            sleep['h'] += dt
            sleep['soc'] += a['soc'] - b['soc']
            sleep['mv'] += a['mv'] - b['mv']
            sleep['n'] += 1
        elif on_battery and a['ev'] not in 'SO':
            awake['h'] += dt
            awake['soc'] += a['soc'] - b['soc']
            awake['mv'] += a['mv'] - b['mv']
            if 'D' in a['f']:
                awake['screen_h'] += dt
        if 'U' in a['f'] and 'C' in a['f']:
            if run is None:
                run = [a, a]
            run[1] = b
        elif run is not None:
            charge.append(((run[1]['t'] - run[0]['t']) / 3600, run[1]['soc'] - run[0]['soc'],
                           run[0]['soc'], run[1]['soc']))
            run = None
    if run is not None:
        charge.append(((run[1]['t'] - run[0]['t']) / 3600, run[1]['soc'] - run[0]['soc'], run[0]['soc'],
                       run[1]['soc']))

    print(f'{len(rows)} records, {(rows[-1]["t"] - rows[0]["t"]) / 3600:.1f} h span' if rows else 'no records')
    if awake['h'] > 0:
        r = awake['soc'] / awake['h']
        print(f'awake on battery: {awake["h"]:.2f} h ({awake["screen_h"] / awake["h"] * 100:.0f}% screen on), '
              f'gauge -{awake["soc"]:.0f}% ({r:.1f} %/h), -{awake["mv"]:.0f} mV')
        if r > 0:
            print(f'  -> about {100 / r:.1f} h of this use from full')
    if sleep['h'] > 0:
        r = sleep['soc'] / sleep['h']
        print(f'standby: {sleep["h"]:.1f} h over {sleep["n"]} stretches, gauge -{sleep["soc"]:.0f}% ({r:.2f} %/h), '
              f'-{sleep["mv"]:.0f} mV')
        if r > 0:
            print(f'  -> about {100 / r / 24:.1f} days of standby from full')
        else:
            print('  (gauge did not move: standby drain is below its 1% resolution over this time)')
    for h, gained, s0, s1 in charge:
        if gained > 0:
            cap = CHARGE_MA * h / (gained / 100)
            print(f'charge {s0}->{s1}% in {h:.2f} h at <={CHARGE_MA} mA -> capacity <= {cap:.0f} mAh '
                  '(upper bound: the current tapers near full)')
    if rows:
        last = rows[-1]
        print(f'now: {last["mv"]} mV, gauge {last["soc"]}%, voltage curve ~ {ocv_soc(last["mv"]):.0f}%')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port', nargs='?', default='COM9')
    ap.add_argument('--file')
    ap.add_argument('--save', help='also write the raw CSV here')
    args = ap.parse_args()
    text = open(args.file, encoding='utf-8').read() if args.file else read_serial(args.port)
    if args.save:
        open(args.save, 'w', encoding='utf-8').write(text)
    analyse(parse(text))
    return 0


if __name__ == '__main__':
    sys.exit(main())
