package dev.coinai.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

/** Codec behaviour the shared vectors do not cover. */
class CtrlCodecTest {
    @Test
    fun unknownTypeDecodesToUnknownAndRoundTrips() {
        val bytes = "c00300aabbcc".hexToBytes()
        val msg = CtrlMessage.decode(bytes)
        assertEquals(CtrlMessage.Unknown(0xC0, "aabbcc".hexToBytes()), msg)
        assertArrayEquals(bytes, msg.encode())
    }

    @Test
    fun unknownTypeWithEmptyValue() {
        assertEquals(CtrlMessage.Unknown(0x3F, ByteArray(0)), CtrlMessage.decode("3f0000".hexToBytes()))
    }

    @Test
    fun malformedInputThrowsProtocolException() {
        val bad = listOf(
            "",             // no header
            "8100",         // short header
            "0309000700",   // len says 9, 2 value bytes present
            "030500070000100000", // len 5 but 6 value bytes follow
            "0305000700001000",   // GET value one byte short
            "06010001",     // CFG_SET reply_wait_s without its u16
            "060300092d00", // unknown CFG_SET key
            "0703000700ff", // unknown REPLY state
            "840300000600", // LIST_RESULT entries not 7-byte aligned
            "0405000700efbeadde", // ACK len 5 with 6 bytes
        )
        for (hex in bad) {
            assertThrows("decode $hex", ProtocolException::class.java) { CtrlMessage.decode(hex.hexToBytes()) }
        }
    }

    @Test
    fun knownTypeWithTrailingBytesIsRejected() {
        // ERROR value is 4 bytes; a 5-byte value is malformed, not silently truncated.
        assertThrows(ProtocolException::class.java) { CtrlMessage.decode("8505000301090000".hexToBytes()) }
    }

    @Test
    fun encodeRejectsOutOfRangeFields() {
        val bad: List<() -> CtrlMessage> = listOf(
            { CtrlMessage.WifiSet("x".repeat(33), "") },
            { CtrlMessage.WifiSet("ok", "p".repeat(65)) },
            { CtrlMessage.Reply(1, ReplyState.DONE, "a".repeat(201)) },
            { CtrlMessage.Get(0x1_0000, 0) },
            { CtrlMessage.Ack(1, 0x1_0000_0000L) },
            { CtrlMessage.Ack(-1, 0) },
            { CtrlMessage.CfgSet(CfgKey.BRIGHTNESS, 256) },
            { CtrlMessage.CfgSet(CfgKey.UTC_OFFSET_MIN, -721) },
            { CtrlMessage.CfgSet(CfgKey.UTC_OFFSET_MIN, 841) },
            { CtrlMessage.CfgSet(CfgKey.REPLY_WAIT_S, -1) },
            { CtrlMessage.ListResult(false, List(35) { ClipEntry(it, 1, true) }) },
            { CtrlMessage.Status(87, false, false, false, 0, "v".repeat(33)) },
        )
        for (make in bad) {
            val msg = make()
            assertThrows("encode $msg", IllegalArgumentException::class.java) { msg.encode() }
        }
    }

    @Test
    fun utcOffsetIsI16TwosComplement() {
        val cases = mapOf(
            -300 to "06030005d4fe", // UTC-5, PROTOCOL.md's example
            -720 to "0603000530fd", // lower bound
            840 to "060300054803", // upper bound
            0 to "060300050000",
            330 to "060300054a01", // India
            -1 to "06030005ffff",
        )
        for ((minutes, hex) in cases) {
            val msg = CtrlMessage.CfgSet(CfgKey.UTC_OFFSET_MIN, minutes)
            assertEquals("encode $minutes", hex, msg.encode().toHex())
            assertEquals("decode $hex", msg, CtrlMessage.decode(hex.hexToBytes()))
        }
    }

    @Test
    fun fullListResultFitsOneAttOperation() {
        val msg = CtrlMessage.ListResult(true, List(Protocol.MAX_LIST_ENTRIES) { ClipEntry(it, 70_000L, it % 2 == 0) })
        val bytes = msg.encode()
        assertEquals(Protocol.TLV_HEADER_SIZE + 1 + 7 * 34, bytes.size)
        assertEquals(true, bytes.size <= Protocol.MAX_TLV_SIZE)
        assertEquals(msg, CtrlMessage.decode(bytes))
    }

    @Test
    fun largeUnsignedValuesRoundTrip() {
        val msgs = listOf(
            CtrlMessage.Ack(0xFFFF, 0xFFFF_FFFFL),
            CtrlMessage.ClipEnded(65535, 0xFFFF_FFFFL, 0x8000_0000L, 0xDEAD_BEEFL, 0xFFFF_FFFFL, Long.MAX_VALUE),
            CtrlMessage.Error(MessageType.GET, ErrorCode.BUSY, Protocol.NO_CLIP),
            CtrlMessage.CfgSet(CfgKey.OFF_AFTER_H, 0xFFFF),
            CtrlMessage.Status(Protocol.BATTERY_UNKNOWN, true, false, true, 65535, ""),
        )
        for (m in msgs) assertEquals(m, CtrlMessage.decode(m.encode()))
    }

    @Test
    fun replyTruncatesAtCharacterBoundary() {
        // 199 ASCII bytes then a 3-byte em dash: the dash would cross 200 bytes and is dropped whole.
        val text = "a".repeat(199) + "\u2014" + "tail"
        val reply = CtrlMessage.Reply.truncating(7, ReplyState.DONE, text)
        assertEquals("a".repeat(199), reply.text)
        reply.encode()

        // A surrogate pair (4 UTF-8 bytes) is never split.
        val emoji = "a".repeat(197) + "\uD83D\uDE00"
        assertEquals("a".repeat(197), CtrlMessage.Reply.truncating(1, ReplyState.DONE, emoji).text)
        assertEquals(emoji, utf8Prefix(emoji, 201))
        assertEquals("short", CtrlMessage.Reply.truncating(1, ReplyState.DONE, "short").text)
    }

    @Test
    fun dataPacketLimits() {
        assertThrows(ProtocolException::class.java) { DataPacket.decode("0700ee0000".hexToBytes()) }
        assertEquals(DataPacket(7, 238, ByteArray(0)), DataPacket.decode("0700ee000000".hexToBytes()))
        assertThrows(IllegalArgumentException::class.java) { DataPacket(1, 0, ByteArray(239)).encode() }
        assertEquals(Protocol.DATA_HEADER_SIZE + 238, DataPacket(1, 0, ByteArray(238)).encode().size)
    }
}
