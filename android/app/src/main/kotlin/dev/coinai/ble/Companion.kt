package dev.coinai.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanFilter
import android.companion.AssociationInfo
import android.companion.AssociationRequest
import android.companion.BluetoothLeDeviceFilter
import android.companion.CompanionDeviceManager
import android.companion.CompanionDeviceService
import android.companion.DevicePresenceEvent
import android.companion.ObservingDevicePresenceRequest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.IntentSender
import android.content.pm.PackageManager
import android.os.ParcelUuid
import android.util.Log
import dev.coinai.protocol.Protocol
import java.util.concurrent.Executor

/**
 * Companion Device Manager pairing and presence (PLAN.md "Pairing", D12), Android 16 (API 36) APIs
 * only: association filtered on the coinai service UUID, then
 * [CompanionDeviceManager.startObservingDevicePresence] with an [ObservingDevicePresenceRequest],
 * which makes the system bind [CoinPresenceService] when the coin is in range.
 */
object CoinPairing {
    fun isSupported(context: Context): Boolean =
        context.packageManager.hasSystemFeature(PackageManager.FEATURE_COMPANION_DEVICE_SETUP)

    fun associations(context: Context): List<AssociationInfo> =
        context.getSystemService(CompanionDeviceManager::class.java)?.myAssociations.orEmpty()

    private fun request(): AssociationRequest =
        AssociationRequest.Builder()
            .addDeviceFilter(
                BluetoothLeDeviceFilter.Builder()
                    .setScanFilter(ScanFilter.Builder().setServiceUuid(ParcelUuid(Protocol.SERVICE_UUID)).build())
                    .build(),
            )
            .setSingleDevice(false)
            .build()

    /**
     * Shows the system device chooser. [launch] starts the chooser's intent sender; [onAssociated]
     * runs once the user picked a coin and the association exists; [onBonded] once bonding ended,
     * true when the bond exists (a failed bond also removes the association: without a bond the
     * coin answers nothing).
     */
    fun associate(
        context: Context,
        executor: Executor,
        launch: (IntentSender) -> Unit,
        onAssociated: (AssociationInfo) -> Unit,
        onBonded: (Boolean) -> Unit,
        onError: (String) -> Unit,
    ) {
        val cdm = context.getSystemService(CompanionDeviceManager::class.java) ?: return onError("Companion devices not supported")
        cdm.associate(request(), executor, object : CompanionDeviceManager.Callback() {
            override fun onAssociationPending(intentSender: IntentSender) = launch(intentSender)
            override fun onAssociationCreated(associationInfo: AssociationInfo) {
                startObserving(context, associationInfo.id)
                bond(context, associationInfo, onBonded)
                onAssociated(associationInfo)
            }
            override fun onFailure(error: CharSequence?) = onError(error?.toString() ?: "association failed")
        })
    }

    fun startObserving(context: Context, associationId: Int) {
        context.getSystemService(CompanionDeviceManager::class.java)?.startObservingDevicePresence(
            ObservingDevicePresenceRequest.Builder().setAssociationId(associationId).build(),
        )
    }

    /**
     * Starts bonding: the system shows the passkey dialog, the user types the digits the coin
     * shows. The first sync starts once the bond exists. Presence reports the coin as soon as the
     * pairing link is up, and a sync started then waits on the passkey dialog until its connect
     * timeout gives up; the coin stays present, so nothing would start another
     * (seen on the emulator, 2026-10-17).
     */
    @SuppressLint("MissingPermission") // The UI requests BLUETOOTH_CONNECT before pairing.
    private fun bond(context: Context, info: AssociationInfo, onBonded: (Boolean) -> Unit) {
        val device = info.associatedDevice?.bleDevice?.device ?: return
        val address = address(info) ?: return
        val app = context.applicationContext
        if (device.bondState == BluetoothDevice.BOND_BONDED) {
            onBonded(true)
            SyncService.start(app, address)
            return
        }
        // A protected system broadcast, so no export flag (as SyncService's time zone receiver).
        app.registerReceiver(object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                val changed = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                if (changed?.address != device.address) return
                when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)) {
                    BluetoothDevice.BOND_BONDED -> {
                        app.unregisterReceiver(this)
                        Log.i(TAG, "bonded with $address")
                        onBonded(true)
                        SyncService.start(app, address)
                    }
                    BluetoothDevice.BOND_NONE -> {
                        app.unregisterReceiver(this)
                        Log.w(TAG, "bonding with $address failed or was cancelled; removing the association")
                        app.getSystemService(CompanionDeviceManager::class.java)?.disassociate(info.id)
                        onBonded(false)
                    }
                }
            }
        }, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
        // false while a bond is already in progress; the receiver still sees it finish.
        if (!device.createBond()) Log.w(TAG, "createBond refused for $address (bond state ${device.bondState})")
    }

    @SuppressLint("MissingPermission") // Callers check BLUETOOTH_CONNECT.
    fun isBonding(context: Context, address: String): Boolean =
        context.getSystemService(BluetoothManager::class.java)?.adapter?.getRemoteDevice(address)?.bondState ==
            BluetoothDevice.BOND_BONDING

    fun address(info: AssociationInfo): String? = info.deviceMacAddress?.toString()?.uppercase()

    const val TAG = "CoinPairing"
}

/**
 * Bound by the system while an associated coin is in BLE range (presence observation). On
 * appearance it starts [SyncService]; the app holds
 * REQUEST_COMPANION_START_FOREGROUND_SERVICES_FROM_BACKGROUND, which exempts this start from the
 * background foreground-service restriction.
 */
class CoinPresenceService : CompanionDeviceService() {
    override fun onDevicePresenceEvent(event: DevicePresenceEvent) {
        if (event.event != DevicePresenceEvent.EVENT_BLE_APPEARED && event.event != DevicePresenceEvent.EVENT_BT_CONNECTED) return
        val info = CoinPairing.associations(this).firstOrNull { it.id == event.associationId } ?: return
        val address = CoinPairing.address(info) ?: return
        if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            Log.w(CoinPairing.TAG, "coin $address present, but Nearby devices permission is not granted")
            return
        }
        if (CoinPairing.isBonding(this, address)) {
            Log.i(CoinPairing.TAG, "coin $address present while bonding; the sync starts once bonded")
            return
        }
        Log.i(CoinPairing.TAG, "coin $address present (event ${event.event})")
        SyncService.start(this, address)
    }
}
