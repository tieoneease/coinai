"""Virtual BLE air for tests without the Android emulator: Bumble controllers on one LocalLink, each exposed as an
HCI tcp-server. The coin host connects to one (`--ble tcp-client:127.0.0.1:9101`), tools/central.py to the other.

    python sim/host/tools/vlink.py [--coin-port 9101] [--phone-port 9102]
"""

from __future__ import annotations

import argparse
import asyncio
import logging

from bumble import hci
from bumble.controller import Controller as BumbleController
from bumble.core import PhysicalTransport
from bumble.link import LocalLink
from bumble.transport import open_transport


class Link(LocalLink):
    """LocalLink stamps every LE ACL packet with the sender controller's *random* address, so a peripheral that
    advertised with its public address gets its packets dropped by the central ("no connection for ...").
    Use the address the sender's connection actually has."""

    def send_acl_data(self, sender_controller, destination_address, transport, data):
        if transport != PhysicalTransport.LE:
            return super().send_acl_data(sender_controller, destination_address, transport, data)
        conn = sender_controller.le_connections.get(destination_address)
        source = conn.self_address if conn is not None else sender_controller.random_address
        dest = self.find_le_controller(destination_address)
        if dest is not None:
            asyncio.get_running_loop().call_soon(lambda: dest.on_link_acl_data(source, transport, data))


class Controller(BumbleController):
    """Bumble's software controller lists LE Set PHY as supported but has no handler for it. Its fallback answers
    with return parameters, which the dispatcher drops for an asynchronous command, so the host gets no Command
    Status and its whole HCI command queue stalls until the 10 s timeout (seen as a 10 s pause in pairing, because
    the coin asks for 2M PHY as soon as it connects). Answer as a 2M-capable controller does."""

    def on_hci_le_set_phy_command(self, command: hci.HCI_LE_Set_PHY_Command) -> None:
        if self.find_connection_by_handle(command.connection_handle) is None:
            return self._send_hci_command_status(hci.HCI_ErrorCode.UNKNOWN_CONNECTION_IDENTIFIER_ERROR,
                                                 command.op_code)
        self._send_hci_command_status(hci.HCI_COMMAND_STATUS_PENDING, command.op_code)
        self.send_hci_packet(hci.HCI_LE_PHY_Update_Complete_Event(
            status=hci.HCI_SUCCESS, connection_handle=command.connection_handle,
            tx_phy=hci.HCI_LE_2M_PHY, rx_phy=hci.HCI_LE_2M_PHY))


async def main(args) -> None:
    link = Link()
    controllers = []
    for name, port, address in (('coin', args.coin_port, args.coin_address),
                                ('phone', args.phone_port, args.phone_address)):
        transport = await open_transport(f'tcp-server:127.0.0.1:{port}')
        controllers.append(Controller(name, host_source=transport.source, host_sink=transport.sink, link=link,
                                      public_address=address))
        print(f'vlink: {name} controller {address} on tcp-server:127.0.0.1:{port}', flush=True)
    print('vlink: ready', flush=True)
    await asyncio.Event().wait()


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--coin-port', type=int, default=9101)
    ap.add_argument('--phone-port', type=int, default=9102)
    ap.add_argument('--coin-address', default='F4:00:00:00:00:01',
                    help='public address of the coin-side controller. The default differs from the coin\'s address, '
                         'as with netsim, so the host advertises a static random address; pass C0:1A:51:00:00:01 '
                         'to exercise public-address advertising')
    ap.add_argument('--phone-address', default='F0:F1:F2:F3:F4:F5')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.WARNING)
    try:
        asyncio.run(main(a))
    except KeyboardInterrupt:
        pass
