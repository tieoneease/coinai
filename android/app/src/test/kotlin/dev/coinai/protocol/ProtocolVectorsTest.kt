package dev.coinai.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.UUID

/**
 * Checks the codec against protocol/vectors/v1.json, read in place from the repo root (wired as a
 * test resource directory in app/build.gradle.kts). The JSON `fields` form is the reference
 * encoder's (protocol/gen_vectors.py); [fromFields] and [toFields] translate it.
 */
class ProtocolVectorsTest {
    private val root: JsonObject = run {
        val stream = checkNotNull(javaClass.getResourceAsStream("/v1.json")) { "protocol/vectors/v1.json not on the test classpath" }
        Json.parseToJsonElement(stream.bufferedReader(Charsets.UTF_8).readText()).jsonObject
    }
    private val ctrl = root.getValue("ctrl").jsonArray.map { it.jsonObject }
    private val typeCodes: Map<String, Int> = ctrl.associate { it.str("type") to it.int("type_code") }

    @Test
    fun constantsMatchVectors() {
        assertEquals(root.int("protocol_version"), Protocol.VERSION)
        assertEquals(UUID.fromString(root.str("service_uuid")), Protocol.SERVICE_UUID)
        assertEquals(UUID.fromString(root.str("ctrl_uuid")), Protocol.CTRL_UUID)
        assertEquals(UUID.fromString(root.str("data_uuid")), Protocol.DATA_UUID)
    }

    @Test
    fun vectorsCoverEveryMessageType() {
        val known = setOf(
            MessageType.HELLO, MessageType.LIST, MessageType.GET, MessageType.ACK, MessageType.WIFI_SET,
            MessageType.CFG_SET, MessageType.REPLY, MessageType.SCENE, MessageType.STATUS, MessageType.CLIP_STARTED,
            MessageType.CLIP_ENDED, MessageType.LIST_RESULT, MessageType.ERROR,
        )
        assertEquals(known, typeCodes.values.toSet())
    }

    @Test
    fun encodeFieldsGivesHex() {
        for (v in ctrl) {
            val msg = fromFields(v.str("type"), v.getValue("fields").jsonObject)
            assertEquals("encode ${v.str("type")} ${v["fields"]}", v.str("hex"), msg.encode().toHex())
        }
    }

    @Test
    fun decodeHexGivesFields() {
        for (v in ctrl) {
            val msg = CtrlMessage.decode(v.str("hex").hexToBytes())
            assertEquals("type code of ${v.str("hex")}", v.int("type_code"), msg.type)
            assertEquals("decode ${v.str("hex")}", v.getValue("fields").jsonObject, toFields(msg))
            val appToDevice = v.str("direction") == "app_to_device"
            assertEquals("direction of ${v.str("type")}", appToDevice, msg.type in 0x01..0x3F)
        }
    }

    @Test
    fun dataVectors() {
        val data = root.getValue("data").jsonArray.map { it.jsonObject }
        assertTrue(data.isNotEmpty())
        for (v in data) {
            val packet = DataPacket(v.int("clip"), v.long("offset"), v.str("payload_hex").hexToBytes())
            assertEquals(v.str("hex"), packet.encode().toHex())
            val decoded = DataPacket.decode(v.str("hex").hexToBytes())
            assertEquals(packet, decoded)
            assertEquals(v.str("payload_hex"), decoded.payload.toHex())
        }
    }

    @Test
    fun sceneSplitVectors() {
        val splits = root.getValue("scene_split").jsonArray.map { it.jsonObject }
        assertTrue(splits.isNotEmpty())
        for (v in splits) {
            val expected = v.getValue("hex").jsonArray.map { it.jsonPrimitive.content }
            val parts = CtrlMessage.Scene.split(v.int("clip"), v.str("json"))
            assertEquals("split of ${v.str("json").take(40)}", expected, parts.map { it.encode().toHex() })
            // The coin joins the pieces back into the same bytes.
            val joined = expected.map { CtrlMessage.decode(it.hexToBytes()) as CtrlMessage.Scene }
            assertEquals(v.str("json"), String(joined.fold(ByteArray(0)) { acc, p -> acc + p.json }, Charsets.UTF_8))
            assertTrue(joined.all { it.parts == expected.size && it.clip == v.int("clip") })
            assertEquals(expected.indices.toList(), joined.map { it.part })
        }
    }

    private fun fromFields(type: String, f: JsonObject): CtrlMessage = when (type) {
        "HELLO" -> CtrlMessage.Hello(phoneUtcMs = f.long("phone_utc_ms"), version = f.int("version"))
        "LIST" -> CtrlMessage.ListClips
        "GET" -> CtrlMessage.Get(f.int("clip"), f.long("offset"))
        "ACK" -> CtrlMessage.Ack(f.int("clip"), f.long("crc32"))
        "WIFI_SET" -> CtrlMessage.WifiSet(f.str("ssid"), f.str("password"))
        "CFG_SET" -> CtrlMessage.CfgSet(CfgKey.valueOf(f.str("key").uppercase()), f.int("value"))
        "REPLY" -> CtrlMessage.Reply(f.int("clip"), ReplyState.valueOf(f.str("state").uppercase()), f.str("text"))
        "SCENE" -> CtrlMessage.Scene(f.int("clip"), f.int("part"), f.int("parts"), f.str("json").toByteArray(Charsets.UTF_8))
        "STATUS" -> CtrlMessage.Status(
            batteryPct = f.int("battery_pct"),
            charging = f.int("charging") == 1,
            usbPower = f.int("usb_power") == 1,
            recording = f.int("recording") == 1,
            pending = f.int("pending"),
            fwVersion = f.str("fw_version"),
            scenes = f["scenes"]?.jsonPrimitive?.int == 1,
        )
        "CLIP_STARTED" -> CtrlMessage.ClipStarted(f.int("clip"), f.int("codec"), f.long("sample_rate"), f.long("start_utc_ms"))
        "CLIP_ENDED" -> CtrlMessage.ClipEnded(f.int("clip"), f.long("size"), f.long("duration_ms"), f.long("crc32"), f.long("voiced_ms"), f.long("start_utc_ms"))
        "LIST_RESULT" -> CtrlMessage.ListResult(
            more = f.getValue("more").jsonPrimitive.boolean,
            entries = f.getValue("entries").jsonArray.map {
                val e = it.jsonObject
                ClipEntry(e.int("clip"), e.long("size"), e.getValue("complete").jsonPrimitive.boolean)
            },
        )
        "ERROR" -> CtrlMessage.Error(typeCodes.getValue(f.str("request")), f.int("code"), f.int("clip"))
        else -> error("no vector mapping for $type")
    }

    private fun toFields(m: CtrlMessage): JsonObject = when (m) {
        is CtrlMessage.Hello -> buildJsonObject { put("version", m.version); put("phone_utc_ms", m.phoneUtcMs) }
        CtrlMessage.ListClips -> JsonObject(emptyMap())
        is CtrlMessage.Get -> buildJsonObject { put("clip", m.clip); put("offset", m.offset) }
        is CtrlMessage.Ack -> buildJsonObject { put("clip", m.clip); put("crc32", m.crc32) }
        is CtrlMessage.WifiSet -> buildJsonObject { put("ssid", m.ssid); put("password", m.password) }
        is CtrlMessage.CfgSet -> buildJsonObject { put("key", m.key.name.lowercase()); put("value", m.value) }
        is CtrlMessage.Reply -> buildJsonObject { put("clip", m.clip); put("state", m.state.name.lowercase()); put("text", m.text) }
        is CtrlMessage.Scene -> buildJsonObject {
            put("clip", m.clip); put("part", m.part); put("parts", m.parts); put("json", String(m.json, Charsets.UTF_8))
        }
        is CtrlMessage.Status -> buildJsonObject {
            put("battery_pct", m.batteryPct)
            put("charging", if (m.charging) 1 else 0)
            put("usb_power", if (m.usbPower) 1 else 0)
            put("recording", if (m.recording) 1 else 0)
            // Vectors from before scenes leave the flag out.
            if (m.scenes) put("scenes", 1)
            put("pending", m.pending)
            put("fw_version", m.fwVersion)
        }
        is CtrlMessage.ClipStarted -> buildJsonObject {
            put("clip", m.clip); put("codec", m.codec); put("sample_rate", m.sampleRate); put("start_utc_ms", m.startUtcMs)
        }
        is CtrlMessage.ClipEnded -> buildJsonObject {
            put("clip", m.clip); put("size", m.size); put("duration_ms", m.durationMs); put("crc32", m.crc32); put("voiced_ms", m.voicedMs); put("start_utc_ms", m.startUtcMs)
        }
        is CtrlMessage.ListResult -> buildJsonObject {
            put("more", m.more)
            put("entries", buildJsonArray {
                for (e in m.entries) add(buildJsonObject { put("clip", e.clip); put("size", e.size); put("complete", e.complete) })
            })
        }
        is CtrlMessage.Error -> buildJsonObject {
            put("request", typeCodes.entries.single { it.value == m.requestType }.key); put("code", m.code); put("clip", m.clip)
        }
        is CtrlMessage.Unknown -> error("vectors contain no unknown types")
    }
}

internal fun String.hexToBytes(): ByteArray {
    require(length % 2 == 0) { "odd hex length" }
    return ByteArray(length / 2) { substring(2 * it, 2 * it + 2).toInt(16).toByte() }
}

private fun JsonObject.str(key: String): String = getValue(key).jsonPrimitive.content
private fun JsonObject.int(key: String): Int = getValue(key).jsonPrimitive.int
private fun JsonObject.long(key: String): Long = getValue(key).jsonPrimitive.long
