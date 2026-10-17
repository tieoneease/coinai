package dev.coinai.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.content.Context
import android.util.Log
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.DataPacket
import dev.coinai.protocol.Protocol
import dev.coinai.protocol.ProtocolException
import dev.coinai.sync.CoinLink
import dev.coinai.sync.Inbound
import dev.coinai.sync.LinkException
import dev.coinai.sync.LinkState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import no.nordicsemi.android.ble.BleManager
import no.nordicsemi.android.ble.ConnectionPriorityRequest
import no.nordicsemi.android.ble.PhyRequest
import no.nordicsemi.android.ble.ktx.suspend
import no.nordicsemi.android.ble.observer.ConnectionObserver

/**
 * The coin over GATT (PROTOCOL.md "Link" and "GATT") with Nordic's Android-BLE-Library (PLAN.md
 * D11). Not yet run against hardware: the firmware's BLE side does not exist.
 *
 * On connect: bond (the system asks for the passkey the coin displays), MTU 247, 2M PHY, high
 * connection priority, then indications on `ctrl` and notifications on `data`.
 */
@SuppressLint("MissingPermission") // SyncService checks BLUETOOTH_CONNECT before creating this.
class CoinBleManager(context: Context, override val deviceAddress: String) : BleManager(context), CoinLink {
    private var ctrl: BluetoothGattCharacteristic? = null
    private var data: BluetoothGattCharacteristic? = null

    private val stateFlow = MutableStateFlow(LinkState.DISCONNECTED)
    override val state: StateFlow<LinkState> = stateFlow

    // Callbacks arrive in order on one thread; one flow keeps ctrl and data in that order. A full
    // buffer drops packets, which the sync engine repairs with a resume GET.
    private val inbound = MutableSharedFlow<Inbound>(extraBufferCapacity = 4096)
    override val incoming: Flow<Inbound> = inbound

    init {
        setConnectionObserver(object : ConnectionObserver {
            override fun onDeviceConnecting(device: BluetoothDevice) { stateFlow.value = LinkState.CONNECTING }
            override fun onDeviceConnected(device: BluetoothDevice) { stateFlow.value = LinkState.CONNECTING }
            override fun onDeviceFailedToConnect(device: BluetoothDevice, reason: Int) { stateFlow.value = LinkState.DISCONNECTED }
            override fun onDeviceReady(device: BluetoothDevice) { stateFlow.value = LinkState.READY }
            override fun onDeviceDisconnecting(device: BluetoothDevice) { stateFlow.value = LinkState.DISCONNECTED }
            override fun onDeviceDisconnected(device: BluetoothDevice, reason: Int) { stateFlow.value = LinkState.DISCONNECTED }
        })
    }

    override fun isRequiredServiceSupported(gatt: BluetoothGatt): Boolean {
        val service = gatt.getService(Protocol.SERVICE_UUID) ?: return false
        ctrl = service.getCharacteristic(Protocol.CTRL_UUID)?.takeIf {
            it.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 &&
                it.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0
        }
        data = service.getCharacteristic(Protocol.DATA_UUID)?.takeIf {
            it.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0
        }
        return ctrl != null && data != null
    }

    override fun initialize() {
        val ctrl = ctrl ?: return
        val data = data ?: return
        // Characteristic values need authenticated encryption (PROTOCOL.md "Security"), so Android
        // encrypts with the stored keys when it first touches them. Without a bond this shows the
        // passkey dialog; the coin displays the 6 digits. Not ensureBond(): with a bond present it
        // removes the bond and pairs again, so every visit after the first dropped the link and
        // asked for a new passkey (seen on the emulator, 2026-10-17).
        createBondInsecure().enqueue()
        // PROTOCOL.md "Link": the data length update to 251 is the controller's job; Android has no
        // API to request it. The coin asks for its 15 ms interval itself during transfer.
        requestMtu(Protocol.MAX_TLV_SIZE + ATT_HEADER).enqueue()
        setPreferredPhy(PhyRequest.PHY_LE_2M_MASK, PhyRequest.PHY_LE_2M_MASK, PhyRequest.PHY_OPTION_NO_PREFERRED).enqueue()
        requestConnectionPriority(ConnectionPriorityRequest.CONNECTION_PRIORITY_HIGH).enqueue()
        setIndicationCallback(ctrl).with { _, value ->
            val bytes = value.value ?: return@with
            try {
                inbound.tryEmit(Inbound.Ctrl(CtrlMessage.decode(bytes)))
            } catch (e: ProtocolException) {
                log(Log.WARN, "malformed ctrl message: ${e.message}")
            }
        }
        enableIndications(ctrl).enqueue()
        setNotificationCallback(data).with { _, value ->
            val bytes = value.value ?: return@with
            try {
                inbound.tryEmit(Inbound.Data(DataPacket.decode(bytes)))
            } catch (e: ProtocolException) {
                log(Log.WARN, "malformed data packet: ${e.message}")
            }
        }
        enableNotifications(data).enqueue()
    }

    override fun onServicesInvalidated() {
        ctrl = null
        data = null
    }

    /** Connects and runs [initialize]; returns when the link is ready. */
    suspend fun connectTo(device: BluetoothDevice) {
        connect(device)
            .useAutoConnect(false)
            .retry(3, 200)
            .timeout(CONNECT_TIMEOUT_MS)
            .suspend()
    }

    override suspend fun send(message: CtrlMessage) {
        val ctrl = ctrl ?: throw LinkException("not connected")
        try {
            writeCharacteristic(ctrl, message.encode(), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT).suspend()
        } catch (e: Exception) {
            if (e is kotlinx.coroutines.CancellationException) throw e
            throw LinkException("ctrl write failed: ${e.message}", e)
        }
    }

    override fun getMinLogPriority(): Int = Log.INFO

    override fun log(priority: Int, message: String) {
        Log.println(priority, TAG, message)
    }

    private companion object {
        const val TAG = "CoinBle"
        /** ATT_MTU 247 = 244-byte TLV + 3-byte ATT header (PROTOCOL.md rule 6). */
        const val ATT_HEADER = 3
        const val CONNECT_TIMEOUT_MS = 15_000L
    }
}
