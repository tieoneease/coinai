package dev.coinai.sync

import dev.coinai.protocol.CfgKey
import dev.coinai.protocol.CtrlMessage
import java.util.TimeZone

/** PROTOCOL.md rule 8: the coin shows a clock only after it has the phone's UTC offset. */
object CoinClock {
    /** Local time minus UTC at [nowMs] in [zone], in whole minutes, daylight saving included. */
    fun utcOffsetMinutes(nowMs: Long, zone: TimeZone = TimeZone.getDefault()): Int = zone.getOffset(nowMs) / 60_000

    /** `CFG_SET utc_offset_min` for [zone] at [nowMs], or null when the offset is outside the protocol's range. */
    fun offsetMessage(nowMs: Long, zone: TimeZone = TimeZone.getDefault()): CtrlMessage.CfgSet? =
        utcOffsetMinutes(nowMs, zone).takeIf { it in CfgKey.UTC_OFFSET_MIN.range }?.let { CtrlMessage.CfgSet(CfgKey.UTC_OFFSET_MIN, it) }
}
