package dev.coinai.testing

import dev.coinai.protocol.CfgKey
import dev.coinai.protocol.ClipEntry
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.DataPacket
import dev.coinai.protocol.ErrorCode
import dev.coinai.protocol.MessageType
import dev.coinai.protocol.Protocol
import dev.coinai.sync.CoinLink
import dev.coinai.sync.Inbound
import dev.coinai.sync.LinkException
import dev.coinai.sync.LinkState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import kotlin.experimental.xor
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds

/**
 * The device side of PROTOCOL.md v1, in memory: answers HELLO with STATUS, LIST with LIST_RESULT
 * (split at [listChunk] entries), streams `data` packets for GET from the requested offset (also
 * while a clip is still recording) and ends with CLIP_ENDED, deletes a clip only on an ACK whose
 * CRC matches and answers it with STATUS, stores CFG_SET values. Every message crosses the real
 * codec in both directions.
 *
 * Fault knobs model what the radio and Android can do to a transfer.
 */
class FakeCoin(private val scope: CoroutineScope, override val deviceAddress: String = "C0:A1:00:00:00:01") : CoinLink {
    class Clip(val id: Int, initial: ByteArray, var complete: Boolean, val durationMs: Long, val voicedMs: Long, val startUtcMs: Long = 0) {
        private val buf = ByteArrayOutputStream().apply { write(initial) }
        @get:Synchronized val size: Long get() = buf.size().toLong()
        @Synchronized fun append(bytes: ByteArray) = buf.write(bytes)
        @Synchronized fun bytes(): ByteArray = buf.toByteArray()
        fun crc(): Long = CRC32().apply { update(bytes()) }.value
    }

    val clips = sortedMapOf<Int, Clip>()
    override val state = MutableStateFlow(LinkState.READY)
    private val out = MutableSharedFlow<Inbound>(extraBufferCapacity = 100_000)
    override val incoming: Flow<Inbound> = out

    /** Everything the app wrote, decoded from the bytes it sent. */
    val fromApp = mutableListOf<CtrlMessage>()
    /** Everything this coin sent on ctrl. */
    val toApp = mutableListOf<CtrlMessage>()
    val gets: List<CtrlMessage.Get> get() = fromApp.filterIsInstance<CtrlMessage.Get>()
    val acks: List<CtrlMessage.Ack> get() = fromApp.filterIsInstance<CtrlMessage.Ack>()

    /** Settings from CFG_SET, as the coin stores them. */
    val settings = mutableMapOf<CfgKey, Int>()

    /** Networks from WIFI_SET, newest first, at most 4; the same SSID again replaces its entry. */
    val wifiNetworks = mutableListOf<Pair<String, String>>()
    /** SSIDs the coin refuses with ERROR 5 even when valid (for example a full flash). */
    var refuseWifi: Set<String> = emptySet()

    /**
     * Clips the coin is sending over Wi-Fi: not listed, not counted, no CLIP_STARTED, and GET or
     * ACK of them answers ERROR 4. [giveBack] and [deliverOverWifi] end the claim.
     */
    val wifiClaimed = mutableSetOf<Int>()
    /** Clips that Wi-Fi claims right after a LIST has listed them. */
    var claimAfterList: Set<Int> = emptySet()

    var listChunk = Protocol.MAX_LIST_ENTRIES
    /** `STATUS` bit 3. */
    var showsScenes = false
    /** Scenes this coin drew: (clip, JSON). */
    val shownScenes = mutableListOf<Pair<Int, String>>()
    /** Card kinds the coin cannot draw; a scene with one gets `ERROR` 5, as an unknown kind does. */
    var refuseKinds: Set<String> = emptySet()
    private var sceneClip = -1
    private var sceneParts = 0
    private var sceneBytes = ByteArrayOutputStream()
    var answerHello = true
    var fwVersion = "0.1.0"
    var packetInterval: Duration = 5.milliseconds

    // Faults. Packet indices count packets within one GET stream, from 0.
    /** Drop the link after this many packets on the current connection (once). */
    var disconnectAfterPackets: Int? = null
    /** Stop streaming (no disconnect, no CLIP_ENDED) after this many packets of the first stream. */
    var stallAfterPackets: Int? = null
    var duplicate: Set<Int> = emptySet()
    /** Packet i is sent after packet i+1. */
    var swapWithNext: Set<Int> = emptySet()
    /** Lost on the first stream only. */
    var dropOnce: Set<Int> = emptySet()
    /** Flip a byte in packet 1 of the first stream (once) or of every stream. */
    var corruptOnce = false
    var corruptAlways = false
    var busyGets = 0
    /** Clips that disappear (deleted on the coin) between LIST and GET. */
    var vanishOnGet: Set<Int> = emptySet()
    /** Drop the link when an ACK arrives, before acting on it (the app sees a failed write). */
    var disconnectOnAck = false
    /**
     * The user discards clip `key` after `value` packets of its stream: the coin deletes it, stops
     * with ERROR 1 instead of CLIP_ENDED and sends STATUS with the new pending count.
     */
    var discardWhileStreaming: Map<Int, Int> = emptyMap()
    /** Extra ctrl messages sent right after the first data packet of each stream. */
    var injectDuringStream: List<CtrlMessage> = emptyList()

    private var stream: Job? = null
    private var streams = 0
    private var packetsThisConnection = 0

    fun addClip(id: Int, bytes: ByteArray, complete: Boolean = true, durationMs: Long = 3000, voicedMs: Long = 2000, startUtcMs: Long = 0): Clip =
        Clip(id, bytes, complete, durationMs, voicedMs, startUtcMs).also { clips[id] = it }

    /** Wi-Fi failed before SamBot was prompted: the clip is the phone's again, offered with CLIP_STARTED. */
    fun giveBack(id: Int) {
        wifiClaimed -= id
        val clip = clips.getValue(id)
        emit(CtrlMessage.ClipStarted(id, Protocol.CODEC_OGG_OPUS, 16_000, clip.startUtcMs))
        emitStatus()
    }

    /** SamBot took the clip over Wi-Fi: the coin deletes it. */
    fun deliverOverWifi(id: Int) {
        wifiClaimed -= id
        clips.remove(id)
    }

    fun reconnect() {
        packetsThisConnection = 0
        state.value = LinkState.READY
    }

    fun disconnect() {
        stream?.cancel()
        state.value = LinkState.DISCONNECTED
    }

    override suspend fun send(message: CtrlMessage) {
        if (state.value != LinkState.READY) throw LinkException("not connected")
        val m = CtrlMessage.decode(message.encode())
        fromApp += m
        when (m) {
            is CtrlMessage.Hello -> if (answerHello) emitStatus()
            is CtrlMessage.CfgSet -> settings[m.key] = m.value
            is CtrlMessage.Scene -> scenePart(m)
            is CtrlMessage.WifiSet -> {
                val s = m.ssid.toByteArray().size
                val p = m.password.toByteArray().size
                if (s !in 1..32 || (p != 0 && p !in 8..64) || m.ssid in refuseWifi) {
                    emit(CtrlMessage.Error(MessageType.WIFI_SET, ErrorCode.INVALID_VALUE, Protocol.NO_CLIP))
                } else {
                    wifiNetworks.removeAll { it.first == m.ssid }
                    wifiNetworks.add(0, m.ssid to m.password)
                    while (wifiNetworks.size > 4) wifiNetworks.removeAt(wifiNetworks.lastIndex)
                }
            }
            CtrlMessage.ListClips -> {
                val entries = clips.values.filter { it.id !in wifiClaimed }.map { ClipEntry(it.id, it.size, it.complete) }
                val chunks = entries.chunked(listChunk).ifEmpty { listOf(emptyList()) }
                chunks.forEachIndexed { i, chunk -> emit(CtrlMessage.ListResult(more = i < chunks.lastIndex, entries = chunk)) }
                wifiClaimed += claimAfterList.filter { it in clips }
                claimAfterList = emptySet()
            }
            is CtrlMessage.Get -> if (m.clip in wifiClaimed) {
                stream?.cancel()
                emit(CtrlMessage.Error(MessageType.GET, ErrorCode.BUSY, m.clip))
            } else {
                startStream(m)
            }
            is CtrlMessage.Ack if m.clip in wifiClaimed -> emit(CtrlMessage.Error(MessageType.ACK, ErrorCode.BUSY, m.clip))
            is CtrlMessage.Ack -> {
                if (disconnectOnAck) {
                    disconnectOnAck = false
                    disconnect()
                    throw LinkException("link lost during ACK")
                }
                val clip = clips[m.clip]
                when {
                    clip == null -> emit(CtrlMessage.Error(MessageType.ACK, ErrorCode.UNKNOWN_CLIP, m.clip))
                    clip.crc() == m.crc32 -> {
                        clips.remove(m.clip)
                        emitStatus()
                    }
                    else -> emit(CtrlMessage.Error(MessageType.ACK, ErrorCode.INVALID_VALUE, m.clip))
                }
            }
            else -> Unit
        }
    }

    private fun emitStatus() = emit(
        CtrlMessage.Status(
            87, charging = false, usbPower = false, recording = clips.values.any { !it.complete },
            pending = clips.count { it.key !in wifiClaimed }, fwVersion = fwVersion, scenes = showsScenes,
        ),
    )

    /**
     * PROTOCOL.md "Scenes": part 0 starts a scene, a part that does not continue it drops it with
     * `ERROR` 5, and after the last part the scene is drawn, or refused with `ERROR` 5 when it is
     * not JSON with a card of a kind the coin draws. A coin without scenes ignores `SCENE`.
     */
    private fun scenePart(m: CtrlMessage.Scene) {
        if (!showsScenes) return
        if (m.part == 0) {
            sceneClip = m.clip
            sceneParts = m.parts
            sceneBytes = ByteArrayOutputStream()
        } else if (m.clip != sceneClip || m.parts != sceneParts || m.part * Protocol.MAX_SCENE_PART_BYTES != sceneBytes.size()) {
            sceneClip = -1
            return emit(CtrlMessage.Error(MessageType.SCENE, ErrorCode.INVALID_VALUE, m.clip))
        }
        sceneBytes.write(m.json)
        if (m.part != m.parts - 1) return
        val json = sceneBytes.toString(Charsets.UTF_8)
        sceneClip = -1
        val kind = runCatching {
            val card = kotlinx.serialization.json.Json.parseToJsonElement(json).let { it as kotlinx.serialization.json.JsonObject }["card"]
                as kotlinx.serialization.json.JsonObject
            (card["kind"] as kotlinx.serialization.json.JsonPrimitive).content
        }.getOrNull()
        if (kind == null || kind in refuseKinds) emit(CtrlMessage.Error(MessageType.SCENE, ErrorCode.INVALID_VALUE, m.clip))
        else shownScenes += m.clip to json
    }

    fun emit(message: CtrlMessage) {
        toApp += message
        check(out.tryEmit(Inbound.Ctrl(CtrlMessage.decode(message.encode()))))
    }

    private fun emitData(packet: DataPacket) {
        check(out.tryEmit(Inbound.Data(DataPacket.decode(packet.encode()))))
    }

    private fun startStream(get: CtrlMessage.Get) {
        stream?.cancel() // a new GET supersedes the running stream
        if (get.clip in vanishOnGet) clips.remove(get.clip)
        val clip = clips[get.clip] ?: return emit(CtrlMessage.Error(MessageType.GET, ErrorCode.UNKNOWN_CLIP, get.clip))
        if (busyGets > 0) {
            busyGets--
            return emit(CtrlMessage.Error(MessageType.GET, ErrorCode.BUSY, get.clip))
        }
        if (get.offset > clip.size) return emit(CtrlMessage.Error(MessageType.GET, ErrorCode.BAD_OFFSET, get.clip))
        val n = streams++
        stream = scope.launch {
            var pos = get.offset
            var index = 0
            var held: DataPacket? = null
            while (true) {
                if (pos < clip.size) {
                    val all = clip.bytes()
                    val len = minOf(Protocol.MAX_DATA_PAYLOAD.toLong(), all.size - pos).toInt()
                    val payload = all.copyOfRange(pos.toInt(), pos.toInt() + len)
                    if (index == 1 && (corruptAlways || (corruptOnce && n == 0))) payload[0] = payload[0] xor 0x5A.toByte()
                    val packet = DataPacket(clip.id, pos, payload)
                    pos += len
                    val i = index++
                    if (n == 0 && stallAfterPackets == i) return@launch
                    if (discardWhileStreaming[clip.id] == i) {
                        clips.remove(clip.id)
                        emit(CtrlMessage.Error(MessageType.GET, ErrorCode.UNKNOWN_CLIP, clip.id))
                        emitStatus()
                        return@launch
                    }
                    packetsThisConnection++
                    if (disconnectAfterPackets != null && packetsThisConnection > disconnectAfterPackets!!) {
                        disconnectAfterPackets = null
                        disconnect()
                        return@launch
                    }
                    when {
                        n == 0 && i in dropOnce -> Unit
                        i in swapWithNext -> held = packet
                        else -> {
                            emitData(packet)
                            if (i in duplicate) emitData(packet)
                            held?.let { emitData(it); held = null }
                        }
                    }
                    if (i == 0) injectDuringStream.forEach(::emit)
                    delay(packetInterval)
                } else if (!clip.complete) {
                    delay(20) // still recording: wait for more audio
                } else {
                    break
                }
            }
            held?.let(::emitData)
            emit(CtrlMessage.ClipEnded(clip.id, clip.size, clip.durationMs, clip.crc(), clip.voicedMs, clip.startUtcMs))
        }
    }
}
