package dev.coinai.sync

import dev.coinai.clips.ClipState
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.ErrorCode
import dev.coinai.protocol.MessageType
import dev.coinai.protocol.Protocol
import dev.coinai.testing.FakeCoin
import dev.coinai.testing.MemoryClips
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.zip.CRC32
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.random.Random

@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
class SyncEngineTest {
    @get:Rule val tmp = TemporaryFolder()

    private val statuses = mutableListOf<CtrlMessage.Status>()
    private val receivedEvents = mutableListOf<ReceivedClip>()
    private val logs = mutableListOf<String>()
    private val listener = object : SyncListener {
        override fun onStatus(device: String, status: CtrlMessage.Status) { statuses += status }
        override fun onClipReceived(clip: ReceivedClip) { receivedEvents += clip }
        override fun onLog(device: String, message: String) { logs += message }
    }

    private fun bytes(n: Int, seed: Int = n) = Random(seed).nextBytes(n)
    private fun crc(b: ByteArray) = CRC32().apply { update(b) }.value

    private fun TestScope.engine(store: ClipStore) =
        SyncEngine(store, SyncConfig(), listener, clock = { testScheduler.currentTime }, io = EmptyCoroutineContext)

    private fun MemoryClips.row(clip: Int) = rows.values.filter { it.clipId == clip }.maxBy { it.id }

    private fun assertDelivered(store: MemoryClips, coin: FakeCoin, clip: Int, expected: ByteArray) {
        val row = store.row(clip)
        assertEquals(ClipState.RECEIVED, row.state)
        assertArrayEquals("file content of clip $clip", expected, File(row.filePath!!).readBytes())
        assertEquals(expected.size.toLong(), row.size)
        assertEquals(crc(expected), row.crc32)
        assertNotNull("ACK recorded", row.ackedAtMs)
        assertTrue("ACK for $clip with the file's CRC", coin.acks.contains(CtrlMessage.Ack(clip, crc(expected))))
        assertFalse("coin deleted clip $clip", clip in coin.clips)
    }

    @Test
    fun helloListGetAckForEveryClip() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val a = bytes(5000)
        val b = bytes(238 * 3) // an exact multiple of the packet payload
        coin.addClip(3, a, durationMs = 6020, voicedMs = 4180)
        coin.addClip(4, b)

        val result = engine(store).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        assertEquals(CtrlMessage.Hello::class, coin.fromApp.first()::class)
        assertEquals(CtrlMessage.ListClips, coin.fromApp[1])
        assertEquals(87, statuses.single().batteryPct)
        assertDelivered(store, coin, 3, a)
        assertDelivered(store, coin, 4, b)
        assertEquals(listOf(3, 4), result.acked)
        assertEquals(listOf(3, 4), receivedEvents.map { it.clip })
        assertEquals(6020L, store.row(3).durationMs)
        assertEquals(4180L, store.row(3).voicedMs)
        assertEquals(listOf(CtrlMessage.Get(3, 0), CtrlMessage.Get(4, 0)), coin.gets)
        // ACK only after the clip was committed: the file exists under its final name.
        assertTrue(File(store.row(3).filePath!!).exists())
        assertFalse(File(tmp.root, "3-1.part").exists())
    }

    @Test
    fun disconnectMidTransferKeepsOffsetAndResumes() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(5000)
        coin.addClip(7, clip)
        coin.disconnectAfterPackets = 8

        val first = engine(store).runSession(coin)
        assertEquals(SessionResult.Outcome.DISCONNECTED, first.outcome)
        val saved = store.row(7).receivedBytes
        assertTrue("progress saved: $saved", saved > 0 && saved <= 8L * Protocol.MAX_DATA_PAYLOAD)
        assertEquals(ClipState.RECEIVING, store.row(7).state)
        assertTrue(coin.acks.isEmpty())
        assertTrue(7 in coin.clips)

        coin.reconnect()
        val second = engine(store).runSession(coin)
        assertEquals(SessionResult.Outcome.COMPLETE, second.outcome)
        assertEquals(CtrlMessage.Get(7, saved), coin.gets.last())
        assertDelivered(store, coin, 7, clip)
    }

    @Test
    fun duplicateAndReorderedPacketsNeedNoRetransmit() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(4000)
        coin.addClip(1, clip)
        coin.duplicate = setOf(0, 2, 5, 16)
        coin.swapWithNext = setOf(3, 9)

        val result = engine(store).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        assertEquals(listOf(CtrlMessage.Get(1, 0)), coin.gets)
        assertDelivered(store, coin, 1, clip)
    }

    @Test
    fun lostPacketIsRefetchedFromTheGap() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(3000)
        coin.addClip(2, clip)
        coin.dropOnce = setOf(3)

        engine(store).runSession(coin)

        assertEquals(listOf(CtrlMessage.Get(2, 0), CtrlMessage.Get(2, 3L * Protocol.MAX_DATA_PAYLOAD)), coin.gets)
        assertDelivered(store, coin, 2, clip)
    }

    @Test
    fun crcMismatchRefetchesFromZeroOnce() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(2000)
        coin.addClip(5, clip)
        coin.corruptOnce = true

        engine(store).runSession(coin)

        assertEquals(listOf(CtrlMessage.Get(5, 0), CtrlMessage.Get(5, 0)), coin.gets)
        assertEquals(1, coin.acks.size)
        assertDelivered(store, coin, 5, clip)
    }

    @Test
    fun repeatedCrcMismatchNeverAcks() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        coin.addClip(5, bytes(2000))
        coin.addClip(6, bytes(500))
        coin.corruptAlways = true

        val result = engine(store).runSession(coin)

        assertEquals(listOf(CtrlMessage.Get(5, 0), CtrlMessage.Get(5, 0), CtrlMessage.Get(6, 0), CtrlMessage.Get(6, 0)), coin.gets)
        assertTrue(coin.acks.isEmpty())
        assertEquals(setOf(5, 6), coin.clips.keys)
        assertEquals(0L, store.row(5).receivedBytes) // the next session starts over
        assertEquals(ClipState.RECEIVING, store.row(5).state)
        assertEquals(listOf(5, 6), result.unfinished)
    }

    @Test
    fun multiMessageListResult() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clips = (1..40).associateWith { bytes(300 + it, seed = it) }
        clips.forEach { (id, b) -> coin.addClip(id, b) }

        val result = engine(store).runSession(coin)

        assertEquals(2, coin.toApp.count { it is CtrlMessage.ListResult })
        assertEquals(listOf(true, false), coin.toApp.filterIsInstance<CtrlMessage.ListResult>().map { it.more })
        assertEquals((1..40).toList(), result.acked)
        clips.forEach { (id, b) -> assertDelivered(store, coin, id, b) }
    }

    @Test
    fun clipStillRecordingWhenGetStarts() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val full = bytes(6000)
        val recording = coin.addClip(9, full.copyOf(1000), complete = false)
        backgroundScope.launch {
            var at = 1000
            while (at < full.size) {
                delay(100)
                val next = minOf(at + 500, full.size)
                recording.append(full.copyOfRange(at, next))
                at = next
            }
            recording.complete = true
        }

        val result = engine(store).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        assertTrue("STATUS shows recording", statuses.single().recording)
        assertEquals(listOf(CtrlMessage.Get(9, 0)), coin.gets)
        assertDelivered(store, coin, 9, full)
    }

    @Test
    fun stallTriggersResumeGet() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(3000)
        coin.addClip(4, clip)
        coin.stallAfterPackets = 5

        engine(store).runSession(coin)

        assertEquals(listOf(CtrlMessage.Get(4, 0), CtrlMessage.Get(4, 5L * Protocol.MAX_DATA_PAYLOAD)), coin.gets)
        assertDelivered(store, coin, 4, clip)
    }

    @Test
    fun unknownClipErrorFailsThatClipOnly() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val keep = bytes(800)
        coin.addClip(10, bytes(800, seed = 1))
        coin.addClip(11, keep)
        coin.vanishOnGet = setOf(10) // listed, then gone before the GET

        val result = engine(store).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        assertTrue(coin.toApp.contains(CtrlMessage.Error(MessageType.GET, ErrorCode.UNKNOWN_CLIP, 10)))
        assertEquals(ClipState.FAILED, store.row(10).state)
        assertDelivered(store, coin, 11, keep)
    }

    @Test
    fun busyGetIsRetried() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(1000)
        coin.addClip(2, clip)
        coin.busyGets = 2

        engine(store).runSession(coin)

        assertEquals(3, coin.gets.size)
        assertDelivered(store, coin, 2, clip)
    }

    @Test
    fun unknownAndUnexpectedMessagesAreIgnored() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(1500)
        coin.addClip(3, clip)
        coin.injectDuringStream = listOf(
            CtrlMessage.Unknown(0xA0, byteArrayOf(1, 2, 3)),
            CtrlMessage.Error(MessageType.CFG_SET, ErrorCode.INVALID_VALUE, Protocol.NO_CLIP),
            CtrlMessage.Status(50, charging = true, usbPower = true, recording = false, pending = 1, fwVersion = "0.1.0"),
        )

        val result = engine(store).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        assertDelivered(store, coin, 3, clip)
        assertEquals(listOf(87, 50), statuses.map { it.batteryPct })
        assertTrue(logs.any { "unknown message type 0xa0" in it })
    }

    @Test
    fun lostAckIsResentWithoutRefetching() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(1200)
        coin.addClip(8, clip)
        coin.disconnectOnAck = true

        val first = engine(store).runSession(coin)
        assertEquals(SessionResult.Outcome.DISCONNECTED, first.outcome)
        assertEquals(ClipState.RECEIVED, store.row(8).state) // committed before the ACK went out
        assertTrue(8 in coin.clips)

        coin.reconnect()
        val getsBefore = coin.gets.size
        engine(store).runSession(coin)

        assertEquals("no second download", getsBefore, coin.gets.size)
        assertDelivered(store, coin, 8, clip)
    }

    @Test
    fun refusedReAckFetchesTheClipAsNew() = runTest {
        // The store holds an old clip 4; the coin's clip 4 is a different recording (id wrapped).
        val store = MemoryClips(tmp.root)
        val old = File(tmp.root, "old.ogg").apply { writeBytes(bytes(100, seed = 99)) }
        store.insert(dev.coinai.clips.ClipRecord(0, "C0:A1:00:00:00:01", 4, ClipState.REPLIED, size = 100, crc32 = crc(old.readBytes()), filePath = old.path))
        val coin = FakeCoin(backgroundScope)
        val fresh = bytes(900)
        coin.addClip(4, fresh)

        engine(store).runSession(coin)

        assertEquals(listOf(CtrlMessage.Get(4, 0)), coin.gets)
        assertEquals(2, store.rows.values.count { it.clipId == 4 })
        assertDelivered(store, coin, 4, fresh)
    }

    @Test
    fun missingStatusDoesNotStopTheSession() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(700)
        coin.addClip(1, clip)
        coin.answerHello = false

        val result = engine(store).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        assertEquals(null, result.status)
        assertDelivered(store, coin, 1, clip)
    }
}
