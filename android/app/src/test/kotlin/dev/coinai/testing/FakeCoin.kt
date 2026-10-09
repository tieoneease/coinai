package dev.coinai.testing

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
 * CRC matches. Every message crosses the real codec in both directions.
 *
 * Fault knobs model what the radio and Android can do to a transfer.
 */
class FakeCoin(private val scope: CoroutineScope, override val deviceAddress: String = "C0:A1:00:00:00:01") : CoinLink {
    class Clip(val id: Int, initial: ByteArray, var complete: Boolean, val durationMs: Long, val voicedMs: Long) {
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

    var listChunk = Protocol.MAX_LIST_ENTRIES
    var answerHello = true
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
    /** Extra ctrl messages sent right after the first data packet of each stream. */
    var injectDuringStream: List<CtrlMessage> = emptyList()

    private var stream: Job? = null
    private var streams = 0
    private var packetsThisConnection = 0

    fun addClip(id: Int, bytes: ByteArray, complete: Boolean = true, durationMs: Long = 3000, voicedMs: Long = 2000): Clip =
        Clip(id, bytes, complete, durationMs, voicedMs).also { clips[id] = it }

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
            is CtrlMessage.Hello -> if (answerHello) emit(
                CtrlMessage.Status(87, charging = false, usbPower = false, recording = clips.values.any { !it.complete }, pending = clips.size, fwVersion = "0.1.0"),
            )
            CtrlMessage.ListClips -> {
                val entries = clips.values.map { ClipEntry(it.id, it.size, it.complete) }
                val chunks = entries.chunked(listChunk).ifEmpty { listOf(emptyList()) }
                chunks.forEachIndexed { i, chunk -> emit(CtrlMessage.ListResult(more = i < chunks.lastIndex, entries = chunk)) }
            }
            is CtrlMessage.Get -> startStream(m)
            is CtrlMessage.Ack -> {
                if (disconnectOnAck) {
                    disconnectOnAck = false
                    disconnect()
                    throw LinkException("link lost during ACK")
                }
                val clip = clips[m.clip]
                when {
                    clip == null -> emit(CtrlMessage.Error(MessageType.ACK, ErrorCode.UNKNOWN_CLIP, m.clip))
                    clip.crc() == m.crc32 -> clips.remove(m.clip)
                    else -> emit(CtrlMessage.Error(MessageType.ACK, ErrorCode.INVALID_VALUE, m.clip))
                }
            }
            else -> Unit
        }
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
            emit(CtrlMessage.ClipEnded(clip.id, clip.size, clip.durationMs, clip.crc(), clip.voicedMs))
        }
    }
}
