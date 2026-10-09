package dev.coinai.sync

import dev.coinai.clips.CoinReply
import dev.coinai.clips.PlainText
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.ReplyState
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.ReceiveChannel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.withTimeoutOrNull
import kotlin.time.Duration
import kotlin.time.Duration.Companion.seconds

/**
 * PROTOCOL.md rule 7: after the last ACK the coin stays connected for `reply_wait_s` and shows
 * `REPLY` progress. [hold] forwards [replies] for [clips] to the link as `REPLY` messages, with
 * the final text made plain and cut to the wire limit, until every clip has a `done` or `failed`
 * reply, the window closes, or the link drops.
 *
 * PROTOCOL-Q: the coin counts the window from its last ACK, the app from the end of the session,
 * so the app can hold on slightly past the coin. The app never sends CFG_SET, so the window is
 * the default 45 s.
 */
class ReplyWindow(
    private val window: Duration = DEFAULT,
    private val log: (String) -> Unit = {},
) {
    /** Returns the clips that got a final reply on the coin. */
    suspend fun hold(link: CoinLink, clips: Set<Int>, replies: ReceiveChannel<CoinReply>): Set<Int> = coroutineScope {
        val waiting = clips.toMutableSet()
        val delivered = mutableSetOf<Int>()
        val lost = async { link.state.first { it == LinkState.DISCONNECTED } }
        withTimeoutOrNull(window) {
            while (waiting.isNotEmpty()) {
                val reply = select<CoinReply?> {
                    replies.onReceiveCatching { it.getOrNull() }
                    lost.onAwait { null }
                } ?: break
                if (reply.device != link.deviceAddress || reply.clipId !in clips) continue
                val final = reply.state == ReplyState.DONE || reply.state == ReplyState.FAILED
                val text = if (final) PlainText.forCoin(reply.text) else ""
                try {
                    link.send(CtrlMessage.Reply.truncating(reply.clipId, reply.state, text))
                    if (final) delivered += reply.clipId
                } catch (e: LinkException) {
                    log("REPLY for clip ${reply.clipId} failed: ${e.message}")
                    break
                }
                if (final) waiting -= reply.clipId
            }
        }
        lost.cancel()
        delivered
    }

    companion object {
        val DEFAULT = 45.seconds
    }
}
