package dev.coinai.clips

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.intOrNull

/**
 * A scene's card as plain text (PROTOCOL.md "Scenes"): the clip row's reply, and the `REPLY` the
 * coin gets when it cannot show the scene. One line per part, in the order the coin draws them.
 */
object SceneSummary {
    private val JSON = Json { ignoreUnknownKeys = true }

    /** The summary of the scene in [sceneJson]; its `say` when the card is missing or unknown, else "". */
    fun of(sceneJson: String): String {
        val scene = runCatching { JSON.parseToJsonElement(sceneJson) as? JsonObject }.getOrNull() ?: return ""
        val card = scene["card"] as? JsonObject
        val text = card?.let(::card).orEmpty()
        return text.ifBlank { scene.str("say").orEmpty() }
    }

    /** The `later` card's task, or null for any other scene. */
    fun laterText(sceneJson: String): String? {
        val scene = runCatching { JSON.parseToJsonElement(sceneJson) as? JsonObject }.getOrNull() ?: return null
        val card = scene["card"] as? JsonObject ?: return null
        return if (card.str("kind") == "later") card.str("text").orEmpty() else null
    }

    private fun card(c: JsonObject): String {
        val lines: List<String?> = when (c.str("kind")) {
            "answer" -> listOf(c.str("title"), c.str("body"))
            "stat" -> listOf(listOfNotNull(c.str("value"), c.str("unit")).joinToString(" "), c.str("label"), c.str("sub"))
            "list" -> listOf(c.str("title")) + c.strings("items").map { "- $it" }
            "steps" -> listOf(c.str("title")) + c.strings("steps").mapIndexed { i, s -> "${i + 1}. $s" }
            "timer" -> listOf(c.str("label"), (c["seconds"] as? JsonPrimitive)?.intOrNull?.let(::duration))
            "later" -> listOf(c.str("text"))
            "confirm" -> listOf(c.str("question"))
            else -> emptyList()
        }
        return lines.mapNotNull { it?.trim()?.takeIf(String::isNotEmpty) }.joinToString("\n")
    }

    /** `90` → "1 min 30 s", `3600` → "1 h". */
    fun duration(seconds: Int): String {
        if (seconds <= 0) return "0 s"
        val h = seconds / 3600
        val m = seconds % 3600 / 60
        val s = seconds % 60
        return listOfNotNull(h.takeIf { it > 0 }?.let { "$it h" }, m.takeIf { it > 0 }?.let { "$it min" }, s.takeIf { it > 0 }?.let { "$it s" })
            .joinToString(" ")
    }

    private fun JsonObject.str(key: String): String? = (get(key) as? JsonPrimitive)?.takeIf { it.isString }?.content

    private fun JsonObject.strings(key: String): List<String> =
        (get(key) as? JsonArray)?.mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> p.isString }?.content?.trim()?.takeIf(String::isNotEmpty) }.orEmpty()
}
