package dev.coinai.sync

import dev.coinai.protocol.CfgKey
import dev.coinai.protocol.CtrlMessage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.util.SimpleTimeZone
import java.util.TimeZone

class CoinClockTest {
    private val january = 1_767_225_600_000L // 2026-01-01T00:00Z
    private val july = 1_782_864_000_000L // 2026-07-01T00:00Z

    private fun offset(zone: String, at: Long) = CoinClock.utcOffsetMinutes(at, TimeZone.getTimeZone(zone))

    @Test
    fun offsetIncludesDaylightSavingAtThatInstant() {
        assertEquals(-300, offset("America/New_York", january))
        assertEquals(-240, offset("America/New_York", july))
        assertEquals(60, offset("Europe/Berlin", january))
        assertEquals(120, offset("Europe/Berlin", july))
        assertEquals(660, offset("Australia/Sydney", january)) // southern summer
        assertEquals(0, offset("UTC", july))
    }

    @Test
    fun oddOffsetsAndTheExtremes() {
        assertEquals(330, offset("Asia/Kolkata", july))
        assertEquals(345, offset("Asia/Kathmandu", july))
        assertEquals(-570, offset("Pacific/Marquesas", july))
        assertEquals(840, offset("Pacific/Kiritimati", july))
        assertEquals(-720, offset("Etc/GMT+12", july))
    }

    @Test
    fun messageIsNullOutsideTheProtocolRange() {
        assertEquals(CtrlMessage.CfgSet(CfgKey.UTC_OFFSET_MIN, -300), CoinClock.offsetMessage(january, TimeZone.getTimeZone("America/New_York")))
        assertNull(CoinClock.offsetMessage(january, SimpleTimeZone(15 * 3_600_000, "too-far-east")))
        assertNull(CoinClock.offsetMessage(january, SimpleTimeZone(-13 * 3_600_000, "too-far-west")))
    }
}
