package dev.coinai.ble

import android.Manifest
import android.app.Service
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.IBinder
import dev.coinai.app.CoinaiApp
import dev.coinai.app.Notifications
import dev.coinai.clips.CoinReply
import dev.coinai.sync.ReplyWindow
import dev.coinai.sync.SessionResult
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

/**
 * Foreground service (type connectedDevice) for one coin visit (PLAN.md D12): connect, run a
 * [dev.coinai.sync.SyncEngine] session, hold the link through the reply window forwarding
 * `REPLY`s (PROTOCOL.md rule 7), disconnect, stop. Started by [CoinPresenceService].
 *
 * Compiles; not yet run (no firmware BLE side, no phone in this task).
 */
class SyncService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var job: Job? = null
    private val app get() = application as CoinaiApp

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForeground must follow startForegroundService promptly, even when there is nothing to do.
        startForeground(Notifications.SYNC_ID, Notifications.sync(this, "Syncing with your coin"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        val address = intent?.getStringExtra(EXTRA_ADDRESS)
        if (job?.isActive == true) return START_NOT_STICKY
        if (address == null || checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            finish()
            return START_NOT_STICKY
        }
        job = scope.launch {
            try {
                visit(address)
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e
                app.log("sync with $address failed: ${e.message}")
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
        try {
            link.connectTo(adapter.getRemoteDevice(address))
            val result = app.syncEngine().runSession(link)
            app.log("session with $address: ${result.outcome}, received ${result.received.size}, acked ${result.acked}")
            if (result.outcome == SessionResult.Outcome.COMPLETE && result.received.isNotEmpty()) {
                ReplyWindow(log = app::log).hold(link, result.received.map { it.clip }.toSet(), replies)
            }
        } finally {
            collector.cancel()
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
