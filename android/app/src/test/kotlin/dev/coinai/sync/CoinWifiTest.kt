package dev.coinai.sync

import dev.coinai.protocol.CfgKey
import dev.coinai.protocol.CtrlMessage
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class CoinWifiTest {
    @Test
    fun ssidIsOneTo32Utf8Bytes() {
        assertNotNull(CoinWifi.ssidError(""))
        assertNull(CoinWifi.ssidError("a"))
        assertNull(CoinWifi.ssidError("s".repeat(32)))
        assertNotNull(CoinWifi.ssidError("s".repeat(33)))
        // Bytes, not characters: 16 two-byte letters fit, 11 three-byte signs (33 bytes) do not.
        assertNull(CoinWifi.ssidError("é".repeat(16)))
        assertNotNull(CoinWifi.ssidError("€".repeat(11)))
        assertNotNull(CoinWifi.ssidError("s".repeat(31) + "é"))
    }

    @Test
    fun passwordIsEmptyForOpenOr8To64Utf8Bytes() {
        assertNull("empty = open network", CoinWifi.passwordError(""))
        assertNotNull(CoinWifi.passwordError("p".repeat(7)))
        assertNull(CoinWifi.passwordError("p".repeat(8)))
        assertNull(CoinWifi.passwordError("p".repeat(64)))
        assertNotNull(CoinWifi.passwordError("p".repeat(65)))
        assertNull("4 two-byte letters are 8 bytes", CoinWifi.passwordError("é".repeat(4)))
        assertNotNull("33 characters, 66 bytes", CoinWifi.passwordError("é".repeat(33)))
        assertNotNull("7 bytes", CoinWifi.passwordError("ab€de"))
    }

    @Test
    fun errorChecksBoth() {
        assertNull(CoinWifi.error("home", ""))
        assertNull(CoinWifi.error("home", "hunter22"))
        assertNotNull(CoinWifi.error("", "hunter22"))
        assertNotNull(CoinWifi.error("home", "short"))
    }

    @Test
    fun limitNetworksEncodeAsWifiSet() {
        val m = CoinNetwork(1, "s".repeat(32), "p".repeat(64)).message()
        val wire = m.encode()
        assertEquals(m, CtrlMessage.decode(wire))
        assertEquals(3 + 1 + 32 + 1 + 64, wire.size)
        // Open network: pass_len 0.
        assertArrayEquals(byteArrayOf(0x05, 6, 0, 4, 'o'.code.toByte(), 'p'.code.toByte(), 'e'.code.toByte(), 'n'.code.toByte(), 0), CoinNetwork(2, "open", "").message().encode())
    }

    @Test
    fun networkToStringHidesThePassword() {
        assertFalse(CoinNetwork(1, "home", "fake-pass-123").toString().contains("fake-pass-123"))
    }

    @Test
    fun deliveryOrderCodes() {
        assertEquals(listOf(0, 1, 2), DeliveryOrder.entries.map { it.code })
        assertEquals(DeliveryOrder.WIFI_FIRST, DeliveryOrder.DEFAULT)
        assertEquals(CtrlMessage.CfgSet(CfgKey.DELIVERY, 2), DeliveryOrder.PHONE_ONLY.message())
        assertArrayEquals(byteArrayOf(0x06, 3, 0, 0x06, 1, 0), DeliveryOrder.PHONE_FIRST.message().encode())
        assertEquals(DeliveryOrder.WIFI_FIRST, DeliveryOrder.fromCode(9))
    }
}
