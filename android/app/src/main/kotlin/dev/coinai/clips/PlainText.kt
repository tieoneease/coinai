package dev.coinai.clips

import java.text.Normalizer

/** Markdown removal for notifications and the coin (PROTOCOL.md rule 7). */
object PlainText {
    private val fence = Regex("^[ \\t]*(```|~~~).*(\\n|$)", RegexOption.MULTILINE)
    private val image = Regex("!\\[([^\\]]*)]\\([^)]*\\)")
    private val link = Regex("\\[([^\\]]+)]\\([^)]*\\)")
    private val autolink = Regex("<(https?://[^>]+)>")
    private val heading = Regex("^\\s{0,3}#{1,6}\\s+", RegexOption.MULTILINE)
    private val quote = Regex("^\\s{0,3}>\\s?", RegexOption.MULTILINE)
    private val bullet = Regex("^(\\s*)[-*+]\\s+(\\[[ xX]]\\s+)?", RegexOption.MULTILINE)
    private val rule = Regex("^[ \\t]{0,3}([-*_][ \\t]*){3,}(\\n|$)", RegexOption.MULTILINE)
    private val tableSep = Regex("^[ \\t]*\\|?[ \\t]*:?-{3,}:?[ \\t]*(\\|[ \\t]*:?-{3,}:?[ \\t]*)*\\|?[ \\t]*(\\n|$)", RegexOption.MULTILINE)
    private val strong = Regex("(\\*\\*|__)(.+?)\\1")
    private val emphasis = Regex("(?<![\\w*])([*_])(?!\\s)(.+?)(?<!\\s)\\1(?![\\w*])")
    private val strike = Regex("~~(.+?)~~")
    private val code = Regex("`([^`]*)`")
    private val html = Regex("</?[a-zA-Z][^>]*>")
    private val blankRuns = Regex("\\n{3,}")

    /** Markdown to readable plain text: markup goes, the words stay. */
    fun fromMarkdown(markdown: String): String {
        var s = markdown.replace("\r\n", "\n")
        s = fence.replace(s, "")
        s = image.replace(s, "$1")
        s = link.replace(s, "$1")
        s = autolink.replace(s, "$1")
        s = rule.replace(s, "")
        s = tableSep.replace(s, "")
        s = heading.replace(s, "")
        s = quote.replace(s, "")
        s = bullet.replace(s, "$1- ")
        s = code.replace(s, "$1")
        s = strong.replace(s, "$2")
        s = strike.replace(s, "$1")
        s = emphasis.replace(s, "$2")
        s = html.replace(s, "")
        s = s.lines().joinToString("\n") { line -> if (line.trim().startsWith("|")) line.trim().trim('|').replace(" | ", "  ").trim() else line.trimEnd() }
        return blankRuns.replace(s, "\n\n").trim()
    }

    private val typographic = mapOf(
        '\u2018' to "'", '\u2019' to "'", '\u201A' to "'", '\u201C' to "\"", '\u201D' to "\"", '\u201E' to "\"",
        '\u2013' to "-", '\u2014' to "-", '\u2212' to "-", '\u2026' to "...", '\u00A0' to " ", '\u2022' to "-",
        '\u00D7' to "x", '\u2192' to "->", '\u2190' to "<-",
    )

    /**
     * Text for the coin's screen: markdown removed, then only characters its font has.
     * PROTOCOL-Q: PROTOCOL.md says "characters the coin's font lacks" without naming the set. The
     * firmware uses LVGL's built-in Montserrat, which covers printable ASCII (plus a degree sign and
     * symbol glyphs); this keeps printable ASCII and °, folds accents and typographic punctuation,
     * and drops the rest.
     */
    fun forCoin(markdown: String): String {
        val plain = fromMarkdown(markdown)
        val out = StringBuilder(plain.length)
        for (ch in Normalizer.normalize(plain, Normalizer.Form.NFKD)) {
            when {
                ch == '\n' -> out.append(' ')
                ch in ' '..'~' || ch == '\u00B0' -> out.append(ch)
                typographic.containsKey(ch) -> out.append(typographic.getValue(ch))
                else -> Unit // combining accents after NFKD, emoji, CJK, ...
            }
        }
        return out.toString().replace(Regex(" {2,}"), " ").trim()
    }
}
