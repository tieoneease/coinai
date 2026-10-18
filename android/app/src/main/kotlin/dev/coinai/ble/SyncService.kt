package dev.coinai.ble

import android.app.Service
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.os.IBinder
import dev.coinai.app.CoinaiApp
import dev.coinai.app.Notifications
import dev.coinai.clips.CoinReply
import dev.coinai.sync.CoinClock
import dev.coinai.sync.LinkException
import dev.coinai.sync.ReplyWindow
import dev.coinai.sync.SessionResult
import dev.coinai.sync.followDelivery
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import no.nordicsemi.android.ble.ktx.suspend
import java.util.TimeZone

/**
 * Foreground service (type connectedDevice) for one coin visit (PLAN.md D12): connect, run a
 * [dev.coinai.sync.SyncEngine] session, hold the link through the reply window forwarding
 * `REPLY`s and `SCENE`s (PROTOCOL.md rule 7, "Scenes"), disconnect, stop. Started by [CoinPresenceService]. A time zone
 * change during the visit is sent to the coin at once (rule 8); the engine sends the offset at the
 * start of every session anyway. The delivery order goes out the same way: the engine sends it
 * after `HELLO` and on change during the session, and this service on change during the reply
 * window.
 *
 * Compiles; not yet run (no firmware BLE side, no phone in this task).
 */
class SyncService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var job: Job? = null
    /** A start arrived during the visit: the coin is (still) there, so a failed visit runs again. */
    private var startedAgain = false
    private val app get() = application as CoinaiApp

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForeground must follow startForegroundService promptly, even when there is nothing
        // to do. The connectedDevice type needs BLUETOOTH_CONNECT; without it the call throws.
        try {
            startForeground(Notifications.SYNC_ID, Notifications.sync(this, "Syncing with your coin"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } catch (e: SecurityException) {
            app.log("cannot sync: ${e.message}")
            stopSelf()
            return START_NOT_STICKY
        }
        val address = intent?.getStringExtra(EXTRA_ADDRESS)
        if (job?.isActive == true) {
            startedAgain = true
            return START_NOT_STICKY
        }
        if (address == null) {
            finish()
            return START_NOT_STICKY
        }
        job = scope.launch {
            try {
                // Once more at most per extra start: a bond that completes while the first visit is
                // timing out on the passkey dialog starts the service again (CoinPairing.bond).
                do {
                    startedAgain = false
                    val failed = try {
                        visit(address)
                        false
                    } catch (e: Exception) {
                        if (e is kotlinx.coroutines.CancellationException) throw e
                        app.log("sync with $address failed: ${e.message}")
                        true
                    }
                } while (failed && startedAgain)
            } finally {
                finish()
            }
        }
        return START_NOT_STICKY
    }

    private fun finish() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private suspend fun visit(address: String) {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter ?: return
        val link = CoinBleManager(this, address)
        // Subscribe before the session: processing can report progress while the session runs.
        val replies = Channel<CoinReply>(Channel.UNLIMITED)
        val collector = scope.launch(start = CoroutineStart.UNDISPATCHED) {
            app.replyBus.replies.filter { it.device == address }.collect { replies.send(it) }
        }
        // Context-registered for the visit only; a system broadcast, so no export flag.
        val zoneChanges = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                val zone = intent.getStringExtra(Intent.EXTRA_TIMEZONE)?.let(TimeZone::getTimeZone) ?: TimeZone.getDefault()
                val message = CoinClock.offsetMessage(System.currentTimeMillis(), zone) ?: return
                scope.launch {
                    try {
                        link.send(message)
                    } catch (e: LinkException) {
                        app.log("UTC offset to $address failed: ${e.message}")
                    }
                }
            }
        }
        var registered = false
        try {
            link.connectTo(adapter.getRemoteDevice(address))
            registerReceiver(zoneChanges, IntentFilter(Intent.ACTION_TIMEZONE_CHANGED))
            registered = true
            val result = app.syncEngine().runSession(link)
            app.log("session with $address: ${result.outcome}, received ${result.received.size}, acked ${result.acked}")
            if (result.outcome == SessionResult.Outcome.COMPLETE && result.received.isNotEmpty()) {
                // The link stays up for the reply window: a delivery order changed now still
                // reaches the coin at once, as during the session.
                val follower = result.delivery?.let { sent ->
                    scope.launch { followDelivery(link, app.settings.delivery, sent, log = { app.log("$address: $it") }) }
                }
                try {
                    ReplyWindow(log = app::log).hold(link, result.received.map { it.clip }.toSet(), replies, scenes = result.status?.scenes == true)
                } finally {
                    follower?.cancel()
                }
            }
        } finally {
            collector.cancel()
            if (registered) unregisterReceiver(zoneChanges)
            withContext(NonCancellable) {
                runCatching { link.disconnect().suspend() }
                link.close()
            }
        }
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }

    companion object {
        const val EXTRA_ADDRESS = "address"

        fun start(context: Context, address: String) {
            context.startForegroundService(Intent(context, SyncService::class.java).putExtra(EXTRA_ADDRESS, address))
        }
    }
}
