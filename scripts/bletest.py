"""Bench client for the coin's BLE protocol (PROTOCOL.md v1) over the PC's Bluetooth adapter.

Windows cannot type the coin's passkey through bleak, so this talks to a coin in bench mode
(`ble bench` on its USB console): no pairing, an unencrypted link, until the coin reboots.

With --serial COM9 the same exchange runs over the USB console instead of the radio: each request
is typed as `ble inject <hex>` and the coin prints its answers and data packets as BLE-CTRL and
BLE-DATA lines. That tests the firmware's protocol layer without a working radio link.

Usage: python scripts/bletest.py [--serial COMx] <command> [options]
  status                         HELLO -> STATUS (also sets the coin's clock)
  list                           LIST -> LIST_RESULT
  get <clip> [-o out.ogg]        fetch a clip, check size, CRC-32 and the Ogg pages
      --regrab-at N              after N bytes, send a new GET from the received length
      --reconnect-at N           after N bytes, disconnect, reconnect and resume from there
      --ack                      ACK the clip afterwards (the coin deletes it)
  config <key> <value>           CFG_SET: reply_wait_s | brightness | idle_timeout_s | off_after_h
  reply <clip> <state> <text>    REPLY (state 0 transcribing, 1 thinking, 2 done, 3 failed)
  watch [seconds]                print every message the coin sends
"""
import argparse
import asyncio
import re
import struct
import sys
import threading
import time
import zlib

from bleak import BleakClient, BleakScanner

from clipget import check_ogg
from coinlink import open_port

SERVICE = "c0a10000-f569-41ae-8676-7f5a60d835fc"
CTRL = "c0a10001-f569-41ae-8676-7f5a60d835fc"
DATA = "c0a10002-f569-41ae-8676-7f5a60d835fc"

HELLO, LIST, GET, ACK, WIFI_SET, CFG_SET, REPLY = range(1, 8)
STATUS, CLIP_STARTED, CLIP_ENDED, LIST_RESULT, ERROR = range(0x81, 0x86)
NAMES = {STATUS: "STATUS", CLIP_STARTED: "CLIP_STARTED", CLIP_ENDED: "CLIP_ENDED", LIST_RESULT: "LIST_RESULT",
         ERROR: "ERROR"}
CFG_KEYS = {"reply_wait_s": (1, "<H"), "brightness": (2, "<B"), "idle_timeout_s": (3, "<H"), "off_after_h": (4, "<H")}


def tlv(msg_type, value=b""):
    return struct.pack("<BH", msg_type, len(value)) + value


def decode(msg_type, v):
    if msg_type == STATUS:
        pct, flags, pending = struct.unpack_from("<BBH", v)
        return {"battery_pct": pct, "charging": bool(flags & 1), "usb": bool(flags & 2), "recording": bool(flags & 4),
                "pending": pending, "fw": v[4:].decode(errors="replace")}
    if msg_type == CLIP_STARTED:
        clip, codec, rate, start = struct.unpack("<HBIQ", v)
        return {"clip": clip, "codec": codec, "rate": rate, "start_utc_ms": start}
    if msg_type == CLIP_ENDED:
        clip, size, dur, crc, voiced, start = struct.unpack("<HIIIIQ", v)
        return {"clip": clip, "size": size, "duration_ms": dur, "crc32": crc, "voiced_ms": voiced, "start_utc_ms": start}
    if msg_type == LIST_RESULT:
        entries = [struct.unpack_from("<HIB", v, 1 + 7 * i) for i in range((len(v) - 1) // 7)]
        return {"more": bool(v[0] & 1), "clips": [{"clip": c, "size": s, "complete": bool(f & 1)} for c, s, f in entries]}
    if msg_type == ERROR:
        req, code, clip = struct.unpack("<BBH", v)
        return {"request": req, "code": code, "clip": clip}
    return v.hex()


class Coin:
    def __init__(self, verbose=False):
        self.client = None
        self.device = None
        self.ctrl = asyncio.Queue()
        self.packets = asyncio.Queue()
        self.verbose = verbose

    async def connect(self):
        if self.device is None:
            print("scanning for the coin ...")
            self.device = await BleakScanner.find_device_by_filter(
                lambda d, adv: SERVICE in [u.lower() for u in adv.service_uuids], timeout=20)
            if self.device is None:
                sys.exit("no coin advertising (is it awake? `ble` on its console shows the state)")
        t0 = time.monotonic()
        self.client = BleakClient(self.device, timeout=30, winrt={"use_cached_services": False})
        await self.client.connect()
        await self.client.start_notify(CTRL, self._on_ctrl)
        await self.client.start_notify(DATA, self._on_data)
        print(f"connected to {self.device.address} in {time.monotonic() - t0:.2f} s, MTU {self.client.mtu_size}")

    async def disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()

    def _on_ctrl(self, _sender, data):
        msg_type, length = struct.unpack_from("<BH", data)
        value = bytes(data[3:])
        if length != len(value):
            print(f"!! bad TLV length {length} for {len(value)} value bytes")
        msg = (msg_type, decode(msg_type, value))
        if self.verbose:
            print(f"<- {NAMES.get(msg_type, hex(msg_type))} {msg[1]}")
        self.ctrl.put_nowait(msg)

    def _on_data(self, _sender, data):
        clip, offset = struct.unpack_from("<HI", data)
        self.packets.put_nowait((clip, offset, bytes(data[6:])))

    async def send(self, msg_type, value=b""):
        await self.client.write_gatt_char(CTRL, tlv(msg_type, value), response=True)

    async def expect(self, msg_type, timeout=5.0):
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"no {NAMES.get(msg_type, msg_type)} within {timeout} s")
            got_type, value = await asyncio.wait_for(self.ctrl.get(), left)
            if got_type == msg_type:
                return value
            if got_type == ERROR:
                raise RuntimeError(f"coin answered ERROR {value}")
            print(f"   (also got {NAMES.get(got_type, got_type)} {value})")

    async def hello(self):
        await self.send(HELLO, struct.pack("<BQ", 1, int(time.time() * 1000)))
        return await self.expect(STATUS)

    async def list(self):
        await self.send(LIST)
        clips = []
        while True:
            r = await self.expect(LIST_RESULT)
            clips += r["clips"]
            if not r["more"]:
                return clips


class SerialCoin(Coin):
    """The protocol over the coin's USB console (`ble inject`), for testing without the radio."""

    LINE = re.compile(r"BLE-(CTRL|DATA) ([0-9a-f]+)")

    def __init__(self, port, verbose=False):
        super().__init__(verbose)
        self.port_name = port
        self.port = None
        self.reader = None
        self.running = False

    async def connect(self):
        self.port = open_port(self.port_name)
        self.loop = asyncio.get_running_loop()
        self.running = True
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        print(f"console link on {self.port_name}")

    def _read(self):
        buf = b""
        while self.running:
            buf += self.port.read(4096)
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                m = self.LINE.search(raw.decode(errors="replace"))
                if not m:
                    continue
                try:
                    data = bytes.fromhex(m.group(2))
                except ValueError:
                    # A line split by other console output. A lost data packet is repaired by a
                    # resume GET, as over the radio.
                    print(f"skipped a damaged console line ({len(raw)} bytes)")
                    continue
                handler = self._on_ctrl if m.group(1) == "CTRL" else self._on_data
                self.loop.call_soon_threadsafe(handler, None, data)

    async def disconnect(self):
        if self.port:
            self.port.write(b"ble inject off\r\n")
            await asyncio.sleep(0.2)
            self.running = False
            self.reader.join(timeout=1)
            self.port.close()
            self.port = None

    async def send(self, msg_type, value=b""):
        self.port.write(f"ble inject {tlv(msg_type, value).hex()}\r\n".encode())

    async def console(self, command):
        self.port.write(f"{command}\r\n".encode())


async def fetch(coin, clip, buf, stop_at=None):
    """Receives packets for `clip` into buf (bytearray) until CLIP_ENDED, or until len(buf) >= stop_at."""
    await coin.send(GET, struct.pack("<HI", clip, len(buf)))
    stray = 0
    while True:
        if stop_at is not None and len(buf) >= stop_at:
            return None
        pkt_task = asyncio.ensure_future(coin.packets.get())
        ctrl_task = asyncio.ensure_future(coin.ctrl.get())
        done, pending = await asyncio.wait({pkt_task, ctrl_task}, timeout=10, return_when=asyncio.FIRST_COMPLETED)
        for t in pending:
            t.cancel()
        if not done:
            raise TimeoutError(f"transfer stalled at {len(buf)} bytes")
        if pkt_task in done:
            c, offset, payload = pkt_task.result()
            if c != clip or offset != len(buf):
                stray += 1  # left over from a replaced GET or an earlier link; a gap is resumed below
            else:
                buf += payload
        if ctrl_task in done:
            msg_type, value = ctrl_task.result()
            if msg_type == CLIP_ENDED and value["clip"] == clip:
                # Packets queued before CLIP_ENDED arrive first: indications and notifications share the link in order.
                while not coin.packets.empty():
                    c, offset, payload = coin.packets.get_nowait()
                    if c == clip and offset == len(buf):
                        buf += payload
                if stray:
                    print(f"   ignored {stray} packets from a replaced stream")
                return value
            if msg_type == ERROR:
                raise RuntimeError(f"coin answered ERROR {value}")
            print(f"   (during transfer: {NAMES.get(msg_type, msg_type)} {value})")


async def cmd_get(coin, args):
    buf = bytearray()
    t0 = time.monotonic()
    ended = None
    if args.regrab_at:
        await fetch(coin, args.clip, buf, stop_at=args.regrab_at)
        print(f"   {len(buf)} bytes received; sending a new GET from there")
    if args.reconnect_at:
        await fetch(coin, args.clip, buf, stop_at=args.reconnect_at)
        print(f"   {len(buf)} bytes received; disconnecting")
        await coin.disconnect()
        coin.packets = asyncio.Queue()
        coin.ctrl = asyncio.Queue()
        await asyncio.sleep(1.0)
        await coin.connect()
        await coin.hello()
        print(f"   reconnected; resuming from {len(buf)}")
    ended = await fetch(coin, args.clip, buf)
    for _ in range(3):
        if len(buf) >= ended["size"]:
            break
        print(f"   CLIP_ENDED at {len(buf)} of {ended['size']} bytes: resuming with a GET from {len(buf)}")
        ended = await fetch(coin, args.clip, buf)
    secs = time.monotonic() - t0
    crc = zlib.crc32(buf)
    ok = ended["size"] == len(buf) and ended["crc32"] == crc
    print(f"clip {args.clip}: {len(buf)} bytes in {secs:.2f} s ({len(buf) / secs / 1024:.1f} KB/s); "
          f"CLIP_ENDED size {ended['size']} crc {ended['crc32']:08x} duration {ended['duration_ms']} ms "
          f"voiced {ended['voiced_ms']} ms start {ended['start_utc_ms']}; computed crc {crc:08x}: {'OK' if ok else 'MISMATCH'}")
    print(f"   ogg: {check_ogg(bytes(buf))}")
    if args.out:
        with open(args.out, "wb") as f:
            f.write(buf)
        print(f"   saved {args.out}")
    if not ok:
        sys.exit(1)
    if args.ack:
        await coin.send(ACK, struct.pack("<HI", args.clip, crc))
        st = await coin.expect(STATUS)
        print(f"ACK sent; coin now has {st['pending']} pending")


async def expect_error(coin, code, what):
    try:
        err = await coin.expect(ERROR, timeout=3)
    except TimeoutError:
        raise AssertionError(f"{what}: no ERROR")
    assert err["code"] == code, f"{what}: ERROR code {err['code']}, wanted {code}"
    return err


async def expect_silence(coin, seconds=1.0):
    try:
        msg_type, value = await asyncio.wait_for(coin.ctrl.get(), seconds)
    except TimeoutError:
        return
    raise AssertionError(f"unexpected {NAMES.get(msg_type, msg_type)} {value}")


async def selftest(coin):
    """Protocol edge cases. Over --serial it also records a clip and fetches it while it records."""
    results = []

    async def check(name, coro):
        try:
            note = await coro
            results.append((name, True, note or ""))
        except Exception as e:  # noqa: BLE001 - report and continue
            results.append((name, False, f"{type(e).__name__}: {e}"))
        while not coin.ctrl.empty():
            coin.ctrl.get_nowait()

    clips = await coin.list()
    ids = [c["clip"] for c in clips]
    sizes = {c["clip"]: c["size"] for c in clips}
    assert clips, "selftest needs at least one stored clip"
    victim = ids[0]

    async def list_paging():
        await coin.send(LIST)
        msgs = []
        while True:
            r = await coin.expect(LIST_RESULT)
            msgs.append(len(r["clips"]))
            if not r["more"]:
                break
        assert sum(msgs) == len(clips) and all(n <= 34 for n in msgs), msgs
        assert len(clips) <= 34 or len(msgs) > 1, msgs
        return f"{len(clips)} clips in messages of {msgs}"

    async def get_unknown():
        await coin.send(GET, struct.pack("<HI", 65000, 0))
        await expect_error(coin, 1, "GET unknown clip")

    async def get_bad_offset():
        await coin.send(GET, struct.pack("<HI", victim, sizes[victim] + 1))
        await expect_error(coin, 2, "GET past the end")

    async def bad_length():
        await coin.send(HELLO, b"\x01")
        await expect_error(coin, 3, "short HELLO")

    async def unknown_type():
        await coin.send(0x7F, b"\x00\x01")
        await expect_silence(coin)
        await coin.hello()

    async def ack_wrong_crc():
        await coin.send(ACK, struct.pack("<HI", victim, 0xDEADBEEF))
        await expect_error(coin, 5, "ACK with a wrong CRC")
        assert victim in [c["clip"] for c in await coin.list()], "clip deleted after a wrong ACK"

    async def cfg():
        await coin.send(CFG_SET, bytes([2]) + struct.pack("<B", 101))
        await expect_error(coin, 5, "brightness 101")
        await coin.send(CFG_SET, bytes([3]) + struct.pack("<H", 2))
        await expect_error(coin, 5, "idle_timeout_s 2")
        await coin.send(CFG_SET, bytes([2, 7, 7]))
        await expect_error(coin, 3, "brightness with a u16")
        await coin.send(CFG_SET, bytes([9]) + struct.pack("<H", 1))
        await expect_error(coin, 5, "unknown key")
        await coin.send(CFG_SET, bytes([2]) + struct.pack("<B", 100))
        await expect_silence(coin)

    async def wifi_refused():
        await coin.send(WIFI_SET, bytes([4]) + b"test" + bytes([8]) + b"password")
        await expect_error(coin, 5, "WIFI_SET on an unauthenticated link")

    async def reply():
        await coin.send(REPLY, struct.pack("<HB", victim, 1))
        await coin.send(REPLY, struct.pack("<HB", victim, 2) + b"selftest reply")
        await expect_silence(coin)
        await coin.send(REPLY, struct.pack("<HB", victim, 9))
        await expect_error(coin, 5, "REPLY state 9")

    async def ack_right():
        buf = bytearray()
        ended = await fetch(coin, victim, buf)
        assert zlib.crc32(buf) == ended["crc32"], "CRC"
        before = len(await coin.list())
        await coin.send(ACK, struct.pack("<HI", victim, ended["crc32"]))
        st = await coin.expect(STATUS)
        after = [c["clip"] for c in await coin.list()]
        assert victim not in after and st["pending"] == before - 1, (st, before)
        return f"clip {victim} deleted; pending {st['pending']}"

    async def live_get():
        await coin.console("rec start")
        started = await coin.expect(CLIP_STARTED)
        now = time.time() * 1000
        assert abs(started["start_utc_ms"] - now) < 3000, f"start time off by {started['start_utc_ms'] - now:.0f} ms"
        clip = started["clip"]
        buf = bytearray()
        await asyncio.sleep(0.5)
        task = asyncio.ensure_future(fetch(coin, clip, buf))
        await asyncio.sleep(2.0)
        during = len(buf)
        await coin.console("rec stop")
        ended = await task
        ok = ended["size"] == len(buf) and ended["crc32"] == zlib.crc32(buf)
        assert ok, f"{len(buf)} bytes, crc {zlib.crc32(buf):08x}; CLIP_ENDED {ended}"
        assert 0 < during < len(buf), f"nothing streamed while recording ({during} of {len(buf)})"
        check_ogg(bytes(buf))
        assert abs(ended["start_utc_ms"] - started["start_utc_ms"]) < 1000, (ended, started)
        return (f"clip {clip}: {during} bytes streamed while recording, {len(buf)} in all, "
                f"{ended['duration_ms']} ms, voiced {ended['voiced_ms']} ms, CRC ok")

    await check("LIST pages of at most 34", list_paging())
    await check("GET unknown clip -> ERROR 1", get_unknown())
    await check("GET past the end -> ERROR 2", get_bad_offset())
    await check("bad length -> ERROR 3", bad_length())
    await check("unknown type ignored", unknown_type())
    await check("ACK wrong CRC -> ERROR 5, clip kept", ack_wrong_crc())
    await check("CFG_SET ranges", cfg())
    await check("WIFI_SET refused without encryption", wifi_refused())
    await check("REPLY states", reply())
    await check("GET + ACK deletes", ack_right())
    if isinstance(coin, SerialCoin):
        await check("CLIP_STARTED + GET while recording", live_get())
    for name, ok, note in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}{'  - ' + note if note else ''}")
    if not all(ok for _, ok, _ in results):
        sys.exit(1)


async def main():
    p = argparse.ArgumentParser()
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--serial", metavar="PORT", help="run over the coin's USB console instead of BLE")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    sub.add_parser("list")
    g = sub.add_parser("get")
    g.add_argument("clip", type=int)
    g.add_argument("-o", "--out")
    g.add_argument("--regrab-at", type=int)
    g.add_argument("--reconnect-at", type=int)
    g.add_argument("--ack", action="store_true")
    c = sub.add_parser("config")
    c.add_argument("key", choices=CFG_KEYS)
    c.add_argument("value", type=int)
    r = sub.add_parser("reply")
    r.add_argument("clip", type=int)
    r.add_argument("state", type=int)
    r.add_argument("text")
    sub.add_parser("selftest")
    w = sub.add_parser("watch")
    w.add_argument("seconds", type=float, nargs="?", default=30)
    args = p.parse_args()

    verbose = args.verbose or args.cmd == "watch"
    coin = SerialCoin(args.serial, verbose) if args.serial else Coin(verbose)
    await coin.connect()
    try:
        st = await coin.hello()
        print(f"STATUS {st}")
        if args.cmd == "list":
            for e in await coin.list():
                print(f"  clip {e['clip']:5}  {e['size']:7} bytes  {'complete' if e['complete'] else 'recording'}")
        elif args.cmd == "get":
            await cmd_get(coin, args)
        elif args.cmd == "config":
            key, fmt = CFG_KEYS[args.key]
            await coin.send(CFG_SET, bytes([key]) + struct.pack(fmt, args.value))
            try:
                print(f"coin refused: {await coin.expect(ERROR, timeout=1.5)}")
            except TimeoutError:
                print("CFG_SET accepted")
        elif args.cmd == "reply":
            text = args.text.encode()[:200]
            await coin.send(REPLY, struct.pack("<HB", args.clip, args.state) + text)
            print("REPLY sent")
        elif args.cmd == "watch":
            await asyncio.sleep(args.seconds)
        elif args.cmd == "selftest":
            await selftest(coin)
    finally:
        await coin.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
