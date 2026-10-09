package dev.coinai.app

import android.app.Application
import android.util.Log
import dev.coinai.clips.ClipProcessor
import dev.coinai.clips.CoinReply
import dev.coinai.clips.ProcessingSettings
import dev.coinai.clips.ReplySink
import dev.coinai.data.ClipDatabase
import dev.coinai.data.ClipRepository
import dev.coinai.protocol.CtrlMessage
import dev.coinai.sambot.SambotClient
import dev.coinai.settings.SambotSettings
import dev.coinai.settings.SettingsStore
import dev.coinai.sync.ReceivedClip
import dev.coinai.sync.SyncEngine
import dev.coinai.sync.SyncListener
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * The hook from processing to a live coin link: [dev.coinai.ble.SyncService] collects [replies]
 * while it holds the link and forwards them as `REPLY`. Nothing is kept for a coin that is gone.
 */
class ReplyBus : ReplySink {
    private val flow = MutableSharedFlow<CoinReply>(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    val replies: SharedFlow<CoinReply> = flow
    override fun offer(reply: CoinReply) {
        flow.tryEmit(reply)
    }
}

/** Process-wide objects. */
class CoinaiApp : Application() {
    val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    val http: OkHttpClient by lazy {
        OkHttpClient.Builder().connectTimeout(15, TimeUnit.SECONDS).readTimeout(60, TimeUnit.SECONDS).build()
    }
    val settings: SettingsStore by lazy { SettingsStore.create(this) }
    val sambotSettings: StateFlow<SambotSettings> by lazy {
        settings.sambot.stateIn(appScope, SharingStarted.Eagerly, SambotSettings(SambotClient.DEFAULT_BASE_URL, ""))
    }
    val clips: ClipRepository by lazy { ClipRepository(ClipDatabase.open(this), File(filesDir, "clips")) }
    val replyBus = ReplyBus()

    val processor: ClipProcessor by lazy {
        ClipProcessor(
            store = clips,
            sambot = {
                val s = settings.sambot.first()
                SambotClient(s.baseUrl, s.token, http)
            },
            settings = { ProcessingSettings(voicedThresholdMs = settings.voicedThresholdMs.first()) },
            notifier = ReplyNotifications(this) { sambotSettings.value.baseUrl },
            replies = replyBus,
        )
    }

    fun syncEngine(): SyncEngine = SyncEngine(
        clips,
        listener = object : SyncListener {
            override fun onStatus(device: String, status: CtrlMessage.Status) {
                appScope.launch { settings.saveDeviceStatus(device, status, System.currentTimeMillis()) }
            }

            override fun onClipReceived(clip: ReceivedClip) = ClipWorker.enqueue(this@CoinaiApp, clip.rowId)
            override fun onLog(device: String, message: String) = log("$device: $message")
        },
    )

    /** Manual resend from the clip list. */
    suspend fun resend(clipRowId: Long): Boolean {
        if (!processor.resend(clipRowId)) return false
        ClipWorker.enqueue(this, clipRowId, replace = true)
        return true
    }

    fun log(message: String) {
        Log.i(TAG, message)
    }

    override fun onCreate() {
        super.onCreate()
        Notifications.createChannels(this)
        // Clips left mid-pipeline (app update, process death) get their work scheduled again.
        // KEEP policy: clips that still have queued work are not doubled.
        appScope.launch { clips.pendingProcessing().forEach { ClipWorker.enqueue(this@CoinaiApp, it) } }
    }

    companion object {
        const val TAG = "coinai"
    }
}
