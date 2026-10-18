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
    fun thinkingCarriesTheCleanedTranscript() = runTest {
        val coin = FakeCoin(backgroundScope)
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        // Folded to the coin's character set (\u2019 \u2192 ', \u00E9 \u2192 e, emoji and CJK dropped), then cut to 200 bytes.
        val transcript = "what\u2019s the caf\u00E9 \uD83D\uDE00 called " + "\u65E5".repeat(10) + "y".repeat(300)
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.THINKING, transcript))
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.DONE, "ok"))

        ReplyWindow().hold(coin, setOf(5), ch)

        val thinking = replies(coin).first()
        assertEquals(ReplyState.THINKING, thinking.state)
        assertTrue(thinking.text, thinking.text.startsWith("what's the cafe called yyy"))
        assertEquals(Protocol.MAX_REPLY_TEXT_BYTES, thinking.text.toByteArray().size)
        assertTrue(thinking.text.all { it in ' '..'~' })
    }

    private fun scene(kind: String, extra: String = "") =
        """{"say":"Here.","mood":"happy","card":{"kind":"$kind"$extra}}"""

    @Test
    fun sceneGoesOutInPartsAndEndsTheClip() = runTest {
        val coin = FakeCoin(backgroundScope).apply { showsScenes = true }
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        val long = scene("steps", ""","title":"Reset","steps":["${"a".repeat(200)}","${"b".repeat(200)}"]""")
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.THINKING, "reset the router"))
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.DONE, "Reset", scene = long))

        val delivered = ReplyWindow().hold(coin, setOf(5), ch, scenes = true)

        assertEquals(setOf(5), delivered)
        assertEquals(listOf(ReplyState.THINKING), replies(coin).map { it.state })
        val parts = coin.fromApp.filterIsInstance<CtrlMessage.Scene>()
        assertEquals(listOf(0, 1, 2), parts.map { it.part })
        assertTrue(parts.all { it.parts == 3 && it.clip == 5 })
        assertEquals(listOf(5 to long), coin.shownScenes)
        assertTrue("waited only the refusal grace", currentTime <= ReplyWindow.SCENE_GRACE.inWholeMilliseconds)
    }

    @Test
    fun refusedSceneGetsItsSummaryAsReply() = runTest {
        val coin = FakeCoin(backgroundScope).apply { showsScenes = true; refuseKinds = setOf("timer") }
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.DONE, "Tea\n3 min", scene = scene("timer", ""","label":"Tea","seconds":180""")))

        val delivered = ReplyWindow().hold(coin, setOf(5), ch, scenes = true)

        assertEquals(setOf(5), delivered)
        assertTrue(coin.shownScenes.isEmpty())
        assertTrue(coin.toApp.any { it is CtrlMessage.Error && it.requestType == dev.coinai.protocol.MessageType.SCENE && it.clip == 5 })
        val fallback = replies(coin).single()
        assertEquals(ReplyState.DONE, fallback.state)
        assertEquals(5, fallback.clip)
        assertEquals("Tea\n3 min", fallback.text)
        // The REPLY follows the SCENE.
        assertTrue(coin.fromApp.indexOf(fallback) > coin.fromApp.indexOfFirst { it is CtrlMessage.Scene })
    }

    @Test
    fun refusalForAnEarlierClipIsHandledWhileAnotherWaits() = runTest {
        val coin = FakeCoin(backgroundScope).apply { showsScenes = true; refuseKinds = setOf("timer") }
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        val held = async { ReplyWindow().hold(coin, setOf(5, 6), ch, scenes = true) }
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.DONE, "Tea – 3 min", scene = scene("timer", ""","label":"Tea","seconds":180""")))
        delay(5_000)
        assertEquals("Tea - 3 min", replies(coin).single().text)
        ch.send(CoinReply(coin.deviceAddress, 6, ReplyState.DONE, "done"))
        assertEquals(setOf(5, 6), held.await())
    }

    @Test
    fun coinWithoutScenesGetsTheSummary() = runTest {
        val coin = FakeCoin(backgroundScope)
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.DONE, "9\n20% of 45", scene = scene("stat", ""","value":"9","label":"20% of 45"""")))

        assertEquals(setOf(5), ReplyWindow().hold(coin, setOf(5), ch, scenes = false))

        assertTrue(coin.fromApp.none { it is CtrlMessage.Scene })
        assertEquals("9\n20% of 45", replies(coin).single().text)
        assertTrue(currentTime < 1000)
    }

    @Test
    fun sceneTooLongForEightPartsGetsTheSummary() = runTest {
        val coin = FakeCoin(backgroundScope).apply { showsScenes = true }
        val ch = Channel<CoinReply>(Channel.UNLIMITED)
        val huge = scene("answer", ""","title":"T","body":"${"x".repeat(Protocol.MAX_SCENE_PART_BYTES * Protocol.MAX_SCENE_PARTS)}"""")
        ch.send(CoinReply(coin.deviceAddress, 5, ReplyState.DONE, "T", scene = huge))

        assertEquals(setOf(5), ReplyWindow().hold(coin, setOf(5), ch, scenes = true))
        assertTrue(coin.fromApp.none { it is CtrlMessage.Scene })
        assertEquals("T", replies(coin).single().text)
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
