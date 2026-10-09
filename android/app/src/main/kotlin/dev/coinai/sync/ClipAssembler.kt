package dev.coinai.sync

import java.io.Closeable
import java.io.File
import java.io.RandomAccessFile
import java.util.TreeMap
import java.util.zip.CRC32

/**
 * Reassembles one clip from `data` packets into [file] by offset. Bytes `[0, contiguous)` are all
 * present; packets past a gap are written in place and tracked until the gap fills, so reordered
 * packets cost nothing and duplicates are ignored.
 */
class ClipAssembler(private val file: File, resumeOffset: Long) : Closeable {
    private val raf: RandomAccessFile
    /** Received ranges beyond [contiguous]: start → end (exclusive), disjoint and not touching. */
    private val ahead = TreeMap<Long, Long>()

    var contiguous: Long
        private set
    var duplicates: Int = 0
        private set
    var outOfOrder: Int = 0
        private set

    init {
        file.parentFile?.mkdirs()
        raf = RandomAccessFile(file, "rw")
        // The stored offset is only trusted as far as the file really reaches; anything past it is
        // an unsynced tail from an earlier session and is dropped.
        contiguous = resumeOffset.coerceIn(0, raf.length())
        raf.setLength(contiguous)
    }

    /** True when bytes past a hole have arrived: the stream skipped something. */
    val hasGap: Boolean get() = ahead.isNotEmpty()

    /** One past the highest byte received. */
    val highWater: Long get() = maxOf(contiguous, ahead.lastEntry()?.value ?: 0L)

    enum class Result { APPENDED, OUT_OF_ORDER, DUPLICATE }

    fun accept(offset: Long, payload: ByteArray): Result {
        val end = offset + payload.size
        if (payload.isEmpty() || end <= contiguous || covered(offset, end)) {
            duplicates++
            return Result.DUPLICATE
        }
        raf.seek(offset)
        raf.write(payload)
        if (offset > contiguous) {
            outOfOrder++
            addAhead(offset, end)
            return Result.OUT_OF_ORDER
        }
        contiguous = end
        // Pull in ranges the new bytes now touch.
        while (true) {
            val first = ahead.firstEntry() ?: break
            if (first.key > contiguous) break
            contiguous = maxOf(contiguous, first.value)
            ahead.remove(first.key)
        }
        return Result.APPENDED
    }

    private fun covered(start: Long, end: Long): Boolean {
        val floor = ahead.floorEntry(start) ?: return false
        return floor.value >= end
    }

    private fun addAhead(start: Long, end: Long) {
        var s = start
        var e = end
        ahead.floorEntry(s)?.let { if (it.value >= s) { s = it.key; e = maxOf(e, it.value); ahead.remove(it.key) } }
        while (true) {
            val next = ahead.ceilingEntry(s) ?: break
            if (next.key > e) break
            e = maxOf(e, next.value)
            ahead.remove(next.key)
        }
        ahead[s] = e
    }

    /** Forces written bytes to storage, so [contiguous] may be recorded as durable. */
    fun sync() {
        raf.fd.sync()
    }

    /** Starts over from offset 0 (after a CRC mismatch or a bad offset). */
    fun reset() {
        ahead.clear()
        contiguous = 0
        raf.setLength(0)
    }

    /** Drops bytes past [length]. */
    fun truncate(length: Long) {
        raf.setLength(length)
        ahead.clear()
        contiguous = minOf(contiguous, length)
    }

    fun length(): Long = raf.length()

    /** zlib-compatible CRC-32 (IEEE 802.3) of the first [length] bytes. */
    fun crc32(length: Long): Long {
        val crc = CRC32()
        val buf = ByteArray(64 * 1024)
        raf.seek(0)
        var left = length
        while (left > 0) {
            val n = raf.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
            if (n < 0) break
            crc.update(buf, 0, n)
            left -= n
        }
        return crc.value
    }

    override fun close() = raf.close()
}
