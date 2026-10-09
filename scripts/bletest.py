"""Bench client for the coin's BLE protocol (PROTOCOL.md v1) over the PC's Bluetooth adapter.

Windows cannot type the coin's passkey through bleak, so this talks to a coin in bench mode
(`ble bench` on its USB console): no pairing, an unencrypted link, until the coin reboots.

Usage: python scripts/bletest.py <command> [options]
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
import struct
import sys
import time
import zlib

from bleak import BleakClient, BleakScanner

from clipget import check_ogg

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
                stray += 1  # left over from a replaced GET, or a gap
                if c == clip and offset > len(buf):
                    raise RuntimeError(f"gap: expected offset {len(buf)}, got {offset}")
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


async def main():
    p = argparse.ArgumentParser()
    p.add_argument("-v", "--verbose", action="store_true")
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
    w = sub.add_parser("watch")
    w.add_argument("seconds", type=float, nargs="?", default=30)
    args = p.parse_args()

    coin = Coin(verbose=args.verbose or args.cmd == "watch")
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
    finally:
        await coin.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
