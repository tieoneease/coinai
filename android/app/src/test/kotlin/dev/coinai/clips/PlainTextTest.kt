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

    private fun coinCharset(s: String) = s.all { it == '\n' || it in ' '..'~' }

    @Test
    fun coinTextKeepsOnlyTheCoinCharacterSet() {
        val text = PlainText.forCoin("**Café** — 21 °C… “quoted” ✅ 日本\nnext line")
        assertEquals("Cafe - 21 degC... \"quoted\"\nnext line", text)
        assertTrue(coinCharset(text))
    }

    @Test
    fun coinMappingsFromProtocolRule7() {
        // ‘ ’ ′ → '   “ ” ″ → "   … → ...   – — − • · → -   NBSP, thin, narrow NBSP → space   ° → " deg"   × → x
        val cases = mapOf(
            "\u2018a\u2019 5\u2032" to "'a' 5'",
            "\u201Cb\u201D 6\u2033" to "\"b\" 6\"",
            "wait\u2026" to "wait...",
            "1\u20132 3\u20144 \u22125 \u2022 a \u00B7 b" to "1-2 3-4 -5 - a - b",
            "a\u00A0b\u2009c\u202Fd" to "a b c d",
            "21\u00B0C" to "21 degC",
            "3\u00D74" to "3x4",
        )
        for ((input, expected) in cases) assertEquals(input, expected, PlainText.forCoin(input))
    }

    @Test
    fun appFoldsMoreSoLittleIsDropped() {
        assertEquals("naive cafe, Ecole, fi, ABC", PlainText.forCoin("naïve café, École, \uFB01, \uFF21\uFF22\uFF23"))
        assertEquals("\"low\" 'q' a->b 1/2", PlainText.forCoin("\u201Elow\u201D \u201Aq\u2019 a\u2192b 1\u20442"))
        assertEquals("tab here", PlainText.forCoin("tab\there"))
        // Emoji (surrogate pairs) and scripts outside the font go; no stray spaces are left.
        assertEquals("ok done", PlainText.forCoin("ok \uD83D\uDE00 done \u65E5\u672C"))
    }

    @Test
    fun coinTextKeepsSingleNewlines() {
        val text = PlainText.forCoin("# Today\n\n- milk\n- eggs  \n\n\nBye")
        assertEquals("Today\n- milk\n- eggs\nBye", text)
    }

    @Test
    fun coinReplyFitsTheWireLimit() {
        val reply = CtrlMessage.Reply.truncating(7, ReplyState.DONE, PlainText.forCoin("°".repeat(150)))
        assertTrue(coinCharset(reply.text))
        assertTrue(reply.text.toByteArray().size <= Protocol.MAX_REPLY_TEXT_BYTES)
        reply.encode()
    }
}
