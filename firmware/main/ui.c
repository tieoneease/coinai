#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "bsp/esp-bsp.h"
#include "clipstore.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "eyes.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "misc/lv_text_private.h"  // lv_text_get_next_line
#include "display/lv_display_private.h"  // inv_areas, LV_INV_BUF_SIZE (see inv_cb)
#include "misc/lv_area_private.h"         // lv_area_join
#include "nvs.h"
#include "power.h"
#include "settings.h"

static const char *TAG = "ui";

#define C 233
#define TICK_MS 15
// Below this much speech the app does not prompt SamBot (D17); the coin says so at once.
#define NO_SPEECH_MS 400
#define RECENT_MAX 5

#define F16 (&lv_font_montserrat_16)
#define F20 (&lv_font_montserrat_20)
#define F24 (&lv_font_montserrat_24)
#define F28 (&lv_font_montserrat_28)
#define F32 (&lv_font_montserrat_32)
#define F48 (&lv_font_montserrat_48)

// Provisional palette (the visual pass is still to come).
#define COL_TEXT 0xF4ECDF
#define COL_DIM 0x8E867A
#define COL_AMBER 0xFFB547
#define COL_REC 0xFF4F45
#define COL_FAINT 0x2E2A25
#define COL_TILE 0x1F1C19

typedef enum {
    M_BOOT, M_OFF, M_BYE, M_FACE, M_LISTEN, M_SEND, M_THINK, M_AWAY, M_DROWSY, M_POWERHOLD, M_PAIR,
    // content: dismissed by BOOT or a swipe; the idle timeout applies
    M_REPLY, M_FAIL, M_SENT, M_SAVED, M_NOSPEECH, M_DISCARDED, M_PAIRED, M_PAIRFAIL, M_CONTROLS, M_RECENT,
    M_DETAIL, M_BRIGHT, M_SETTINGS, M_PHONE, M_ABOUT, M_POWERCONF,
} ui_mode_t;

static const char *MODE_NAMES[] = {
    "boot", "off", "bye", "face", "listen", "send", "think", "away", "drowsy", "powerhold", "pair",
    "reply", "fail", "sent", "saved", "nospeech", "discarded", "paired", "pairfail", "controls", "recent",
    "detail", "bright", "settings", "phone", "about", "powerconf",
};

static bool is_content(ui_mode_t m)
{
    return m >= M_REPLY;
}

// ---------- inputs ----------

typedef struct {
    bool rec;
    float level;
    ble_status_t ble;
    bool pmic_ok;
    pmic_status_t pmic;
} inputs_t;

typedef struct {
    ui_ev_t ev;
    audio_clip_result_t clip;
} ui_msg_t;

static QueueHandle_t s_q;
static ui_hooks_t s_hooks;
static inputs_t s_in, s_prev;
static bool s_prev_valid;
static int64_t s_last_ble_us;
static ui_state_t s_status;  // from the status task, 5 Hz
static bool s_status_valid;
static float s_now;          // UI clock, s
static int64_t s_last_us;
static ui_mode_t s_mode = M_BOOT;

// requests from other tasks (under the display lock)
static volatile int s_bye_req = -1;
static volatile bool s_bye_done = true;
static bool s_drowsy, s_drowsy_shown;
static bool s_drowsy_done;  // this idle period's drowsy flow has started

// ---------- items: UI objects that fade and slide on springs ----------

// Long text is one label per line, so lines fade in one after another (60 ms apart) and only a line
// or two re-render per frame: a whole 340 px reply fading at once costs 15-20 ms a frame on the coin.
#define HEARD_LINES 9   // 190 px at F16 (20 px a line), the smallest fit_font font
#define BODY_LINES 12   // 240 px at F16

enum {
    IT_STATUS, IT_CHIP, IT_CHIP_H, IT_TIMER, IT_HINT, IT_CAP, IT_BODY, IT_SUB, IT_PASS,
    IT_TILE0, IT_TILE1, IT_TILE2, IT_TILE3, IT_ROW0, IT_ROW1, IT_ROW2, IT_ROW3, IT_VALUE,
    IT_HEARD0, IT_LINE0 = IT_HEARD0 + HEARD_LINES, IT_COUNT = IT_LINE0 + BODY_LINES,
};

#define KEEP(id) (1ULL << (id))
#define GROUP(first, n) ((((1ULL << (n)) - 1)) << (first))
#define HEARD_GROUP GROUP(IT_HEARD0, HEARD_LINES)
#define LINE_GROUP GROUP(IT_LINE0, BODY_LINES)

typedef struct {
    lv_obj_t *obj;
    spring_t o, y;
    bool on;
    int opa, ty;
    float show_at, hide_at;  // a delayed fade in or out (0 = none)
} item_t;

static item_t s_items[IT_COUNT];
static lv_obj_t *s_scr;

static void item_apply(item_t *it)
{
    int opa = (int)lroundf(fminf(fmaxf(it->o.x, 0), 1) * 255);
    if (opa != it->opa) {
        lv_obj_set_style_opa(it->obj, (lv_opa_t)opa, 0);
        it->opa = opa;
    }
    int ty = (int)lroundf(it->y.x);
    if (ty != it->ty) {
        lv_obj_set_style_translate_y(it->obj, ty, 0);
        it->ty = ty;
    }
}

// Shows an item that already exists (also one fading out); NULL if it has to be built.
static lv_obj_t *item_show(int id)
{
    item_t *it = &s_items[id];
    if (!it->obj) {
        return NULL;
    }
    it->on = true;
    it->show_at = it->hide_at = 0;
    spring_to(&it->o, 1, M_UI);
    spring_to(&it->y, 0, M_UI);
    return it->obj;
}

static lv_obj_t *item_attach(int id, lv_obj_t *obj, float from_y)
{
    item_t *it = &s_items[id];
    if (it->obj) {
        lv_obj_delete(it->obj);
    }
    it->obj = obj;
    spring_init(&it->o, 0, M_UI);
    spring_init(&it->y, from_y, M_UI);
    it->opa = it->ty = -9999;
    item_apply(it);
    return item_show(id);
}

// As item_attach, but the item starts fading in after `delay` s.
static lv_obj_t *item_attach_later(int id, lv_obj_t *obj, float from_y, float delay)
{
    item_attach(id, obj, from_y);
    if (delay > 0) {
        item_t *it = &s_items[id];
        it->show_at = s_now + delay;
        spring_to(&it->o, 0, M_UI);
        spring_to(&it->y, from_y, M_UI);
    }
    return obj;
}

static lv_obj_t *item_get(int id)
{
    return s_items[id].on ? s_items[id].obj : NULL;
}

static void item_hide(int id, float to_y)
{
    item_t *it = &s_items[id];
    if (it->obj && it->on) {
        it->on = false;
        it->show_at = it->hide_at = 0;
        spring_to(&it->o, 0, M_UI);
        spring_to(&it->y, to_y, M_UI);
    }
}

// Hides every item not in `keep` (a KEEP/GROUP mask); lines of text go 25 ms apart.
static void items_hide_except(uint64_t keep)
{
    for (int i = 0; i < IT_COUNT; i++) {
        if (keep & KEEP(i)) {
            continue;
        }
        int line = i >= IT_LINE0 ? i - IT_LINE0 : i >= IT_HEARD0 ? i - IT_HEARD0 : -1;
        if (line > 0 && s_items[i].on) {
            if (!s_items[i].hide_at) {
                s_items[i].hide_at = s_now + 0.025f * line;
            }
        } else {
            item_hide(i, -10);
        }
    }
}

static void items_clear(void)
{
    for (int i = 0; i < IT_COUNT; i++) {
        if (s_items[i].obj) {
            lv_obj_delete(s_items[i].obj);
        }
        memset(&s_items[i], 0, sizeof s_items[i]);
    }
}

static void items_step(float dt)
{
    for (int i = 0; i < IT_COUNT; i++) {
        item_t *it = &s_items[i];
        if (!it->obj) {
            continue;
        }
        if (it->show_at && it->on && s_now >= it->show_at) {
            it->show_at = 0;
            spring_to(&it->o, 1, M_UI);
            spring_to(&it->y, 0, M_UI);
        }
        if (it->hide_at && it->on && s_now >= it->hide_at) {
            item_hide(i, -10);
        }
        spring_step(&it->o, dt);
        spring_step(&it->y, dt);
        if (!it->on && it->o.x < 0.01f) {
            lv_obj_delete(it->obj);
            memset(it, 0, sizeof *it);
            continue;
        }
        item_apply(it);
    }
}

// ---------- builders ----------

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

static lv_obj_t *row_box(lv_obj_t *parent, int gap)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static lv_obj_t *dot(lv_obj_t *parent, int size, uint32_t color)
{
    lv_obj_t *d = box(parent);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

static void place(lv_obj_t *o, int y_center)
{
    lv_obj_align(o, LV_ALIGN_CENTER, 0, y_center - C);
}

// Reply text from the phone may hold any UTF-8; the built-in fonts cover ASCII. Common punctuation
// is mapped, anything else dropped.
static void ascii_text(char *dst, size_t n, const char *src)
{
    size_t o = 0;
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
        const char *rep = "";
        switch (cp) {
        case 0x2018: case 0x2019: case 0x2032: rep = "'"; break;
        case 0x201C: case 0x201D: case 0x2033: rep = "\""; break;
        case 0x2026: rep = "..."; break;
        case 0x2013: case 0x2014: case 0x2212: rep = "-"; break;
        case 0x00A0: case 0x202F: case 0x2009: rep = " "; break;
        case 0x00B0: rep = " deg"; break;
        case 0x00D7: rep = "x"; break;
        case 0x2022: case 0x00B7: rep = "-"; break;
        default: break;
        }
        for (const char *r = rep; *r && o + 1 < n; r++) {
            dst[o++] = *r;
        }
    }
    dst[o] = 0;
}

// The largest font in which the text fits the box.
static const lv_font_t *fit_font(const char *text, int width, int max_h, const lv_font_t *largest)
{
    const lv_font_t *fonts[] = {F32, F28, F24, F20, F16};
    bool started = false;
    for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++) {
        if (fonts[i] == largest) {
            started = true;
        }
        if (!started) {
            continue;
        }
        lv_point_t sz;
        lv_text_get_size(&sz, text, fonts[i], 0, 2, width, LV_TEXT_FLAG_NONE);
        if (sz.y <= max_h) {
            return fonts[i];
        }
    }
    return F16;
}

static lv_obj_t *build_caption(const char *text, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = label(s_scr, font, color, text);
    place(l, y);
    return l;
}

// Wrapped text whose first line is centred on y0; the font shrinks to fit max_h.
static lv_obj_t *build_body(const char *utf8, int y0, int width, int max_h, const lv_font_t *largest, uint32_t color)
{
    char text[256];
    ascii_text(text, sizeof text, utf8);
    const lv_font_t *font = fit_font(text, width, max_h, largest);
    // Width and wrapping before the text: a label first measured at zero width wraps every letter
    // onto its own line, and its first layout then invalidates that full-height box (334x466,
    // a whole-screen redraw on the coin).
    lv_obj_t *l = label(s_scr, font, color, "");
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(l, 2, 0);
    lv_obj_update_layout(l);  // the label measures text against its coords, not its width style
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y0 - lv_font_get_line_height(font) / 2);
    return l;
}

// Wrapped text as one label per line (see HEARD_LINES), the first line centred on y0; the font
// shrinks to fit max_h. Lines fade in `stagger` s apart. Returns the number of lines.
static int build_lines(int first, int max_lines, const char *utf8, int y0, int width, int max_h,
                       const lv_font_t *largest, uint32_t color, float stagger)
{
    char text[256];
    ascii_text(text, sizeof text, utf8);
    const lv_font_t *font = fit_font(text, width, max_h, largest);
    lv_text_attributes_t at;
    lv_text_attributes_init(&at);
    at.max_width = width;
    at.line_space = 2;
    int32_t line_h = lv_font_get_line_height(font) + 2;
    uint32_t len = (uint32_t)strlen(text), i = 0;
    int n = 0;
    while (i < len && n < max_lines) {
        uint32_t k = lv_text_get_next_line(text + i, len - i, font, NULL, &at);
        if (k == 0) {
            break;
        }
        char line[160];
        uint32_t m = k < sizeof line - 1 ? k : sizeof line - 1;
        memcpy(line, text + i, m);
        while (m && (line[m - 1] == ' ' || line[m - 1] == '\n')) {
            m--;
        }
        line[m] = 0;
        i += k;
        if (!m) {
            continue;
        }
        lv_obj_t *l = label(s_scr, font, color, line);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y0 - line_h / 2 + n * line_h);
        item_attach_later(first + n, l, 14, n * stagger);
        n++;
    }
    for (int j = n; j < max_lines; j++) {
        item_hide(first + j, -10);
    }
    return n;
}

static bool clock_known(void)
{
    int16_t off = (int16_t)settings_get(SET_UTC_OFFSET_MIN);
    return time(NULL) > 1700000000 && off != SETTINGS_UTC_OFFSET_UNSET;
}

static bool local_time(struct tm *out)
{
    if (!clock_known()) {
        return false;
    }
    time_t t = time(NULL) + (int16_t)settings_get(SET_UTC_OFFSET_MIN) * 60;
    gmtime_r(&t, out);
    return true;
}

static lv_obj_t *build_status(void)
{
    lv_obj_t *r = row_box(s_scr, 12);
    label(r, F32, COL_TEXT, "");       // 0: time
    label(r, F20, COL_DIM, "");        // 1: battery symbol
    label(r, F20, COL_DIM, "");        // 2: percent
    place(r, 354);
    return r;
}

static void update_status(lv_obj_t *r)
{
    lv_obj_t *t = lv_obj_get_child(r, 0), *b = lv_obj_get_child(r, 1), *p = lv_obj_get_child(r, 2);
    struct tm tm;
    char buf[24];
    if (local_time(&tm)) {
        snprintf(buf, sizeof buf, "%d:%02d", tm.tm_hour, tm.tm_min);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_HIDDEN);
        set_text(t, buf);
    } else {
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    }
    int pct = s_in.pmic_ok && s_in.pmic.battery_present ? s_in.pmic.battery_pct : -1;
    bool chg = s_in.pmic_ok && s_in.pmic.charging;
    if (pct < 0) {
        set_text(b, s_in.pmic_ok && s_in.pmic.vbus_good ? LV_SYMBOL_USB : "");
        set_text(p, "");
        return;
    }
    const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3
                    : pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    snprintf(buf, sizeof buf, "%s%s", chg ? LV_SYMBOL_CHARGE " " : "", sym);
    set_text(b, buf);
    snprintf(buf, sizeof buf, "%d%%", pct);
    set_text(p, buf);
    uint32_t col = pct <= 15 && !chg ? COL_REC : COL_DIM;
    lv_obj_set_style_text_color(b, lv_color_hex(chg ? COL_AMBER : col), 0);
    lv_obj_set_style_text_color(p, lv_color_hex(col), 0);
}

static lv_obj_t *build_chip(int y)
{
    lv_obj_t *c = row_box(s_scr, 8);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(c, 18, 0);
    lv_obj_set_style_pad_ver(c, 7, 0);
    dot(c, 9, COL_AMBER);
    label(c, F20, COL_TEXT, "");
    place(c, y);
    return c;
}

static void update_chip(lv_obj_t *c, const char *text, bool with_dot)
{
    lv_obj_t *d = lv_obj_get_child(c, 0);
    if (with_dot) {
        lv_obj_remove_flag(d, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    set_text(lv_obj_get_child(c, 1), text);
}

static lv_obj_t *build_timer(void)
{
    lv_obj_t *r = row_box(s_scr, 10);
    dot(r, 16, COL_REC);
    label(r, F32, COL_TEXT, "0:00");
    place(r, 372);
    return r;
}

// Controls: the tiles' icons. Sun and moon are drawn; gear and power are font symbols.
static void icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    intptr_t kind = (intptr_t)lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = lv_color_hex(COL_TEXT);
    if (kind == 0) {
        lv_area_t c = {cx - 8, cy - 8, cx + 8, cy + 8};
        lv_draw_rect(layer, &r, &c);
        lv_draw_line_dsc_t l;
        lv_draw_line_dsc_init(&l);
        l.color = lv_color_hex(COL_TEXT);
        l.width = 3;
        l.round_start = l.round_end = 1;
        for (int i = 0; i < 8; i++) {
            float an = i * (float)M_PI / 4;
            l.p1 = (lv_point_precise_t){cx + (int32_t)lroundf(cosf(an) * 13), cy + (int32_t)lroundf(sinf(an) * 13)};
            l.p2 = (lv_point_precise_t){cx + (int32_t)lroundf(cosf(an) * 18), cy + (int32_t)lroundf(sinf(an) * 18)};
            lv_draw_line(layer, &l);
        }
    } else {
        lv_area_t c = {cx - 14, cy - 14, cx + 14, cy + 14};
        lv_draw_rect(layer, &r, &c);
        r.bg_color = lv_color_hex(COL_TILE);
        lv_area_t m = {cx - 5, cy - 17, cx + 19, cy + 7};
        lv_draw_rect(layer, &r, &m);
    }
}

static const struct {
    int x, y;
    const char *name;
} TILES[4] = {{176, 142, "Brightness"}, {290, 142, "Screen off"}, {176, 262, "Settings"}, {290, 262, "Power off"}};

static lv_obj_t *build_tile(int i)
{
    lv_obj_t *t = box(s_scr);
    lv_obj_set_size(t, 112, 120);
    lv_obj_set_pos(t, TILES[i].x - 56, TILES[i].y - 40);
    lv_obj_t *c = dot(t, 80, COL_TILE);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 0);
    if (i < 2) {
        lv_obj_add_event_cb(c, icon_draw_cb, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)i);
    } else {
        lv_obj_t *s = label(c, F28, COL_TEXT, i == 2 ? LV_SYMBOL_SETTINGS : LV_SYMBOL_POWER);
        lv_obj_center(s);
    }
    lv_obj_t *l = label(t, F16, COL_DIM, TILES[i].name);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 92);
    return t;
}

#define ROW_Y0 178
#define ROW_DY 80

static lv_obj_t *build_row(int i, const char *title, const char *sub, bool divider)
{
    lv_obj_t *r = box(s_scr);
    lv_obj_set_width(r, 340);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(r, 4, 0);
    char t[96];
    ascii_text(t, sizeof t, title);
    lv_obj_t *lt = label(r, F24, COL_TEXT, t);
    lv_label_set_long_mode(lt, LV_LABEL_LONG_MODE_DOTS);  // one line, ending in "..."
    lv_obj_set_size(lt, 320, lv_font_get_line_height(F24));
    label(r, F16, COL_DIM, sub);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, ROW_Y0 + i * ROW_DY - 24);
    if (divider) {
        lv_obj_t *d = box(r);
        lv_obj_set_size(d, 300 - i * 30, 2);
        lv_obj_set_style_bg_color(d, lv_color_hex(COL_FAINT), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }
    return r;
}

// ---------- recent replies (kept in NVS: they outlive standby) ----------

typedef struct {
    uint16_t clip;
    uint8_t state;
    uint8_t pad;
    uint32_t utc_s;  // 0 = unknown
    char text[201];
} recent_t;

static recent_t s_recent[RECENT_MAX];
static uint8_t s_nrecent;

static void recent_load(void)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof s_recent;
    if (nvs_get_blob(h, "recent", s_recent, &len) == ESP_OK && len % sizeof(recent_t) == 0) {
        s_nrecent = (uint8_t)(len / sizeof(recent_t));
    }
    nvs_close(h);
}

static void recent_push(uint16_t clip, int state, const char *text)
{
    if (s_nrecent && s_recent[0].clip == clip && s_recent[0].state == state) {
        return;
    }
    memmove(&s_recent[1], &s_recent[0], sizeof(recent_t) * (RECENT_MAX - 1));
    recent_t *r = &s_recent[0];
    memset(r, 0, sizeof *r);
    r->clip = clip;
    r->state = (uint8_t)state;
    r->utc_s = time(NULL) > 1700000000 ? (uint32_t)time(NULL) : 0;
    snprintf(r->text, sizeof r->text, "%.200s", text);
    if (s_nrecent < RECENT_MAX) {
        s_nrecent++;
    }
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "recent", s_recent, sizeof(recent_t) * s_nrecent);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------- flows: timelines of steps; each step returns the wait before the next ----------

typedef float (*flow_fn)(int step);
#define FLOW_END (-1.0f)
#define FLOW_HOLD 1e9f

static flow_fn s_flow;
static int s_step;
static float s_flow_at;
static uint32_t s_flow_gen;
static const char *s_flow_name = "";

#define FLOW(f) flow_start(f, #f)

static float f_bye(int k);
static float f_drowsy(int k);
static float f_unsleep(int k);
static float f_collapse(int k);
static float f_wake(int k);
static float f_power_on(int k);
static float f_wait_rec(int k);

static void flow_start(flow_fn f, const char *name)
{
    // A goodbye leaves the eyes faded out; anything but the goodbyes and wakes brings them back.
    if (s_drowsy_shown && f != f_bye && f != f_unsleep && f != f_drowsy) {
        s_drowsy_shown = false;  // something else took over from drowsy: its z's go
        eyes_zzz(false);
    }
    bool from_off = s_mode == M_OFF || s_mode == M_BYE;
    if (from_off && f != f_bye && f != f_collapse && f != f_wake && f != f_power_on && f != f_wait_rec) {
        eyes_visible_to(1, M_UI);
        eyes_spread_to(1, M_MOOD);
    }
    s_flow = f;
    s_step = 0;
    s_flow_at = s_now;
    s_flow_gen++;
    s_flow_name = name + 2;  // "f_..."
}

static void flow_run(void)
{
    for (int guard = 0; s_flow && s_now >= s_flow_at && guard < 16; guard++) {
        uint32_t gen = s_flow_gen;
        flow_fn f = s_flow;
        float d = f(s_step++);
        if (gen != s_flow_gen) {
            continue;  // the step started another flow
        }
        if (d < 0) {
            s_flow = NULL;
            break;
        }
        s_flow_at = s_now + d;
    }
}

// flow parameters
static uint16_t s_clip;           // the clip a send/away flow is about
static bool s_listen_closed;      // the listen flow opens from closed eyes (a wake)
static bool s_discard_flag;       // UI_EV_DISCARDED came before the clip result
static char s_text[201];
static float s_t0, s_rec_t0;
static float s_discard_t0 = -1;  // BOOT held while recording: when the countdown began
static float s_rec_end_t = -1;   // the recording ended; the clip result is due
static int s_tap_x, s_tap_y, s_detail;
static ui_bye_t s_bye_kind;
static bool s_hold_down;          // finger down on a hold-to-confirm screen
static float s_hold_t0;
static float s_face_sent_until;   // the face's chip shows "Sent" until then
static int s_bright;

static bool low_battery(void)
{
    return s_in.pmic_ok && s_in.pmic.battery_present && s_in.pmic.battery_pct >= 0 && s_in.pmic.battery_pct <= 15 &&
           !s_in.pmic.charging;
}

static expr_t rest_expr(void)
{
    return low_battery() ? EXPR_TIRED : EXPR_NEUTRAL;
}

static float f_face(int k);
static float f_think(int k);

static void go_face(void)
{
    FLOW(f_face);
}

static float f_face(int k)
{
    switch (k) {
    case 0:
        s_mode = M_FACE;
        items_hide_except(KEEP(IT_STATUS) | KEEP(IT_CHIP));
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_zzz(false);
        eyes_set_level(0);
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        eyes_set_expr(rest_expr(), M_MOOD);
        eyes_blink_slow(low_battery() ? 1.8f : 1);
        eyes_look(0, 0, M_MOOD);
        eyes_auto(true, true);
        return 0.14f;
    default: {
        lv_obj_t *st = item_show(IT_STATUS);
        if (!st) {
            st = item_attach(IT_STATUS, build_status(), 14);
        }
        update_status(st);
        return FLOW_END;
    }
    }
}

static float f_listen(int k)
{
    switch (k) {
    case 0:
        s_mode = M_LISTEN;
        s_discard_flag = false;
        s_discard_t0 = s_rec_end_t = -1;
        items_hide_except(0);
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_zzz(false);
        eyes_quiet(0.9f);
        if (s_listen_closed) {
            eyes_reset(LAYOUT_LISTEN);
            eyes_snap_expr(EXPR_CLOSED);
            eyes_auto(true, false);
            eyes_look(0, -4, M_SACCADE);
            return 0.04f;
        }
        eyes_auto(true, false);
        eyes_look(0, -4, M_SACCADE);
        eyes_param_to(EP_H, 96, M_SACCADE);  // anticipation: a quick squash
        return 0.07f;
    case 1:
        eyes_set_expr(EXPR_LISTEN, M_POP);
        eyes_layout(LAYOUT_LISTEN, M_LAYOUT);
        s_rec_t0 = s_now - (s_listen_closed ? 0.3f : 0.27f);  // audio started before the eyes did
        item_attach(IT_TIMER, build_timer(), 14);
        return 0.09f;
    case 2:
        item_attach(IT_HINT, build_caption("Release to send", 418, F20, COL_DIM), 14);
        return FLOW_HOLD;
    default:
        return FLOW_HOLD;
    }
}

static float f_discarded(int k)
{
    switch (k) {
    case 0:
        s_mode = M_DISCARDED;
        items_hide_except(0);
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_set_level(0);
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        eyes_set_expr(EXPR_SQUEEZE, M_SACCADE);
        return 0.22f;
    case 1:
        eyes_set_expr(EXPR_WORRIED, M_POP);
        item_attach(IT_CAP, build_caption("Discarded", 372, F24, COL_TEXT), 14);
        return 1.2f;
    case 2:
        eyes_blink(1, 1);
        return 0.3f;
    default:
        go_face();
        return FLOW_END;
    }
}

static float f_nospeech(int k)
{
    switch (k) {
    case 0:
        s_mode = M_NOSPEECH;
        items_hide_except(0);
        eyes_set_level(0);
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        eyes_set_expr(EXPR_CONFUSED, M_POP);
        eyes_look(16, -12, M_SACCADE);
        return 0.2f;
    case 1:
        item_attach(IT_CAP, build_caption("Didn't hear anything", 372, F24, COL_TEXT), 14);
        return 1.3f;
    case 2:
        eyes_blink(2, 1);
        return 0.3f;
    case 3:
        eyes_set_expr(EXPR_NEUTRAL, M_MOOD);
        eyes_look(0, 0, M_SACCADE);
        return 0.5f;
    default:
        go_face();
        return FLOW_END;
    }
}

// Sending: the rim fills as the clip's tail goes out; shown for at least 0.5 s so it reads.
static float s_send_actual;

static float f_send(int k)
{
    switch (k) {
    case 0:
        s_mode = M_SEND;
        items_hide_except(0);
        eyes_set_level(0);
        eyes_quiet(1.2f);
        eyes_auto(true, false);
        eyes_set_expr(EXPR_FOCUS, M_SACCADE);
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        eyes_look(30, -8, M_SACCADE);  // toward the phone
        eyes_rim_track(true);
        eyes_rim(RIM_PROGRESS, lv_color_hex(COL_AMBER));
        eyes_rim_progress(0);
        s_t0 = s_now;
        s_send_actual = 0;
        return FLOW_HOLD;
    default:
        return FLOW_HOLD;
    }
}

static float f_sent(int k)
{
    switch (k) {
    case 0:
        s_mode = M_SENT;
        items_hide_except(0);
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_set_expr(EXPR_JOY, M_POP);
        eyes_kick_y(-280);
        eyes_look(0, 0, M_SACCADE);
        item_attach(IT_CAP, build_caption("Sent", 372, F28, COL_TEXT), 14);
        return 1.6f;
    default:
        go_face();
        return FLOW_END;
    }
}

static void show_heard(const char *text)
{
    char q[230];
    snprintf(q, sizeof q, "\"%s\"", text);
    build_lines(IT_HEARD0, HEARD_LINES, q, 196, 330, 190, F24, COL_DIM, 0.06f);
}

static float f_think(int k)
{
    switch (k) {
    case 0:
        s_mode = M_THINK;
        items_hide_except(HEARD_GROUP);
        eyes_rim_track(true);
        eyes_rim(RIM_ORBIT, lv_color_hex(COL_AMBER));
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_THINK, M_MOOD);
        eyes_auto(true, false);
        eyes_look(-22, -24, M_MOOD);
        return 0.16f;
    case 1:
        if (s_text[0]) {
            show_heard(s_text);
        }
        return 0.62f;
    default:
        eyes_look(k % 2 ? 16 : -22, k % 2 ? -24 : -26, M_MOOD);
        return 0.62f;
    }
}

static float f_reply(int k)
{
    switch (k) {
    case 0:
        s_mode = M_REPLY;
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        items_hide_except(0);
        eyes_quiet(1.0f);
        eyes_auto(true, false);
        eyes_set_expr(EXPR_SURPRISED, M_POP);
        eyes_look(0, -4, M_SACCADE);
        eyes_scale_to(0.62f, M_POP);
        eyes_y_to(120, M_POP);
        return 0.1f;
    case 1:
        eyes_set_expr(EXPR_JOY, M_POP);
        eyes_kick_y(-320);
        return 0.36f;
    case 2:
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_CONTENT, M_MOOD);
        eyes_look(0, 20, M_MOOD);
        return 0.07f;
    case 3:
        build_lines(IT_LINE0, BODY_LINES, s_text, 184, 340, 230, F28, COL_TEXT, 0.06f);
        return 0.9f;
    case 4:
        eyes_blink(1, 1);
        return 0.01f;
    case 5: case 6: case 7: case 8: case 9: case 10: {
        int j = (k - 5) / 2;  // the eyes read along, line by line
        eyes_look((k - 5) % 2 ? 12 : -12, 14 + j * 6, M_SLOW);
        return 0.48f;
    }
    default:
        eyes_look(0, 18, M_MOOD);
        eyes_auto(true, true);
        return FLOW_END;
    }
}

static float f_fail(int k)
{
    switch (k) {
    case 0:
        s_mode = M_FAIL;
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        items_hide_except(0);
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_auto(true, false);
        eyes_set_expr(EXPR_SAD, M_DROOP);
        eyes_look(-8, 16, M_DROOP);
        return 0.3f;
    case 1:
        item_attach(IT_BODY, build_caption("Didn't reach SamBot", 204, F28, COL_TEXT), 14);
        item_attach(IT_SUB, build_body(s_text[0] ? s_text : "The phone will try again.", 252, 330, 120, F20, COL_DIM), 14);
        return 0.9f;
    case 2:
        eyes_y_to(110, (motion_t){6, 1});  // a sigh
        return 0.7f;
    default:
        eyes_y_to(100, (motion_t){8, 0.9f});
        eyes_blink(1, 1.6f);
        return FLOW_END;
    }
}

static float f_saved(int k)
{
    switch (k) {
    case 0:
        s_mode = M_SAVED;
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_HAPPY, M_MOOD);
        eyes_look(0, 16, M_MOOD);
        return 0.12f;
    case 1:
        item_attach(IT_BODY, build_caption("Saved on the coin", 206, F28, COL_TEXT), 14);
        item_attach(IT_SUB, build_caption("Your phone isn't in reach.", 252, F20, COL_DIM), 14);
        return 0.22f;
    case 2: {
        char t[24];
        snprintf(t, sizeof t, "%u waiting", s_in.ble.pending ? s_in.ble.pending : 1);
        lv_obj_t *c = item_attach(IT_CHIP_H, build_chip(330), 14);
        update_chip(c, t, true);
        return 0.7f;
    }
    case 3:
        eyes_look(0, 30, M_MOOD);  // a look at the chip
        return 0.65f;
    case 4:
        eyes_look(0, 16, M_MOOD);
        return 4.0f;
    default:
        go_face();
        return FLOW_END;
    }
}

static float f_away(int k)
{
    switch (k) {
    case 0:
        s_mode = M_AWAY;
        items_hide_except(0);
        eyes_set_level(0);
        eyes_auto(true, false);
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        eyes_set_expr(EXPR_FOCUS, M_SACCADE);
        eyes_look(-38, -6, M_SACCADE);  // looking for the phone
        return 0.32f;
    case 1: eyes_look(38, -6, M_SACCADE); return 0.36f;
    case 2: eyes_look(-22, -4, M_SACCADE); return 0.22f;
    case 3:
        eyes_set_expr(EXPR_WORRIED, M_MOOD);
        eyes_look(0, 4, M_MOOD);
        return 0.42f;
    default:
        FLOW(f_saved);
        return FLOW_END;
    }
}

static float f_pair(int k)
{
    switch (k) {
    case 0:
        s_mode = M_PAIR;
        items_hide_except(0);
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_zzz(false);
        eyes_auto(true, false);
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_CURIOUS, M_POP);
        eyes_look(0, 18, M_MOOD);
        return 0.15f;
    case 1: {
        char code[16];
        uint32_t pk = s_in.ble.passkey;
        snprintf(code, sizeof code, "%03lu %03lu", (unsigned long)(pk / 1000), (unsigned long)(pk % 1000));
        item_attach(IT_CAP, build_caption("Pair with phone", 178, F20, COL_DIM), 14);
        item_attach(IT_PASS, build_caption(code, 258, F48, COL_TEXT), 14);
        item_attach(IT_SUB, build_caption("Type this on your phone", 330, F20, COL_TEXT), 14);
        return 1.4f;
    }
    case 2:
        eyes_blink(1, 1);
        return FLOW_HOLD;
    default:
        return FLOW_HOLD;
    }
}

static float f_paired(int k)
{
    switch (k) {
    case 0:
        s_mode = M_PAIRED;
        items_hide_except(0);
        eyes_set_expr(EXPR_JOY, M_POP);
        eyes_kick_y(-260);
        eyes_look(0, 0, M_SACCADE);
        return 0.16f;
    case 1:
        item_attach(IT_BODY, build_caption("Paired", 238, F32, COL_TEXT), 14);
        return 1.6f;
    default:
        go_face();
        return FLOW_END;
    }
}

static float f_pairfail(int k)
{
    switch (k) {
    case 0:
        s_mode = M_PAIRFAIL;
        items_hide_except(0);
        eyes_set_expr(EXPR_SAD, M_DROOP);
        eyes_look(-6, 14, M_DROOP);
        return 0.3f;
    case 1:
        item_attach(IT_BODY, build_caption("Pairing failed", 214, F28, COL_TEXT), 14);
        item_attach(IT_SUB, build_caption("Try again from the app.", 258, F20, COL_DIM), 14);
        return 2.2f;
    default:
        go_face();
        return FLOW_END;
    }
}

static float f_controls(int k)
{
    if (k == 0) {
        s_mode = M_CONTROLS;
        item_hide(IT_STATUS, 50);
        item_hide(IT_CHIP, 50);
        items_hide_except(KEEP(IT_STATUS) | KEEP(IT_CHIP));
        eyes_layout(LAYOUT_FOOTER, M_LAYOUT);
        eyes_set_expr(EXPR_HAPPY, M_MOOD);
        eyes_auto(true, false);
        eyes_look(0, -34, M_MOOD);  // looking up at the tiles
        return 0.07f;
    }
    if (k <= 4) {
        item_attach(IT_TILE0 + k - 1, build_tile(k - 1), -60);
        return 0.04f;
    }
    return FLOW_END;
}

static int recent_rows(char titles[4][48], char subs[4][32], int *reply_idx)
{
    int n = 0;
    uint16_t pending = s_in.ble.pending;
    if (pending && !s_in.rec) {
        snprintf(titles[n], 48, "%u clip%s waiting", pending, pending == 1 ? "" : "s");
        snprintf(subs[n], 32, "%s", s_in.ble.connected ? "Sending to the phone" : "Phone not in reach");
        reply_idx[n++] = -1;
    }
    for (int i = 0; i < s_nrecent && n < 3; i++) {
        snprintf(titles[n], 48, "%.44s", s_recent[i].text[0] ? s_recent[i].text : "(no text)");
        char when[12] = "";
        if (s_recent[i].utc_s && clock_known()) {
            time_t t = s_recent[i].utc_s + (int16_t)settings_get(SET_UTC_OFFSET_MIN) * 60;
            struct tm tm;
            gmtime_r(&t, &tm);
            snprintf(when, sizeof when, " - %d:%02d", tm.tm_hour, tm.tm_min);
        }
        snprintf(subs[n], 32, "%s%s", s_recent[i].state == 3 ? "Failed" : "Reply", when);
        reply_idx[n++] = i;
    }
    return n;
}

static int s_row_reply[4];
static int s_nrows;

static float f_recent(int k)
{
    static char titles[4][48], subs[4][32];
    if (k == 0) {
        s_mode = M_RECENT;
        item_hide(IT_STATUS, -50);
        item_hide(IT_CHIP, -50);
        items_hide_except(KEEP(IT_STATUS) | KEEP(IT_CHIP));
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_NEUTRAL, M_MOOD);
        eyes_auto(true, false);
        eyes_look(0, 24, M_MOOD);
        s_nrows = recent_rows(titles, subs, s_row_reply);
        return 0.07f;
    }
    if (s_nrows == 0) {
        item_attach(IT_CAP, build_caption("Nothing yet", 236, F24, COL_DIM), 60);
        return FLOW_END;
    }
    if (k <= s_nrows) {
        int i = k - 1;
        item_attach(IT_ROW0 + i, build_row(i, titles[i], subs[i], i < s_nrows - 1), 60);
        return 0.05f;
    }
    return FLOW_END;
}

static float f_detail(int k)
{
    switch (k) {
    case 0:
        s_mode = M_DETAIL;
        items_hide_except(0);
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_CONTENT, M_MOOD);
        eyes_look(0, 20, M_MOOD);
        return 0.07f;
    default:
        build_lines(IT_LINE0, BODY_LINES, s_recent[s_detail].text, 178, 340, 240, F28, COL_TEXT, 0.05f);
        return FLOW_END;
    }
}

static void update_bright_value(void)
{
    lv_obj_t *v = item_get(IT_VALUE);
    if (v) {
        char t[8];
        snprintf(t, sizeof t, "%d%%", s_bright);
        set_text(lv_obj_get_child(v, 0), t);
    }
    eyes_rim_progress(s_bright / 100.0f);
    // The brighter the screen, the more the bot squints.
    eyes_param_to(EP_LIDT, 0.05f + 0.3f * (s_bright / 100.0f), M_MOOD);
}

static float f_bright(int k)
{
    switch (k) {
    case 0:
        s_mode = M_BRIGHT;
        items_hide_except(0);
        s_bright = settings_get(SET_BRIGHTNESS);
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_CONTENT, M_MOOD);
        eyes_look(0, 20, M_MOOD);
        eyes_rim_track(true);
        eyes_rim(RIM_PROGRESS, lv_color_hex(COL_TEXT));
        return 0.07f;
    default: {
        lv_obj_t *b = box(s_scr);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        label(b, F48, COL_TEXT, "");
        label(b, F20, COL_DIM, "Brightness");
        label(b, F16, COL_DIM, "Drag up or down");
        place(b, 262);
        item_attach(IT_VALUE, b, 14);
        update_bright_value();
        return FLOW_END;
    }
    }
}

static const uint16_t IDLE_CHOICES[] = {15, 30, 60, 120, 300};

static float f_settings(int k)
{
    static char sub0[24], sub1[32], sub2[40];
    if (k == 0) {
        s_mode = M_SETTINGS;
        items_hide_except(0);
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_NEUTRAL, M_MOOD);
        eyes_look(0, 24, M_MOOD);
        snprintf(sub0, sizeof sub0, "%u s - tap to change", settings_get(SET_IDLE_S));
        snprintf(sub1, sizeof sub1, "%s", s_in.ble.connected ? "Connected" : "Not connected");
        snprintf(sub2, sizeof sub2, "Version %.24s", esp_app_get_description()->version);
        return 0.07f;
    }
    if (k <= 3) {
        const char *t[] = {"Screen timeout", "Phone", "About"};
        const char *s[] = {sub0, sub1, sub2};
        item_attach(IT_ROW0 + k - 1, build_row(k - 1, t[k - 1], s[k - 1], k < 3), 60);
        return 0.05f;
    }
    return FLOW_END;
}

static float f_phone(int k)
{
    switch (k) {
    case 0:
        s_mode = M_PHONE;
        items_hide_except(0);
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_WORRIED, M_MOOD);
        eyes_look(0, 20, M_MOOD);
        s_hold_down = false;
        return 0.07f;
    default:
        item_attach(IT_BODY, build_caption(s_in.ble.connected ? "Phone connected" : "Phone not connected", 200, F28,
                                           COL_TEXT), 14);
        item_attach(IT_SUB, build_body("Hold the screen for 2 s to forget this phone.", 252, 320, 90, F20, COL_DIM), 14);
        return FLOW_END;
    }
}

static float f_forgotten(int k)
{
    switch (k) {
    case 0:
        s_mode = M_PAIRFAIL;
        items_hide_except(0);
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_set_expr(EXPR_SAD, M_DROOP);
        item_attach(IT_BODY, build_caption("Phone forgotten", 226, F28, COL_TEXT), 14);
        return 2.0f;
    default:
        go_face();
        return FLOW_END;
    }
}

static float f_about(int k)
{
    switch (k) {
    case 0:
        s_mode = M_ABOUT;
        items_hide_except(0);
        eyes_layout(LAYOUT_HEADER, M_LAYOUT);
        eyes_set_expr(EXPR_CURIOUS, M_MOOD);
        eyes_look(0, 22, M_MOOD);
        return 0.07f;
    default: {
        size_t total = 0, used = 0;
        clipstore_usage(&total, &used);
        char t[220];
        char batt[40] = "No battery";
        if (s_in.pmic_ok && s_in.pmic.battery_present && s_in.pmic.battery_pct >= 0) {
            snprintf(batt, sizeof batt, "Battery %d%%, %u mV%s", s_in.pmic.battery_pct, s_in.pmic.vbat_mv,
                     s_in.pmic.charging ? ", charging" : "");
        }
        snprintf(t, sizeof t, "coinai %.24s\n%s\n%d clips, %u of %u KB used\nAwake %lu s",
                 esp_app_get_description()->version, batt, clipstore_count(), (unsigned)(used / 1024),
                 (unsigned)(total / 1024), (unsigned long)(esp_timer_get_time() / 1000000));
        lv_obj_t *l = label(s_scr, F20, COL_TEXT, t);
        lv_obj_set_style_text_line_space(l, 8, 0);
        place(l, 262);
        item_attach(IT_BODY, l, 14);
        return FLOW_END;
    }
    }
}

static float f_powerconf(int k)
{
    switch (k) {
    case 0:
        s_mode = M_POWERCONF;
        items_hide_except(0);
        s_hold_down = false;
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        eyes_set_expr(EXPR_WORRIED, M_MOOD);
        eyes_look(0, 0, M_MOOD);
        eyes_auto(true, false);
        return 0.1f;
    default:
        item_attach(IT_CAP, build_caption("Hold the screen to power off", 380, F20, COL_TEXT), 14);
        return FLOW_END;
    }
}

// BOOT+PWR: the rim counts 3 s while the eyes squeeze shut; the goodbye takes over at the end.
static float f_powerhold(int k)
{
    switch (k) {
    case 0:
        s_mode = M_POWERHOLD;
        items_hide_except(0);
        eyes_zzz(false);
        eyes_auto(true, false);
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        eyes_look(0, 0, M_MOOD);
        eyes_set_expr(EXPR_WORRIED, M_MOOD);
        eyes_rim_track(true);
        eyes_rim(RIM_PROGRESS, lv_color_hex(COL_TEXT));
        eyes_rim_progress(0);
        s_t0 = s_now;
        item_attach(IT_CAP, build_caption("Keep holding to power off", 380, F20, COL_TEXT), 14);
        return FLOW_HOLD;
    default:
        return FLOW_HOLD;
    }
}

static void squeeze_progress(float el, float total)
{
    float u = fminf(fmaxf((el - 0.6f) / (total - 0.6f), 0), 1);
    eyes_rim_progress(el / total);
    eyes_param_to(EP_H, 122 + (26 - 122) * u * u, M_MOOD);
    eyes_param_to(EP_W, 92 + (110 - 92) * u, M_MOOD);
    eyes_param_to(EP_LIDT, 0.24f * (1 - u), M_MOOD);
}

static void bye_done(void)
{
    s_mode = M_OFF;
    s_bye_done = true;
    eyes_auto(false, false);
}

static float f_collapse(int k)
{
    switch (k) {
    case 0:
        eyes_set_expr(EXPR_CLOSED, M_SACCADE);
        return 0.15f;
    case 1:
        eyes_spread_to(0, M_LAYOUT);
        eyes_param_to(EP_W, 70, M_LAYOUT);
        return 0.28f;
    case 2:
        eyes_set_expr(EXPR_DOT, M_SACCADE);
        eyes_spread_to(0, M_LAYOUT);
        return 0.17f;
    case 3:
        eyes_visible_to(0, (motion_t){22, 1});
        return 0.26f;
    default:
        bye_done();
        return FLOW_END;
    }
}

static float f_bye(int k)
{
    switch (k) {
    case 0:
        s_mode = M_BYE;
        items_hide_except(0);
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_set_level(0);
        eyes_auto(false, false);
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        if (s_bye_kind == UI_BYE_POWEROFF) {
            eyes_set_expr(EXPR_JOY, M_POP);
            eyes_kick_y(-220);
            item_attach(IT_CAP, build_caption("Bye!", 360, F32, COL_TEXT), 14);
            return 0.65f;
        }
        if (s_bye_kind == UI_BYE_BATTERY) {
            eyes_set_expr(EXPR_TIRED, M_DROOP);
            item_attach(IT_CAP, build_caption("Battery empty", 366, F28, COL_TEXT), 14);
            return 1.3f;
        }
        eyes_zzz(false);
        if (s_drowsy_shown) {
            return 0.01f;  // already closed
        }
        eyes_set_expr(EXPR_CLOSED, M_MOOD);
        return 0.16f;
    case 1:
        if (s_bye_kind == UI_BYE_POWEROFF || s_bye_kind == UI_BYE_BATTERY) {
            item_hide(IT_CAP, 0);
            FLOW(f_collapse);
            return FLOW_END;
        }
        eyes_visible_to(0, (motion_t){14, 1});
        return 0.3f;
    default:
        bye_done();
        return FLOW_END;
    }
}

static float f_drowsy(int k)
{
    switch (k) {
    case 0:
        s_mode = M_DROWSY;
        s_drowsy_shown = true;
        items_hide_except(KEEP(IT_STATUS));
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_auto(false, false);
        eyes_layout(LAYOUT_FULL, M_LAYOUT);
        eyes_look(0, 8, M_SLOW);
        eyes_set_expr(EXPR_SLEEPY, M_SLEEPY);
        return 0.9f;
    case 1:
        eyes_blink(1, 2.8f);
        return 1.2f;
    case 2:
        eyes_set_expr(EXPR_CLOSED, (motion_t){9, 0.95f});
        item_hide(IT_STATUS, 8);
        return 0.4f;
    default:
        eyes_zzz(true);
        return FLOW_HOLD;
    }
}

static float f_wake(int k)
{
    switch (k) {
    case 0:
        s_mode = M_BOOT;
        s_drowsy_shown = false;
        items_hide_except(0);
        eyes_reset(LAYOUT_FULL);
        eyes_snap_expr(EXPR_CLOSED);
        eyes_rim(RIM_OFF, lv_color_black());
        eyes_rim_track(false);
        eyes_zzz(false);
        eyes_auto(false, false);
        return 0.05f;
    case 1: eyes_set_expr(EXPR_SURPRISED, M_POP); return 0.17f;
    case 2: eyes_set_expr(EXPR_NEUTRAL, M_MOOD); return 0.1f;
    case 3: eyes_blink(2, 1); return 0.42f;
    case 4: eyes_look(-26, -4, M_SACCADE); return 0.36f;
    default:
        eyes_look(0, 0, M_SACCADE);
        go_face();
        return FLOW_END;
    }
}

// Waking from drowsy: a startle, then the face.
static float f_unsleep(int k)
{
    switch (k) {
    case 0:
        s_drowsy_shown = false;
        eyes_zzz(false);
        eyes_set_expr(EXPR_SURPRISED, M_POP);
        return 0.17f;
    default:
        go_face();
        return FLOW_END;
    }
}

static float f_power_on(int k)
{
    switch (k) {
    case 0:
        s_mode = M_BOOT;
        items_clear();
        eyes_reset(LAYOUT_FULL);
        eyes_snap_expr(EXPR_DOT);
        eyes_spread_snap(0);
        eyes_visible_snap(0);
        eyes_auto(false, false);
        eyes_visible_to(1, (motion_t){22, 1});
        return 0.17f;
    case 1: eyes_set_expr(EXPR_CLOSED, M_POP); return 0.2f;
    case 2: eyes_spread_to(1, M_POP); return 0.24f;
    case 3: eyes_set_expr(EXPR_SURPRISED, M_POP); return 0.17f;
    case 4: eyes_set_expr(EXPR_NEUTRAL, M_MOOD); eyes_blink(2, 1); return 0.45f;
    case 5: eyes_look(-30, -4, M_SACCADE); return 0.36f;
    case 6: eyes_look(28, -4, M_SACCADE); return 0.36f;
    default:
        eyes_look(0, 0, M_SACCADE);
        go_face();
        return FLOW_END;
    }
}

// A PTT wake: the eyes stay shut until the recording is seen (or the press was only a tap).
static float f_wait_rec(int k)
{
    if (k == 0) {
        s_mode = M_BOOT;
        eyes_reset(LAYOUT_LISTEN);
        eyes_snap_expr(EXPR_CLOSED);
        eyes_auto(false, false);
        return 1.0f;
    }
    FLOW(f_wake);
    return FLOW_END;
}

static float f_boop(int k)
{
    switch (k) {
    case 0:
        eyes_ripple(s_tap_x, s_tap_y);
        eyes_quiet(1.2f);
        eyes_auto(true, false);
        eyes_look(fminf(fmaxf((s_tap_x - 233) * 0.25f, -40), 40), fminf(fmaxf((s_tap_y - 200) * 0.2f, -24), 24), M_SACCADE);
        eyes_set_expr(EXPR_SURPRISED, M_POP);
        return 0.11f;
    case 1:
        eyes_set_expr(EXPR_JOY, M_POP);
        eyes_kick_y(-240);
        return 0.65f;
    case 2:
        eyes_set_expr(rest_expr(), M_MOOD);
        eyes_look(0, 0, M_MOOD);
        return 0.4f;
    default:
        eyes_auto(true, true);
        return FLOW_END;
    }
}

static float f_dizzy(int k)
{
    switch (k) {
    case 0:
        eyes_auto(true, false);
        eyes_dizzy();
        eyes_set_expr(EXPR_CONFUSED, M_POP);
        return 1.5f;
    case 1:
        eyes_blink(2, 1);
        eyes_set_expr(rest_expr(), M_MOOD);
        eyes_look(0, 0, M_SACCADE);
        return 0.4f;
    default:
        eyes_auto(true, true);
        return FLOW_END;
    }
}

static float f_glance_boot(int k)
{
    switch (k) {
    case 0:
        eyes_quiet(1);
        eyes_auto(true, false);
        eyes_look(40, 6, M_SACCADE);  // toward the buttons
        eyes_blink(1, 1);
        return 0.55f;
    default:
        eyes_look(0, 0, M_SACCADE);
        eyes_auto(true, true);
        return FLOW_END;
    }
}

static float f_plugin(int k)
{
    switch (k) {
    case 0:
        eyes_auto(true, false);
        eyes_look(0, 36, M_SACCADE);  // a glance down at the port
        eyes_set_expr(EXPR_SURPRISED, M_POP);
        return 0.16f;
    case 1:
        eyes_set_expr(EXPR_CONTENT, M_MOOD);
        eyes_blink_slow(1);
        return 0.8f;
    default:
        eyes_look(0, 0, M_MOOD);
        eyes_set_expr(EXPR_NEUTRAL, M_MOOD);
        eyes_auto(true, true);
        return FLOW_END;
    }
}

static float f_sent_hop(int k)
{
    switch (k) {
    case 0:
        eyes_set_expr(EXPR_JOY, M_POP);
        eyes_kick_y(-240);
        return 0.7f;
    default:
        eyes_set_expr(rest_expr(), M_MOOD);
        return FLOW_END;
    }
}

// ---------- demo: scripted inputs, so flows run without a phone or a finger ----------

typedef enum { DEMO_NONE, DEMO_TALK, DEMO_FAIL, DEMO_AWAY, DEMO_NOSPEECH, DEMO_PAIR, DEMO_POWER, DEMO_SLEEP,
               DEMO_LOWBATT, DEMO_DISCARD } demo_t;
static const char *DEMO_NAMES[] = {"", "talk", "fail", "away", "nospeech", "pair", "power", "sleep", "lowbatt",
                                   "discard"};
static demo_t s_demo;
static float s_demo_t0;
static int s_demo_step;
static inputs_t s_demo_in;

static const char *DEMO_HEARD = "Remind me to call the dentist tomorrow at 9, and add milk to the shopping list.";
static const char *DEMO_REPLY = "Done. Reminder set for 9:00 tomorrow to call the dentist, and milk is on your "
                                "shopping list.";

static float voice(float t)
{
    float syl = 0.5f + 0.5f * sinf(t * 2 * (float)M_PI * 4.2f + 1.7f * sinf(t * 2.3f));
    float word = sinf(t * 2 * (float)M_PI * 0.9f + 0.8f * sinf(t * 0.7f)) > -0.55f ? 1 : 0.06f;
    float v = word * (0.65f + 0.35f * sinf(t * 1.3f + 0.4f)) * (0.35f + 0.65f * syl * syl);
    return fminf(fmaxf(v, 0), 1);
}

static void demo_clip_done(uint32_t voiced)
{
    ui_msg_t m = {.ev = UI_EV_CLIP_DONE, .clip = {.id = 9000, .duration_ms = 2600, .voiced_ms = voiced}};
    xQueueSend(s_q, &m, 0);
}

static void demo_post(ui_ev_t ev)
{
    ui_msg_t m = {.ev = ev};
    xQueueSend(s_q, &m, 0);
}

static void demo_reply(int state, const char *text)
{
    s_demo_in.ble.reply_clip = 9000;
    s_demo_in.ble.reply_state = state;
    snprintf(s_demo_in.ble.reply_text, sizeof s_demo_in.ble.reply_text, "%s", text);
}

// Returns false when the script has ended.
static bool demo_tick(void)
{
    float t = s_now - s_demo_t0;
    inputs_t *d = &s_demo_in;
    if (d->rec) {
        d->level = s_demo == DEMO_NOSPEECH ? 0.02f : voice(t);
    }
#define AT(n, sec) (s_demo_step == (n) && t >= (sec) && (s_demo_step++, true))
    switch (s_demo) {
    case DEMO_TALK:
    case DEMO_FAIL: {
        if (AT(0, 0.0f)) { d->rec = true; d->ble.connected = true; d->ble.pending = 1; }
        if (AT(1, 2.6f)) { d->rec = false; d->ble.transferring = true; d->ble.tx_clip = 9000; d->ble.tx_size = 7800;
                        d->ble.tx_offset = 6900; demo_clip_done(2200); }
        if (AT(2, 2.9f)) { d->ble.tx_offset = 7800; }
        if (AT(3, 3.2f)) { d->ble.transferring = false; d->ble.pending = 0; d->ble.reply_window = true;
                        demo_reply(0, ""); }
        if (AT(4, 3.9f)) { demo_reply(1, DEMO_HEARD); }
        if (AT(5, 6.4f)) { demo_reply(s_demo == DEMO_FAIL ? 3 : 2, s_demo == DEMO_FAIL ?
                        "No internet on the phone. It will try again." : DEMO_REPLY);
                        d->ble.reply_window = false; }
        if (AT(6, 13.0f)) { return false; }
        break;
    }
    case DEMO_AWAY: {
        if (AT(0, 0.0f)) { d->rec = true; d->ble.connected = false; d->ble.pending = 1; }
        if (AT(1, 1.8f)) { d->rec = false; demo_clip_done(1500); }
        if (AT(2, 9.0f)) { d->ble.connected = true; d->ble.transferring = true; }
        if (AT(3, 10.2f)) { d->ble.transferring = false; d->ble.pending = 0; }
        if (AT(4, 13.0f)) { return false; }
        break;
    }
    case DEMO_NOSPEECH: {
        if (AT(0, 0.0f)) { d->rec = true; d->ble.connected = true; d->ble.pending = 1; }
        if (AT(1, 1.4f)) { d->rec = false; demo_clip_done(120); }
        if (AT(2, 5.0f)) { d->ble.pending = 0; return false; }
        break;
    }
    case DEMO_DISCARD: {
        if (AT(0, 0.0f)) { d->rec = true; d->ble.pending = 1; }
        if (AT(1, 1.6f)) { demo_post(UI_EV_DISCARD_ARMED); }
        if (AT(2, 2.6f)) { demo_post(UI_EV_DISCARDED); d->rec = false; d->ble.pending = 0;
                        ui_msg_t m = {.ev = UI_EV_CLIP_DONE, .clip = {.id = 9000, .discarded = true}};
                        xQueueSend(s_q, &m, 0); }
        if (AT(3, 5.0f)) { return false; }
        break;
    }
    case DEMO_PAIR: {
        if (AT(0, 0.0f)) { d->ble.connected = true; d->ble.passkey = 481205; }
        if (AT(1, 4.5f)) { d->ble.passkey = 0; d->ble.authenticated = true; }
        if (AT(2, 7.5f)) { return false; }
        break;
    }
    case DEMO_POWER: {
        if (AT(0, 0.0f)) { demo_post(UI_EV_COMBO_START); }
        if (AT(1, 3.0f)) { s_bye_kind = UI_BYE_POWEROFF; FLOW(f_bye); }
        if (AT(2, 5.2f)) { FLOW(f_power_on); }
        if (AT(3, 8.0f)) { return false; }
        break;
    }
    case DEMO_SLEEP: {
        if (AT(0, 0.0f)) { s_drowsy = true; }
        if (AT(1, 4.0f)) { s_bye_kind = UI_BYE_STANDBY; FLOW(f_bye); }
        if (AT(2, 5.5f)) { s_drowsy = false; FLOW(f_wake); }
        if (AT(3, 8.0f)) { return false; }
        break;
    }
    case DEMO_LOWBATT: {
        if (AT(0, 0.0f)) { d->pmic_ok = true; d->pmic.battery_present = true; d->pmic.battery_pct = 9;
                        d->pmic.charging = false; d->pmic.vbus_good = false; go_face(); }
        if (AT(1, 4.0f)) { d->pmic.charging = true; d->pmic.vbus_good = true; }
        if (AT(2, 7.0f)) { return false; }
        break;
    }
    default:
        return false;
    }
#undef AT
    return true;
}

static void demo_start(demo_t which)
{
    s_demo = which;
    s_demo_t0 = s_now;
    s_demo_step = 0;
    s_demo_in = s_in;
    memset(&s_demo_in.ble, 0, sizeof s_demo_in.ble);
    s_demo_in.ble.reply_state = -1;
    s_demo_in.rec = false;
    s_prev = s_demo_in;  // no edges from switching sources
    lv_display_trigger_activity(NULL);  // a demo counts as use: the screen comes on and stays on
    if (s_mode != M_FACE) {
        go_face();
    }
}

// ---------- inputs ----------

static void gather(void)
{
    if (s_demo) {
        if (!demo_tick()) {
            s_demo = DEMO_NONE;
            s_prev_valid = false;  // back to real inputs without reacting to the switch
            s_last_ble_us = 0;
        } else {
            s_in = s_demo_in;
            return;
        }
    }
    s_in.rec = audio_is_recording();
    s_in.level = fminf(fmaxf((audio_level() - 6) / 40.0f, 0), 1);
    int64_t now = esp_timer_get_time();
    if (!s_last_ble_us || now - s_last_ble_us > 50000) {
        ble_get_status(&s_in.ble);
        s_last_ble_us = now;
    }
    if (s_status_valid) {
        s_in.pmic_ok = s_status.pmic_ok;
        s_in.pmic = s_status.pmic;
    }
}

// ---------- reactions to inputs and events ----------

static bool busy_mode(void)
{
    return s_mode == M_LISTEN || s_mode == M_BYE || s_mode == M_POWERHOLD || s_mode == M_OFF || s_mode == M_BOOT;
}

static void on_clip_done(const audio_clip_result_t *c)
{
    if (s_mode != M_LISTEN) {
        return;
    }
    if (c->discarded) {
        if (s_discard_flag) {
            FLOW(f_discarded);
        } else {
            go_face();  // a tap: the goodbye follows
        }
        return;
    }
    s_clip = c->id;
    if (c->voiced_ms != CLIP_VOICED_UNKNOWN && c->voiced_ms < NO_SPEECH_MS) {
        FLOW(f_nospeech);
    } else if (s_in.ble.connected) {
        FLOW(f_send);
    } else {
        FLOW(f_away);
    }
}

static void handle_event(const ui_msg_t *m)
{
    switch (m->ev) {
    case UI_EV_CLIP_DONE:
        on_clip_done(&m->clip);
        break;
    case UI_EV_DISCARDED:
        s_discard_flag = true;
        break;
    case UI_EV_DISCARD_ARMED:
        if (s_mode == M_LISTEN) {
            lv_obj_t *h = item_get(IT_HINT);
            if (h) {
                set_text(h, "Hold to discard");
            }
            eyes_rim_track(true);
            eyes_rim(RIM_PROGRESS, lv_color_hex(COL_REC));
            s_discard_t0 = s_now;
        }
        break;
    case UI_EV_DISCARD_CANCEL:
        if (s_mode == M_LISTEN) {
            lv_obj_t *h = item_get(IT_HINT);
            if (h) {
                set_text(h, "Release to send");
            }
            eyes_rim(RIM_OFF, lv_color_black());
            eyes_rim_track(false);
            s_discard_t0 = -1;
        }
        break;
    case UI_EV_COMBO_START:
        if (s_mode != M_BYE) {
            FLOW(f_powerhold);
        }
        break;
    case UI_EV_COMBO_CANCEL:
        if (s_mode == M_POWERHOLD || s_flow == f_powerhold) {
            eyes_blink(1, 1);
            go_face();
        }
        break;
    case UI_EV_BOOT_DOWN:
        if (is_content(s_mode) || s_mode == M_THINK || s_mode == M_SEND || s_mode == M_AWAY) {
            go_face();
        } else if (s_mode == M_FACE && !s_flow) {
            FLOW(f_glance_boot);
        }
        break;
    case UI_EV_BOOT_UP:
        break;
    }
}

// Compares the inputs with the last tick's and starts at most one flow; edges that a busy screen
// cannot act on stay pending until it can.
static void react(void)
{
    const inputs_t *in = &s_in, *pv = &s_prev;
    if (!s_drowsy) {
        s_drowsy_done = false;  // a new idle period may get sleepy again
    }
    if (!s_prev_valid) {
        s_prev = s_in;
        s_prev_valid = true;
        return;
    }
    uint32_t gen = s_flow_gen;
#define STARTED() (gen != s_flow_gen)

    // A goodbye from the control loop outranks everything but a new recording.
    if (s_bye_req >= 0) {
        s_bye_kind = (ui_bye_t)s_bye_req;
        s_bye_req = -1;
        FLOW(f_bye);
    }

    if (in->rec && !pv->rec) {
        if (s_mode == M_BYE) {
            s_bye_done = true;  // the control loop sees the recording and stays awake
        }
        s_listen_closed = s_mode == M_OFF || s_mode == M_BOOT || s_mode == M_BYE || s_mode == M_DROWSY;
        s_drowsy_shown = false;
        FLOW(f_listen);
        goto out;
    }
    if (s_mode == M_LISTEN) {
        eyes_set_level(in->level);
        lv_obj_t *t = item_get(IT_TIMER);
        if (t) {
            float el = fmaxf(0, s_now - s_rec_t0);
            char buf[12];
            snprintf(buf, sizeof buf, "%d:%02d", (int)(el / 60), (int)el % 60);
            set_text(lv_obj_get_child(t, 1), buf);
            lv_obj_set_style_bg_opa(lv_obj_get_child(t, 0), (lv_opa_t)(140 + 115 * cosf(el * (float)M_PI * 1.25f)), 0);
        }
        if (s_discard_t0 >= 0) {
            eyes_rim_progress((s_now - s_discard_t0) / 1.0f);  // app_main discards after 1 s
        }
        if (!in->rec && pv->rec) {
            s_rec_end_t = s_now;
        }
        if (!in->rec && s_rec_end_t >= 0 && s_now - s_rec_end_t > 0.8f) {
            go_face();  // no clip result arrived
        }
    }

    if (busy_mode() || STARTED()) {
        s_prev.rec = in->rec;
        return;
    }

    // Pairing.
    if (in->ble.passkey && s_mode != M_PAIR) {
        FLOW(f_pair);
    } else if (s_mode == M_PAIR) {
        if (in->ble.passkey && in->ble.passkey != pv->ble.passkey) {
            FLOW(f_pair);
        } else if (!in->ble.passkey) {
            if (in->ble.authenticated) {
                FLOW(f_paired);
            } else {
                FLOW(f_pairfail);
            }
        }
        goto out;
    }

    // Replies from the phone (PROTOCOL.md REPLY).
    bool reply_changed = in->ble.reply_state != pv->ble.reply_state || in->ble.reply_clip != pv->ble.reply_clip ||
                         strcmp(in->ble.reply_text, pv->ble.reply_text) != 0;
    if (reply_changed && in->ble.reply_state >= 0) {
        memcpy(s_text, in->ble.reply_text, sizeof s_text - 1);
        s_text[sizeof s_text - 1] = 0;
        if (in->ble.reply_state >= 2 && !s_demo) {
            recent_push(in->ble.reply_clip, in->ble.reply_state, in->ble.reply_text);
        }
        if (in->ble.reply_state == 2) {
            FLOW(f_reply);
        } else if (in->ble.reply_state == 3) {
            FLOW(f_fail);
        } else if (s_mode != M_THINK) {
            FLOW(f_think);
        } else if (in->ble.reply_state == 1 && s_text[0]) {
            show_heard(s_text);
        }
        goto out;
    }

    switch (s_mode) {
    case M_SEND: {
        bool done = in->ble.pending == 0 || (pv->ble.transferring && !in->ble.transferring && in->ble.pending == 0);
        if (in->ble.transferring && in->ble.tx_clip == s_clip && in->ble.tx_size) {
            s_send_actual = fmaxf(s_send_actual, (float)in->ble.tx_offset / in->ble.tx_size);
        } else if (!done) {
            s_send_actual = fmaxf(s_send_actual, 0.9f);  // the tail is still streaming
        }
        if (done) {
            s_send_actual = 1;
        }
        float u = fminf((s_now - s_t0) / 0.5f, 1);
        float ease = u < 0.5f ? 2 * u * u : 1 - (2 - 2 * u) * (2 - 2 * u) / 2;
        eyes_rim_progress(fminf(ease, s_send_actual));
        if (done && u >= 1) {
            if (in->ble.reply_window) {
                s_text[0] = 0;
                FLOW(f_think);
            } else {
                FLOW(f_sent);
            }
        } else if (!in->ble.connected && s_now - s_t0 > 2) {
            FLOW(f_saved);
        } else if (s_now - s_t0 > 15) {
            FLOW(f_saved);
        }
        break;
    }
    case M_AWAY:
        if (in->ble.connected && in->ble.transferring) {
            FLOW(f_send);
        }
        break;
    case M_THINK:
        if (!in->ble.reply_window && pv->ble.reply_window && in->ble.reply_state < 2) {
            go_face();  // the reply window closed without an answer: it is on the phone
        }
        break;
    case M_POWERCONF:
        if (s_hold_down) {
            float el = s_now - s_hold_t0;
            squeeze_progress(el, 1.5f);
            if (el >= 1.5f) {
                s_hold_down = false;
                if (s_hooks.power_off) {
                    s_hooks.power_off();
                }
            }
        }
        break;
    case M_PHONE:
        if (s_hold_down && s_now - s_hold_t0 >= 2.0f) {
            s_hold_down = false;
            ble_forget_bonds();
            FLOW(f_forgotten);
        } else if (s_hold_down) {
            eyes_rim_progress((s_now - s_hold_t0) / 2.0f);
        }
        break;
    default:
        break;
    }

    // The face: its one chip, the battery mood, plugging in.
    if (STARTED()) {
        goto out;
    }
    if (s_mode == M_FACE) {
        static float last_chip;
        if (pv->ble.pending && !in->ble.pending) {
            s_face_sent_until = s_now + 1.6f;
            last_chip = 0;
            FLOW(f_sent_hop);
        }
        if (s_now - last_chip > 0.2f) {
            last_chip = s_now;
            lv_obj_t *st = item_get(IT_STATUS);
            if (st) {
                update_status(st);
            }
            char t[32] = "";
            bool with_dot = true;
            static size_t s_total, s_used;
            static float s_usage_at = -100;
            if (s_now - s_usage_at > 5) {
                s_usage_at = s_now;
                clipstore_usage(&s_total, &s_used);
            }
            if (s_now < s_face_sent_until) {
                snprintf(t, sizeof t, "Sent");
                with_dot = false;
            } else if (s_total && s_used * 10 > s_total * 9) {
                snprintf(t, sizeof t, "Storage almost full");
            } else if (in->ble.pending) {
                snprintf(t, sizeof t, "%s", in->ble.transferring ? "Sending..." : "");
                if (!t[0]) {
                    snprintf(t, sizeof t, "%u waiting", in->ble.pending);
                }
            } else if (low_battery()) {
                snprintf(t, sizeof t, "Battery low");
                with_dot = false;
            }
            if (t[0]) {
                lv_obj_t *c = item_show(IT_CHIP);
                if (!c) {
                    c = item_attach(IT_CHIP, build_chip(410), 14);
                }
                update_chip(c, t, with_dot);
            } else {
                item_hide(IT_CHIP, 10);
            }
        }
        bool low = low_battery(), was_low = pv->pmic_ok && pv->pmic.battery_present && pv->pmic.battery_pct >= 0 &&
                                                pv->pmic.battery_pct <= 15 && !pv->pmic.charging;
        if (in->pmic.charging && !pv->pmic.charging && in->pmic_ok && pv->pmic_ok) {
            FLOW(f_plugin);
        } else if (low != was_low && !s_flow) {
            eyes_set_expr(rest_expr(), M_MOOD);
            eyes_blink_slow(low ? 1.8f : 1);
        }
        if (STARTED()) {
            goto out;
        }
    }

    // Drowsy: the idle timeout is near.
    if (s_drowsy && !s_drowsy_done && !s_drowsy_shown && (s_mode == M_FACE || is_content(s_mode))) {
        s_drowsy_done = true;
        FLOW(f_drowsy);
    } else if (!s_drowsy && s_drowsy_shown && s_mode == M_DROWSY) {
        FLOW(f_unsleep);
    }
out:
    s_prev = s_in;
#undef STARTED
}

// ---------- touch ----------

static lv_point_t s_press;
static float s_press_t;
static bool s_pressed;
static int s_drag_base;

static void on_tap(int x, int y)
{
    switch (s_mode) {
    case M_FACE:
        if (item_get(IT_CHIP) && abs(y - 410) < 30) {
            FLOW(f_recent);
        } else {
            s_tap_x = x;
            s_tap_y = y;
            FLOW(f_boop);
        }
        break;
    case M_CONTROLS:
        for (int i = 0; i < 4; i++) {
            int dx = x - TILES[i].x, dy = y - TILES[i].y;
            if (dx * dx + dy * dy < 48 * 48) {
                eyes_ripple(TILES[i].x, TILES[i].y);
                if (i == 0) {
                    FLOW(f_bright);
                } else if (i == 1) {
                    if (s_hooks.standby) {
                        s_hooks.standby();
                    }
                } else if (i == 2) {
                    FLOW(f_settings);
                } else {
                    FLOW(f_powerconf);
                }
                return;
            }
        }
        eyes_ripple(x, y);
        break;
    case M_RECENT:
        for (int i = 0; i < s_nrows; i++) {
            if (abs(y - (ROW_Y0 + i * ROW_DY)) < ROW_DY / 2 && s_row_reply[i] >= 0) {
                s_detail = s_row_reply[i];
                FLOW(f_detail);
                return;
            }
        }
        break;
    case M_SETTINGS:
        for (int i = 0; i < 3; i++) {
            if (abs(y - (ROW_Y0 + i * ROW_DY)) < ROW_DY / 2) {
                if (i == 0) {
                    uint16_t cur = settings_get(SET_IDLE_S), next = IDLE_CHOICES[0];
                    for (size_t j = 0; j < sizeof IDLE_CHOICES / sizeof IDLE_CHOICES[0]; j++) {
                        if (IDLE_CHOICES[j] > cur) {
                            next = IDLE_CHOICES[j];
                            break;
                        }
                    }
                    settings_set(SET_IDLE_S, next);
                    lv_obj_t *r = item_get(IT_ROW0);
                    if (r) {
                        char sub[24];
                        snprintf(sub, sizeof sub, "%u s - tap to change", next);
                        set_text(lv_obj_get_child(r, 1), sub);
                    }
                    eyes_ripple(x, y);
                } else if (i == 1) {
                    FLOW(f_phone);
                } else {
                    FLOW(f_about);
                }
                return;
            }
        }
        break;
    default:
        break;
    }
}

static void on_swipe(int dir)  // +1 down, -1 up
{
    switch (s_mode) {
    case M_FACE:
        if (dir > 0) {
            FLOW(f_controls);
        } else {
            FLOW(f_recent);
        }
        break;
    case M_CONTROLS:
        if (dir < 0) {
            for (int i = IT_TILE0; i <= IT_TILE3; i++) {
                item_hide(i, -60);
            }
            go_face();
        }
        break;
    case M_RECENT:
        if (dir > 0) {
            for (int i = IT_ROW0; i <= IT_ROW3; i++) {
                item_hide(i, 60);
            }
            go_face();
        }
        break;
    case M_BRIGHT:
        break;  // dragging sets the value
    default:
        if (is_content(s_mode)) {
            go_face();
        }
        break;
    }
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "touch down at %d,%d", (int)p.x, (int)p.y);
        s_press = p;
        s_press_t = s_now;
        s_pressed = true;
        s_drag_base = s_bright;
        if ((s_mode == M_POWERCONF || s_mode == M_PHONE) && !s_hold_down) {
            s_hold_down = true;
            s_hold_t0 = s_now;
            eyes_rim_track(true);
            eyes_rim(RIM_PROGRESS, lv_color_hex(s_mode == M_PHONE ? COL_REC : COL_TEXT));
            eyes_rim_progress(0);
        }
    } else if (code == LV_EVENT_PRESSING && s_pressed && s_mode == M_BRIGHT) {
        int v = s_drag_base - (int)(p.y - s_press.y) / 3;
        v = v < 1 ? 1 : v > 100 ? 100 : v;
        if (v != s_bright) {
            s_bright = v;
            display_set_brightness(v);
            update_bright_value();
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_pressed) {
        s_pressed = false;
        ESP_LOGI(TAG, "touch up at %d,%d", (int)p.x, (int)p.y);
        if (s_hold_down) {
            s_hold_down = false;
            if (s_mode == M_POWERCONF) {
                eyes_rim(RIM_OFF, lv_color_black());
                eyes_rim_track(false);
                go_face();
            } else {
                eyes_rim(RIM_OFF, lv_color_black());
                eyes_rim_track(false);
            }
            return;
        }
        if (s_mode == M_BRIGHT) {
            settings_set(SET_BRIGHTNESS, (uint16_t)s_bright);
            return;
        }
        if (code == LV_EVENT_PRESS_LOST) {
            return;  // no swipe or tap from a lost press
        }
        int dx = p.x - s_press.x, dy = p.y - s_press.y;
        if (abs(dy) > 60 && abs(dy) > abs(dx)) {
            on_swipe(dy > 0 ? 1 : -1);
        } else if (abs(dx) < 30 && abs(dy) < 30 && s_now - s_press_t < 0.6f) {
            on_tap(p.x, p.y);
        }
    }
}

// ---------- IMU: tilt, shake, face down (5 Hz samples from the status task) ----------

static void imu_sample(const imu_sample_t *s)
{
    // Axes as in the simulator (sim/host/coinhost/imu.py): flat and face up = (0, 0, +1 g); rolling
    // right gives ax < 0. Not yet checked on the real board.
    static int shake_hits, face_down;
    static float shake_at, dizzy_at = -10;
    bool idle_screen = (s_mode == M_FACE || is_content(s_mode)) && !s_in.rec;
    if (s_mode == M_FACE && s->az > 0.3f) {
        eyes_set_tilt(fminf(fmaxf(-s->ax * 1.6f, -1), 1), fminf(fmaxf(-s->ay, -1), 1));
    } else {
        eyes_set_tilt(0, 0);
    }
    float a = sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
    float g = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz);
    if (fabsf(a - 1) > 0.5f || g > 150) {
        shake_hits = s_now - shake_at < 0.7f ? shake_hits + 1 : 1;
        shake_at = s_now;
        if (shake_hits >= 2 && s_mode == M_FACE && s_now - dizzy_at > 3) {
            dizzy_at = s_now;
            shake_hits = 0;
            FLOW(f_dizzy);
        }
    }
    face_down = s->az < -0.7f ? face_down + 1 : 0;
    if (face_down == 8 && idle_screen && s_hooks.standby) {  // 1.6 s face down: screen off
        s_hooks.standby();
    }
}

// ---------- frame-rate measurement (ui perf) ----------

typedef struct {
    uint32_t us, wait_us, px;
    uint8_t mode;
    const char *flow;
} slow_frame_t;

#define SLOW_FRAMES 6
#define LOG_AREAS 24

static int s_log_frames;  // ui perf areas <n> [min ms]: print the flushed areas of the next n slow frames
static uint32_t s_log_min_us;
static lv_area_t s_log_area[LOG_AREAS];
static int s_nlog;
static lv_area_t s_log_inv[LV_INV_BUF_SIZE];  // the frame's dirty areas before LVGL joins them
static int s_ninv;

static struct {
    int64_t since_us, refr_start_us, wait_start_us;
    uint32_t frames, refr_sum_us, refr_max_us, wait_sum_us, frame_wait_us;
    uint64_t px;
    uint32_t frame_px;
    uint32_t ticks, tick_sum_us, tick_max_us;
    uint32_t merged;  // dirty areas merged by inv_cb instead of overflowing LVGL's list
    slow_frame_t slow[SLOW_FRAMES];  // the slowest frames, for finding hitches
} s_perf;

static uint32_t area_size(const lv_area_t *a)
{
    return (uint32_t)lv_area_get_width(a) * (uint32_t)lv_area_get_height(a);
}

// LVGL keeps 32 dirty areas per frame and redraws the whole screen when a 33rd comes (lv_refr.c).
// A busy frame (lines of text fading while the eyes and rim move, two UI ticks per slow refresh)
// can get there: 188 kpx, 29 ms on the coin. Near the limit, a new area is merged into the
// existing one it grows least, and handed back as that area, so lv_inv_area sees it as already
// covered and stores nothing.
static void inv_cb(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_target(e);
    lv_area_t *a = lv_event_get_param(e);
    if (s_log_frames && a && lv_area_get_size(a) > 100000) {
        // A layout or style change dirtying most of the screen; add esp_backtrace_print() here to
        // find which (it found the zero-width label measure fixed in build_body).
        printf("big dirty area [%d,%d %dx%d]\n", (int)a->x1, (int)a->y1, (int)lv_area_get_width(a),
               (int)lv_area_get_height(a));
    }
    if (!disp || !a || disp->inv_p < LV_INV_BUF_SIZE - 1) {
        return;
    }
    int best = -1;
    uint32_t best_cost = UINT32_MAX;
    for (int i = 0; i < disp->inv_p; i++) {
        lv_area_t j;
        lv_area_join(&j, &disp->inv_areas[i], a);
        uint32_t cost = area_size(&j) - area_size(&disp->inv_areas[i]);
        if (cost < best_cost) {
            best_cost = cost;
            best = i;
        }
    }
    if (best >= 0) {
        lv_area_join(&disp->inv_areas[best], &disp->inv_areas[best], a);
        *a = disp->inv_areas[best];
        s_perf.merged++;
    }
}

static void perf_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        lv_display_t *disp = lv_event_get_target(e);
        s_ninv = 0;
        if (s_log_frames && disp) {
            for (int i = 0; i < disp->inv_p && i < LV_INV_BUF_SIZE; i++) {
                s_log_inv[s_ninv++] = disp->inv_areas[i];
            }
        }
        s_perf.refr_start_us = esp_timer_get_time();
        s_perf.frame_px = 0;
        s_perf.frame_wait_us = 0;
    } else if (code == LV_EVENT_FLUSH_WAIT_START) {
        s_perf.wait_start_us = esp_timer_get_time();
    } else if (code == LV_EVENT_FLUSH_WAIT_FINISH) {
        s_perf.frame_wait_us += (uint32_t)(esp_timer_get_time() - s_perf.wait_start_us);
    } else if (code == LV_EVENT_FLUSH_START) {
        const lv_area_t *a = lv_event_get_param(e);
        if (a) {
            s_perf.frame_px += lv_area_get_size(a);
            if (s_log_frames && s_nlog < LOG_AREAS) {
                s_log_area[s_nlog++] = *a;
            }
        }
    } else if (code == LV_EVENT_REFR_READY && s_perf.frame_px) {
        uint32_t us = (uint32_t)(esp_timer_get_time() - s_perf.refr_start_us);
        s_perf.frames++;
        s_perf.refr_sum_us += us;
        if (us > s_perf.refr_max_us) {
            s_perf.refr_max_us = us;
        }
        s_perf.px += s_perf.frame_px;
        s_perf.wait_sum_us += s_perf.frame_wait_us;
        if (s_log_frames && us >= s_log_min_us) {
            s_log_frames--;
            printf("frame %.1f ms, %lu px, mode %s:", us / 1000.0, (unsigned long)s_perf.frame_px, MODE_NAMES[s_mode]);
            for (int i = 0; i < s_nlog; i++) {
                printf(" [%ld,%ld %ldx%ld]", (long)s_log_area[i].x1, (long)s_log_area[i].y1,
                       (long)lv_area_get_width(&s_log_area[i]), (long)lv_area_get_height(&s_log_area[i]));
            }
            printf("%s", "\n  dirty:");
            for (int i = 0; i < s_ninv; i++) {
                printf(" [%ld,%ld %ldx%ld]", (long)s_log_inv[i].x1, (long)s_log_inv[i].y1,
                       (long)lv_area_get_width(&s_log_inv[i]), (long)lv_area_get_height(&s_log_inv[i]));
            }
            printf("%s", "\n");
        }
        s_nlog = 0;
        int min = 0;
        for (int i = 1; i < SLOW_FRAMES; i++) {
            if (s_perf.slow[i].us < s_perf.slow[min].us) {
                min = i;
            }
        }
        if (us > s_perf.slow[min].us) {
            s_perf.slow[min] = (slow_frame_t){us, s_perf.frame_wait_us, s_perf.frame_px, (uint8_t)s_mode,
                                              s_flow ? s_flow_name : "-"};
        }
    }
}

static void perf_reset(void)
{
    memset(&s_perf, 0, sizeof s_perf);
    s_perf.since_us = esp_timer_get_time();
}

// ---------- the tick ----------

static float s_bench_until;  // ui bench: redraw the whole screen every tick until then

static void tick_cb(lv_timer_t *t)
{
    int64_t now = esp_timer_get_time();
    float dt = s_last_us ? (now - s_last_us) / 1e6f : TICK_MS / 1000.0f;
    s_last_us = now;
    if (dt > 0.05f) {
        dt = 0.05f;
    }
    s_now += dt;
    gather();
    ui_msg_t m;
    while (xQueueReceive(s_q, &m, 0) == pdTRUE) {
        handle_event(&m);
    }
    react();
    if (s_mode == M_POWERHOLD) {
        squeeze_progress(s_now - s_t0, 3.0f);
        // app_main cancels when either button comes up. Should that event be missed (it can arrive
        // in the same tick as the start), this screen must not stay up keeping the coin awake.
        bool pwr, boot;
        input_get(&pwr, &boot);
        if (!s_demo && s_flow == f_powerhold && s_bye_req < 0 && (!pwr || !boot) && s_now - s_t0 > 0.1f) {
            eyes_blink(1, 1);
            go_face();
        }
    }
    flow_run();
    eyes_step(dt);
    items_step(dt);
    if (s_now < s_bench_until) {
        lv_obj_invalidate(s_scr);
    }
    uint32_t us = (uint32_t)(esp_timer_get_time() - now);
    s_perf.ticks++;
    s_perf.tick_sum_us += us;
    if (us > s_perf.tick_max_us) {
        s_perf.tick_max_us = us;
    }
}

// ---------- API ----------

void ui_preinit(void)
{
    if (!s_q) {
        s_q = xQueueCreate(8, sizeof(ui_msg_t));
    }
}

void ui_event(ui_ev_t ev, const audio_clip_result_t *clip)
{
    if (!s_q) {
        return;
    }
    ui_msg_t m = {.ev = ev};
    if (clip) {
        m.clip = *clip;
    }
    xQueueSend(s_q, &m, 0);
}

void ui_init(const ui_hooks_t *hooks, int wake_kind)
{
    ui_preinit();
    if (hooks) {
        s_hooks = *hooks;
    }
    recent_load();
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_PRESS_LOST, NULL);  // must not leave a hold armed
    eyes_init(s_scr);
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_display_add_event_cb(disp, perf_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, perf_cb, LV_EVENT_FLUSH_START, NULL);
        lv_display_add_event_cb(disp, perf_cb, LV_EVENT_REFR_READY, NULL);
        lv_display_add_event_cb(disp, perf_cb, LV_EVENT_FLUSH_WAIT_START, NULL);
        lv_display_add_event_cb(disp, perf_cb, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
        lv_display_add_event_cb(disp, inv_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }
    perf_reset();
    memset(&s_prev, 0, sizeof s_prev);  // a recording already running at boot is an edge
    s_prev.ble.reply_state = -1;
    s_prev_valid = true;
    switch ((wake_kind_t)wake_kind) {
    case WAKE_COLD:
        FLOW(f_power_on);
        break;
    case WAKE_PTT:
    case WAKE_COLD_PTT:
    case WAKE_TIMER_TEST:
        FLOW(f_wait_rec);
        break;
    default:
        FLOW(f_wake);
        break;
    }
    lv_timer_create(tick_cb, TICK_MS, NULL);
    ESP_LOGI(TAG, "eyes up (%s)", power_wake_name((wake_kind_t)wake_kind));
}

void ui_update(const ui_state_t *st)
{
    s_status = *st;
    s_status_valid = true;
    if (st->imu_ok && s_mode != M_OFF) {
        imu_sample(&st->imu);
    }
}

void ui_goodbye(ui_bye_t kind)
{
    if (bsp_display_lock(200) != ESP_OK) {
        return;
    }
    if (s_mode == M_OFF) {
        bsp_display_unlock();
        return;
    }
    s_bye_done = false;
    s_bye_req = kind;
    bsp_display_unlock();
    int64_t until = esp_timer_get_time() + 2500 * 1000;
    while (!s_bye_done && esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ui_screen_on(void)
{
    if (bsp_display_lock(200) != ESP_OK) {
        return;
    }
    if (s_mode == M_OFF) {
        FLOW(f_wake);
    }
    bsp_display_unlock();
}

bool ui_keep_awake(void)
{
    return s_demo != DEMO_NONE || s_hold_down || s_mode == M_POWERHOLD;
}

void ui_set_drowsy(bool drowsy)
{
    if (bsp_display_lock(200) != ESP_OK) {
        return;
    }
    s_drowsy = drowsy;
    bsp_display_unlock();
}

int ui_console(int argc, char **argv)
{
    if (argc < 2) {
        printf("ui perf [reset] | ui demo <talk|fail|away|nospeech|discard|pair|power|sleep|lowbatt> | "
               "ui show <face|controls|recent|settings|about|bright> | ui expr <name> | ui state\n");
        return 1;
    }
    if (bsp_display_lock(1000) != ESP_OK) {
        printf("display busy\n");
        return 1;
    }
    int rc = 0;
    if (strcmp(argv[1], "perf") == 0) {
        double s = (esp_timer_get_time() - s_perf.since_us) / 1e6;
        uint32_t n = s_perf.frames;
        printf("ui perf: %.1f s, %lu frames (%.1f fps), refresh mean %.2f ms max %.2f ms, %.1f kpx/frame, busy %.0f%%\n",
               s, (unsigned long)n, n / (s > 0 ? s : 1), n ? s_perf.refr_sum_us / 1000.0 / n : 0,
               s_perf.refr_max_us / 1000.0, n ? s_perf.px / 1000.0 / n : 0, s > 0 ? s_perf.refr_sum_us / 1e4 / s : 0);
        printf("  of which waiting for the panel %.2f ms/frame; UI tick mean %.2f ms max %.2f ms; %lu dirty areas merged\n",
               n ? s_perf.wait_sum_us / 1000.0 / n : 0, s_perf.ticks ? s_perf.tick_sum_us / 1000.0 / s_perf.ticks : 0,
               s_perf.tick_max_us / 1000.0, (unsigned long)s_perf.merged);
        for (int i = 0; i < SLOW_FRAMES; i++) {
            const slow_frame_t *f = &s_perf.slow[i];
            if (f->us) {
                printf("  slow: %.1f ms (panel wait %.1f), %lu px, mode %s, flow %s\n", f->us / 1000.0, f->wait_us / 1000.0,
                       (unsigned long)f->px, MODE_NAMES[f->mode], f->flow);
            }
        }
        if (argc > 2 && strcmp(argv[2], "reset") == 0) {
            perf_reset();
        }
        if (argc > 3 && strcmp(argv[2], "areas") == 0) {
            s_log_frames = atoi(argv[3]);
            s_log_min_us = argc > 4 ? (uint32_t)(atof(argv[4]) * 1000) : 0;
        }
    } else if (strcmp(argv[1], "demo") == 0 && argc > 2) {
        demo_t which = DEMO_NONE;
        for (int i = 1; i < (int)(sizeof DEMO_NAMES / sizeof DEMO_NAMES[0]); i++) {
            if (strcmp(argv[2], DEMO_NAMES[i]) == 0) {
                which = (demo_t)i;
            }
        }
        if (which) {
            demo_start(which);
            printf("demo %s\n", argv[2]);
        } else {
            printf("unknown demo\n");
            rc = 1;
        }
    } else if (strcmp(argv[1], "show") == 0 && argc > 2) {
        const struct {
            const char *name;
            flow_fn fn;
        } shows[] = {{"face", f_face}, {"controls", f_controls}, {"recent", f_recent}, {"settings", f_settings},
                     {"about", f_about}, {"bright", f_bright}};
        rc = 1;
        for (size_t i = 0; i < sizeof shows / sizeof shows[0]; i++) {
            if (strcmp(argv[2], shows[i].name) == 0) {
                flow_start(shows[i].fn, "f_show");
                rc = 0;
            }
        }
        printf(rc ? "unknown screen\n" : "ok\n");
    } else if (strcmp(argv[1], "expr") == 0 && argc > 2) {
        expr_t e = eyes_expr_from_name(argv[2]);
        if (e == EXPR_COUNT) {
            printf("unknown expression\n");
            rc = 1;
        } else {
            eyes_set_expr(e, M_MOOD);
            printf("ok\n");
        }
    } else if (strcmp(argv[1], "bench") == 0 && argc > 2) {
        s_bench_until = s_now + (float)atof(argv[2]);
        perf_reset();
        printf("full-screen redraws for %s s; then: ui perf\n", argv[2]);
    } else if (strcmp(argv[1], "rim") == 0 && argc > 2) {
        bool track = argc > 3 && strcmp(argv[3], "track") == 0;
        eyes_rim_track(track);
        if (strcmp(argv[2], "orbit") == 0) {
            eyes_rim(RIM_ORBIT, lv_color_hex(COL_AMBER));
        } else if (strcmp(argv[2], "off") == 0) {
            eyes_rim(RIM_OFF, lv_color_black());
        } else {
            eyes_rim(RIM_PROGRESS, lv_color_hex(COL_AMBER));
            eyes_rim_progress((float)atof(argv[2]) / 100);
        }
        printf("ok\n");
    } else if (strcmp(argv[1], "state") == 0) {
        printf("ui: mode %s, flow %s, expr %s, eyes y %.0f, demo %s\n", MODE_NAMES[s_mode], s_flow ? s_flow_name : "-",
               eyes_expr_name(eyes_expr()), eyes_y(), DEMO_NAMES[s_demo]);
    } else {
        printf("unknown ui command\n");
        rc = 1;
    }
    bsp_display_unlock();
    return rc;
}
