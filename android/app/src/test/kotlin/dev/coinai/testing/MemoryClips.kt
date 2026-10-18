package dev.coinai.testing

import dev.coinai.clips.ClipRecord
import dev.coinai.clips.ClipState
import dev.coinai.clips.ProcessingStore
import dev.coinai.protocol.CtrlMessage
import dev.coinai.sync.ClipStore
import dev.coinai.sync.ReceivedClip
import dev.coinai.sync.TransferRecord
import dev.coinai.sync.promotePartFile
import java.io.File

/** In-memory [ClipStore] and [ProcessingStore] with real files under [root]; mirrors ClipRepository. */
class MemoryClips(private val root: File) : ClipStore, ProcessingStore {
    val rows = java.util.concurrent.ConcurrentHashMap<Long, ClipRecord>()
    private var nextId = 1L
    /** Every saveProgress call, in order: (clip, bytes). */
    val progress = mutableListOf<Pair<Int, Long>>()
    var claims = 0

    private fun part(r: ClipRecord) = File(root, "${r.clipId}-${r.id}.part")
    private fun ClipRecord.toTransfer() = TransferRecord(
        id, device, clipId, receivedBytes, state != ClipState.RECEIVING && filePath != null, ackedAtMs != null, size, crc32, part(this),
    )

    @Synchronized fun insert(record: ClipRecord): ClipRecord {
        val r = record.copy(id = nextId++)
        rows[r.id] = r
        return r
    }

    override suspend fun latest(device: String, clip: Int) =
        rows.values.filter { it.device == device && it.clipId == clip }.maxByOrNull { it.id }?.toTransfer()

    override suspend fun unfinished(device: String) =
        rows.values.filter { it.device == device && it.state == ClipState.RECEIVING }.sortedBy { it.id }.map { it.clipId }.distinct()

    override suspend fun beginTransfer(device: String, clip: Int): TransferRecord {
        val existing = rows.values.filter { it.device == device && it.clipId == clip }.maxByOrNull { it.id }
        if (existing != null && existing.state == ClipState.RECEIVING) return existing.toTransfer()
        return insert(ClipRecord(0, device, clip, ClipState.RECEIVING)).toTransfer()
    }

    override suspend fun saveProgress(record: TransferRecord, durableBytes: Long) {
        progress += record.clip to durableBytes
        rows[record.rowId] = rows.getValue(record.rowId).copy(receivedBytes = durableBytes)
    }

    override suspend fun commitReceived(record: TransferRecord, ended: CtrlMessage.ClipEnded, startedAtMs: Long, receivedAtMs: Long): ReceivedClip {
        val final = File(root, "${record.clip}-${record.rowId}.ogg")
        promotePartFile(record.partFile, final)
        rows[record.rowId] = rows.getValue(record.rowId).copy(
            state = ClipState.RECEIVED, size = ended.size, crc32 = ended.crc32, durationMs = ended.durationMs,
            voicedMs = ended.voicedMs, receivedBytes = ended.size, startedAtMs = startedAtMs, receivedAtMs = receivedAtMs,
            filePath = final.path,
        )
        return ReceivedClip(record.rowId, record.device, record.clip, final, ended.size, ended.durationMs, ended.voicedMs)
    }

    override suspend fun markAcked(record: TransferRecord, atMs: Long) {
        rows[record.rowId] = rows.getValue(record.rowId).copy(ackedAtMs = atMs)
    }

    override suspend fun discardTransfer(record: TransferRecord) {
        record.partFile.delete()
        rows.computeIfPresent(record.rowId) { _, r -> r.takeIf { it.state != ClipState.RECEIVING } }
    }

    override suspend fun get(id: Long) = rows[id]

    override suspend fun saveTranscript(id: Long, transcript: String) {
        val r = rows.getValue(id)
        if (r.state == ClipState.RECEIVED) rows[id] = r.copy(state = ClipState.TRANSCRIBED, transcript = transcript, error = null)
    }

    override suspend fun claimPrompt(id: Long): Boolean = synchronized(this) {
        val r = rows.getValue(id)
        if (r.state != ClipState.TRANSCRIBED) return false
        claims++
        rows[id] = r.copy(state = ClipState.PROMPTED)
        true
    }

    override suspend fun releasePrompt(id: Long) {
        val r = rows.getValue(id)
        if (r.state == ClipState.PROMPTED) rows[id] = r.copy(state = ClipState.TRANSCRIBED)
    }

    override suspend fun saveReply(id: Long, reply: String, partial: Boolean) {
        val r = rows.getValue(id)
        if (r.state == ClipState.PROMPTED) rows[id] = r.copy(state = ClipState.REPLIED, reply = reply, replyPartial = partial, error = null)
    }

    override suspend fun saveSceneReply(id: Long, scene: String, summary: String): Boolean = synchronized(this) {
        val r = rows.getValue(id)
        if (r.state != ClipState.TRANSCRIBED) return false
        rows[id] = r.copy(state = ClipState.REPLIED, scene = scene, reply = summary, replyPartial = false, error = null)
        true
    }

    override suspend fun saveHandoff(id: Long, scene: String, handoff: String): Boolean = synchronized(this) {
        val r = rows.getValue(id)
        if (r.state != ClipState.TRANSCRIBED) return false
        rows[id] = r.copy(scene = scene, handoff = handoff)
        true
    }

    override suspend fun setState(id: Long, state: ClipState, error: String?) {
        rows[id] = rows.getValue(id).copy(state = state, error = error)
    }

    override suspend fun transition(id: Long, from: ClipState, to: ClipState, error: String?): Boolean = synchronized(this) {
        val r = rows.getValue(id)
        if (r.state != from) return false
        rows[id] = r.copy(state = to, error = error)
        true
    }

    override suspend fun prepareResend(id: Long): Boolean {
        val r = rows[id] ?: return false
        if (!r.state.resendable || r.filePath == null || !File(r.filePath).exists()) return false
        val to = if (r.transcript.isNullOrBlank()) ClipState.RECEIVED else ClipState.TRANSCRIBED
        rows[id] = r.copy(state = to, forceSpeech = r.forceSpeech || r.state == ClipState.NO_SPEECH, error = null)
        return true
    }
}
