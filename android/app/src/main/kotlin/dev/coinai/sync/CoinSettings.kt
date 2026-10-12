package dev.coinai.sync

import dev.coinai.protocol.CfgKey
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.Protocol
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import java.io.IOException

/** `CFG_SET` key `0x06` (PROTOCOL.md "Wi-Fi delivery"): who sends a new clip to SamBot. */
enum class DeliveryOrder(val code: Int) {
    /** The coin over Wi-Fi; the phone only when no known network answers or SamBot is not reached. The default. */
    WIFI_FIRST(0),
    /** The phone when it is connected as the clip ends; otherwise the coin over Wi-Fi. */
    PHONE_FIRST(1),
    PHONE_ONLY(2);

    fun message(): CtrlMessage.CfgSet = CtrlMessage.CfgSet(CfgKey.DELIVERY, code)

    companion object {
        val DEFAULT = WIFI_FIRST
        fun fromCode(code: Int): DeliveryOrder = entries.firstOrNull { it.code == code } ?: DEFAULT
    }
}

/**
 * A network for the coin's Wi-Fi delivery (`WIFI_SET`). [id] names this version of the entry in the
 * app's list: adding the same SSID again makes a new one. [toString] leaves the password out.
 */
class CoinNetwork(val id: Long, val ssid: String, val password: String) {
    fun message(): CtrlMessage.WifiSet = CtrlMessage.WifiSet(ssid, password)
    override fun toString(): String = "CoinNetwork(id=$id, ssid=$ssid, ${if (password.isEmpty()) "open" else "password set"})"
}

/**
 * The `WIFI_SET` rules of PROTOCOL.md, in UTF-8 bytes: SSID 1–32, password empty (an open network)
 * or 8–64. The coin keeps [COIN_KEEPS] networks, the newest first. There is no message to make the
 * coin forget one: removing a network from the app's list leaves it on the coin until newer ones
 * push it out.
 */
object CoinWifi {
    const val COIN_KEEPS = 4
    const val MIN_PASSWORD_BYTES = 8

    fun bytes(s: String): Int = s.toByteArray(Charsets.UTF_8).size

    fun ssidError(ssid: String): String? {
        val n = bytes(ssid)
        return when {
            n == 0 -> "Enter the network name"
            n > Protocol.MAX_SSID_BYTES -> "Network name too long: $n bytes, at most ${Protocol.MAX_SSID_BYTES}"
            else -> null
        }
    }

    /** An empty password is an open network. */
    fun passwordError(password: String): String? {
        val n = bytes(password)
        return when {
            n == 0 -> null
            n < MIN_PASSWORD_BYTES -> "Password too short: $n bytes, at least $MIN_PASSWORD_BYTES"
            n > Protocol.MAX_PASSWORD_BYTES -> "Password too long: $n bytes, at most ${Protocol.MAX_PASSWORD_BYTES}"
            else -> null
        }
    }

    fun error(ssid: String, password: String): String? = ssidError(ssid) ?: passwordError(password)
}

/** What a session sends the coin from the app's settings, after `HELLO`. */
interface CoinSettings {
    /**
     * The delivery order. Its current value goes out after `HELLO` in every session, and each
     * change while the session runs. Null: the session sends none.
     */
    val delivery: Flow<DeliveryOrder>?

    /** Networks [device] has not taken or refused yet, oldest first, so the newest ends up first on the coin. */
    suspend fun pendingNetworks(device: String): List<CoinNetwork>

    /** [device] took [network] (`errorCode` null) or refused it with `ERROR` [errorCode]. */
    suspend fun networkResult(device: String, network: CoinNetwork, errorCode: Int?)

    companion object {
        val NONE: CoinSettings = object : CoinSettings {
            override val delivery: Flow<DeliveryOrder>? = null
            override suspend fun pendingNetworks(device: String) = emptyList<CoinNetwork>()
            override suspend fun networkResult(device: String, network: CoinNetwork, errorCode: Int?) = Unit
        }
    }
}

/**
 * Sends `CFG_SET delivery` each time [order] differs from what the coin last got, starting from
 * [sent]; runs until cancelled or the link fails. [onSent] sees each value written.
 */
suspend fun followDelivery(
    link: CoinLink,
    order: Flow<DeliveryOrder>,
    sent: DeliveryOrder,
    onSent: (DeliveryOrder) -> Unit = {},
    log: (String) -> Unit = {},
) {
    var last = sent
    try {
        order.distinctUntilChanged().collect {
            if (it == last) return@collect
            link.send(it.message())
            last = it
            onSent(it)
        }
    } catch (e: IOException) {
        log("delivery order change not sent: ${e.message}")
    }
}
