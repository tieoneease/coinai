package dev.coinai.data

import androidx.room3.withWriteTransaction
import dev.coinai.clips.ClipRecord
import dev.coinai.clips.ClipState
import dev.coinai.clips.ProcessingStore
import dev.coinai.protocol.CtrlMessage
import dev.coinai.sync.ClipStore
import dev.coinai.sync.ReceivedClip
import dev.coinai.sync.TransferRecord
import dev.coinai.sync.promotePartFile
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import java.io.File

/**
 * Room-backed storage for both the sync engine and clip processing. Clip files live under [root]
 * (app-private storage): `<device>/<clip>-<row>.part` while receiving, `.ogg` once verified.
 */
class ClipRepository(
    private val db: ClipDatabase,
    private val root: File,
    private val clock: () -> Long = System::currentTimeMillis,
) : ClipStore, ProcessingStore {
    private val dao = db.clips()

    val clips: Flow<List<ClipRecord>> = dao.observeAll().map { list -> list.map { it.toRecord() } }

    private fun dir(device: String) = File(root, device.replace(Regex("[^A-Za-z0-9]"), "-"))
    private fun partFile(device: String, clip: Int, row: Long) = File(dir(device), "$clip-$row.part")
    private fun finalFile(device: String, clip: Int, row: Long) = File(dir(device), "$clip-$row.ogg")

    private fun ClipEntity.toTransfer() = TransferRecord(
        rowId = id,
        device = device,
        clip = clipId,
        durableBytes = receivedBytes,
        received = state != ClipState.RECEIVING && filePath != null,
        acked = ackedAtMs != null,
        size = size,
        crc32 = crc32,
        partFile = partFile(device, clipId, id),
    )

    // ClipStore

    override suspend fun latest(device: String, clip: Int): TransferRecord? = dao.latest(device, clip)?.toTransfer()

    override suspend fun beginTransfer(device: String, clip: Int): TransferRecord = db.withWriteTransaction {
        val existing = dao.latest(device, clip)
        if (existing != null && existing.state == ClipState.RECEIVING) {
            existing.toTransfer()
        } else {
            val id = dao.insert(ClipEntity(device = device, clipId = clip, state = ClipState.RECEIVING, createdAtMs = clock()))
            dao.get(id)!!.toTransfer()
        }
    }

    override suspend fun saveProgress(record: TransferRecord, durableBytes: Long) {
        dao.setProgress(record.rowId, durableBytes)
    }

    override suspend fun commitReceived(
        record: TransferRecord, ended: CtrlMessage.ClipEnded, startedAtMs: Long, receivedAtMs: Long,
    ): ReceivedClip {
        val final = finalFile(record.device, record.clip, record.rowId)
        promotePartFile(record.partFile, final)
        // Room commits (and SQLite syncs) before this returns, so the ACK that follows is safe.
        dao.markReceived(
            record.rowId, ended.size, ended.crc32, ended.durationMs, ended.voicedMs, startedAtMs, receivedAtMs, final.path,
        )
        return ReceivedClip(record.rowId, record.device, record.clip, final, ended.size, ended.durationMs, ended.voicedMs)
    }

    override suspend fun markAcked(record: TransferRecord, atMs: Long) = dao.markAcked(record.rowId, atMs)

    override suspend fun markTransferFailed(record: TransferRecord, reason: String) {
        record.partFile.delete()
        dao.setState(record.rowId, ClipState.FAILED, reason)
    }

    // ProcessingStore

    override suspend fun get(id: Long): ClipRecord? = dao.get(id)?.toRecord()

    override suspend fun saveTranscript(id: Long, transcript: String) {
        dao.saveTranscript(id, transcript)
    }

    override suspend fun claimPrompt(id: Long): Boolean = db.withWriteTransaction {
        dao.transition(id, ClipState.TRANSCRIBED, ClipState.PROMPTED) == 1
    }

    override suspend fun releasePrompt(id: Long) {
        db.withWriteTransaction { dao.transition(id, ClipState.PROMPTED, ClipState.TRANSCRIBED) }
    }

    override suspend fun saveReply(id: Long, reply: String, partial: Boolean) {
        dao.saveReply(id, reply, partial)
    }

    override suspend fun setState(id: Long, state: ClipState, error: String?) = dao.setState(id, state, error)

    override suspend fun transition(id: Long, from: ClipState, to: ClipState, error: String?): Boolean =
        db.withWriteTransaction { dao.transitionWithError(id, from, to, error) == 1 }

    override suspend fun prepareResend(id: Long): Boolean = db.withWriteTransaction {
        val clip = dao.get(id) ?: return@withWriteTransaction false
        if (!clip.state.resendable || clip.filePath == null || !File(clip.filePath).exists()) return@withWriteTransaction false
        val to = if (clip.transcript.isNullOrBlank()) ClipState.RECEIVED else ClipState.TRANSCRIBED
        dao.resetForResend(id, to, forceSpeech = clip.forceSpeech || clip.state == ClipState.NO_SPEECH)
        true
    }

    /** Clips whose processing should be (re)scheduled, for example after an app update. */
    suspend fun pendingProcessing(): List<Long> =
        dao.inStates(listOf(ClipState.RECEIVED, ClipState.TRANSCRIBED, ClipState.PROMPTED)).map { it.id }
}
