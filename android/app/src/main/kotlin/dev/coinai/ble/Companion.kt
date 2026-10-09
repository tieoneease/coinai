package dev.coinai.ble

import android.annotation.SuppressLint
import android.bluetooth.le.ScanFilter
import android.companion.AssociationInfo
import android.companion.AssociationRequest
import android.companion.BluetoothLeDeviceFilter
import android.companion.CompanionDeviceManager
import android.companion.CompanionDeviceService
import android.companion.DevicePresenceEvent
import android.companion.ObservingDevicePresenceRequest
import android.content.Context
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
     * runs once the user picked a coin and the association exists.
     */
    fun associate(
        context: Context,
        executor: Executor,
        launch: (IntentSender) -> Unit,
        onAssociated: (AssociationInfo) -> Unit,
        onError: (String) -> Unit,
    ) {
        val cdm = context.getSystemService(CompanionDeviceManager::class.java) ?: return onError("Companion devices not supported")
        cdm.associate(request(), executor, object : CompanionDeviceManager.Callback() {
            override fun onAssociationPending(intentSender: IntentSender) = launch(intentSender)
            override fun onAssociationCreated(associationInfo: AssociationInfo) {
                startObserving(context, associationInfo.id)
                bond(associationInfo)
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

    /** Starts bonding: the system shows the passkey dialog, the user types the digits the coin shows. */
    @SuppressLint("MissingPermission") // The UI requests BLUETOOTH_CONNECT before pairing.
    private fun bond(info: AssociationInfo) {
        val device = info.associatedDevice?.bleDevice?.device ?: return
        if (!device.createBond()) Log.w(TAG, "createBond refused for ${device.address}")
    }

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
        Log.i(CoinPairing.TAG, "coin $address present (event ${event.event})")
        SyncService.start(this, address)
    }
}
