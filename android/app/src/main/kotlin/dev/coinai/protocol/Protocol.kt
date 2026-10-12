package dev.coinai.protocol

import java.io.ByteArrayOutputStream
import java.util.UUID

/**
 * Constants of PROTOCOL.md v1. `ProtocolVectorsTest` checks the UUIDs and version against
 * protocol/vectors/v1.json, the shared source of truth with the firmware.
 */
object Protocol {
    const val VERSION: Int = 1

    val SERVICE_UUID: UUID = UUID.fromString("c0a10000-f569-41ae-8676-7f5a60d835fc")
    val CTRL_UUID: UUID = UUID.fromString("c0a10001-f569-41ae-8676-7f5a60d835fc")
    val DATA_UUID: UUID = UUID.fromString("c0a10002-f569-41ae-8676-7f5a60d835fc")

    /** One TLV per ATT operation: at most 244 bytes at MTU 247. */
    const val MAX_TLV_SIZE: Int = 244
    const val TLV_HEADER_SIZE: Int = 3
    const val MAX_VALUE_SIZE: Int = MAX_TLV_SIZE - TLV_HEADER_SIZE

    const val DATA_HEADER_SIZE: Int = 6
    const val MAX_DATA_PAYLOAD: Int = 238

    const val MAX_SSID_BYTES: Int = 32
    const val MAX_PASSWORD_BYTES: Int = 64
    const val MAX_REPLY_TEXT_BYTES: Int = 200
    const val MAX_FW_VERSION_BYTES: Int = 32
    const val MAX_LIST_ENTRIES: Int = 34

    const val CODEC_OGG_OPUS: Int = 1
    const val BATTERY_UNKNOWN: Int = 255
    /** `clip` value in `ERROR` when the error concerns no clip. */
    const val NO_CLIP: Int = 0xFFFF
}

/** TLV type codes. `0x01`–`0x3F` go app → device; `0x80`–`0xBF` go device → app. */
object MessageType {
    const val HELLO: Int = 0x01
    const val LIST: Int = 0x02
    const val GET: Int = 0x03
    const val ACK: Int = 0x04
    const val WIFI_SET: Int = 0x05
    const val CFG_SET: Int = 0x06
    const val REPLY: Int = 0x07
    const val STATUS: Int = 0x81
    const val CLIP_STARTED: Int = 0x82
    const val CLIP_ENDED: Int = 0x83
    const val LIST_RESULT: Int = 0x84
    const val ERROR: Int = 0x85
}

/** `code` values of `ERROR`. */
object ErrorCode {
    const val UNKNOWN_CLIP: Int = 1
    const val BAD_OFFSET: Int = 2
    const val BAD_LENGTH: Int = 3
    const val BUSY: Int = 4
    const val INVALID_VALUE: Int = 5
}

enum class ReplyState(val code: Int) {
    TRANSCRIBING(0), THINKING(1), DONE(2), FAILED(3);

    companion object {
        fun fromCode(code: Int): ReplyState =
            entries.firstOrNull { it.code == code } ?: throw ProtocolException("unknown REPLY state $code")
    }
}

/**
 * `CFG_SET` keys; [width] is the byte width of the little-endian value, [signed] whether it is
 * two's complement. [range] is what [CtrlMessage.encode] accepts: the wire range for the unsigned
 * keys (the coin checks their documented ranges and answers `ERROR` 5), and the documented
 * −720..840 for `utc_offset_min`, the only signed key.
 */
enum class CfgKey(val code: Int, val width: Int, val signed: Boolean = false, val range: IntRange = 0..(if (width == 1) 0xFF else 0xFFFF)) {
    REPLY_WAIT_S(0x01, 2),
    BRIGHTNESS(0x02, 1),
    IDLE_TIMEOUT_S(0x03, 2),
    OFF_AFTER_H(0x04, 2),
    /** Local time minus UTC in minutes (PROTOCOL.md rule 8); i16 on the wire. */
    UTC_OFFSET_MIN(0x05, 2, signed = true, range = -720..840),
    /** Delivery order (PROTOCOL.md "Wi-Fi delivery"): 0 Wi-Fi first, 1 phone first, 2 phone only. */
    DELIVERY(0x06, 2);

    companion object {
        fun fromCode(code: Int): CfgKey =
            entries.firstOrNull { it.code == code } ?: throw ProtocolException("unknown CFG_SET key $code")
    }
}

/** A malformed message on the wire. Unknown TLV types are not malformed: they decode to [CtrlMessage.Unknown]. */
class ProtocolException(message: String) : Exception(message)

/**
 * A `ctrl` characteristic message: `type u8, len u16 LE, value`. Integers are unsigned on the wire;
 * u16 travels as [Int] and u32 as [Long]. [encode] rejects out-of-range fields with
 * [IllegalArgumentException]; [decode] rejects malformed bytes with [ProtocolException].
 */
sealed class CtrlMessage {
    abstract val type: Int

    // app → device

    data class Hello(val phoneUtcMs: Long, val version: Int = Protocol.VERSION) : CtrlMessage() {
        override val type: Int get() = MessageType.HELLO
    }

    data object ListClips : CtrlMessage() {
        override val type: Int get() = MessageType.LIST
    }

    data class Get(val clip: Int, val offset: Long) : CtrlMessage() {
        override val type: Int get() = MessageType.GET
    }

    data class Ack(val clip: Int, val crc32: Long) : CtrlMessage() {
        override val type: Int get() = MessageType.ACK
    }

    data class WifiSet(val ssid: String, val password: String) : CtrlMessage() {
        override val type: Int get() = MessageType.WIFI_SET
    }

    data class CfgSet(val key: CfgKey, val value: Int) : CtrlMessage() {
        override val type: Int get() = MessageType.CFG_SET
    }

    data class Reply(val clip: Int, val state: ReplyState, val text: String) : CtrlMessage() {
        override val type: Int get() = MessageType.REPLY

        companion object {
            /** A [Reply] whose text is cut to [Protocol.MAX_REPLY_TEXT_BYTES] at a character boundary. */
            fun truncating(clip: Int, state: ReplyState, text: String): Reply =
                Reply(clip, state, utf8Prefix(text, Protocol.MAX_REPLY_TEXT_BYTES))
        }
    }

    // device → app

    data class Status(
        val batteryPct: Int,
        val charging: Boolean,
        val usbPower: Boolean,
        val recording: Boolean,
        val pending: Int,
        val fwVersion: String,
    ) : CtrlMessage() {
        override val type: Int get() = MessageType.STATUS
        val batteryKnown: Boolean get() = batteryPct != Protocol.BATTERY_UNKNOWN
    }

    data class ClipStarted(val clip: Int, val codec: Int, val sampleRate: Long, val startUtcMs: Long) : CtrlMessage() {
        override val type: Int get() = MessageType.CLIP_STARTED
    }

    data class ClipEnded(val clip: Int, val size: Long, val durationMs: Long, val crc32: Long, val voicedMs: Long, val startUtcMs: Long = 0) : CtrlMessage() {
        override val type: Int get() = MessageType.CLIP_ENDED
    }

    data class ListResult(val more: Boolean, val entries: List<ClipEntry>) : CtrlMessage() {
        override val type: Int get() = MessageType.LIST_RESULT
    }

    data class Error(val requestType: Int, val code: Int, val clip: Int) : CtrlMessage() {
        override val type: Int get() = MessageType.ERROR
    }

    /** A TLV type this version does not know. PROTOCOL.md rule 5: ignore it, never fail on it. */
    class Unknown(override val type: Int, value: ByteArray) : CtrlMessage() {
        private val bytes = value.copyOf()
        val value: ByteArray get() = bytes.copyOf()

        override fun equals(other: Any?): Boolean =
            other is Unknown && other.type == type && other.bytes.contentEquals(bytes)

        override fun hashCode(): Int = 31 * type + bytes.contentHashCode()
        override fun toString(): String = "Unknown(type=0x%02x, value=%s)".format(type, bytes.toHex())
    }

    fun encode(): ByteArray {
        val v = LeWriter()
        when (this) {
            is Hello -> { v.u8(version); v.u64(phoneUtcMs) }
            ListClips -> Unit
            is Get -> { v.u16(clip); v.u32(offset) }
            is Ack -> { v.u16(clip); v.u32(crc32) }
            is WifiSet -> {
                v.shortString(ssid, Protocol.MAX_SSID_BYTES, "ssid")
                v.shortString(password, Protocol.MAX_PASSWORD_BYTES, "password")
            }
            is CfgSet -> {
                require(value in key.range) { "$key value $value out of range ${key.range}" }
                v.u8(key.code)
                if (key.width == 1) v.u8(value) else v.u16(value and 0xFFFF)
            }
            is Reply -> { v.u16(clip); v.u8(state.code); v.restString(text, Protocol.MAX_REPLY_TEXT_BYTES, "text") }
            is Status -> {
                v.u8(batteryPct)
                v.u8((if (charging) 1 else 0) or (if (usbPower) 2 else 0) or (if (recording) 4 else 0))
                v.u16(pending)
                v.restString(fwVersion, Protocol.MAX_FW_VERSION_BYTES, "fw_version")
            }
            is ClipStarted -> { v.u16(clip); v.u8(codec); v.u32(sampleRate); v.u64(startUtcMs) }
            is ClipEnded -> { v.u16(clip); v.u32(size); v.u32(durationMs); v.u32(crc32); v.u32(voicedMs); v.u64(startUtcMs) }
            is ListResult -> {
                require(entries.size <= Protocol.MAX_LIST_ENTRIES) { "LIST_RESULT holds at most ${Protocol.MAX_LIST_ENTRIES} entries" }
                v.u8(if (more) 1 else 0)
                for (e in entries) { v.u16(e.clip); v.u32(e.size); v.u8(if (e.complete) 1 else 0) }
            }
            is Error -> { v.u8(requestType); v.u8(code); v.u16(clip) }
            is Unknown -> v.bytes(value)
        }
        val value = v.toByteArray()
        require(type in 0..0xFF) { "type $type out of range" }
        require(value.size <= Protocol.MAX_VALUE_SIZE) { "value of ${value.size} bytes exceeds ${Protocol.MAX_VALUE_SIZE}" }
        return LeWriter().apply { u8(type); u16(value.size); bytes(value) }.toByteArray()
    }

    companion object {
        /** Decodes exactly one TLV; [bytes] must be the whole ATT value. */
        fun decode(bytes: ByteArray): CtrlMessage {
            if (bytes.size < Protocol.TLV_HEADER_SIZE) throw ProtocolException("TLV of ${bytes.size} bytes has no header")
            val header = LeReader(bytes)
            val type = header.u8()
            val len = header.u16()
            if (bytes.size != Protocol.TLV_HEADER_SIZE + len) {
                throw ProtocolException("TLV length $len does not match ${bytes.size - Protocol.TLV_HEADER_SIZE} value bytes")
            }
            val r = LeReader(bytes.copyOfRange(Protocol.TLV_HEADER_SIZE, bytes.size))
            val msg = when (type) {
                MessageType.HELLO -> { val version = r.u8(); Hello(phoneUtcMs = r.u64(), version = version) }
                MessageType.LIST -> ListClips
                MessageType.GET -> Get(r.u16(), r.u32())
                MessageType.ACK -> Ack(r.u16(), r.u32())
                MessageType.WIFI_SET -> WifiSet(r.shortString(), r.shortString())
                MessageType.CFG_SET -> {
                    val key = CfgKey.fromCode(r.u8())
                    val raw = if (key.width == 1) r.u8() else r.u16()
                    CfgSet(key, if (key.signed) raw.toShort().toInt() else raw)
                }
                MessageType.REPLY -> Reply(r.u16(), ReplyState.fromCode(r.u8()), r.restString())
                MessageType.STATUS -> {
                    val battery = r.u8()
                    val flags = r.u8()
                    Status(battery, flags and 1 != 0, flags and 2 != 0, flags and 4 != 0, r.u16(), r.restString())
                }
                MessageType.CLIP_STARTED -> ClipStarted(r.u16(), r.u8(), r.u32(), r.u64())
                MessageType.CLIP_ENDED -> ClipEnded(r.u16(), r.u32(), r.u32(), r.u32(), r.u32(), r.u64())
                MessageType.LIST_RESULT -> {
                    val more = r.u8() and 1 != 0
                    if (r.remaining % CLIP_ENTRY_SIZE != 0) throw ProtocolException("LIST_RESULT entries are not $CLIP_ENTRY_SIZE-byte aligned")
                    ListResult(more, List(r.remaining / CLIP_ENTRY_SIZE) { ClipEntry(r.u16(), r.u32(), r.u8() and 1 != 0) })
                }
                MessageType.ERROR -> Error(r.u8(), r.u8(), r.u16())
                else -> Unknown(type, r.rest())
            }
            r.requireEnd(type)
            return msg
        }

        private const val CLIP_ENTRY_SIZE = 7
    }
}

/** One `LIST_RESULT` entry. */
data class ClipEntry(val clip: Int, val size: Long, val complete: Boolean)

/** A `data` notification: `[clip u16 LE][offset u32 LE][payload]`. */
class DataPacket(val clip: Int, val offset: Long, payload: ByteArray) {
    private val data = payload.copyOf()
    val payload: ByteArray get() = data.copyOf()
    val payloadSize: Int get() = data.size

    fun encode(): ByteArray {
        require(data.size <= Protocol.MAX_DATA_PAYLOAD) { "payload of ${data.size} bytes exceeds ${Protocol.MAX_DATA_PAYLOAD}" }
        val w = LeWriter()
        w.u16(clip)
        w.u32(offset)
        w.bytes(data)
        return w.toByteArray()
    }

    override fun equals(other: Any?): Boolean =
        other is DataPacket && other.clip == clip && other.offset == offset && other.data.contentEquals(data)

    override fun hashCode(): Int = (31 * clip + offset.hashCode()) * 31 + data.contentHashCode()
    override fun toString(): String = "DataPacket(clip=$clip, offset=$offset, payload=${data.size} bytes)"

    companion object {
        /** Payload length is not capped on decode: a larger MTU may carry more. */
        fun decode(bytes: ByteArray): DataPacket {
            if (bytes.size < Protocol.DATA_HEADER_SIZE) throw ProtocolException("data packet of ${bytes.size} bytes has no header")
            val r = LeReader(bytes)
            return DataPacket(r.u16(), r.u32(), r.rest())
        }
    }
}

/** The longest prefix of [text] that is at most [maxBytes] in UTF-8, never splitting a character. */
fun utf8Prefix(text: String, maxBytes: Int): String {
    var bytes = 0
    var i = 0
    while (i < text.length) {
        val cp = text.codePointAt(i)
        val n = when {
            cp < 0x80 -> 1
            cp < 0x800 -> 2
            cp < 0x10000 -> 3
            else -> 4
        }
        if (bytes + n > maxBytes) break
        bytes += n
        i += Character.charCount(cp)
    }
    return text.substring(0, i)
}

internal fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it.toInt() and 0xFF) }

private class LeWriter {
    private val out = ByteArrayOutputStream()

    fun u8(v: Int) {
        require(v in 0..0xFF) { "u8 $v out of range" }
        out.write(v)
    }

    fun u16(v: Int) {
        require(v in 0..0xFFFF) { "u16 $v out of range" }
        out.write(v and 0xFF)
        out.write(v ushr 8)
    }

    fun u32(v: Long) {
        require(v in 0..0xFFFF_FFFFL) { "u32 $v out of range" }
        for (i in 0 until 4) out.write(((v ushr (8 * i)) and 0xFF).toInt())
    }

    fun u64(v: Long) {
        for (i in 0 until 8) out.write(((v ushr (8 * i)) and 0xFF).toInt())
    }

    fun bytes(b: ByteArray) = out.write(b)

    /** `len u8` then the UTF-8 bytes. */
    fun shortString(s: String, maxBytes: Int, name: String) {
        val b = s.toByteArray(Charsets.UTF_8)
        require(b.size <= maxBytes) { "$name is ${b.size} bytes, max $maxBytes" }
        u8(b.size)
        bytes(b)
    }

    /** UTF-8 bytes taking the rest of the value. */
    fun restString(s: String, maxBytes: Int, name: String) {
        val b = s.toByteArray(Charsets.UTF_8)
        require(b.size <= maxBytes) { "$name is ${b.size} bytes, max $maxBytes" }
        bytes(b)
    }

    fun toByteArray(): ByteArray = out.toByteArray()
}

private class LeReader(private val b: ByteArray) {
    private var pos = 0
    val remaining: Int get() = b.size - pos

    private fun need(n: Int) {
        if (remaining < n) throw ProtocolException("value truncated: need $n bytes at offset $pos, have $remaining")
    }

    fun u8(): Int {
        need(1)
        return b[pos++].toInt() and 0xFF
    }

    fun u16(): Int = u8() or (u8() shl 8)

    fun u32(): Long {
        need(4)
        var v = 0L
        for (i in 0 until 4) v = v or ((b[pos++].toLong() and 0xFF) shl (8 * i))
        return v
    }

    fun u64(): Long {
        need(8)
        var v = 0L
        for (i in 0 until 8) v = v or ((b[pos++].toLong() and 0xFF) shl (8 * i))
        return v
    }

    fun bytes(n: Int): ByteArray {
        need(n)
        return b.copyOfRange(pos, pos + n).also { pos += n }
    }

    fun rest(): ByteArray = bytes(remaining)
    fun shortString(): String = String(bytes(u8()), Charsets.UTF_8)
    fun restString(): String = String(rest(), Charsets.UTF_8)

    fun requireEnd(type: Int) {
        if (remaining != 0) throw ProtocolException("type 0x%02x has %d trailing bytes".format(type, remaining))
    }
}
