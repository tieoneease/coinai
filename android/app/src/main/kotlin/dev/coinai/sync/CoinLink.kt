package dev.coinai.sync

import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.DataPacket
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.map
import java.io.IOException

enum class LinkState { DISCONNECTED, CONNECTING, READY }

/** Something the coin sent: a `ctrl` indication or a `data` notification. */
sealed interface Inbound {
    data class Ctrl(val message: CtrlMessage) : Inbound
    data class Data(val packet: DataPacket) : Inbound
}

/** A write to the coin failed, usually because the link dropped. */
class LinkException(message: String, cause: Throwable? = null) : IOException(message, cause)

/**
 * The app's view of one connected coin (PROTOCOL.md, GATT). Implemented by the BLE layer and by
 * the test simulator; the sync engine depends only on this.
 */
interface CoinLink {
    /** The bonded device's address; the key under which its clips are stored. */
    val deviceAddress: String

    val state: StateFlow<LinkState>

    /**
     * Everything the coin sends, from both characteristics, in arrival order. A hot flow: messages
     * that arrive while nobody collects are lost, so collect before [send]ing a request.
     */
    val incoming: Flow<Inbound>

    /** Writes one ctrl message with response. Throws [LinkException] when the write fails. */
    suspend fun send(message: CtrlMessage)
}

val CoinLink.ctrl: Flow<CtrlMessage> get() = incoming.filterIsInstance<Inbound.Ctrl>().map { it.message }
val CoinLink.data: Flow<DataPacket> get() = incoming.filterIsInstance<Inbound.Data>().map { it.packet }
