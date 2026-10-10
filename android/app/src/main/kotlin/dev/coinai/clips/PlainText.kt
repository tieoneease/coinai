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

    /** PROTOCOL.md rule 7: what the coin maps itself. Applied here too, so the coin gets only its own character set. */
    private val coinMapped = mapOf(
        0x2018 to "'", 0x2019 to "'", 0x2032 to "'",
        0x201C to "\"", 0x201D to "\"", 0x2033 to "\"",
        0x2026 to "...",
        0x2013 to "-", 0x2014 to "-", 0x2212 to "-", 0x2022 to "-", 0x00B7 to "-",
        0x00A0 to " ", 0x2009 to " ", 0x202F to " ",
        0x00B0 to " deg",
        0x00D7 to "x",
    )

    /** Further folds the app makes so that common text is not dropped. */
    private val appMapped = mapOf(
        0x201A to "'", 0x201B to "'", 0x00B4 to "'", 0x201E to "\"", 0x201F to "\"", 0x00AB to "\"", 0x00BB to "\"",
        0x2010 to "-", 0x2011 to "-", 0x2012 to "-", 0x2015 to "-", 0x2043 to "-", 0x25E6 to "-",
        0x2192 to "->", 0x2190 to "<-", 0x2044 to "/", 0x00F7 to "/",
        '\t'.code to " ", '\r'.code to "\n",
    )

    private val spaceRuns = Regex(" {2,}")
    private val lineEdges = Regex(" *\n *")
    private val newlineRuns = Regex("\n{2,}")

    /**
     * Text for the coin's screen (PROTOCOL.md rule 7): markdown removed, then only the coin's
     * character set, printable ASCII (0x20-0x7E) and newline. The coin's own mappings are applied
     * here, plus a few more; accents are folded (e with acute -> e); anything else (emoji, CJK, ...)
     * is dropped. Blank lines become single newlines: the screen is small.
     */
    fun forCoin(markdown: String): String {
        val plain = fromMarkdown(markdown)
        val out = StringBuilder(plain.length)
        var i = 0
        while (i < plain.length) {
            val cp = plain.codePointAt(i)
            i += Character.charCount(cp)
            val mapped = coinMapped[cp] ?: appMapped[cp]
            if (mapped != null) {
                out.append(mapped)
                continue
            }
            // Compatibility decomposition: accented letters split into letter + combining accent,
            // ligatures and full-width forms become ASCII, the other Unicode spaces a space.
            for (ch in Normalizer.normalize(String(Character.toChars(cp)), Normalizer.Form.NFKD)) {
                val folded = coinMapped[ch.code] ?: appMapped[ch.code]
                when {
                    ch == '\n' || ch in ' '..'~' -> out.append(ch)
                    folded != null -> out.append(folded)
                    else -> Unit // combining accents, emoji, CJK, ...
                }
            }
        }
        var s = spaceRuns.replace(out.toString(), " ")
        s = lineEdges.replace(s, "\n")
        return newlineRuns.replace(s, "\n").trim()
    }
}
