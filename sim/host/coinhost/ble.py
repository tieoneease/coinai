"""Bluetooth bridge: a Bumble GATT peripheral that mirrors the coin's NimBLE peripheral.

The coin (over IPC) decides everything a NimBLE app decides: advertising, the passkey, write results,
indications and notifications. This module turns those into Bumble calls and reports link events back.
"""

from __future__ import annotations

import asyncio
import collections
import logging
import pathlib
import struct
import time
import uuid as uuidlib
from typing import TYPE_CHECKING

from . import ipc, tlv

if TYPE_CHECKING:
    from .supervisor import Sim

log = logging.getLogger('coinsim.ble')

DEVICE_NAME = 'coinai'
MANUFACTURER = 'coinai'
MODEL = 'ESP32-S3-Touch-AMOLED-1.75C'
START_CREDITS = 12
RETRY_S = 5.0
OPEN_TIMEOUT_S = 10.0
WRITE_TIMEOUT_S = 30.0
PASSKEY_TIMEOUT_S = 10.0

# BLE_WRITE_RESULT -> ATT error code
WRITE_RESULT_ATT = {1: 0x05, 2: 0x0D, 3: 0x09, 4: 0x0E}

HCI_REMOTE_USER_TERMINATED = 0x13
HCI_CONNECTION_TIMEOUT = 0x08


def adv_data(name: str = DEVICE_NAME) -> bytes:
    """Flags (LE General Discoverable, BR/EDR not supported), complete 128-bit UUID list, complete name."""
    u = uuidlib.UUID(tlv.SERVICE_UUID).bytes[::-1]
    n = name.encode()
    return bytes([2, 0x01, 0x06, 17, 0x07]) + u + bytes([len(n) + 1, 0x09]) + n


class BleBridge:
    def __init__(self, sim: 'Sim', spec: str, address: str, data_dir: pathlib.Path) -> None:
        self.sim = sim
        self.spec = spec
        self.address = address.upper()
        self.keys_path = data_dir / 'ble_keys.json'
        self.enabled = spec != 'none'
        self.status = 'disabled' if not self.enabled else 'connecting'
        self.error: str | None = None
        self.next_retry: float | None = None
        self.device = None
        self.transport = None
        self.own_address_type = 'public'
        self.controller_address: str | None = None
        self.ctrl_char = None
        self.data_char = None
        self.battery_char = None
        # coin-side state
        self.coin_ready = False
        self.adv = ipc.Adv(False, 0, -1)
        self.advertising = False
        self.adv_until: float | None = None
        self._adv_timer: asyncio.Task | None = None
        self._adv_lock = asyncio.Lock()
        self.fw_version = ''
        self.battery_pct: int | None = None
        self.tx_power: int | None = None
        self.conn_params_req: tuple | None = None
        # radio
        self.rssi = -55
        self.out_of_range = False
        # connection
        self.conn = None
        self.mtu = 23
        self.security = (False, False, False)
        self.subscribed = {1: False, 2: False}
        self.passkey: int | None = None
        self.pairing = False
        self.bonds = 0
        self._write_seq = 0
        self._write_futs: dict[int, asyncio.Future] = {}
        self._passkey_fut: asyncio.Future | None = None
        self._credit_targets: collections.deque[int] = collections.deque()
        self._credit_queue = None
        self._ind_tasks: set[asyncio.Task] = set()
        self.credits_in_flight = 0
        self.notify_count = 0
        self.notify_bytes = 0
        self._closing = False
        self._task: asyncio.Task | None = None

    # ================================================================ lifecycle
    def start(self) -> None:
        if self.enabled:
            self._task = asyncio.create_task(self._run(), name='ble')

    async def close(self) -> None:
        self._closing = True
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
        await self._teardown()

    def _bumble_spec(self) -> str:
        return self.spec

    async def _run(self) -> None:
        from bumble.transport import open_transport

        while not self._closing:
            self.status = 'connecting'
            self.next_retry = None
            self.sim.mark_dirty()
            try:
                self.transport = await asyncio.wait_for(open_transport(self._bumble_spec()), OPEN_TIMEOUT_S)
                await asyncio.wait_for(self._bring_up(), OPEN_TIMEOUT_S)
            except asyncio.CancelledError:
                raise
            except BaseException as e:  # noqa: BLE001 - any failure means "no radio", never a crash
                error = f'{type(e).__name__}: {e}' if str(e) else type(e).__name__
                if self.spec.startswith('android-netsim') and 'port not found' in error:
                    error = 'netsim not found: start the Android emulator (no netsim.ini in %LOCALAPPDATA%/Temp)'
                if error != self.error:
                    log.info('no radio (%s): %s; retrying every %g s', self.spec, error, RETRY_S)
                self.error = error
                await self._teardown()
                await self._wait_retry()
                continue
            self.status = 'up'
            self.error = None
            self.sim.log_ble('radio', f'radio up via {self.spec}; address {self.address} ({self.own_address_type})')
            self.sim.mark_dirty()
            await self._apply_adv()
            terminated = getattr(self.transport.source, 'terminated', None)
            try:
                if terminated is not None:
                    await asyncio.shield(terminated)
                else:
                    await asyncio.Event().wait()
                self.error = 'transport closed'
            except asyncio.CancelledError:
                raise
            except BaseException as e:  # noqa: BLE001
                self.error = f'transport lost: {e}'
            self.sim.log_ble('radio', f'radio lost: {self.error}')
            await self._teardown()
            await self._wait_retry()

    async def _wait_retry(self) -> None:
        self.status = 'no radio'
        self.next_retry = time.monotonic() + RETRY_S
        self.sim.mark_dirty()
        await asyncio.sleep(RETRY_S)

    async def _bring_up(self) -> None:
        from bumble import gatt, hci
        from bumble.device import Device, DeviceConfiguration
        from bumble.keys import JsonKeyStore
        from bumble.pairing import PairingConfig
        from bumble.profiles.battery_service import BatteryService

        config = DeviceConfiguration()
        config.name = DEVICE_NAME
        config.address = hci.Address(self.address, hci.Address.RANDOM_DEVICE_ADDRESS)
        config.advertising_data = adv_data()
        config.scan_response_data = b''
        device = Device.from_config_with_hci(config, self.transport.source, self.transport.sink)
        self.keys_path.parent.mkdir(parents=True, exist_ok=True)
        device.keystore = JsonKeyStore(namespace=self.address, filename=str(self.keys_path))

        self.ctrl_char = gatt.Characteristic(
            tlv.CTRL_UUID,
            gatt.Characteristic.Properties.WRITE | gatt.Characteristic.Properties.INDICATE,
            gatt.Characteristic.WRITEABLE,
            gatt.CharacteristicValue(write=self._on_ctrl_write),
        )
        self.data_char = gatt.Characteristic(
            tlv.DATA_UUID, gatt.Characteristic.Properties.NOTIFY, gatt.Characteristic.Permissions(0), b'')
        device.add_service(gatt.Service(tlv.SERVICE_UUID, [self.ctrl_char, self.data_char]))

        def dis_char(uuid, fn):
            return gatt.Characteristic(uuid, gatt.Characteristic.Properties.READ, gatt.Characteristic.READABLE,
                                       gatt.CharacteristicValue(read=lambda _c: fn().encode()))

        device.add_service(gatt.Service(gatt.GATT_DEVICE_INFORMATION_SERVICE, [
            dis_char(gatt.GATT_MANUFACTURER_NAME_STRING_CHARACTERISTIC, lambda: MANUFACTURER),
            dis_char(gatt.GATT_MODEL_NUMBER_STRING_CHARACTERISTIC, lambda: MODEL),
            dis_char(gatt.GATT_FIRMWARE_REVISION_STRING_CHARACTERISTIC, lambda: self.fw_version),
        ]))
        bas = BatteryService(lambda _c: self.battery_pct if self.battery_pct is not None else 100)
        self.battery_char = bas.battery_level_characteristic
        device.add_service(bas)

        device.pairing_config_factory = self._pairing_config
        device.on('connection', self._on_connection)
        device.on('characteristic_subscription', self._on_subscription)
        self.device = device
        self._PairingConfig = PairingConfig
        await device.power_on()

        self.controller_address = str(device.public_address)
        if device.public_address == hci.Address(self.address, hci.Address.PUBLIC_DEVICE_ADDRESS):
            self.own_address_type = 'public'
        else:
            # the controller's public address is not ours (netsim assigns its own; HCI has no standard command
            # to set it). Advertise with the configured address as a static random address instead. The phone then
            # sees C0:1A:51:00:00:01 of type random, not public; whether netsim's Chip address can make it public is
            # unverified.
            self.own_address_type = 'random static'
            if (int(self.address[:2], 16) & 0xC0) != 0xC0:
                log.warning('address %s is not a valid static random address (top bits must be 11)', self.address)
        self.bonds = len(await device.keystore.get_all())
        q = device.host.le_acl_packet_queue or device.host.acl_packet_queue
        self._credit_queue = q
        if q is not None:
            q.on('flow', self._check_credits)

    async def _teardown(self) -> None:
        self._cancel_adv_timer()
        self.advertising = False
        if self.conn is not None:
            self._link_down(HCI_CONNECTION_TIMEOUT, log_text='radio gone')
        device, transport = self.device, self.transport
        self.device = None
        self.transport = None
        self._credit_queue = None
        if transport is not None:
            try:
                await asyncio.wait_for(transport.close(), 3)
            except BaseException:  # noqa: BLE001
                pass
        del device
        self.sim.mark_dirty()

    # ================================================================ pairing
    def _pairing_config(self, connection):
        from bumble.pairing import PairingConfig, PairingDelegate

        bridge = self

        class CoinDelegate(PairingDelegate):
            def __init__(self) -> None:
                super().__init__(io_capability=PairingDelegate.DISPLAY_OUTPUT_ONLY)

            async def generate_passkey(self) -> int:
                return await bridge._request_passkey(connection)

            async def display_number(self, number: int, digits: int) -> None:
                bridge.passkey = number
                bridge.sim.mark_dirty()

        addr_type = (PairingConfig.AddressType.PUBLIC if self.own_address_type == 'public'
                     else PairingConfig.AddressType.RANDOM)
        return PairingConfig(sc=True, mitm=True, bonding=True, delegate=CoinDelegate(),
                             identity_address_type=addr_type)

    async def _request_passkey(self, connection) -> int:
        self.pairing = True
        loop = asyncio.get_running_loop()
        self._passkey_fut = loop.create_future()
        if not self.sim.send(ipc.encode(ipc.BLE_PASSKEY_REQ)):
            self._passkey_fut = None
            raise RuntimeError('coin not running')
        self.sim.log_ble('event', 'pairing: passkey requested from the coin')
        try:
            passkey = await asyncio.wait_for(self._passkey_fut, PASSKEY_TIMEOUT_S)
        except asyncio.TimeoutError:
            self.sim.log_ble('event', 'pairing: the coin did not answer BLE_PASSKEY_REQ; disconnecting')
            asyncio.create_task(self._disconnect(connection, HCI_REMOTE_USER_TERMINATED))
            raise
        finally:
            self._passkey_fut = None
        self.passkey = passkey % 1000000
        self.sim.mark_dirty()
        return self.passkey

    # ================================================================ connection events
    def _on_connection(self, connection) -> None:
        from bumble import hci

        if connection.role != hci.Role.PERIPHERAL:
            return
        self._cancel_adv_timer()
        self.advertising = False
        if self.out_of_range or not self.coin_ready:
            asyncio.create_task(self._disconnect(connection, HCI_REMOTE_USER_TERMINATED))
            return
        self.conn = connection
        self.mtu = connection.att_mtu
        self.security = (False, False, False)
        self.subscribed = {1: False, 2: False}
        self.passkey = None
        self.pairing = False
        self._credit_targets.clear()
        self.credits_in_flight = 0
        addr = connection.peer_address
        # peer_addr_type is the over-the-air type as NimBLE's peer_ota_addr reports it: 0 public, 1 random.
        self.sim.send(ipc.ble_connected(connection.handle, 1 if addr.is_random else 0, bytes(addr)))
        self.sim.send(ipc.ble_rssi(self.rssi))
        self.sim.log_ble('event', f'connected: {addr} handle 0x{connection.handle:04x}')

        connection.on('disconnection', lambda reason: self._on_disconnection(connection, reason))
        connection.on('connection_att_mtu_update', lambda: self._on_mtu(connection))
        connection.on('connection_encryption_change', lambda: self._on_encryption(connection))
        connection.on('pairing_start', lambda: self._on_pairing_start(connection))
        connection.on('pairing', lambda keys: self._on_pairing(connection, keys))
        connection.on('pairing_failure', lambda reason: self._on_pairing_failure(connection, reason))
        connection.on('connection_parameters_update', lambda: self._on_conn_params(connection))
        self.sim.mark_dirty()

    def _on_disconnection(self, connection, reason: int) -> None:
        if connection is not self.conn:
            return
        self._link_down(reason, log_text=f'disconnected: reason 0x{reason:02x}')

    def _link_down(self, reason: int, log_text: str) -> None:
        self.conn = None
        for fut in self._write_futs.values():
            if not fut.done():
                fut.cancel()
        self._write_futs.clear()
        if self._passkey_fut and not self._passkey_fut.done():
            self._passkey_fut.cancel()
        self.passkey = None
        self.pairing = False
        self.security = (False, False, False)
        self.subscribed = {1: False, 2: False}
        self._credit_targets.clear()
        self.credits_in_flight = 0
        self.mtu = 23
        self.sim.send(ipc.ble_disconnected(reason))
        self.sim.log_ble('event', log_text)
        self.sim.mark_dirty()

    def _on_mtu(self, connection) -> None:
        if connection is self.conn:
            self.mtu = connection.att_mtu
            self.sim.send(ipc.ble_mtu(self.mtu))
            self.sim.log_ble('event', f'MTU {self.mtu}')
            self.sim.mark_dirty()

    def _on_pairing_start(self, connection) -> None:
        self.pairing = True
        self.sim.log_ble('event', 'pairing started (LE Secure Connections, MITM, bonding; DisplayOnly)')
        self.sim.mark_dirty()

    def _on_encryption(self, connection) -> None:
        if connection is not self.conn:
            return
        if self.pairing:
            return  # reported when pairing completes, as NimBLE's ENC_CHANGE does
        asyncio.create_task(self._report_security(connection))

    def _on_pairing(self, connection, keys) -> None:
        self.pairing = False
        self.passkey = None
        self.sim.log_ble('event', f'paired and bonded with {connection.peer_address}')
        asyncio.create_task(self._report_security(connection, bonded_hint=True))

    def _on_pairing_failure(self, connection, reason) -> None:
        self.pairing = False
        self.passkey = None
        self.sim.log_ble('event', f'pairing failed: reason {reason}')
        self.sim.mark_dirty()

    async def _report_security(self, connection, bonded_hint: bool = False) -> None:
        if connection is not self.conn or self.device is None:
            return
        bonded = bonded_hint
        try:
            all_keys = await self.device.keystore.get_all()
            self.bonds = len(all_keys)
            if not bonded:
                bonded = any(name == str(connection.peer_address) or
                             name == connection.peer_address.to_string(False) for name, _ in all_keys)
        except Exception:  # noqa: BLE001
            pass
        enc = connection.is_encrypted
        self.security = (enc, bool(enc and connection.authenticated), bool(enc and bonded))
        self.sim.send(ipc.ble_security(*self.security))
        e, a, b = self.security
        self.sim.log_ble('event', f'security: encrypted={int(e)} authenticated={int(a)} bonded={int(b)}')
        self.sim.mark_dirty()

    def _on_conn_params(self, connection) -> None:
        p = connection.parameters
        self.sim.log_ble('event', f'connection interval {p.connection_interval:.2f} ms, latency '
                                  f'{p.peripheral_latency}, timeout {p.supervision_timeout:.0f} ms')

    def _on_subscription(self, connection, characteristic, notify_enabled: bool, indicate_enabled: bool) -> None:
        conn = getattr(connection, 'connection', connection)
        if conn is not self.conn:
            return
        if characteristic is self.ctrl_char:
            which, enabled = 1, indicate_enabled
        elif characteristic is self.data_char:
            which, enabled = 2, notify_enabled
        else:
            return
        self.subscribed[which] = enabled
        self.sim.send(ipc.ble_subscribe(which, enabled))
        self.sim.log_ble('event', f'{"ctrl indications" if which == 1 else "data notifications"} '
                                  f'{"on" if enabled else "off"}')
        self.sim.mark_dirty()

    # ================================================================ GATT traffic
    async def _on_ctrl_write(self, connection, value: bytes) -> None:
        from bumble.att import ATT_Error

        self.sim.log_ble('rx', tlv.describe(value), raw_len=len(value))
        if connection is not self.conn or not self.sim.coin_connected:
            raise ATT_Error(0x0E)
        self._write_seq = (self._write_seq + 1) & 0xFFFF
        seq = self._write_seq
        fut = asyncio.get_running_loop().create_future()
        self._write_futs[seq] = fut
        self.sim.send(ipc.ble_write(seq, bytes(value)))
        try:
            result = await asyncio.wait_for(fut, WRITE_TIMEOUT_S)
        except (asyncio.TimeoutError, asyncio.CancelledError):
            raise ATT_Error(0x0E)
        finally:
            self._write_futs.pop(seq, None)
        if result:
            code = WRITE_RESULT_ATT.get(result, 0x0E)
            self.sim.log_ble('event', f'write refused: result {result} -> ATT error 0x{code:02x}')
            raise ATT_Error(code)

    async def _indicate(self, seq: int, value: bytes) -> None:
        conn = self.conn
        confirmed = False
        if conn is not None and self.device is not None:
            self.sim.log_ble('tx', tlv.describe(value), raw_len=len(value))
            try:
                # NimBLE's ble_gatts_indicate_custom does not check the CCCD; neither does force=True.
                await self.device.indicate_subscriber(conn, self.ctrl_char, value, force=True)
                confirmed = conn is self.conn
            except BaseException as e:  # noqa: BLE001 - timeout or disconnect
                self.sim.log_ble('event', f'indication not confirmed: {type(e).__name__}')
        self.sim.send(ipc.ble_ind_done(seq, confirmed))

    def _notify(self, value: bytes) -> None:
        from bumble import att

        conn = self.conn
        if conn is None or self.device is None:
            return  # no credit used: the coin's notify fails with "not connected"
        value = value[:max(0, conn.att_mtu - 3)]
        pdu = att.ATT_Handle_Value_Notification(attribute_handle=self.data_char.handle, attribute_value=value)
        self.device.gatt_server.send_gatt_pdu(conn, bytes(pdu))
        self.notify_count += 1
        self.notify_bytes += len(value)
        q = self._credit_queue
        if q is None:
            self.sim.send(ipc.ble_credit(1))
            return
        self._credit_targets.append(q.queued)
        self.credits_in_flight += 1
        self.sim.log_data(value)
        self._check_credits()

    def _check_credits(self) -> None:
        """Return a credit for every notification Bumble has handed to the controller."""
        q = self._credit_queue
        if q is None or not self._credit_targets:
            return
        packets = getattr(q, '_packets', None)
        handed = q.queued - len(packets) if packets is not None else q.completed
        n = 0
        while self._credit_targets and self._credit_targets[0] <= handed:
            self._credit_targets.popleft()
            n += 1
        if n:
            self.credits_in_flight -= n
            self.sim.send(ipc.ble_credit(n))

    # ================================================================ coin -> host
    def on_coin_hello(self, fw_version: str) -> None:
        self.fw_version = fw_version
        self.coin_ready = True
        # BLE_READY is sent right after HELLO even when the radio bridge is down; the coin's own radio
        # is always up. Its advertising is applied when the bridge comes up.
        self.sim.send(ipc.encode(ipc.BLE_READY))

    def on_coin_gone(self) -> None:
        """The coin process exited (sleep, reset, crash, off): its stack is gone."""
        self.coin_ready = False
        self.adv = ipc.Adv(False, 0, -1)
        self._cancel_adv_timer()
        if self.conn is not None:
            conn = self.conn
            self._link_down(HCI_CONNECTION_TIMEOUT, log_text='coin stopped: link dropped')
            asyncio.create_task(self._disconnect(conn, HCI_REMOTE_USER_TERMINATED))
        asyncio.create_task(self._apply_adv())

    def on_coin_message(self, t: int, p: bytes) -> None:
        if t == ipc.BLE_ADV:
            self.adv = ipc.decode_adv(p)
            self.sim.log_ble('event', f'advertising {"on" if self.adv.on else "off"}'
                             + (f', {self.adv.interval_ms:g} ms' if self.adv.on else '')
                             + (f', for {self.adv.duration_ms} ms' if self.adv.on and self.adv.duration_ms >= 0 else ''))
            asyncio.create_task(self._apply_adv())
        elif t == ipc.BLE_INDICATE:
            seq = struct.unpack_from('<H', p)[0]
            task = asyncio.create_task(self._indicate(seq, p[2:]))
            self._ind_tasks.add(task)
            task.add_done_callback(self._ind_tasks.discard)
        elif t == ipc.BLE_NOTIFY:
            self._notify(p)
        elif t == ipc.BLE_WRITE_RESULT:
            seq, result = struct.unpack_from('<HB', p)
            fut = self._write_futs.get(seq)
            if fut and not fut.done():
                fut.set_result(result)
        elif t == ipc.BLE_PASSKEY:
            passkey = struct.unpack_from('<I', p)[0]
            if self._passkey_fut and not self._passkey_fut.done():
                self._passkey_fut.set_result(passkey)
        elif t == ipc.BLE_DISCONNECT:
            if self.conn is not None:
                asyncio.create_task(self._disconnect(self.conn, HCI_REMOTE_USER_TERMINATED))
        elif t == ipc.BLE_FORGET:
            asyncio.create_task(self.forget_bonds())
        elif t == ipc.BLE_CONN_PARAMS:
            itvl_min, itvl_max, latency, timeout = struct.unpack_from('<HHHH', p)
            self.conn_params_req = (itvl_min * 1.25, itvl_max * 1.25, latency, timeout * 10)
            asyncio.create_task(self._update_conn_params())
        elif t == ipc.BLE_LINK_SETUP:
            asyncio.create_task(self._link_setup())
        elif t == ipc.BLE_TX_POWER:
            self.tx_power = struct.unpack_from('<b', p)[0]
        elif t == ipc.BLE_BATTERY:
            self.battery_pct = p[0]
            if self.device is not None and self.conn is not None:
                asyncio.create_task(self._safe(self.device.notify_subscribers(self.battery_char)))
        self.sim.mark_dirty()

    async def _safe(self, coro) -> None:
        try:
            await coro
        except Exception as e:  # noqa: BLE001
            log.debug('ignored: %s', e)

    async def _disconnect(self, connection, reason: int) -> None:
        try:
            await connection.disconnect(reason)
        except Exception as e:  # noqa: BLE001
            log.debug('disconnect: %s', e)

    async def _update_conn_params(self) -> None:
        conn, req = self.conn, self.conn_params_req
        if conn is None or self.device is None or req is None:
            return
        self.sim.log_ble('event', f'conn params requested: {req[0]:g}-{req[1]:g} ms, latency {req[2]}, timeout {req[3]} ms')
        try:
            await self.device.update_connection_parameters(conn, req[0], req[1], req[2], req[3])
        except Exception as e:  # noqa: BLE001
            self.sim.log_ble('event', f'conn params update failed: {e}')

    async def _link_setup(self) -> None:
        conn = self.conn
        if conn is None:
            return
        try:
            await conn.set_data_length(251, 2120)
        except Exception as e:  # noqa: BLE001
            log.debug('data length: %s', e)
        try:
            from bumble.hci import Phy

            await conn.set_phy(tx_phys=[Phy.LE_2M], rx_phys=[Phy.LE_2M])
        except Exception as e:  # noqa: BLE001
            log.debug('phy: %s', e)

    async def forget_bonds(self) -> None:
        if self.device is not None:
            try:
                await self.device.keystore.delete_all()
            except Exception as e:  # noqa: BLE001
                log.warning('forget: %s', e)
        elif self.keys_path.exists():
            self.keys_path.unlink()
        self.bonds = 0
        self.sim.log_ble('event', 'bonds deleted')
        self.sim.mark_dirty()

    # ================================================================ advertising
    def _cancel_adv_timer(self) -> None:
        if self._adv_timer:
            self._adv_timer.cancel()
            self._adv_timer = None
        self.adv_until = None

    async def _apply_adv(self) -> None:
        async with self._adv_lock:
            want = (self.adv.on and self.coin_ready and not self.out_of_range and self.conn is None
                    and self.device is not None)
            device = self.device
            self._cancel_adv_timer()
            if device is None:
                self.advertising = False
                return
            try:
                await device.stop_advertising()
                self.advertising = False
                if want:
                    from bumble import hci
                    from bumble.device import AdvertisingType

                    ms = self.adv.interval_ms
                    own = (hci.OwnAddressType.PUBLIC if self.own_address_type == 'public'
                           else hci.OwnAddressType.RANDOM)
                    await device.start_advertising(
                        advertising_type=AdvertisingType.UNDIRECTED_CONNECTABLE_SCANNABLE,
                        own_address_type=own, auto_restart=False, advertising_data=adv_data(),
                        scan_response_data=b'', advertising_interval_min=ms, advertising_interval_max=ms)
                    self.advertising = True
                    if self.adv.duration_ms >= 0:
                        self.adv_until = time.monotonic() + self.adv.duration_ms / 1000
                        self._adv_timer = asyncio.create_task(self._adv_expire(self.adv.duration_ms / 1000))
            except Exception as e:  # noqa: BLE001
                self.sim.log_ble('event', f'advertising error: {e}')
            self.sim.mark_dirty()

    async def _adv_expire(self, seconds: float) -> None:
        await asyncio.sleep(seconds)
        self._adv_timer = None
        self.adv_until = None
        if self.device is not None and self.conn is None:
            try:
                await self.device.stop_advertising()
            except Exception:  # noqa: BLE001
                pass
        self.advertising = False
        self.adv = ipc.Adv(False, self.adv.interval, -1)
        self.sim.send(ipc.encode(ipc.BLE_ADV_DONE))
        self.sim.log_ble('event', 'timed advertising phase ended')
        self.sim.mark_dirty()

    # ================================================================ simulated radio controls
    def set_rssi(self, rssi: int) -> None:
        rssi = max(-127, min(20, int(rssi)))
        if rssi != self.rssi:
            self.rssi = rssi
            if self.conn is not None:
                self.sim.send(ipc.ble_rssi(rssi))
            self.sim.mark_dirty()

    def set_out_of_range(self, on: bool) -> None:
        if on == self.out_of_range:
            return
        self.out_of_range = on
        self.sim.log_ble('event', 'out of range' if on else 'back in range')
        if on and self.conn is not None:
            conn = self.conn
            # the coin gets BLE_DISCONNECTED 0x08 (supervision timeout), as on a real link loss. The phone sees
            # 0x13: a Bumble host cannot make its controller go silent, only terminate the link.
            self._link_down(HCI_CONNECTION_TIMEOUT, log_text='link lost (out of range)')
            asyncio.create_task(self._disconnect(conn, HCI_REMOTE_USER_TERMINATED))
        asyncio.create_task(self._apply_adv())
        self.sim.mark_dirty()

    # ================================================================ UI
    def snapshot(self) -> dict:
        conn = self.conn
        return {
            'enabled': self.enabled,
            'transport': self.spec,
            'status': self.status,
            'error': self.error,
            'retry_in_s': round(max(0.0, self.next_retry - time.monotonic()), 1) if self.next_retry else None,
            'address': self.address,
            'address_type': self.own_address_type,
            'controller_address': self.controller_address,
            'coin_ready': self.coin_ready,
            'adv_requested': self.adv.on,
            'advertising': self.advertising,
            'adv_interval_ms': self.adv.interval_ms if self.adv.on else None,
            'adv_remaining_ms': int(max(0.0, self.adv_until - time.monotonic()) * 1000) if self.adv_until else None,
            'connected': conn is not None,
            'peer': str(conn.peer_address) if conn is not None else None,
            'mtu': self.mtu if conn is not None else None,
            'encrypted': self.security[0],
            'authenticated': self.security[1],
            'bonded': self.security[2],
            'subscribed_ctrl': self.subscribed[1],
            'subscribed_data': self.subscribed[2],
            'pairing': self.pairing,
            'passkey': f'{self.passkey:06d}' if self.passkey is not None else None,
            'bonds': self.bonds,
            'rssi': self.rssi,
            'out_of_range': self.out_of_range,
            'tx_power_dbm': self.tx_power,
            'battery_pct': self.battery_pct,
            'fw_version': self.fw_version,
            'credits_in_flight': self.credits_in_flight,
            'notifications': self.notify_count,
        }
