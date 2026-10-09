"""coinsim host: simulates the board around the coin's firmware and serves the browser UI.

    python sim/host/coinsim.py [--coin sim/coin/build/coinsim.exe] [--http-port 8740] [--ble android-netsim]

Open http://127.0.0.1:8740/ and hold PWR to power the coin on. Ctrl+C stops the host and the coin.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import pathlib
import shlex
import signal
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))

from coinhost.supervisor import Sim  # noqa: E402
from coinhost.web import make_app  # noqa: E402


def coin_command(spec: str) -> list[str]:
    path = pathlib.Path(spec)
    if not path.is_absolute() and not path.exists() and (REPO / spec).exists():
        path = REPO / spec
    if path.exists() or ' ' not in spec.strip():
        if path.suffix.lower() == '.py':
            return [sys.executable, '-u', str(path.resolve())]
        return [str(path.resolve() if path.exists() else path)]
    return shlex.split(spec, posix=sys.platform != 'win32')


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--coin', default='sim/coin/build/coinsim.exe',
                    help='coin executable, a .py stand-in (e.g. sim/host/fake_coin.py) or a full command line')
    ap.add_argument('--data', default=str(REPO / 'sim' / '.data' / 'default'), help='data directory')
    ap.add_argument('--http-port', type=int, default=8740)
    ap.add_argument('--http-host', default='127.0.0.1')
    ap.add_argument('--ble', default='android-netsim',
                    help='android-netsim (default), none, or any Bumble transport moniker (e.g. tcp-client:127.0.0.1:9101)')
    ap.add_argument('--address', default='C0:1A:51:00:00:01', help='the coin\'s Bluetooth address')
    ap.add_argument('--power-on', action='store_true', help='power the coin on at start (as if PWR was held)')
    ap.add_argument('--battery', type=float, default=None, help='initial battery percent')
    ap.add_argument('--no-usb', action='store_true', help='start with the USB cable unplugged')
    ap.add_argument('-v', '--verbose', action='store_true')
    return ap.parse_args(argv)


async def amain(args) -> None:
    from aiohttp import web

    sim = Sim(coin_command(args.coin), pathlib.Path(args.data).resolve(), args.ble, args.address,
              evidence_dir=REPO / 'evidence')
    if args.battery is not None:
        sim.pmic.battery_pct = max(0.0, min(100.0, args.battery))
    if args.no_usb:
        sim.pmic.usb = False
    app = make_app(sim)
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, args.http_host, args.http_port)
    await sim.start()
    await site.start()
    print(f'coinsim: http://{args.http_host}:{args.http_port}/  (IPC port {sim.port}, BLE {args.ble}, '
          f'data {sim.data_dir})', flush=True)
    if args.power_on:
        await sim.power_on('--power-on')
    if sys.platform == 'win32':  # Ctrl+Break (and console close) stop the host as cleanly as Ctrl+C
        loop, task = asyncio.get_running_loop(), asyncio.current_task()
        signal.signal(signal.SIGBREAK, lambda *_: loop.call_soon_threadsafe(task.cancel))
    try:
        await asyncio.Event().wait()
    finally:
        await sim.close()
        await runner.cleanup()


def main(argv=None) -> int:
    args = parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format='%(asctime)s %(levelname).1s %(name)s: %(message)s', datefmt='%H:%M:%S')
    if not args.verbose:
        logging.getLogger('bumble').setLevel(logging.WARNING)
    try:
        asyncio.run(amain(args))
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    print('coinsim: stopped', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
