package dev.coinai.sync

import dev.coinai.clips.CoinReply
import dev.coinai.clips.PlainText
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.ErrorCode
import dev.coinai.protocol.MessageType
import dev.coinai.protocol.ReplyState
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.ReceiveChannel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.withTimeoutOrNull
import kotlin.time.Duration
import kotlin.time.Duration.Companion.seconds

/**
 * PROTOCOL.md rule 7: after the last ACK the coin stays connected for `reply_wait_s` and shows
 * `REPLY` progress. [hold] forwards [replies] for [clips] to the link as `REPLY` messages, with
 * the text (the transcript while `thinking`, the answer when final) made plain for the coin's
 * character set and cut to the wire limit, until every clip has a `done` or `failed` reply, the
 * window closes, or the link drops.
 *
 * A reply with a scene (PROTOCOL.md "Scenes") is the clip's final answer: it goes out as its
 * `SCENE` parts, in order, when the coin shows scenes. If the coin refuses it (`ERROR` 5 for
 * `SCENE` and the clip, which arrives within [sceneGrace] of the last part, also after the last
 * clip's answer), the clip gets a `REPLY` `done` with the card's summary instead. A coin that
 * does not show scenes, or a scene too long for 8 parts, gets that `REPLY` at once.
 *
 * The coin times its window from its last ACK and goes to Standby when it closes, which drops the
 * link and ends [hold]; [window] (from the end of the session) is the app's backstop. The app does
 * not set `reply_wait_s`, so the coin uses its stored value, 45 s unless something else set it.
 */
class ReplyWindow(
    private val window: Duration = DEFAULT,
    private val log: (String) -> Unit = {},
    private val sceneGrace: Duration = SCENE_GRACE,
) {
    /** Returns the clips that got a final reply on the coin. [scenes] is the coin's `STATUS` bit 3. */
    suspend fun hold(link: CoinLink, clips: Set<Int>, replies: ReceiveChannel<CoinReply>, scenes: Boolean = false): Set<Int> = coroutineScope {
        val waiting = clips.toMutableSet()
        val delivered = mutableSetOf<Int>()
        /** Scenes sent that the coin may still refuse: clip → the card's summary. */
        val unconfirmed = mutableMapOf<Int, String>()
        val lost = async { link.state.first { it == LinkState.DISCONNECTED } }
        val refused = Channel<Int>(Channel.UNLIMITED)
        // Subscribed before anything is sent: `incoming` is hot.
        val errors = launch(start = CoroutineStart.UNDISPATCHED) {
            link.ctrl.collect { m ->
                if (m is CtrlMessage.Error && m.requestType == MessageType.SCENE && m.code == ErrorCode.INVALID_VALUE) refused.send(m.clip)
            }
        }

        /** False when the link failed. */
        suspend fun send(message: CtrlMessage, clip: Int): Boolean = try {
            link.send(message)
            true
        } catch (e: LinkException) {
            log("${message::class.simpleName?.uppercase()} for clip $clip failed: ${e.message}")
            false
        }

        suspend fun reply(clip: Int, state: ReplyState, text: String): Boolean =
            send(CtrlMessage.Reply.truncating(clip, state, PlainText.forCoin(text)), clip)

        /** Handles one reply; false ends the window (the link failed). */
        suspend fun forward(r: CoinReply): Boolean {
            if (r.device != link.deviceAddress || r.clipId !in clips) return true
            val final = r.state == ReplyState.DONE || r.state == ReplyState.FAILED
            val parts = r.scene?.takeIf { scenes }?.let { json ->
                try {
                    CtrlMessage.Scene.split(r.clipId, json)
                } catch (e: IllegalArgumentException) {
                    log("scene for clip ${r.clipId} not sent: ${e.message}")
                    null
                }
            }
            if (parts != null) {
                for (p in parts) if (!send(p, r.clipId)) return false
                unconfirmed[r.clipId] = r.text
            } else if (!reply(r.clipId, r.state, r.text)) {
                return false
            }
            if (final) {
                delivered += r.clipId
                waiting -= r.clipId
            }
            return true
        }

        /** `ERROR` 5 for a scene: the coin cannot show it, so it gets the summary. False when the link failed. */
        suspend fun fallBack(clip: Int): Boolean {
            val summary = unconfirmed.remove(clip) ?: return true
            log("coin refused the scene for clip $clip; sending its summary as REPLY")
            return reply(clip, ReplyState.DONE, summary)
        }

        withTimeoutOrNull(window) {
            while (waiting.isNotEmpty()) {
                val goOn = select {
                    replies.onReceiveCatching { result -> result.getOrNull()?.let { forward(it) } ?: false }
                    refused.onReceive { fallBack(it) }
                    lost.onAwait { false }
                }
                if (!goOn) break
            }
        }
        // An accepted SCENE gets no answer; a refused one gets ERROR soon after its last part.
        // PROTOCOL-Q: the protocol gives no bound for that ERROR; SCENE_GRACE is the app's guess.
        if (unconfirmed.isNotEmpty() && link.state.value != LinkState.DISCONNECTED) {
            withTimeoutOrNull(sceneGrace) {
                while (unconfirmed.isNotEmpty()) {
                    val goOn = select {
                        refused.onReceive { fallBack(it) }
                        lost.onAwait { false }
                    }
                    if (!goOn) break
                }
            }
        }
        errors.cancel()
        lost.cancel()
        delivered
    }

    companion object {
        val DEFAULT = 45.seconds
        /** How long after a scene's last part a refusal is still expected. */
        val SCENE_GRACE = 2.seconds
    }
}
