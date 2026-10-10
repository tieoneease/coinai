"""A phone-like Bumble GATT central for the coin (PROTOCOL.md v1). Works against the simulator's Bumble peripheral
(over tools/vlink.py, or netsim) and, later, the real coinsim.exe behind it.

    python sim/host/tools/central.py --transport tcp-client:127.0.0.1:9102 \
        --passkey-from https://coinsim.localhost/api/state  hello list "get 7 0" "wait 1"

Flow: scan for the coinai service UUID, connect, MTU 247, discover, subscribe (CCCDs are open before pairing),
write; on ATT 0x05 (Insufficient Authentication) pair with LE Secure Connections + MITM + bonding as KeyboardOnly,
typing the passkey the coin shows (--passkey, or fetched from the host's GET /api/state), then retry the write.

Commands: hello | list | get CLIP [OFFSET] | ack CLIP CRC32HEX|last | cfg KEY VALUE | reply CLIP STATE TEXT |
          wifi SSID PASSWORD | raw HEX | wait SECONDS | read-dis | read-battery | disconnect
The last line of output is `RESULT {json}` with everything received.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import pathlib
import socket
import struct
import sys
import time
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from bumble import gatt, hci  # noqa: E402
from bumble.att import ATT_Error  # noqa: E402
from bumble.core import UUID, AdvertisingData  # noqa: E402
from bumble.device import Device, DeviceConfiguration, Peer  # noqa: E402
from bumble.keys import JsonKeyStore  # noqa: E402
from bumble.pairing import PairingConfig, PairingDelegate  # noqa: E402
from bumble.transport import open_transport  # noqa: E402

from coinhost import tlv  # noqa: E402

ATT_INSUFFICIENT_AUTHENTICATION = 0x05


def say(*a) -> None:
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def resolve_localhost_names() -> None:
    """RFC 6761: every *.localhost name is loopback. Browsers resolve portless's https://<name>.localhost themselves;
    the Windows resolver does not (portless would have to edit the hosts file as administrator). Only the address
    lookup changes: TLS still checks the certificate against the real name."""
    lookup = socket.getaddrinfo
    if getattr(lookup, 'localhost_names', False):
        return

    def getaddrinfo(host, *args, **kwargs):
        if isinstance(host, str) and host.lower().rstrip('.').endswith('.localhost'):
            host = '127.0.0.1'
        return lookup(host, *args, **kwargs)

    getaddrinfo.localhost_names = True
    socket.getaddrinfo = getaddrinfo


def fetch_passkey(url: str, timeout: float = 15.0) -> int | None:
    resolve_localhost_names()
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            with urllib.request.urlopen(url, timeout=2) as r:
                state = json.loads(r.read())
            pk = state.get('ble', {}).get('passkey')
            if pk:
                return int(pk)
        except Exception as e:  # noqa: BLE001
            say(f'passkey fetch: {e}')
        time.sleep(0.1)
    return None


class KeyboardDelegate(PairingDelegate):
    def __init__(self, args) -> None:
        super().__init__(io_capability=PairingDelegate.KEYBOARD_INPUT_ONLY)
        self.args = args
        self.entered: int | None = None

    async def get_number(self) -> int | None:
        if self.args.passkey is not None:
            pk = self.args.passkey
        elif self.args.passkey_from:
            pk = await asyncio.to_thread(fetch_passkey, self.args.passkey_from)
        else:
            pk = int(await asyncio.to_thread(input, 'passkey shown on the coin: '))
        self.entered = pk
        say(f'pairing: entering passkey {pk:06d}' if pk is not None else 'pairing: no passkey')
        return pk


class Central:
    def __init__(self, args) -> None:
        self.args = args
        self.result = {'indications': [], 'notifications': 0, 'data_bytes': 0, 'data': {}, 'att_errors': [],
                       'paired': False, 'mtu': None, 'dis': {}, 'battery': None, 'adv': {}}
        self.ind_queue: asyncio.Queue = asyncio.Queue()
        self.last_crc: int | None = None
        self.delegate = KeyboardDelegate(args)

    # ------------------------------------------------------------ callbacks
    def on_indication(self, value: bytes) -> None:
        text = tlv.describe(value)
        say(f'<- ctrl {text}')
        self.result['indications'].append(text)
        if value[:1] == bytes([tlv.CLIP_ENDED]) and len(value) >= 3 + 14:
            self.last_crc = struct.unpack_from('<I', value, 3 + 10)[0]  # clip u16, size u32, duration u32, crc32
        self.ind_queue.put_nowait(value)

    def on_notification(self, value: bytes) -> None:
        parsed = tlv.parse_data(value)
        self.result['notifications'] += 1
        if parsed:
            clip, off, n = parsed
            self.result['data_bytes'] += n
            d = self.result['data'].setdefault(str(clip), {'first': off, 'end': off, 'packets': 0, 'bytes': 0,
                                                          'in_order': True})
            if off != d['end'] and d['packets']:
                d['in_order'] = False
            d['end'] = off + n
            d['packets'] += 1
            d['bytes'] += n

    # ------------------------------------------------------------ steps
    async def scan(self, device: Device) -> hci.Address:
        found = asyncio.get_running_loop().create_future()
        times = []
        service = UUID(tlv.SERVICE_UUID)

        def on_adv(adv) -> None:
            uuids = adv.data.get(AdvertisingData.COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS) or []
            if service in uuids:
                now = time.monotonic()
                if not times or now - times[-1] > 0.005:  # one event: ADV_IND and its SCAN_RSP report together
                    times.append(now)
                if not found.done():
                    self.result['adv'] = {
                        'address': str(adv.address), 'connectable': adv.is_connectable,
                        'name': adv.data.get(AdvertisingData.COMPLETE_LOCAL_NAME),
                        'flags': adv.data.get(AdvertisingData.FLAGS), 'rssi': adv.rssi,
                        'data_hex': bytes(adv.data).hex()}
                    found.set_result(adv.address)

        device.on('advertisement', on_adv)
        await device.start_scanning(filter_duplicates=False)
        try:
            address = await asyncio.wait_for(found, self.args.scan_timeout)
            if self.args.measure_adv:
                await asyncio.sleep(self.args.measure_adv)
                if len(times) > 2:
                    self.result['adv']['measured_interval_ms'] = round(
                        (times[-1] - times[0]) / (len(times) - 1) * 1000, 1)
        finally:
            await device.stop_scanning()
            device.remove_listener('advertisement', on_adv)
        say(f'found coinai at {address}: {self.result["adv"]}')
        return address

    async def write(self, peer: Peer, ctrl, value: bytes, connection) -> bool:
        say(f'-> ctrl {tlv.describe(value)}')
        for attempt in range(2):
            try:
                await peer.write_value(ctrl, value, with_response=True)
                return True
            except ATT_Error as e:
                self.result['att_errors'].append(e.error_code)
                say(f'write refused: ATT error 0x{e.error_code:02x}')
                if e.error_code == ATT_INSUFFICIENT_AUTHENTICATION and attempt == 0 and not self.args.no_pair:
                    say('pairing (LE Secure Connections, MITM, bonding; KeyboardOnly)')
                    await connection.pair()
                    self.result['paired'] = True
                    say(f'paired: encrypted={connection.is_encrypted} authenticated={connection.authenticated}')
                    continue
                return False
        return False

    async def run(self) -> int:
        args = self.args
        transport = await open_transport(args.transport)
        config = DeviceConfiguration()
        config.name = 'central'
        config.address = hci.Address(args.address)
        device = Device.from_config_with_hci(config, transport.source, transport.sink)
        if args.keys:
            device.keystore = JsonKeyStore(namespace='central', filename=args.keys)
        public = {'value': False}
        device.pairing_config_factory = lambda conn: PairingConfig(
            sc=True, mitm=True, bonding=True, delegate=self.delegate,
            identity_address_type=PairingConfig.AddressType.PUBLIC if public['value'] else PairingConfig.AddressType.RANDOM)
        rc = 1
        try:
            await device.power_on()
            # Like a phone: connect from the identity address the bond is stored under (a phone would use an RPA
            # that resolves to it). Without a controller public address, the static random address is the identity.
            public['value'] = device.public_address != hci.Address.ANY and not args.random
            own = hci.OwnAddressType.PUBLIC if public['value'] else hci.OwnAddressType.RANDOM
            target = await self.scan(device)
            say(f'connecting to {target} from {device.public_address if public["value"] else device.random_address}')
            connection = await device.connect(target, own_address_type=own, timeout=args.connect_timeout)
            say(f'connected: handle 0x{connection.handle:04x}')
            peer = Peer(connection)
            self.result['mtu'] = await peer.request_mtu(args.mtu)
            say(f'MTU {self.result["mtu"]}')
            await peer.discover_services()
            for s in peer.services:
                await s.discover_characteristics()
            ctrl = peer.get_characteristics_by_uuid(UUID(tlv.CTRL_UUID))[0]
            data = peer.get_characteristics_by_uuid(UUID(tlv.DATA_UUID))[0]
            await peer.subscribe(data, self.on_notification, prefer_notify=True)
            await peer.subscribe(ctrl, self.on_indication, prefer_notify=False)
            say('subscribed to ctrl (indicate) and data (notify)')
            if args.keys and await device.keystore.get(str(connection.peer_address)):
                say('bonded: encrypting with the stored key')
                await connection.encrypt()
            rc = 0
            for cmd in args.commands:
                if not await self.command(cmd, peer, ctrl, connection):
                    rc = 2
            if self.delegate.entered is not None:
                self.result['passkey_entered'] = f'{self.delegate.entered:06d}'
            self.result['encrypted'] = connection.is_encrypted
            self.result['authenticated'] = connection.authenticated
            if connection.handle in device.connections:
                await connection.disconnect()
        except Exception as e:  # noqa: BLE001
            say(f'error: {type(e).__name__}: {e}')
            self.result['error'] = f'{type(e).__name__}: {e}'
            rc = 1
            for conn in list(device.connections.values()):  # a virtual link has no supervision timeout
                try:
                    await asyncio.wait_for(conn.disconnect(), 3)
                except Exception:  # noqa: BLE001
                    pass
        finally:
            await transport.close()
        print('RESULT ' + json.dumps(self.result), flush=True)
        return rc

    async def command(self, cmd: str, peer: Peer, ctrl, connection) -> bool:
        parts = cmd.split()
        name, a = parts[0], parts[1:]
        if name == 'wait':
            await asyncio.sleep(float(a[0]) if a else 1.0)
            return True
        if name == 'disconnect':
            await connection.disconnect()
            return True
        if name == 'read-dis':
            for label, uuid in (('manufacturer', gatt.GATT_MANUFACTURER_NAME_STRING_CHARACTERISTIC),
                                ('model', gatt.GATT_MODEL_NUMBER_STRING_CHARACTERISTIC),
                                ('firmware', gatt.GATT_FIRMWARE_REVISION_STRING_CHARACTERISTIC)):
                chars = peer.get_characteristics_by_uuid(uuid)
                if chars:
                    self.result['dis'][label] = (await peer.read_value(chars[0])).decode()
            names = peer.get_characteristics_by_uuid(gatt.GATT_DEVICE_NAME_CHARACTERISTIC)
            if names:
                self.result['dis']['gap_name'] = (await peer.read_value(names[0])).decode()
            say(f'DIS {self.result["dis"]}')
            return True
        if name == 'read-battery':
            chars = peer.get_characteristics_by_uuid(gatt.GATT_BATTERY_LEVEL_CHARACTERISTIC)
            self.result['battery'] = (await peer.read_value(chars[0]))[0] if chars else None
            say(f'battery {self.result["battery"]} %')
            return True
        if name == 'hello':
            value = tlv.hello(int(time.time() * 1000))
        elif name == 'list':
            value = tlv.list_()
        elif name == 'get':
            value = tlv.get(int(a[0]), int(a[1]) if len(a) > 1 else 0)
        elif name == 'ack':
            crc = self.last_crc if a[1] == 'last' else int(a[1], 16)
            value = tlv.ack(int(a[0]), crc if crc is not None else 0)
        elif name == 'cfg':
            value = tlv.cfg_set(int(a[0]), int(a[1]))
        elif name == 'reply':
            value = tlv.reply(int(a[0]), int(a[1]), ' '.join(a[2:]))
        elif name == 'wifi':
            value = tlv.wifi_set(a[0].encode(), a[1].encode())
        elif name == 'raw':
            value = bytes.fromhex(''.join(a))
        else:
            say(f'unknown command {name}')
            return False
        ok = await self.write(peer, ctrl, value, connection)
        if ok and name in ('hello', 'list', 'get', 'ack'):
            try:  # these have answers; GET's ends with CLIP_ENDED (or ERROR)
                while True:
                    v = await asyncio.wait_for(self.ind_queue.get(), self.args.answer_timeout)
                    if name != 'get' or v[:1] in (bytes([tlv.CLIP_ENDED]), bytes([tlv.ERROR])):
                        break
            except asyncio.TimeoutError:
                say(f'no answer to {name}')
                return False
        return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--transport', default='tcp-client:127.0.0.1:9102')
    ap.add_argument('--address', default='F0:F1:F2:F3:F4:F5', help='own (static random) address')
    ap.add_argument('--passkey', type=int, default=None)
    ap.add_argument('--passkey-from', default=None, help='URL of the host\'s /api/state to read the passkey from')
    ap.add_argument('--keys', default=None, help='JSON keystore file for the central (bonds survive runs)')
    ap.add_argument('--no-pair', action='store_true')
    ap.add_argument('--random', action='store_true', help='connect from the static random address, not the public one')
    ap.add_argument('--mtu', type=int, default=247)
    ap.add_argument('--scan-timeout', type=float, default=15.0)
    ap.add_argument('--connect-timeout', type=float, default=10.0)
    ap.add_argument('--answer-timeout', type=float, default=10.0)
    ap.add_argument('--measure-adv', type=float, default=0.0, help='seconds to keep scanning to measure the interval')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('commands', nargs='*')
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.WARNING)
    return asyncio.run(Central(args).run())


if __name__ == '__main__':
    sys.exit(main())
