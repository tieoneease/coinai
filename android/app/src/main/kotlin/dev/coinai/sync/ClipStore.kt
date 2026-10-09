package dev.coinai.sync

import dev.coinai.protocol.CtrlMessage
import java.io.File
import java.io.IOException
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * One clip's transfer as the store knows it. A clip id can occur more than once per device (ids
 * wrap after 65535), so a record is a row, and [latest][ClipStore.latest] returns the newest.
 */
data class TransferRecord(
    val rowId: Long,
    val device: String,
    val clip: Int,
    /** Bytes from offset 0 that are synced to [partFile]; the resume offset for `GET`. */
    val durableBytes: Long,
    val received: Boolean,
    val acked: Boolean,
    val size: Long?,
    val crc32: Long?,
    /** Where the engine assembles the clip until it is verified. */
    val partFile: File,
)

/** A clip that is complete, CRC-checked and committed; ready for processing. */
data class ReceivedClip(
    val rowId: Long,
    val device: String,
    val clip: Int,
    val file: File,
    val size: Long,
    val durationMs: Long,
    val voicedMs: Long,
)

/** Storage the [SyncEngine] needs. The app implements it with Room; tests use an in-memory one. */
interface ClipStore {
    suspend fun latest(device: String, clip: Int): TransferRecord?

    /** The clip's unfinished record, or a new one when there is none (or the latest is received). */
    suspend fun beginTransfer(device: String, clip: Int): TransferRecord

    /** Records that [durableBytes] of the part file are synced. Never ahead of the file. */
    suspend fun saveProgress(record: TransferRecord, durableBytes: Long)

    /**
     * Adopts the synced, verified part file (for example with [promotePartFile]) and marks the clip
     * received. Must be durable when it returns: the engine ACKs right after, and the coin then
     * deletes its copy.
     */
    suspend fun commitReceived(record: TransferRecord, ended: CtrlMessage.ClipEnded, startedAtMs: Long, receivedAtMs: Long): ReceivedClip

    suspend fun markAcked(record: TransferRecord, atMs: Long)

    /** The transfer cannot finish, for example because the coin no longer has the clip. */
    suspend fun markTransferFailed(record: TransferRecord, reason: String)
}

/** Atomically renames a synced part file to its final name. */
fun promotePartFile(part: File, final: File) {
    final.parentFile?.mkdirs()
    try {
        Files.move(part.toPath(), final.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
    } catch (e: AtomicMoveNotSupportedException) {
        throw IOException("cannot rename $part atomically", e)
    }
}
