package dev.coinai.sync

import dev.coinai.clips.ClipState
import dev.coinai.protocol.CfgKey
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.ErrorCode
import dev.coinai.protocol.MessageType
import dev.coinai.testing.FakeCoin
import dev.coinai.testing.MemoryClips
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.TimeZone
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.random.Random

/** The app's side of PROTOCOL.md "Wi-Fi delivery": `WIFI_SET`, `CFG_SET delivery`, and busy clips. */
class WifiDeliverySyncTest {
    @get:Rule val tmp = TemporaryFolder()

    private class FakeSettings(networks: List<CoinNetwork> = emptyList(), order: DeliveryOrder? = DeliveryOrder.WIFI_FIRST) : CoinSettings {
        val order = MutableStateFlow(order ?: DeliveryOrder.DEFAULT)
        override val delivery = if (order == null) null else this.order
        val pending = networks.toMutableList()
        val results = mutableListOf<Pair<String, Int?>>()

        override suspend fun pendingNetworks(device: String) = pending.toList()
        override suspend fun networkResult(device: String, network: CoinNetwork, errorCode: Int?) {
            results += network.ssid to errorCode
            pending.removeAll { it.id == network.id }
        }
    }

    private fun bytes(n: Int, seed: Int = n) = Random(seed).nextBytes(n)
    private val logs = mutableListOf<String>()
    private val listener = object : SyncListener {
        override fun onLog(device: String, message: String) { logs += message }
    }

    private fun TestScope.engine(store: ClipStore, settings: CoinSettings, config: SyncConfig = SyncConfig()) = SyncEngine(
        store, config, listener, clock = { testScheduler.currentTime }, io = EmptyCoroutineContext,
        timeZone = { TimeZone.getTimeZone("UTC") }, settings = settings,
    )

    private fun MemoryClips.row(clip: Int) = rows.values.filter { it.clipId == clip }.maxBy { it.id }

    private fun assertDelivered(store: MemoryClips, coin: FakeCoin, clip: Int, expected: ByteArray) {
        val row = store.row(clip)
        assertEquals(ClipState.RECEIVED, row.state)
        assertArrayEquals(expected, File(row.filePath!!).readBytes())
        assertFalse("coin deleted clip $clip", clip in coin.clips)
    }

    // Wi-Fi networks

    @Test
    fun queuedNetworksGoOutAfterHelloAndStatusAndAreMarked() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        coin.addClip(1, bytes(600))
        coin.refuseWifi = setOf("guest")
        val settings = FakeSettings(listOf(CoinNetwork(10, "home", "fake-pass-1"), CoinNetwork(11, "guest", ""), CoinNetwork(12, "cafe", "")))

        val result = engine(store, settings).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        val sent = coin.fromApp
        assertEquals(CtrlMessage.Hello::class, sent[0]::class)
        assertEquals(CfgKey.UTC_OFFSET_MIN, (sent[1] as CtrlMessage.CfgSet).key)
        assertEquals(DeliveryOrder.WIFI_FIRST.message(), sent[2])
        // STATUS (the answer to HELLO) came before the first WIFI_SET; each WIFI_SET is followed
        // by a LIST, the barrier that shows whether the coin refused it.
        assertTrue(coin.toApp.first() is CtrlMessage.Status)
        assertEquals(
            listOf(
                CtrlMessage.WifiSet("home", "fake-pass-1"), CtrlMessage.ListClips,
                CtrlMessage.WifiSet("guest", ""), CtrlMessage.ListClips,
                CtrlMessage.WifiSet("cafe", ""), CtrlMessage.ListClips,
                CtrlMessage.Get(1, 0),
            ),
            sent.subList(3, 10),
        )
        assertEquals(listOf("home" to null, "guest" to ErrorCode.INVALID_VALUE, "cafe" to null), settings.results)
        assertEquals(listOf("cafe", "home"), coin.wifiNetworks.map { it.first }) // newest first
        assertEquals(listOf(1), result.acked)

        // Nothing pending any more: the next session sends no WIFI_SET and one LIST.
        coin.fromApp.clear()
        engine(store, settings).runSession(coin)
        assertTrue(coin.fromApp.none { it is CtrlMessage.WifiSet })
        assertEquals(1, coin.fromApp.count { it == CtrlMessage.ListClips })
    }

    @Test
    fun benchModeLinkKeepsNetworksQueued() = runTest {
        val coin = FakeCoin(backgroundScope)
        coin.fwVersion = "0.1.0-bench"
        val settings = FakeSettings(listOf(CoinNetwork(1, "home", "fake-pass-1")))

        engine(MemoryClips(tmp.root), settings).runSession(coin)

        assertTrue(coin.fromApp.none { it is CtrlMessage.WifiSet })
        assertEquals(1, settings.pending.size)
        assertTrue(settings.results.isEmpty())
    }

    // Delivery order

    @Test
    fun deliveryFollowsHelloAndChangesGoOutAtOnce() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(8000)
        coin.addClip(1, clip) // about 34 packets, 170 ms of streaming
        val settings = FakeSettings(order = DeliveryOrder.PHONE_FIRST)
        backgroundScope.launch {
            while (coin.gets.isEmpty()) delay(1)
            delay(50)
            settings.order.value = DeliveryOrder.PHONE_ONLY
        }

        val result = engine(store, settings).runSession(coin)

        assertEquals(DeliveryOrder.PHONE_FIRST.message(), coin.fromApp[2])
        val deliveries = coin.fromApp.filterIsInstance<CtrlMessage.CfgSet>().filter { it.key == CfgKey.DELIVERY }
        assertEquals(listOf(DeliveryOrder.PHONE_FIRST.message(), DeliveryOrder.PHONE_ONLY.message()), deliveries)
        assertTrue("sent during the transfer", coin.fromApp.indexOf(DeliveryOrder.PHONE_ONLY.message()) < coin.fromApp.indexOfFirst { it is CtrlMessage.Ack })
        assertEquals(DeliveryOrder.PHONE_ONLY.code, coin.settings[CfgKey.DELIVERY])
        assertEquals(DeliveryOrder.PHONE_ONLY, result.delivery)
        assertDelivered(store, coin, 1, clip)

        // Every session sends it after HELLO, unchanged or not.
        coin.fromApp.clear()
        engine(store, settings).runSession(coin)
        assertEquals(DeliveryOrder.PHONE_ONLY.message(), coin.fromApp[2])
    }

    @Test
    fun followDeliveryAfterTheSession() = runTest {
        val coin = FakeCoin(backgroundScope)
        val order = MutableStateFlow(DeliveryOrder.WIFI_FIRST)
        val job = backgroundScope.launch { followDelivery(coin, order, DeliveryOrder.WIFI_FIRST) }
        delay(10)
        assertTrue("the value already sent is not sent again", coin.fromApp.isEmpty())
        order.value = DeliveryOrder.PHONE_ONLY
        delay(10)
        order.value = DeliveryOrder.WIFI_FIRST
        delay(10)
        job.cancel()
        assertEquals(listOf(DeliveryOrder.PHONE_ONLY.message(), DeliveryOrder.WIFI_FIRST.message()), coin.fromApp)
    }

    // Busy clips (sent over Wi-Fi)

    @Test
    fun busyClipIsSkippedAndTheRestDelivered() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val second = bytes(1500, seed = 2)
        coin.addClip(1, bytes(900, seed = 1))
        coin.addClip(2, second)
        coin.claimAfterList = setOf(1) // listed, then Wi-Fi takes it before the GET

        val result = engine(store, FakeSettings()).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        assertEquals("clip 2 right after the busy answer", listOf(CtrlMessage.Get(1, 0), CtrlMessage.Get(2, 0)), coin.gets.take(2))
        assertEquals("retried up to maxBusyRetries", 1 + SyncConfig().maxBusyRetries, coin.gets.count { it.clip == 1 })
        assertDelivered(store, coin, 2, second)
        assertEquals(listOf(2), result.acked)
        assertEquals(listOf(1), result.unfinished)
        assertTrue("no row for a clip nothing of which arrived", store.rows.values.none { it.clipId == 1 })
    }

    @Test
    fun clipOfferedBackWithClipStartedIsFetched() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val first = bytes(900, seed = 1)
        val second = bytes(3000, seed = 2)
        coin.addClip(1, first, startUtcMs = 1_791_000_000_000L)
        coin.addClip(2, second)
        coin.claimAfterList = setOf(1)
        // Wi-Fi fails while clip 2 streams: the coin offers clip 1 back, complete.
        backgroundScope.launch {
            while (coin.gets.none { it.clip == 2 }) delay(1)
            coin.giveBack(1)
        }

        val result = engine(store, FakeSettings(), SyncConfig(maxBusyRetries = 0)).runSession(coin)

        assertEquals(SessionResult.Outcome.COMPLETE, result.outcome)
        assertEquals(listOf(CtrlMessage.Get(1, 0), CtrlMessage.Get(2, 0), CtrlMessage.Get(1, 0)), coin.gets)
        assertDelivered(store, coin, 1, first)
        assertDelivered(store, coin, 2, second)
        assertEquals(1_791_000_000_000L, store.row(1).startedAtMs)
        assertEquals(listOf(2, 1), result.acked)
        assertEquals(emptyList<Int>(), result.unfinished)
    }

    @Test
    fun clipStartedForAClipAlreadyHereIsReAckedNotFetched() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        val clip = bytes(700)
        coin.addClip(3, clip)
        coin.disconnectOnAck = true
        engine(store, FakeSettings()).runSession(coin) // received, the ACK lost
        coin.reconnect()
        coin.wifiClaimed += 3 // not listed now; Wi-Fi gives it back during the next session
        coin.addClip(4, bytes(3000, seed = 4))
        backgroundScope.launch {
            while (coin.gets.none { it.clip == 4 }) delay(1)
            coin.giveBack(3)
        }
        val getsBefore = coin.gets.size

        engine(store, FakeSettings()).runSession(coin)

        assertEquals("only clip 4 downloaded", listOf(CtrlMessage.Get(4, 0)), coin.gets.drop(getsBefore))
        assertFalse(3 in coin.clips)
        assertEquals(ClipState.RECEIVED, store.row(3).state)
    }

    @Test
    fun partialClipBusyThenDeliveredOverWifiIsDroppedSilently() = runTest {
        val store = MemoryClips(tmp.root)
        val coin = FakeCoin(backgroundScope)
        coin.addClip(5, bytes(5000))
        coin.disconnectAfterPackets = 6

        // Session 1: part of clip 5 arrives, then the link drops.
        assertEquals(SessionResult.Outcome.DISCONNECTED, engine(store, FakeSettings()).runSession(coin).outcome)
        val saved = store.row(5).receivedBytes
        assertTrue(saved > 0)

        // Session 2: Wi-Fi holds clip 5. LIST leaves it out; the GET for the unfinished transfer
        // is answered busy, and the progress is kept.
        coin.reconnect()
        coin.wifiClaimed += 5
        val second = engine(store, FakeSettings(), SyncConfig(maxBusyRetries = 0)).runSession(coin)
        assertEquals(SessionResult.Outcome.COMPLETE, second.outcome)
        assertEquals(CtrlMessage.Get(5, saved), coin.gets.last())
        assertTrue(coin.toApp.contains(CtrlMessage.Error(MessageType.GET, ErrorCode.BUSY, 5)))
        assertEquals(ClipState.RECEIVING, store.row(5).state)
        assertEquals(saved, store.row(5).receivedBytes)

        // Session 3: SamBot got it over Wi-Fi; the coin deleted it. ERROR 1: dropped without a trace.
        coin.deliverOverWifi(5)
        val third = engine(store, FakeSettings()).runSession(coin)
        assertEquals(SessionResult.Outcome.COMPLETE, third.outcome)
        assertTrue(coin.toApp.contains(CtrlMessage.Error(MessageType.GET, ErrorCode.UNKNOWN_CLIP, 5)))
        assertTrue("no row for clip 5", store.rows.values.none { it.clipId == 5 })
        assertTrue("partial file deleted", tmp.root.listFiles()!!.none { it.name.startsWith("5-") })
        assertEquals(emptyList<Int>(), third.unfinished)
    }
}
