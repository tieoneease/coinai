package dev.coinai.app

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import dev.coinai.MainActivity
import dev.coinai.clips.ClipRecord
import dev.coinai.clips.PlainText
import dev.coinai.clips.ReplyNotifier
import dev.coinai.clips.SceneSummary

object Notifications {
    const val CHANNEL_REPLIES = "replies"
    const val CHANNEL_SYNC = "sync"
    const val SYNC_ID = 1
    private const val PREVIEW_CHARS = 600

    fun createChannels(context: Context) {
        val nm = context.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannels(
            listOf(
                NotificationChannel(CHANNEL_REPLIES, "SamBot replies", NotificationManager.IMPORTANCE_DEFAULT),
                NotificationChannel(CHANNEL_SYNC, "Coin sync", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Shown while the app talks to the coin"
                },
            ),
        )
    }

    /** The foreground-service notification for [dev.coinai.ble.SyncService]. */
    fun sync(context: Context, text: String): Notification =
        Notification.Builder(context, CHANNEL_SYNC)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("coinai")
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(
                PendingIntent.getActivity(context, 0, Intent(context, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE),
            )
            .build()

    fun canPost(context: Context): Boolean =
        context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

    /** A reply notification; tapping opens the SamBot web client at [baseUrl] with ACTION_VIEW. */
    fun reply(context: Context, clip: ClipRecord, reply: String, baseUrl: String): Notification {
        val preview = PlainText.fromMarkdown(reply).ifBlank { "(no text in the reply)" }.take(PREVIEW_CHARS)
        val open = Intent(Intent.ACTION_VIEW, Uri.parse(baseUrl)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        return Notification.Builder(context, CHANNEL_REPLIES)
            .setSmallIcon(android.R.drawable.stat_notify_chat)
            .setContentTitle(clip.transcript?.let { "SamBot: $it" }?.take(80) ?: "SamBot replied")
            .setContentText(preview)
            .setStyle(Notification.BigTextStyle().bigText(preview))
            .setAutoCancel(true)
            .setContentIntent(PendingIntent.getActivity(context, clip.id.toInt(), open, PendingIntent.FLAG_IMMUTABLE))
            .build()
    }

    /** A hand-off that did not reach SamBot; the coin still shows "SamBot is on it". Opens the app. */
    fun handoffFailed(context: Context, clip: ClipRecord, reason: String): Notification {
        val task = clip.scene?.let(SceneSummary::laterText)?.takeIf { it.isNotBlank() } ?: clip.handoff ?: clip.transcript.orEmpty()
        val text = "$task\n$reason".trim()
        return Notification.Builder(context, CHANNEL_REPLIES)
            .setSmallIcon(android.R.drawable.stat_notify_error)
            .setContentTitle("SamBot didn't get it")
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setAutoCancel(true)
            .setContentIntent(
                PendingIntent.getActivity(context, clip.id.toInt(), Intent(context, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE),
            )
            .build()
    }
}

class ReplyNotifications(private val context: Context, private val baseUrl: () -> String) : ReplyNotifier {
    override fun replyReady(clip: ClipRecord, reply: String) {
        if (!Notifications.canPost(context)) return
        context.getSystemService(NotificationManager::class.java)
            .notify(REPLY_TAG, clip.id.toInt(), Notifications.reply(context, clip, reply, baseUrl()))
    }

    override fun handoffFailed(clip: ClipRecord, reason: String) {
        if (!Notifications.canPost(context)) return
        context.getSystemService(NotificationManager::class.java)
            .notify(REPLY_TAG, clip.id.toInt(), Notifications.handoffFailed(context, clip, reason))
    }

    private companion object {
        const val REPLY_TAG = "reply"
    }
}
