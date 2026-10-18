#include "scene.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "eyes.h"
#include "misc/lv_text_private.h"  // lv_text_get_next_line

#define C 233             // screen centre and radius
#define PAD 24            // kept clear of the round edge on each side
#define TOP_HEADER 168    // first line's top edge under the header eyes
#define TOP_PEEK 96       // ... under the peeking eyes
#define BOTTOM 420        // last line's bottom edge
#define GAP 10            // between slots
#define LINE_SPACE 2
#define SAY_MAX 160

#define F20 (&lv_font_montserrat_20)
#define F24 (&lv_font_montserrat_24)
#define F28 (&lv_font_montserrat_28)
#define F32 (&lv_font_montserrat_32)
#define F48 (&lv_font_montserrat_48)

EXT_RAM_BSS_ATTR static scene_t s_scene;

static const char *KIND_NAMES[CARD_COUNT] = {"answer", "stat", "list", "steps", "timer", "confirm", "later"};
// The fields each kind takes ("kind" itself is implied); anything else is reported.
static const char *KIND_FIELDS[CARD_COUNT] = {
    "title body", "value unit label sub", "title items", "title steps", "label seconds", "question yes no", "text",
};

scene_t *scene_get(void)
{
    return &s_scene;
}

const char *scene_kind_name(card_kind_t k)
{
    return k < CARD_COUNT ? KIND_NAMES[k] : "?";
}

int scene_ascii(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    int dropped = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s && o + 4 < n) {
        if (*s < 0x80) {
            dst[o++] = (char)(*s == '\n' || *s >= 0x20 ? *s : ' ');
            s++;
            continue;
        }
        uint32_t cp = 0;
        int len = (*s >= 0xF0) ? 4 : (*s >= 0xE0) ? 3 : (*s >= 0xC0) ? 2 : 1;
        cp = *s & (len == 4 ? 0x07 : len == 3 ? 0x0F : 0x1F);
        int i = 1;
        for (; i < len && s[i]; i++) {
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        s += i;  // never past the terminator of a cut-off sequence
        if (i < len) {
            break;
        }
        const char *rep = NULL;
        switch (cp) {
        case 0x2018: case 0x2019: case 0x2032: rep = "'"; break;
        case 0x201C: case 0x201D: case 0x2033: rep = "\""; break;
        case 0x2026: rep = "..."; break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: rep = "-"; break;
        case 0x00A0: case 0x202F: case 0x2009: case 0x2007: rep = " "; break;
        case 0x00B0: rep = " deg"; break;
        case 0x00D7: rep = "x"; break;
        case 0x2022: case 0x00B7: rep = "-"; break;
        default: dropped++; break;
        }
        for (const char *r = rep; r && *r && o + 1 < n; r++) {
            dst[o++] = *r;
        }
    }
    dst[o] = 0;
    return dropped;
}

// ---------- issues ----------

static void issue(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void issue(const char *fmt, ...)
{
    scene_t *s = &s_scene;
    size_t o = strlen(s->issues);
    if (o + 4 >= sizeof s->issues) {
        return;
    }
    if (o) {
        o += snprintf(s->issues + o, sizeof s->issues - o, "; ");
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->issues + o, sizeof s->issues - o, fmt, ap);
    va_end(ap);
    s->n_issues++;
}

// ---------- text on a round screen ----------

// Usable width for a line spanning top..bottom: the chord of the circle at its outer edge.
static int chord_w(int top, int bottom)
{
    int d = abs(top - C) > abs(bottom - C) ? abs(top - C) : abs(bottom - C);
    if (d >= C) {
        return 0;
    }
    return (int)(2 * sqrtf((float)(C * C - d * d))) - 2 * PAD;
}

static int line_h(const lv_font_t *f)
{
    return lv_font_get_line_height(f) + LINE_SPACE;
}

typedef struct {
    int page, row;
    bool dim;
} line_tag_t;

// Breaks text into lines from y_top, each as wide as the circle allows there (at most slot_w).
// Sets *over when text is left after max_lines or y_max. With commit, appends the lines.
static int flow(const char *text, const lv_font_t *font, int y_top, int slot_w, int max_lines, int y_max, bool commit,
                line_tag_t tag, bool *over)
{
    scene_t *s = &s_scene;
    int lh = line_h(font);
    uint32_t len = (uint32_t)strlen(text), i = 0;
    int n = 0;
    lv_text_attributes_t at;
    lv_text_attributes_init(&at);
    at.line_space = LINE_SPACE;
    *over = false;
    while (i < len) {
        while (i < len && (text[i] == ' ' || text[i] == '\n')) {
            i++;
        }
        if (i >= len) {
            break;
        }
        int top = y_top + n * lh;
        if (n >= max_lines || top + lh > y_max) {
            *over = true;
            break;
        }
        int w = chord_w(top, top + lh);
        at.max_width = w < slot_w ? w : slot_w;
        uint32_t k = lv_text_get_next_line(text + i, len - i, font, NULL, &at);
        if (k == 0) {
            *over = true;
            break;
        }
        if (commit && s->n_lines < SCENE_LINES) {
            scene_line_t *l = &s->lines[s->n_lines++];
            uint32_t m = k < sizeof l->text - 1 ? k : sizeof l->text - 1;
            memcpy(l->text, text + i, m);
            while (m && (l->text[m - 1] == ' ' || l->text[m - 1] == '\n')) {
                m--;
            }
            l->text[m] = 0;
            l->y = (int16_t)(top + lh / 2);
            l->font = font;
            l->page = (int8_t)tag.page;
            l->row = (int8_t)tag.row;
            l->dim = tag.dim;
        }
        i += k;
        n++;
    }
    return n;
}

// Lays out one text slot in the largest allowed font it fits; the smallest when nothing fits,
// cut off with "..." and reported. Returns the y below it (plus GAP); y unchanged for empty text.
static int slot(const char *name, const char *text, const lv_font_t *const *fonts, int n_fonts, int y, int slot_w,
                int max_lines, int y_max, line_tag_t tag)
{
    if (!text || !text[0]) {
        return y;
    }
    bool over = true;
    const lv_font_t *font = fonts[n_fonts - 1];
    for (int i = 0; i < n_fonts && over; i++) {
        flow(text, fonts[i], y, slot_w, max_lines, y_max, false, tag, &over);
        if (!over) {
            font = fonts[i];
        }
    }
    int first = s_scene.n_lines;
    int n = flow(text, font, y, slot_w, max_lines, y_max, true, tag, &over);
    if (over) {
        issue("%s: too long for %d line%s", name, max_lines, max_lines == 1 ? "" : "s");
        if (n && s_scene.n_lines > first) {
            char *t = s_scene.lines[s_scene.n_lines - 1].text;
            size_t m = strlen(t);
            if (m > 3) {
                strcpy(t + m - 3, "...");
            }
        }
    }
    return y + n * line_h(font) + GAP;
}

// ---------- parsing ----------

static bool get_str(const cJSON *card, const char *key, char *dst, size_t n, bool required, size_t max_chars)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(card, key);
    dst[0] = 0;
    if (!v) {
        if (required) {
            issue("%s: missing", key);
        }
        return false;
    }
    if (!cJSON_IsString(v)) {
        issue("%s: not a string", key);
        return false;
    }
    int dropped = scene_ascii(dst, n, v->valuestring);
    if (dropped) {
        issue("%s: %d character%s the coin can't draw", key, dropped, dropped == 1 ? "" : "s");
    }
    if (max_chars && strlen(dst) > max_chars) {
        issue("%s: %u characters, max %u", key, (unsigned)strlen(dst), (unsigned)max_chars);
        dst[max_chars] = 0;
    }
    return dst[0] != 0;
}

static int get_items(const cJSON *card, const char *key, char items[][120], int min, int max)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(card, key);
    if (!cJSON_IsArray(a)) {
        issue("%s: %s", key, a ? "not a list" : "missing");
        return 0;
    }
    int n = cJSON_GetArraySize(a);
    if (n < min || n > max) {
        issue("%s: %d entries, want %d-%d", key, n, min, max);
    }
    int k = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, a)
    {
        if (k >= max) {
            break;
        }
        if (!cJSON_IsString(v)) {
            issue("%s[%d]: not a string", key, k);
            continue;
        }
        int dropped = scene_ascii(items[k], 120, v->valuestring);
        if (dropped) {
            issue("%s[%d]: %d character%s the coin can't draw", key, k, dropped, dropped == 1 ? "" : "s");
        }
        k++;
    }
    return k;
}

static void check_fields(const cJSON *card, card_kind_t kind)
{
    const cJSON *f;
    cJSON_ArrayForEach(f, card)
    {
        if (strcmp(f->string, "kind") == 0) {
            continue;
        }
        const char *list = KIND_FIELDS[kind];
        size_t n = strlen(f->string);
        const char *p = strstr(list, f->string);
        bool ok = false;
        while (p) {
            if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) {
                ok = true;
                break;
            }
            p = strstr(p + 1, f->string);
        }
        if (!ok) {
            issue("%s: not a %s field", f->string, KIND_NAMES[kind]);
        }
    }
}

static const lv_font_t *const TITLE_FONTS[] = {F32, F28};
static const lv_font_t *const BODY_FONTS[] = {F24, F20};
static const lv_font_t *const STEP_FONTS[] = {F28, F24};
static const lv_font_t *const ONLY_F24[] = {F24};
static const lv_font_t *const ONLY_F20[] = {F20};

static int text_w(const char *t, const lv_font_t *f)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, 0, LINE_SPACE, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

// Parsed text, kept for the layout passes.
EXT_RAM_BSS_ATTR static char s_a[300], s_b[300];
EXT_RAM_BSS_ATTR static char s_items[SCENE_ITEMS][120];

static void parse_card(const cJSON *card)
{
    scene_t *s = &s_scene;
    switch (s->kind) {
    case CARD_ANSWER:
        get_str(card, "title", s_a, sizeof s_a, true, 0);
        get_str(card, "body", s_b, sizeof s_b, false, 0);
        break;
    case CARD_STAT: {
        get_str(card, "value", s->value, sizeof s->value, true, 0);
        get_str(card, "unit", s->unit, sizeof s->unit, false, 0);
        get_str(card, "label", s_a, sizeof s_a, true, 0);
        get_str(card, "sub", s_b, sizeof s_b, false, 0);
        int unit_w = s->unit[0] ? text_w(s->unit, F28) + 8 : 0;
        s->value_font = F48;
        if (text_w(s->value, F48) + unit_w > 320) {
            s->value_font = F32;
            if (text_w(s->value, F32) + unit_w > 320) {
                issue("value: too wide");
            }
        }
        break;
    }
    case CARD_LIST:
        s->eyes = EYES_PEEK;
        get_str(card, "title", s_a, sizeof s_a, false, 0);
        s->n_items = get_items(card, "items", s_items, 1, 5);
        break;
    case CARD_STEPS:
        get_str(card, "title", s_a, sizeof s_a, false, 0);
        s->n_items = get_items(card, "steps", s_items, 2, SCENE_ITEMS);
        break;
    case CARD_TIMER: {
        get_str(card, "label", s_a, sizeof s_a, false, 0);
        const cJSON *sec = cJSON_GetObjectItemCaseSensitive(card, "seconds");
        if (!cJSON_IsNumber(sec)) {
            issue("seconds: %s", sec ? "not a number" : "missing");
        } else if (sec->valuedouble < 1 || sec->valuedouble > 86400) {
            issue("seconds: %g, want 1-86400", sec->valuedouble);
        } else {
            s->seconds = (int32_t)sec->valuedouble;
        }
        s->value_font = F48;
        s->value_y = 262;
        break;
    }
    case CARD_CONFIRM:
        get_str(card, "question", s_a, sizeof s_a, true, 0);
        get_str(card, "yes", s->yes, sizeof s->yes, true, 0);
        get_str(card, "no", s->no, sizeof s->no, true, 0);
        if (text_w(s->yes, F24) > 120) {
            issue("yes: too wide");
        }
        if (text_w(s->no, F24) > 120) {
            issue("no: too wide");
        }
        break;
    case CARD_LATER:
        get_str(card, "text", s_a, sizeof s_a, true, 0);
        break;
    default:
        break;
    }
}

// Lays the card out from y0; returns the bottom of its text.
static int arrange(int y0)
{
    scene_t *s = &s_scene;
    const line_tag_t plain = {-1, -1, false}, dim = {-1, -1, true};
    int y = y0;
    s->n_lines = 0;
    switch (s->kind) {
    case CARD_ANSWER:
        y = slot("title", s_a, TITLE_FONTS, 2, y, 340, 2, BOTTOM, plain);
        y = slot("body", s_b, BODY_FONTS, 2, y, 330, 4, BOTTOM, dim);
        break;
    case CARD_STAT:
        s->value_y = (int16_t)(y + line_h(s->value_font) / 2);
        y += line_h(s->value_font) + 4;
        y = slot("label", s_a, ONLY_F24, 1, y, 320, 2, BOTTOM, plain);
        y = slot("sub", s_b, ONLY_F20, 1, y, 300, 2, BOTTOM, dim);
        break;
    case CARD_LIST: {
        y = slot("title", s_a, ONLY_F24, 1, y, 300, 1, BOTTOM, dim);
        int row_h = 48;
        // One font for every row: the largest that each item fits in.
        int f = 0;
        for (int i = 0; i < s->n_items && f == 0; i++) {
            bool over;
            flow(s_items[i], BODY_FONTS[0], y + row_h / 2 + i * row_h - line_h(F24) / 2, 260, 1, BOTTOM, false,
                 (line_tag_t){-1, i, false}, &over);
            f = over ? 1 : 0;
        }
        for (int i = 0; i < s->n_items; i++) {
            s->row_y[i] = (int16_t)(y + row_h / 2 + i * row_h);
            char name[24];
            snprintf(name, sizeof name, "items[%d]", i);
            line_tag_t t = {-1, i, false};
            // One line beside the check ring: 260 px of the row's 300.
            slot(name, s_items[i], BODY_FONTS + f, 2 - f, s->row_y[i] - line_h(F24) / 2, 260, 1, BOTTOM, t);
        }
        y += s->n_items * row_h + GAP;
        break;
    }
    case CARD_STEPS: {
        y = slot("title", s_a, ONLY_F20, 1, y, 300, 1, BOTTOM, dim);
        int bottom = y;
        for (int i = 0; i < s->n_items; i++) {
            char name[24];
            snprintf(name, sizeof name, "steps[%d]", i);
            line_tag_t t = {i, -1, false};
            int b = slot(name, s_items[i], STEP_FONTS, 2, y + 6, 330, 4, 384, t);  // the page count sits at 404
            bottom = b > bottom ? b : bottom;
        }
        y = bottom;
        break;
    }
    case CARD_TIMER:
        y = slot("label", s_a, ONLY_F24, 1, 186, 300, 1, 236, dim);
        break;
    case CARD_CONFIRM:
        y = slot("question", s_a, STEP_FONTS, 2, y, 330, 3, 318, plain);
        s->pills_y = 356;
        break;
    case CARD_LATER:
        y = slot("text", s_a, STEP_FONTS, 2, y, 330, 3, BOTTOM - 34, plain);
        s->caption_y = (int16_t)(y + line_h(F20) / 2);  // "SamBot is on it" (ui.c)
        y += line_h(F20) + GAP;
        break;
    default:
        break;
    }
    return y - GAP;
}

// Short cards sit in the middle of the space under the eyes (a little above centre), not at its
// top. A second pass at the lower start keeps it only if nothing new overflows.
static void layout(void)
{
    scene_t *s = &s_scene;
    int top = s->eyes == EYES_PEEK ? TOP_PEEK : TOP_HEADER;
    int area_bottom = 0;  // 0: stays at the top
    switch (s->kind) {
    case CARD_ANSWER: case CARD_STAT: case CARD_LATER: area_bottom = 404; break;
    case CARD_CONFIRM: area_bottom = 318; break;
    default: break;
    }
    int n0 = s->n_issues;
    size_t len0 = strlen(s->issues);
    int bottom = arrange(top);
    if (!area_bottom || bottom >= area_bottom) {
        return;
    }
    int n1 = s->n_issues;
    int shift = (area_bottom - bottom) * 2 / 5;
    s->n_issues = n0;
    s->issues[len0] = 0;
    arrange(top + shift);
    if (s->n_issues > n1) {
        s->n_issues = n0;
        s->issues[len0] = 0;
        arrange(top);
    }
}

bool scene_load(const char *json, size_t len)
{
    scene_t *s = &s_scene;
    memset(s, 0, sizeof *s);
    s->expr = EXPR_NEUTRAL;
    s->kind = CARD_COUNT;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        issue("not a JSON object");
        cJSON_Delete(root);
        return false;
    }
    char mood[24];
    if (get_str(root, "mood", mood, sizeof mood, true, 0)) {
        int e = eyes_expr_from_name(mood);
        if (e == EXPR_COUNT || e == EXPR_CLOSED || e == EXPR_DOT || e == EXPR_SQUEEZE || e == EXPR_LISTEN) {
            issue("mood: \"%s\" is not a mood", mood);
        } else {
            s->expr = e;
        }
    }
    get_str(root, "say", s->say, sizeof s->say, true, SAY_MAX);
    const cJSON *card = cJSON_GetObjectItemCaseSensitive(root, "card");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(card, "kind");
    if (cJSON_IsString(kind)) {
        for (int k = 0; k < CARD_COUNT; k++) {
            if (strcmp(kind->valuestring, KIND_NAMES[k]) == 0) {
                s->kind = (card_kind_t)k;
            }
        }
    }
    cJSON_ArrayForEach(kind, root)
    {
        if (strcmp(kind->string, "say") && strcmp(kind->string, "mood") && strcmp(kind->string, "card")) {
            issue("%s: not a scene field", kind->string);
        }
    }
    if (s->kind == CARD_COUNT) {
        issue(card ? "card: unknown kind" : "card: missing");
        cJSON_Delete(root);
        return false;
    }
    check_fields(card, s->kind);
    s->eyes = EYES_HEADER;
    parse_card(card);
    layout();
    cJSON_Delete(root);
    return true;
}

// ---------- console ----------

EXT_RAM_BSS_ATTR static char s_buf[4096];
static size_t s_len;

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

void scene_summary(char *dst, size_t n)
{
    const scene_t *s = &s_scene;
    size_t used = 0;
    dst[0] = 0;
    if (s->kind == CARD_TIMER) {
        used += snprintf(dst, n, "%d:%02d", (int)(s->seconds / 60), (int)(s->seconds % 60));
    } else if (s->value[0]) {
        used += snprintf(dst + used, n - used, "%s%s%s", s->value, s->unit[0] ? " " : "", s->unit);
    }
    for (int i = 0; i < s->n_lines && used + 1 < n; i++) {
        used += snprintf(dst + used, n - used, "%s%s", used ? " " : "", s->lines[i].text);
    }
}

static void print_result(bool shown)
{
    scene_t *s = &s_scene;
    printf("SCENE %s kind=%s lines=%d issues=%d%s%s\n", shown ? "shown" : "rejected", scene_kind_name(s->kind),
           s->n_lines, s->n_issues, s->n_issues ? ": " : "", s->issues);
}

int scene_console(int argc, char **argv, bool (*show)(void))
{
    const char *sub = argc > 2 ? argv[2] : "";
    if (strcmp(sub, "begin") == 0) {
        s_len = 0;
        return 0;
    }
    if (strcmp(sub, "+") == 0 && argc > 3) {
        const char *h = argv[3];
        for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
            int hi = hexval(h[i]), lo = hexval(h[i + 1]);
            if (hi < 0 || lo < 0 || s_len >= sizeof s_buf - 1) {
                printf("SCENE error: bad hex or too long\n");
                return 1;
            }
            s_buf[s_len++] = (char)(hi << 4 | lo);
        }
        return 0;
    }
    if (strcmp(sub, "end") == 0) {
        if (bsp_display_lock(2000) != ESP_OK) {
            printf("SCENE error: display busy\n");
            return 1;
        }
        bool ok = scene_load(s_buf, s_len);
        bool shown = ok && show();
        bsp_display_unlock();
        print_result(shown);
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "last") == 0) {
        print_result(true);
        return 0;
    }
    printf("usage: ui scene begin | + <hex> | end | last\n");
    return 1;
}
