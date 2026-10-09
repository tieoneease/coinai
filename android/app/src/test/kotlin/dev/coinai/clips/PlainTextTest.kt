package dev.coinai.clips

import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.Protocol
import dev.coinai.protocol.ReplyState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PlainTextTest {
    @Test
    fun markdownIsRemovedAndWordsKept() {
        val md = """
            # Shopping list

            Added **milk** and _bread_ to your [list](https://example.com/list).
            - eggs
            * `butter`
            > quoted ~~not~~ text

            ```kotlin
            val x = 1
            ```
            | item | qty |
            |------|-----|
            | milk | 2 |
        """.trimIndent()
        val plain = PlainText.fromMarkdown(md)
        assertEquals(
            """
            Shopping list

            Added milk and bread to your list.
            - eggs
            - butter
            quoted not text

            val x = 1
            item  qty
            milk  2
            """.trimIndent(),
            plain,
        )
    }

    @Test
    fun snakeCaseAndArithmeticSurvive() {
        assertEquals("use file_name_here and 2 * 3 * 4", PlainText.fromMarkdown("use file_name_here and 2 * 3 * 4"))
    }

    @Test
    fun coinTextKeepsOnlyFontCharacters() {
        val text = PlainText.forCoin("**Café** — 21 °C… “quoted” ✅ 日本\nnext line")
        assertEquals("Cafe - 21 °C... \"quoted\" next line", text)
        assertTrue(text.all { it in ' '..'~' || it == '°' })
    }

    @Test
    fun coinReplyFitsTheWireLimit() {
        val reply = CtrlMessage.Reply.truncating(7, ReplyState.DONE, PlainText.forCoin("°".repeat(150)))
        assertTrue(reply.text.toByteArray().size <= Protocol.MAX_REPLY_TEXT_BYTES)
        reply.encode()
    }
}
