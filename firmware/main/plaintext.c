#include "plaintext.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---------- markdown, one line at a time ----------

static bool is_ws(char c)
{
    return c == ' ' || c == '\t';
}

static bool is_word(char c)  // Java's \w (ASCII)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool is_fence(const char *l)
{
    while (is_ws(*l)) {
        l++;
    }
    return strncmp(l, "```", 3) == 0 || strncmp(l, "~~~", 3) == 0;
}

// ^[ \t]{0,3}([-*_][ \t]*){3,}$
static bool is_rule(const char *l)
{
    int i = 0;
    while (i < 3 && is_ws(l[i])) {
        i++;
    }
    int marks = 0;
    for (; l[i]; i++) {
        if (l[i] == '-' || l[i] == '*' || l[i] == '_') {
            marks++;
        } else if (!is_ws(l[i]) || marks == 0) {
            return false;
        }
    }
    return marks >= 3;
}

// A table's header separator: |---|:--:| and the like.
static bool is_table_sep(const char *l)
{
    int dashes = 0, run = 0;
    for (; *l; l++) {
        if (*l == '-') {
            run++;
            dashes = run > dashes ? run : dashes;
        } else if (*l == '|' || *l == ':' || is_ws(*l)) {
            run = 0;
        } else {
            return false;
        }
    }
    return dashes >= 3;
}

// Copies s[from, to) to *o.
static void put(char **o, const char *s, size_t from, size_t to)
{
    memcpy(*o, s + from, to - from);
    *o += to - from;
}

// open TEXT close, TEXT at least `min` bytes without `stop`: keeps TEXT.
static size_t span(const char *s, size_t i, const char *open, const char *close, size_t min, char stop, size_t *text0,
                   size_t *text1)
{
    size_t lo = strlen(open), lc = strlen(close);
    if (strncmp(s + i, open, lo) != 0) {
        return 0;
    }
    for (size_t j = i + lo; s[j]; j++) {
        if (j - (i + lo) >= min && strncmp(s + j, close, lc) == 0) {
            *text0 = i + lo;
            *text1 = j;
            return j + lc - i;
        }
        if (stop && s[j] == stop) {
            return 0;
        }
    }
    return 0;
}

// One inline pass; `kind` picks the construct. Writes into out (never longer than s).
static void inline_pass(const char *s, char *out, int kind)
{
    char *o = out;
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        size_t t0, t1, len = 0;
        switch (kind) {
        case 0:  // ![alt](url) -> alt
            if (strncmp(s + i, "![", 2) == 0) {
                size_t a0, a1, l1 = span(s, i + 1, "[", "](", 0, ']', &a0, &a1);
                if (l1) {
                    size_t u0, u1, l2 = span(s, a1 + 1, "(", ")", 0, 0, &u0, &u1);
                    if (l2) {
                        t0 = a0, t1 = a1, len = (a1 + 1 + l2) - i;
                    }
                }
            }
            break;
        case 1: {  // [text](url) -> text
            size_t a0, a1, l1 = span(s, i, "[", "](", 1, ']', &a0, &a1);
            if (l1) {
                size_t u0, u1, l2 = span(s, a1 + 1, "(", ")", 0, 0, &u0, &u1);
                if (l2) {
                    t0 = a0, t1 = a1, len = (a1 + 1 + l2) - i;
                }
            }
            break;
        }
        case 2:  // <https://...> -> the address
            if (strncmp(s + i, "<http://", 8) == 0 || strncmp(s + i, "<https://", 9) == 0) {
                len = span(s, i, "<", ">", 1, 0, &t0, &t1);
            }
            break;
        case 3:  // `code`
            len = span(s, i, "`", "`", 0, 0, &t0, &t1);
            break;
        case 4:  // **strong** and __strong__
            len = span(s, i, "**", "**", 1, 0, &t0, &t1);
            if (!len) {
                len = span(s, i, "__", "__", 1, 0, &t0, &t1);
            }
            break;
        case 5:  // ~~strike~~
            len = span(s, i, "~~", "~~", 1, 0, &t0, &t1);
            break;
        case 6: {  // *emphasis* and _emphasis_: (?<![\w*])([*_])(?!\s)(.+?)(?<!\s)\1(?![\w*])
            char c = s[i];
            if ((c == '*' || c == '_') && (i == 0 || !(is_word(s[i - 1]) || s[i - 1] == '*')) && s[i + 1] &&
                !is_ws(s[i + 1])) {
                for (size_t j = i + 2; j < n; j++) {
                    if (s[j] == c && !is_ws(s[j - 1]) && (j + 1 == n || !(is_word(s[j + 1]) || s[j + 1] == '*'))) {
                        t0 = i + 1, t1 = j, len = j + 1 - i;
                        break;
                    }
                }
            }
            break;
        }
        case 7:  // <tag ...> and </tag>
            if (s[i] == '<') {
                size_t k = i + 1 + (s[i + 1] == '/');
                char c = s[k];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                    const char *gt = strchr(s + k, '>');
                    if (gt) {
                        t0 = t1 = 0, len = (size_t)(gt - s) + 1 - i;
                    }
                }
            }
            break;
        }
        if (len) {
            put(&o, s, t0, t1);
            i += len;
        } else {
            *o++ = s[i++];
        }
    }
    *o = 0;
}

// The line-level rules, then the inline ones; false drops the line. tmp: as large as line.
static bool clean_line(char *line, char *tmp)
{
    if (is_rule(line) || is_table_sep(line)) {
        return false;
    }
    char *p = line;
    // Heading and quote prefixes: ^\s{0,3}#{1,6}\s+ and ^\s{0,3}>\s?
    int sp = 0;
    while (sp < 3 && is_ws(p[sp])) {
        sp++;
    }
    int h = 0;
    while (h < 7 && p[sp + h] == '#') {
        h++;
    }
    if (h >= 1 && h <= 6 && is_ws(p[sp + h])) {
        p += sp + h;
        while (is_ws(*p)) {
            p++;
        }
    } else if (p[sp] == '>') {
        p += sp + 1 + is_ws(p[sp + 1]);
    }
    memmove(line, p, strlen(p) + 1);
    // Bullets: ^(\s*)[-*+]\s+(\[[ xX]]\s+)? -> "$1- "
    size_t ind = 0;
    while (is_ws(line[ind])) {
        ind++;
    }
    char b = line[ind];
    if ((b == '-' || b == '*' || b == '+') && is_ws(line[ind + 1])) {
        size_t k = ind + 1;
        while (is_ws(line[k])) {
            k++;
        }
        if (line[k] == '[' && (line[k + 1] == ' ' || line[k + 1] == 'x' || line[k + 1] == 'X') && line[k + 2] == ']' &&
            is_ws(line[k + 3])) {
            k += 3;
            while (is_ws(line[k])) {
                k++;
            }
        }
        line[ind] = '-';
        line[ind + 1] = ' ';
        memmove(line + ind + 2, line + k, strlen(line + k) + 1);
    }
    static const int ORDER[] = {0, 1, 2, 3, 4, 5, 6, 7};  // image, link, autolink, code, strong, strike, em, html
    for (size_t k = 0; k < sizeof ORDER / sizeof ORDER[0]; k++) {
        inline_pass(line, tmp, ORDER[k]);
        strcpy(line, tmp);
    }
    // Table rows: | a | b | -> "a  b"
    char *t = line;
    while (is_ws(*t)) {
        t++;
    }
    if (*t == '|') {
        while (*t == '|' || is_ws(*t)) {
            t++;
        }
        char *o = tmp;
        for (; *t; t++) {
            if (t[0] == ' ' && t[1] == '|' && t[2] == ' ') {
                *o++ = ' ';
                *o++ = ' ';
                t += 2;
            } else {
                *o++ = *t;
            }
        }
        while (o > tmp && (o[-1] == '|' || is_ws(o[-1]))) {
            o--;
        }
        *o = 0;
        strcpy(line, tmp);
    }
    size_t n = strlen(line);
    while (n && is_ws(line[n - 1])) {
        line[--n] = 0;
    }
    return true;
}

// ---------- the coin's character set ----------

// U+00C0..U+00FF and U+0100..U+017F folded like NFKD; '?' = dropped.
static const char LATIN1[] = "AAAAAA?CEEEEIIII?NOOOOO?" "?UUUUY??" "aaaaaa?ceeeeiiii?nooooo?" "?uuuuy?y";
static const char LATIN_A[] = "AaAaAaCcCcCcCcDd??EeEeEeEeEeGgGgGgGgHh??IiIiIiIiI?IiJjKk?LlLlLlLl??NnNnNnn??OoOoOo??RrRrRr"
                              "SsSsSsSsTtTt??UuUuUuUuUuUuWwYyYZzZzZzs";

static const char *map_cp(uint32_t cp)
{
    switch (cp) {
    case 0x2018: case 0x2019: case 0x2032: case 0x201A: case 0x201B: case 0x00B4:
        return "'";
    case 0x201C: case 0x201D: case 0x2033: case 0x201E: case 0x201F: case 0x00AB: case 0x00BB:
        return "\"";
    case 0x2026:
        return "...";
    case 0x2013: case 0x2014: case 0x2212: case 0x2022: case 0x00B7: case 0x2010: case 0x2011: case 0x2012:
    case 0x2015: case 0x2043: case 0x25E6:
        return "-";
    case 0x00A0: case 0x2009: case 0x202F: case 0x205F: case 0x3000:
        return " ";
    case 0x00B0:
        return " deg";
    case 0x00D7:
        return "x";
    case 0x2192:
        return "->";
    case 0x2190:
        return "<-";
    case 0x2044: case 0x00F7:
        return "/";
    case 0xFB00:
        return "ff";
    case 0xFB01:
        return "fi";
    case 0xFB02:
        return "fl";
    case 0xFB03:
        return "ffi";
    case 0xFB04:
        return "ffl";
    }
    if (cp >= 0x2000 && cp <= 0x200A) {
        return " ";
    }
    static char one[2];
    one[1] = 0;
    if (cp >= 0xC0 && cp <= 0xFF) {
        one[0] = LATIN1[cp - 0xC0];
    } else if (cp >= 0x100 && cp <= 0x17F) {
        one[0] = LATIN_A[cp - 0x100];
    } else if (cp >= 0xFF01 && cp <= 0xFF5E) {
        one[0] = (char)(cp - 0xFF01 + 0x21);  // full-width ASCII
    } else {
        return NULL;
    }
    return one[0] == '?' ? NULL : one;
}

// UTF-8 to the coin's set; invalid sequences are dropped. out: at least 2 * strlen(s) + 1.
static void to_coin_charset(const char *s, char *out)
{
    const uint8_t *p = (const uint8_t *)s;
    char *o = out;
    while (*p) {
        uint32_t cp;
        int n;
        if (*p < 0x80) {
            cp = *p, n = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F, n = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F, n = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07, n = 4;
        } else {
            p++;
            continue;
        }
        int k = 1;
        for (; k < n && (p[k] & 0xC0) == 0x80; k++) {
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        p += k;
        if (k < n) {
            continue;  // cut sequence
        }
        if (cp == '\t') {
            *o++ = ' ';
        } else if (cp == '\r' || cp == '\n') {
            *o++ = '\n';
        } else if (cp >= 0x20 && cp <= 0x7E) {
            *o++ = (char)cp;
        } else {
            const char *m = map_cp(cp);
            if (m) {
                size_t l = strlen(m);
                memcpy(o, m, l);
                o += l;
            }
        }
    }
    *o = 0;
}

size_t plaintext_for_coin(const char *markdown, char *out, size_t out_len)
{
    if (!out_len) {
        return 0;
    }
    out[0] = 0;
    size_t n = strlen(markdown);
    char *text = malloc(n + 2);
    char *line = malloc(n + 2);
    char *tmp = malloc(n + 2);
    char *coin = malloc(2 * n + 2);
    if (!text || !line || !tmp || !coin) {
        free(text), free(line), free(tmp), free(coin);
        return 0;
    }
    // Lines: fenced-block markers go, the rest is cleaned.
    char *t = text;
    for (const char *p = markdown; *p;) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        memcpy(line, p, l);
        line[l] = 0;
        if (l && line[l - 1] == '\r') {
            line[l - 1] = 0;
        }
        p += l + (e ? 1 : 0);
        if (is_fence(line) || !clean_line(line, tmp)) {
            continue;
        }
        size_t ll = strlen(line);
        memcpy(t, line, ll);
        t += ll;
        *t++ = '\n';
    }
    *t = 0;
    to_coin_charset(text, coin);
    // Space runs to one space, no spaces at line edges, no blank lines, trimmed.
    char *o = out, *end = out + out_len - 1;
    bool cut = false;
    for (char *c = coin; *c && !cut; c++) {
        char ch = *c;
        if (ch == ' ' && (o == out || o[-1] == ' ' || o[-1] == '\n')) {
            continue;
        }
        if (ch == '\n') {
            while (o > out && o[-1] == ' ') {
                o--;
            }
            if (o == out || o[-1] == '\n') {
                continue;
            }
        }
        if (o == end) {
            cut = true;
            break;
        }
        *o++ = ch;
    }
    while (o > out && (o[-1] == ' ' || o[-1] == '\n')) {
        o--;
    }
    if (cut) {
        // Room for "...": back off to the last word boundary when one is near.
        char *lim = end - 3 < out ? out : end - 3;
        if (o > lim) {
            o = lim;
        }
        char *sp = o;
        while (sp > out && sp > o - 24 && sp[-1] != ' ' && sp[-1] != '\n') {
            sp--;
        }
        if (sp > out && sp > o - 24) {
            o = sp;
        }
        while (o > out && (o[-1] == ' ' || o[-1] == '\n')) {
            o--;
        }
        if (o + 3 <= end) {
            memcpy(o, "...", 3);
            o += 3;
        }
    }
    *o = 0;
    free(text), free(line), free(tmp), free(coin);
    return (size_t)(o - out);
}
