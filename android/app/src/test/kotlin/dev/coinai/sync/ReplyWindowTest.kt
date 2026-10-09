package dev.coinai.sync

import dev.coinai.clips.CoinReply
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.Protocol
import dev.coinai.protocol.ReplyState
import dev.coinai.testing.FakeCoin
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.test.currentTime
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.time.Duration.Companion.seconds

@OptIn(ExperimentalCoroutinesApi::class)
class ReplyWindowTest {
    private fun replies(coin: FakeCoin) = coin.fromApp.filterIsInstance<CtrlMessage.Reply>()

    @Test
    fun forwardsProgressAndPlainFinalTextThenLeaves() = runTest {
        val coin = FakeCoin(backgroundScope)
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.TRANSCRIBING, ""))
        ch.send(CoinReply("other-coin", 5, ReplyState.DONE, "not for this coin"))
        ch.send(CoinReply(coin.deviceAddress, 9, ReplyState.DONE, "not from this visit"))
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.THINKING, ""))
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.DONE, "**Done** — it’s " + "x".repeat(300)))

        val delivered = ReplyWindow().hold(coin, setOf(5), ch)

        assertEquals(setOf(5), delivered)
        val sent = replies(coin)
        assertEquals(listOf(ReplyState.TRANSCRIBING, ReplyState.THINKING, ReplyState.DONE), sent.map { it.state })
        assertTrue(sent.all { it.clip == 5 })
        val text = sent.last().text
        assertTrue(text, text.startsWith("Done - it's xxx"))
        assertEquals(Protocol.MAX_REPLY_TEXT_BYTES, text.toByteArray().size)
        assertTrue("left as soon as the reply was final", currentTime < 1000)
    }

    @Test
    fun windowClosesAfterTheDefault() = runTest {
        val coin = FakeCoin(backgroundScope)
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        val delivered = ReplyWindow().hold(coin, setOf(1), ch)
        assertTrue(delivered.isEmpty())
        assertEquals(45.seconds.inWholeMilliseconds, currentTime)
    }

    @Test
    fun linkLossEndsTheWindow() = runTest {
        val coin = FakeCoin(backgroundScope)
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        val held = async { ReplyWindow().hold(coin, setOf(1, 2), ch) }
        ch.send(CoinReply(coin.deviceAddress, 1, ReplyState.FAILED, "SamBot: agent is not running"))
        delay(3_000)
        coin.disconnect()
        assertEquals(setOf(1), held.await())
        assertEquals(3_000, currentTime)
        assertEquals("SamBot: agent is not running", replies(coin).single().text)
    }
}
