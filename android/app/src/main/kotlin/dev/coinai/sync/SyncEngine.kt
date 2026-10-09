package dev.coinai.sync

import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.ErrorCode
import dev.coinai.protocol.MessageType
import dev.coinai.protocol.Protocol
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.io.IOException
import kotlin.coroutines.CoroutineContext
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.seconds

data class SyncConfig(
    /** How long to wait for `STATUS` and each `LIST_RESULT`. */
    val responseTimeout: Duration = 5.seconds,
    /** No packet for this long during a transfer counts as a stall and triggers a resume `GET`. */
    val stallTimeout: Duration = 10.seconds,
    /** Resume `GET`s per clip per session after gaps or stalls before giving up until next session. */
    val maxResumes: Int = 3,
    val busyRetryDelay: Duration = 500.milliseconds,
    val maxBusyRetries: Int = 5,
    /** Sync the part file and record progress every this many new bytes. */
    val progressStep: Long = 32 * 1024,
)

/** Callbacks from a session. They run on the session's coroutine; keep them short. */
interface SyncListener {
    fun onStatus(device: String, status: CtrlMessage.Status) {}
    fun onClipReceived(clip: ReceivedClip) {}
    fun onLog(device: String, message: String) {}

    companion object {
        val NONE: SyncListener = object : SyncListener {}
    }
}

data class SessionResult(
    val outcome: Outcome,
    val status: CtrlMessage.Status?,
    val received: List<ReceivedClip>,
    /** Clips ACKed this session (newly received ones and re-ACKs). */
    val acked: List<Int>,
    /** Clips still on the coin that this session did not finish. */
    val unfinished: List<Int>,
    val error: String? = null,
) {
    enum class Outcome { COMPLETE, DISCONNECTED, FAILED }
}

/**
 * Runs PROTOCOL.md sessions against a [CoinLink]: `HELLO` → `STATUS`, `LIST` → `LIST_RESULT`
 * (possibly several messages), then for each clip not yet received a `GET` from the stored resume
 * offset, reassembly, size and CRC-32 check, a durable commit, and only then `ACK`.
 */
class SyncEngine(
    private val store: ClipStore,
    private val config: SyncConfig = SyncConfig(),
    private val listener: SyncListener = SyncListener.NONE,
    private val clock: () -> Long = System::currentTimeMillis,
    /** Where file I/O runs; tests pass their own dispatcher. */
    private val io: CoroutineContext = Dispatchers.IO,
) {
    suspend fun runSession(link: CoinLink): SessionResult = coroutineScope {
        val inbox = Channel<Event>(Channel.UNLIMITED)
        // Subscribe before the first request goes out: `incoming` is hot.
        val pump = launch(start = CoroutineStart.UNDISPATCHED) { link.incoming.collect { inbox.send(Event.In(it)) } }
        val watch = launch(start = CoroutineStart.UNDISPATCHED) {
            link.state.first { it == LinkState.DISCONNECTED }
            inbox.send(Event.Lost)
        }
        try {
            Session(link, inbox).run()
        } finally {
            pump.cancel()
            watch.cancel()
        }
    }

    private sealed interface Event {
        data class In(val inbound: Inbound) : Event
        data object Lost : Event
    }

    private class LinkLost : Exception()
    private class SessionFailure(message: String) : Exception(message)

    private inner class Session(private val link: CoinLink, private val inbox: Channel<Event>) {
        private val device = link.deviceAddress
        private var status: CtrlMessage.Status? = null
        private val received = mutableListOf<ReceivedClip>()
        private val acked = mutableListOf<Int>()
        /** Clips to visit, in order; grows when the coin announces new recordings. */
        private val queue = ArrayDeque<Int>()
        private val visited = mutableSetOf<Int>()
        private val startTimes = mutableMapOf<Int, Long>()
        /** Clips re-ACKed from the store this session, and those the coin then refused. */
        private val reAcked = mutableSetOf<Int>()
        private val fetchAsNew = mutableSetOf<Int>()
        private var current: Transfer? = null

        suspend fun run(): SessionResult {
            val outcome = try {
                send(CtrlMessage.Hello(phoneUtcMs = clock()))
                awaitStatus()
                for (entry in list()) enqueue(entry.clip)
                while (queue.isNotEmpty()) {
                    visit(queue.removeFirst())
                    // PROTOCOL-Q: ACK has no success reply. Only a refused re-ACK (ERROR) is
                    // answered, so after re-ACKs wait briefly for one before ending the session.
                    if (queue.isEmpty() && reAcked.isNotEmpty()) settle()
                }
                SessionResult.Outcome.COMPLETE to null
            } catch (_: LinkLost) {
                SessionResult.Outcome.DISCONNECTED to "link lost"
            } catch (e: SessionFailure) {
                SessionResult.Outcome.FAILED to e.message
            } catch (e: IOException) {
                if (link.state.value == LinkState.DISCONNECTED) SessionResult.Outcome.DISCONNECTED to "link lost"
                else SessionResult.Outcome.FAILED to (e.message ?: e.toString())
            } finally {
                current?.let { saveAndClose(it) }
                current = null
            }
            outcome.second?.let { log("session ended: $it") }
            val unfinished = (queue + visited).filter { it !in acked }.distinct()
            return SessionResult(outcome.first, status, received.toList(), acked.toList(), unfinished, outcome.second)
        }

        // Requests

        private suspend fun send(message: CtrlMessage) {
            if (link.state.value == LinkState.DISCONNECTED) throw LinkLost()
            link.send(message)
        }

        /** The next inbound item, or null after [timeout]. Throws [LinkLost] when the link drops. */
        private suspend fun next(timeout: Duration): Inbound? {
            val event = withTimeoutOrNull(timeout) { inbox.receive() } ?: return null
            return when (event) {
                is Event.In -> event.inbound
                Event.Lost -> throw LinkLost()
            }
        }

        private suspend fun awaitStatus() {
            val deadline = clock() + config.responseTimeout.inWholeMilliseconds
            while (true) {
                val left = (deadline - clock()).milliseconds
                // PROTOCOL-Q: HELLO without a STATUS answer. The session goes on without status
                // rather than failing: the clip transfer does not depend on it.
                val inbound = if (left.isPositive()) next(left) else null
                if (inbound == null) {
                    log("no STATUS after HELLO")
                    return
                }
                if (inbound is Inbound.Ctrl && inbound.message is CtrlMessage.Status) {
                    onStatus(inbound.message)
                    return
                }
                aside(inbound)
            }
        }

        private suspend fun list(): List<dev.coinai.protocol.ClipEntry> {
            val entries = mutableListOf<dev.coinai.protocol.ClipEntry>()
            var busy = 0
            send(CtrlMessage.ListClips)
            while (true) {
                val inbound = next(config.responseTimeout) ?: throw SessionFailure("no LIST_RESULT")
                val msg = (inbound as? Inbound.Ctrl)?.message
                when {
                    msg is CtrlMessage.ListResult -> {
                        entries += msg.entries
                        if (!msg.more) return entries
                    }
                    msg is CtrlMessage.Error && msg.requestType == MessageType.LIST -> {
                        if (msg.code == ErrorCode.BUSY && busy++ < config.maxBusyRetries) {
                            delay(config.busyRetryDelay)
                            entries.clear()
                            send(CtrlMessage.ListClips)
                        } else {
                            throw SessionFailure("LIST failed with error ${msg.code}")
                        }
                    }
                    else -> aside(inbound)
                }
            }
        }

        private fun enqueue(clip: Int) {
            if (clip !in visited && clip !in queue) queue.addLast(clip)
        }

        private suspend fun settle() {
            val deadline = clock() + config.responseTimeout.inWholeMilliseconds
            while (queue.isEmpty()) {
                val left = (deadline - clock()).milliseconds
                if (!left.isPositive()) return
                aside(next(left) ?: return)
            }
        }

        // Clips

        private suspend fun visit(clip: Int) {
            visited += clip
            val known = store.latest(device, clip)
            if (known != null && known.received && known.crc32 != null && clip !in fetchAsNew) {
                // Received before, but the coin still has it: the ACK was lost. Re-ACK the stored CRC.
                // PROTOCOL-Q: clip ids wrap after 65535, so a received id could in theory be a new
                // clip. The coin deletes only on a matching CRC, so a wrong re-ACK cannot lose data;
                // an ERROR(ACK) for it makes the next session fetch it as new (see aside()).
                send(CtrlMessage.Ack(clip, known.crc32))
                store.markAcked(known, clock())
                acked += clip
                reAcked += clip
                return
            }
            transfer(store.beginTransfer(device, clip))
        }

        private inner class Transfer(val record: TransferRecord, val assembler: ClipAssembler) {
            var durable = assembler.contiguous
            var resumes = 0
            var busy = 0
            var restartedFromZero = false
        }

        private suspend fun transfer(record: TransferRecord) {
            val assembler = withContext(io) { ClipAssembler(record.partFile, record.durableBytes) }
            val t = Transfer(record, assembler)
            current = t
            val clip = record.clip
            // PROTOCOL-Q: the protocol does not say what a GET does while another stream runs.
            // The engine assumes a new GET (resume, restart, next clip) replaces the running
            // stream. Late packets of another clip are dropped; late packets of this clip carry
            // the same bytes at the same offsets, so reassembly takes them as duplicates or data.
            send(CtrlMessage.Get(clip, assembler.contiguous))
            while (true) {
                val inbound = next(config.stallTimeout)
                if (inbound == null) {
                    if (!resume(t, "stalled at ${assembler.contiguous}")) return giveUp(t)
                    continue
                }
                when (inbound) {
                    is Inbound.Data -> {
                        val p = inbound.packet
                        if (p.clip != clip) continue // From an earlier GET; ignore.
                        withContext(io) { assembler.accept(p.offset, p.payload) }
                        if (assembler.contiguous - t.durable >= config.progressStep) checkpoint(t)
                    }
                    is Inbound.Ctrl -> when (val m = inbound.message) {
                        is CtrlMessage.ClipEnded if m.clip == clip -> if (ended(t, m)) return
                        is CtrlMessage.Error if m.clip == clip && m.requestType == MessageType.GET -> if (error(t, m)) return
                        else -> aside(inbound)
                    }
                }
            }
        }

        /** Handles CLIP_ENDED for the current transfer; true when the transfer is over. */
        private suspend fun ended(t: Transfer, m: CtrlMessage.ClipEnded): Boolean {
            val a = t.assembler
            if (a.contiguous < m.size) {
                // PROTOCOL-Q: the protocol has no retransmit request. A gap (a lost notification)
                // is repaired with a resume GET from the first missing byte.
                if (resume(t, "gap: ${a.contiguous} of ${m.size} bytes contiguous")) return false
                giveUp(t)
                return true
            }
            val crc = withContext(io) {
                if (a.length() > m.size) a.truncate(m.size) // bytes past the announced end are not part of the file
                a.crc32(m.size)
            }
            if (crc != m.crc32) {
                log("clip ${m.clip}: CRC %08x, expected %08x".format(crc, m.crc32))
                if (t.restartedFromZero) {
                    // Start the next session from 0 too, not from a full but corrupt file.
                    withContext(io) { a.reset(); a.sync() }
                    t.durable = 0
                    store.saveProgress(t.record, 0)
                    giveUp(t, "CRC mismatch twice")
                    return true
                }
                restartFromZero(t)
                return false
            }
            withContext(io) { a.sync(); a.close() }
            current = null
            val now = clock()
            // PROTOCOL.md rule 4: CLIP_ENDED carries the start time when the coin knew the time
            // while recording; otherwise receive time minus duration is the estimate.
            val started = m.startUtcMs.takeIf { it != 0L }
                ?: startTimes[m.clip]?.takeIf { it != 0L }
                ?: (now - m.durationMs)
            val clip = store.commitReceived(t.record, m, startedAtMs = started, receivedAtMs = now)
            received += clip
            listener.onClipReceived(clip)
            send(CtrlMessage.Ack(m.clip, m.crc32))
            store.markAcked(t.record, clock())
            acked += m.clip
            return true
        }

        /** Handles ERROR for the current GET; true when the transfer is over. */
        private suspend fun error(t: Transfer, m: CtrlMessage.Error): Boolean = when (m.code) {
            ErrorCode.UNKNOWN_CLIP -> {
                withContext(io) { t.assembler.close() }
                current = null
                store.markTransferFailed(t.record, "the coin no longer has this clip")
                true
            }
            ErrorCode.BAD_OFFSET -> if (t.restartedFromZero) {
                giveUp(t, "bad offset at 0")
                true
            } else {
                restartFromZero(t)
                false
            }
            ErrorCode.BUSY -> if (t.busy++ < config.maxBusyRetries) {
                delay(config.busyRetryDelay)
                send(CtrlMessage.Get(t.record.clip, t.assembler.contiguous))
                false
            } else {
                giveUp(t, "coin busy")
                true
            }
            else -> {
                giveUp(t, "GET failed with error ${m.code}")
                true
            }
        }

        private suspend fun resume(t: Transfer, why: String): Boolean {
            if (t.resumes++ >= config.maxResumes) return false
            log("clip ${t.record.clip}: $why; resuming")
            checkpoint(t)
            send(CtrlMessage.Get(t.record.clip, t.assembler.contiguous))
            return true
        }

        private suspend fun restartFromZero(t: Transfer) {
            t.restartedFromZero = true
            withContext(io) { t.assembler.reset(); t.assembler.sync() }
            t.durable = 0
            store.saveProgress(t.record, 0)
            send(CtrlMessage.Get(t.record.clip, 0))
        }

        private suspend fun checkpoint(t: Transfer) {
            val bytes = t.assembler.contiguous
            if (bytes == t.durable) return
            withContext(io) { t.assembler.sync() }
            store.saveProgress(t.record, bytes)
            t.durable = bytes
        }

        /** Stops this clip for the session, keeping its progress for the next one. */
        private suspend fun giveUp(t: Transfer, why: String = "giving up for this session") {
            log("clip ${t.record.clip}: $why")
            saveAndClose(t)
            current = null
        }

        private suspend fun saveAndClose(t: Transfer) {
            // Runs on the way out of a cancelled or failed session too.
            withContext(kotlinx.coroutines.NonCancellable) {
                runCatching { checkpoint(t) }
                runCatching { withContext(io) { t.assembler.close() } }
            }
        }

        // Unsolicited and out-of-band messages

        private fun aside(inbound: Inbound) {
            val msg = (inbound as? Inbound.Ctrl)?.message ?: return // data for no current transfer
            when (msg) {
                is CtrlMessage.Status -> onStatus(msg)
                // PROTOCOL-Q: the protocol does not say whether CLIP_STARTED / CLIP_ENDED arrive
                // outside a GET stream when the user records during a session. If they do, the clip
                // is fetched in this session.
                is CtrlMessage.ClipStarted -> {
                    startTimes[msg.clip] = msg.startUtcMs
                    enqueue(msg.clip)
                }
                is CtrlMessage.ClipEnded -> enqueue(msg.clip)
                is CtrlMessage.Error -> {
                    log("ERROR for ${typeName(msg.requestType)}: code ${msg.code}, clip ${msg.clip}")
                    // A refused re-ACK means the stored copy is not the clip the coin holds under
                    // this id (the id wrapped): fetch it as a new clip.
                    if (msg.requestType == MessageType.ACK && msg.code == ErrorCode.INVALID_VALUE && msg.clip in reAcked) {
                        reAcked -= msg.clip
                        acked -= msg.clip
                        if (fetchAsNew.add(msg.clip)) queue.addLast(msg.clip)
                    }
                }
                is CtrlMessage.Unknown -> log("ignoring unknown message type 0x%02x".format(msg.type))
                else -> log("ignoring unexpected ${msg::class.simpleName}")
            }
        }

        private fun onStatus(s: CtrlMessage.Status) {
            status = s
            listener.onStatus(device, s)
        }

        private fun log(message: String) = listener.onLog(device, message)
    }

    private fun typeName(type: Int) = when (type) {
        MessageType.HELLO -> "HELLO"
        MessageType.LIST -> "LIST"
        MessageType.GET -> "GET"
        MessageType.ACK -> "ACK"
        MessageType.CFG_SET -> "CFG_SET"
        MessageType.REPLY -> "REPLY"
        MessageType.WIFI_SET -> "WIFI_SET"
        else -> "0x%02x".format(type)
    }

    companion object {
        /** Packet payload at MTU 247; exposed for tests and the BLE layer. */
        const val PAYLOAD_PER_PACKET = Protocol.MAX_DATA_PAYLOAD
    }
}
