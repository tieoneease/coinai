#include "kbd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "widgets/textarea/lv_textarea_private.h"  // pwd_tmp: wiped on close

// Geometry for the 466 px round panel. Every key corner stays inside the circle: the rows sit in
// its widest band (y 168-362), the widest row (10 keys) closest to the centre.
#define C 233
#define TITLE_Y 88     // centre
#define FIELD_Y 112    // top
#define FIELD_H 44
#define FIELD_W 320
#define ROW0_Y 168     // top of the first key row
#define ROW_PITCH 50
#define KEY_H 44
#define KEY_W 39
#define GAP 4
#define SIDE_W 52      // shift, backspace, #+= / 123 on row 3
#define MID_W 297      // row 3 between its side keys
#define PAGE_W 66      // row 4
#define SPACE_W 172
#define DONE_W 96
#define ROW4_GAP 5
#define HINT_Y 394     // centre
#define BAND_Y0 (ROW0_Y - 14)  // the keys object (touch slop above row 1 and below row 4)
#define BAND_Y1 (ROW0_Y + 3 * ROW_PITCH + KEY_H + 14)
#define BUBBLE_W 58
#define BUBBLE_H 68
#define REPEAT_FIRST_MS 450
#define REPEAT_MS 70
#define DOUBLE_TAP_MS 350
#define MAX_KEYS 40

// Tokens shared with ui.c (its provisional palette).
#define COL_TEXT 0xF4ECDF
#define COL_DIM 0x8E867A
#define COL_AMBER 0xFFB547
#define COL_ERR 0xFF4F45
#define COL_KEY 0x1F1C19
#define COL_SPECIAL 0x2E2A25
#define COL_PRESSED 0x4A443C
#define COL_DONE_PRESSED 0xFFD08A

typedef enum { K_CHAR, K_SHIFT, K_BKSP, K_PAGE, K_SPACE, K_DONE } ktype_t;
typedef enum { PG_LOWER, PG_UPPER, PG_NUM, PG_SYM } page_t;

typedef struct {
    lv_area_t a;
    ktype_t type;
    char ch;         // K_CHAR
    uint8_t row;
    uint8_t to;      // K_PAGE: the page it opens
    const char *label;
} kkey_t;

typedef struct {
    kbd_cfg_t cfg;
    char title[64], hint[64], placeholder[32];
    char error[64];          // shown instead of the hint until the text changes ("" none)
    lv_obj_t *root, *title_l, *field, *eye, *eye_l, *keys, *bubble, *hint_l;
    page_t page;
    bool caps;
    uint32_t shift_tap_ms;
    int pressed;             // key index, -1 none
    lv_timer_t *repeat;
    bool shown;              // password visible
    kkey_t k[MAX_KEYS];
    int nk;
} kbd_t;

static const char *const ROWS[4][3] = {
    [PG_LOWER] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
    [PG_UPPER] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
    [PG_NUM] = {"1234567890", "-/:;()$&@\"", ".,?!'"},
    [PG_SYM] = {"[]{}#%^*+=", "_\\|~<>`", ".,?!'"},
};

static void add_key(kbd_t *k, int x, int w, int row, ktype_t type, char ch, uint8_t to, const char *label)
{
    if (k->nk >= MAX_KEYS) {
        return;
    }
    kkey_t *key = &k->k[k->nk++];
    int y = ROW0_Y + row * ROW_PITCH;
    key->a = (lv_area_t){x, y, x + w - 1, y + KEY_H - 1};
    key->type = type;
    key->ch = ch;
    key->row = (uint8_t)row;
    key->to = to;
    key->label = label;
}

static void char_row(kbd_t *k, const char *chars, int row, int x0, int w, int gap)
{
    for (int i = 0; chars[i]; i++) {
        add_key(k, x0 + i * (w + gap), w, row, K_CHAR, chars[i], 0, NULL);
    }
}

static void layout(kbd_t *k)
{
    k->nk = 0;
    for (int r = 0; r < 2; r++) {
        int n = (int)strlen(ROWS[k->page][r]);
        int width = n * KEY_W + (n - 1) * GAP;
        char_row(k, ROWS[k->page][r], r, C - width / 2, KEY_W, GAP);
    }
    // Row 3: shift (letters) or the other symbol page, the middle keys, backspace.
    int x0 = C - (2 * SIDE_W + 2 * GAP + MID_W) / 2;
    bool letters = k->page == PG_LOWER || k->page == PG_UPPER;
    if (letters) {
        add_key(k, x0, SIDE_W, 2, K_SHIFT, 0, 0, LV_SYMBOL_UP);
    } else {
        add_key(k, x0, SIDE_W, 2, K_PAGE, 0, k->page == PG_NUM ? PG_SYM : PG_NUM, k->page == PG_NUM ? "#+=" : "123");
    }
    const char *mid = ROWS[k->page][2];
    int n = (int)strlen(mid);
    int w = (MID_W - (n - 1) * GAP) / n;
    int mid_x = x0 + SIDE_W + GAP + (MID_W - (n * w + (n - 1) * GAP)) / 2;
    char_row(k, mid, 2, mid_x, w, GAP);
    add_key(k, x0 + SIDE_W + GAP + MID_W + GAP, SIDE_W, 2, K_BKSP, 0, 0, LV_SYMBOL_BACKSPACE);
    // Row 4: page, space, Done.
    int x = C - (PAGE_W + SPACE_W + DONE_W + 2 * ROW4_GAP) / 2;
    add_key(k, x, PAGE_W, 3, K_PAGE, 0, letters ? PG_NUM : PG_LOWER, letters ? "123" : "abc");
    x += PAGE_W + ROW4_GAP;
    add_key(k, x, SPACE_W, 3, K_SPACE, ' ', 0, "space");
    x += SPACE_W + ROW4_GAP;
    add_key(k, x, DONE_W, 3, K_DONE, 0, 0, "Done");
}

static size_t text_len(const kbd_t *k)
{
    return strlen(lv_textarea_get_text(k->field));
}

static bool done_ok(const kbd_t *k)
{
    return text_len(k) >= k->cfg.min_len;
}

// The preview above a pressed character key, kept inside the circle.
static lv_area_t bubble_area(const kkey_t *key)
{
    int cx = (key->a.x1 + key->a.x2) / 2;
    int y2 = key->a.y1 - 6, y1 = y2 - BUBBLE_H + 1;
    int dy = C - y1;
    int half = (int)sqrtf((float)(C * C - dy * dy)) - 6;  // the chord at the bubble's top edge
    int x1 = cx - BUBBLE_W / 2;
    if (x1 < C - half) {
        x1 = C - half;
    }
    if (x1 + BUBBLE_W > C + half) {
        x1 = C + half - BUBBLE_W;
    }
    return (lv_area_t){x1, y1, x1 + BUBBLE_W - 1, y2};
}

static void inval_key(kbd_t *k, int i)
{
    if (i < 0 || i >= k->nk) {
        return;
    }
    lv_area_t a = k->k[i].a;
    lv_area_increase(&a, 2, 2);
    lv_obj_invalidate_area(k->keys, &a);
}

// The preview is its own object on top (it overlaps the field above row 1).
static void update_bubble(kbd_t *k)
{
    if (k->pressed < 0 || k->k[k->pressed].type != K_CHAR) {
        lv_obj_add_flag(k->bubble, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const kkey_t *key = &k->k[k->pressed];
    lv_area_t b = bubble_area(key);
    char ch[2] = {key->ch, 0};
    lv_label_set_text(k->bubble, ch);
    lv_obj_set_pos(k->bubble, b.x1, b.y1);
    lv_obj_remove_flag(k->bubble, LV_OBJ_FLAG_HIDDEN);
}

static void inval_type(kbd_t *k, ktype_t type)
{
    for (int i = 0; i < k->nk; i++) {
        if (k->k[i].type == type) {
            inval_key(k, i);
        }
    }
}

// One-character strings for the key labels (draw tasks keep the pointer until the frame is drawn).
static const char *char_str(char c)
{
    static char s_chars[95][2];
    if (c < 0x20 || c > 0x7E) {
        return "";
    }
    s_chars[c - 0x20][0] = c;
    return s_chars[c - 0x20];
}

// `text` must outlive the frame (a literal or char_str()).
static void draw_text(lv_layer_t *layer, const lv_area_t *a, const char *text, const lv_font_t *font, uint32_t color,
                      lv_opa_t opa)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.color = lv_color_hex(color);
    d.opa = opa;
    d.align = LV_TEXT_ALIGN_CENTER;
    int32_t lh = lv_font_get_line_height(font);
    lv_area_t t = {a->x1, (a->y1 + a->y2 + 1 - lh) / 2, a->x2, (a->y1 + a->y2 + 1 - lh) / 2 + lh - 1};
    lv_draw_label(layer, &d, &t);
}

static void draw_rect(lv_layer_t *layer, const lv_area_t *a, uint32_t color, int radius, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, a);
}

static void draw_cb(lv_event_t *e)
{
    kbd_t *k = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    // LVGL renders the screen in 24-line bands and calls this once per band; every draw task it is
    // given costs an allocation and a list walk even when clipped away, so keys outside the band
    // are skipped here (a redraw of all keys took about 40% longer without).
    const lv_area_t *clip = &layer->_clip_area;
    bool ok = done_ok(k);
    for (int i = 0; i < k->nk; i++) {
        const kkey_t *key = &k->k[i];
        const lv_opa_t opa = LV_OPA_COVER;
        lv_area_t a = key->a;
        if (a.x2 < clip->x1 || a.x1 > clip->x2 || a.y2 < clip->y1 || a.y1 > clip->y2) {
            continue;
        }
        bool down = i == k->pressed;
        uint32_t bg = COL_KEY, fg = COL_TEXT;
        const lv_font_t *font = &lv_font_montserrat_24;
        const char *label = key->type == K_CHAR ? char_str(key->ch) : key->label;
        switch (key->type) {
        case K_CHAR:
            break;
        case K_SPACE:
            fg = COL_DIM;
            font = &lv_font_montserrat_16;
            break;
        case K_SHIFT:
            bg = k->page == PG_UPPER ? COL_TEXT : COL_SPECIAL;
            fg = k->page == PG_UPPER ? 0x000000 : COL_TEXT;
            font = &lv_font_montserrat_20;
            break;
        case K_BKSP:
        case K_PAGE:
            bg = COL_SPECIAL;
            font = &lv_font_montserrat_20;
            break;
        case K_DONE:
            bg = ok ? COL_AMBER : COL_KEY;
            fg = ok ? 0x000000 : COL_DIM;
            font = &lv_font_montserrat_20;
            break;
        }
        if (down) {
            bg = key->type == K_DONE && ok ? COL_DONE_PRESSED : key->type == K_SHIFT && k->page == PG_UPPER ? COL_DIM
                                                                                                              : COL_PRESSED;
        }
        draw_rect(layer, &a, bg, 10, opa);
        draw_text(layer, &a, label, font, fg, opa);
        if (key->type == K_SHIFT && k->caps) {  // caps lock: a bar under the arrow
            lv_area_t bar = {(a.x1 + a.x2) / 2 - 8, a.y2 - 9, (a.x1 + a.x2) / 2 + 7, a.y2 - 7};
            draw_rect(layer, &bar, fg, 1, opa);
        }
    }
}

// The key under the finger: inside a key, or the nearest one in the row (the gaps belong to a key).
static int key_at(const kbd_t *k, lv_point_t p)
{
    int row = -1;
    for (int r = 0; r < 4; r++) {
        int y1 = ROW0_Y + r * ROW_PITCH - (r == 0 ? 14 : (ROW_PITCH - KEY_H) / 2);
        int y2 = ROW0_Y + r * ROW_PITCH + KEY_H - 1 + (r == 3 ? 14 : (ROW_PITCH - KEY_H) / 2);
        if (p.y >= y1 && p.y <= y2) {
            row = r;
            break;
        }
    }
    if (row < 0) {
        return -1;
    }
    int best = -1, best_d = 18;  // beyond this past the row's ends: no key
    for (int i = 0; i < k->nk; i++) {
        const kkey_t *key = &k->k[i];
        if (key->row != row) {
            continue;
        }
        int d = p.x < key->a.x1 ? key->a.x1 - p.x : p.x > key->a.x2 ? p.x - key->a.x2 : 0;
        if (d < best_d || (d == 0 && best_d == 0)) {
            best = i;
            best_d = d;
            if (d == 0) {
                break;
            }
        }
    }
    return best;
}

static void set_page(kbd_t *k, page_t page)
{
    if (page == k->page) {
        return;
    }
    k->page = page;
    if (page != PG_UPPER) {
        k->caps = false;
    }
    k->pressed = -1;
    update_bubble(k);
    layout(k);
    lv_obj_invalidate(k->keys);
}

static void update_done(kbd_t *k)
{
    inval_type(k, K_DONE);
}

static void key_cb(kbd_t *k, int i)
{
    if (k->cfg.on_key && i >= 0) {
        const kkey_t *key = &k->k[i];
        k->cfg.on_key((key->a.x1 + key->a.x2) / 2, (key->a.y1 + key->a.y2) / 2, k->cfg.ctx);
    }
}

static void clear_error(kbd_t *k)
{
    if (k->error[0]) {
        k->error[0] = 0;
        lv_label_set_text(k->hint_l, k->hint);
        lv_obj_set_style_text_color(k->hint_l, lv_color_hex(COL_DIM), 0);
    }
}

static void insert(kbd_t *k, char c)
{
    clear_error(k);
    bool was_ok = done_ok(k);
    lv_textarea_add_char(k->field, (uint32_t)(uint8_t)c);
    if (done_ok(k) != was_ok) {
        update_done(k);
    }
}

static void backspace(kbd_t *k)
{
    clear_error(k);
    bool was_ok = done_ok(k);
    lv_textarea_delete_char(k->field);
    if (done_ok(k) != was_ok) {
        update_done(k);
    }
}

static void repeat_cb(lv_timer_t *t)
{
    kbd_t *k = lv_timer_get_user_data(t);
    if (k->pressed >= 0 && k->k[k->pressed].type == K_BKSP) {
        backspace(k);
        lv_timer_set_period(t, REPEAT_MS);
    } else {
        lv_timer_pause(t);
    }
}

static void commit(kbd_t *k, int i)
{
    const kkey_t *key = &k->k[i];
    switch (key->type) {
    case K_CHAR:
        insert(k, key->ch);
        if (k->page == PG_UPPER && !k->caps) {
            set_page(k, PG_LOWER);  // one-shot shift
        }
        break;
    case K_SPACE:
        insert(k, ' ');
        break;
    case K_SHIFT: {
        uint32_t now = lv_tick_get();
        if (k->page == PG_LOWER) {
            set_page(k, PG_UPPER);
        } else if (!k->caps && now - k->shift_tap_ms < DOUBLE_TAP_MS) {
            k->caps = true;  // double tap: caps lock
            inval_type(k, K_SHIFT);
        } else {
            set_page(k, PG_LOWER);
        }
        k->shift_tap_ms = now;
        break;
    }
    case K_PAGE:
        set_page(k, (page_t)key->to);
        break;
    case K_DONE:
        kbd_done(k->root);
        break;
    case K_BKSP:
        break;  // on press
    }
}

static void keys_event_cb(lv_event_t *e)
{
    kbd_t *k = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = {0, 0};
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    if (code == LV_EVENT_PRESSED) {
        int i = key_at(k, p);
        k->pressed = i;
        inval_key(k, i);
        update_bubble(k);
        key_cb(k, i);
        if (i >= 0 && k->k[i].type == K_BKSP) {
            backspace(k);
            lv_timer_set_period(k->repeat, REPEAT_FIRST_MS);
            lv_timer_reset(k->repeat);
            lv_timer_resume(k->repeat);
        }
    } else if (code == LV_EVENT_PRESSING) {
        int i = key_at(k, p);
        if (i != k->pressed) {
            // Sliding: the key under the finger at release counts. Backspace deletes only on a
            // press (repeat_cb stops once the finger leaves it).
            inval_key(k, k->pressed);
            k->pressed = i;
            inval_key(k, i);
            update_bubble(k);
            key_cb(k, i);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int i = k->pressed;
        k->pressed = -1;
        lv_timer_pause(k->repeat);
        inval_key(k, i);
        update_bubble(k);
        if (code == LV_EVENT_RELEASED && i >= 0 && k->k[i].type != K_BKSP) {
            commit(k, i);
        }
    }
}

static void set_shown(kbd_t *k, bool shown)
{
    if (k->cfg.kind != KBD_PASSWORD || shown == k->shown) {
        return;
    }
    k->shown = shown;
    lv_textarea_set_password_mode(k->field, !shown);
    lv_label_set_text(k->eye_l, shown ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(k->eye_l, lv_color_hex(shown ? COL_TEXT : COL_DIM), 0);
    if (k->cfg.on_reveal) {
        k->cfg.on_reveal(shown, k->cfg.ctx);
    }
}

static void eye_cb(lv_event_t *e)
{
    kbd_t *k = lv_event_get_user_data(e);
    set_shown(k, !k->shown);
}

static void opa_cb(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

static void fade_in(lv_obj_t *o, uint32_t delay)
{
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Best effort: the text's current buffers. LVGL reallocates them as the text grows and frees the
// old ones unwiped, so pieces of a password can stay in free heap until reused.
static void wipe(kbd_t *k)
{
    lv_textarea_t *ta = (lv_textarea_t *)k->field;
    if (ta->pwd_tmp) {
        memset(ta->pwd_tmp, 0, strlen(ta->pwd_tmp));
    }
    char *shown = lv_label_get_text(ta->label);
    if (shown) {
        memset(shown, 0, strlen(shown));
    }
}

static void delete_cb(lv_event_t *e)
{
    kbd_t *k = lv_event_get_user_data(e);
    lv_anim_delete(k->title_l, NULL);
    lv_anim_delete(k->field, NULL);
    lv_anim_delete(k->hint_l, NULL);
    if (k->eye) {
        lv_anim_delete(k->eye, NULL);
    }
    wipe(k);
    if (k->repeat) {
        lv_timer_delete(k->repeat);
    }
    memset(k, 0, sizeof *k);
    free(k);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s, int y_center, int width)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, width, lv_font_get_line_height(font));
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, C - width / 2, y_center - lv_font_get_line_height(font) / 2);
    return l;
}

lv_obj_t *kbd_create(lv_obj_t *parent, const kbd_cfg_t *cfg)
{
    kbd_t *k = heap_caps_calloc(1, sizeof *k, MALLOC_CAP_SPIRAM);  // internal RAM is the scarce kind
    if (!k) {
        k = calloc(1, sizeof *k);
    }
    if (!k) {
        return NULL;
    }
    k->cfg = *cfg;
    snprintf(k->title, sizeof k->title, "%s", cfg->title ? cfg->title : "");
    snprintf(k->hint, sizeof k->hint, "%s", cfg->hint ? cfg->hint : "");
    snprintf(k->placeholder, sizeof k->placeholder, "%s", cfg->placeholder ? cfg->placeholder : "");
    k->cfg.title = k->title;
    k->cfg.hint = k->hint;
    k->cfg.placeholder = k->placeholder;
    k->cfg.initial = NULL;
    k->pressed = -1;

    k->root = plain(parent);
    lv_obj_set_size(k->root, 2 * C, 2 * C);
    lv_obj_set_pos(k->root, 0, 0);
    lv_obj_set_user_data(k->root, k);
    lv_obj_add_event_cb(k->root, delete_cb, LV_EVENT_DELETE, k);

    k->title_l = text(k->root, &lv_font_montserrat_16, COL_DIM, k->title, TITLE_Y, 300);

    bool pw = cfg->kind == KBD_PASSWORD;
    k->field = lv_textarea_create(k->root);
    lv_obj_t *f = k->field;
    lv_obj_remove_style_all(f);
    lv_obj_set_size(f, FIELD_W, FIELD_H);
    lv_obj_set_pos(f, C - FIELD_W / 2, FIELD_Y);
    lv_obj_set_scrollbar_mode(f, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(f, lv_color_hex(COL_KEY), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f, FIELD_H / 2, 0);
    lv_obj_set_style_text_font(f, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(f, lv_color_hex(COL_TEXT), 0);
    int pad_v = (FIELD_H - lv_font_get_line_height(&lv_font_montserrat_24)) / 2;
    lv_obj_set_style_pad_top(f, pad_v, 0);
    lv_obj_set_style_pad_bottom(f, pad_v, 0);
    lv_obj_set_style_pad_left(f, 20, 0);
    lv_obj_set_style_pad_right(f, pw ? 48 : 20, 0);
    lv_obj_set_style_border_color(f, lv_color_hex(COL_AMBER), LV_PART_CURSOR);
    lv_obj_set_style_border_width(f, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(f, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_pad_left(f, -1, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(f, 600, LV_PART_CURSOR);
    lv_obj_set_style_text_color(f, lv_color_hex(COL_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_one_line(f, true);
    lv_textarea_set_max_length(f, cfg->max_len ? cfg->max_len : 64);
    lv_textarea_set_placeholder_text(f, k->placeholder);
    lv_textarea_set_password_show_time(f, 900);
    lv_textarea_set_password_mode(f, pw);
    if (cfg->initial) {
        lv_textarea_set_text(f, cfg->initial);
    }

    if (pw) {
        k->eye = lv_obj_create(k->root);
        lv_obj_remove_style_all(k->eye);
        lv_obj_remove_flag(k->eye, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(k->eye, 44, FIELD_H);
        lv_obj_set_pos(k->eye, C + FIELD_W / 2 - 48, FIELD_Y);
        lv_obj_set_ext_click_area(k->eye, 6);
        lv_obj_add_event_cb(k->eye, eye_cb, LV_EVENT_CLICKED, k);
        k->eye_l = lv_label_create(k->eye);
        lv_obj_set_style_text_font(k->eye_l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(k->eye_l, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(k->eye_l, LV_SYMBOL_EYE_CLOSE);
        lv_obj_center(k->eye_l);
    }

    k->keys = lv_obj_create(k->root);
    lv_obj_t *kb = k->keys;
    lv_obj_remove_style_all(kb);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_PRESS_LOCK);  // a finger sliding off the band still owns the press
    lv_obj_set_size(kb, 2 * C, BAND_Y1 - BAND_Y0);
    lv_obj_set_pos(kb, 0, BAND_Y0);
    lv_obj_add_event_cb(kb, draw_cb, LV_EVENT_DRAW_MAIN_END, k);
    lv_obj_add_event_cb(kb, keys_event_cb, LV_EVENT_PRESSED, k);
    lv_obj_add_event_cb(kb, keys_event_cb, LV_EVENT_PRESSING, k);
    lv_obj_add_event_cb(kb, keys_event_cb, LV_EVENT_RELEASED, k);
    lv_obj_add_event_cb(kb, keys_event_cb, LV_EVENT_PRESS_LOST, k);

    k->bubble = lv_label_create(k->root);
    lv_obj_t *b = k->bubble;
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, BUBBLE_W, BUBBLE_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PRESSED), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(b, (BUBBLE_H - lv_font_get_line_height(&lv_font_montserrat_48)) / 2, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);

    k->hint_l = text(k->root, &lv_font_montserrat_16, COL_DIM, k->hint, HINT_Y, 280);

    k->page = PG_LOWER;
    layout(k);
    k->repeat = lv_timer_create(repeat_cb, REPEAT_FIRST_MS, k);
    lv_timer_pause(k->repeat);
    fade_in(k->title_l, 0);
    fade_in(k->field, 40);
    if (k->eye) {
        fade_in(k->eye, 40);
    }
    fade_in(k->hint_l, 220);
    return k->root;
}

static kbd_t *get(lv_obj_t *kbd)
{
    return kbd ? lv_obj_get_user_data(kbd) : NULL;
}

const char *kbd_text(lv_obj_t *kbd)
{
    kbd_t *k = get(kbd);
    return k ? lv_textarea_get_text(k->field) : "";
}

void kbd_set_hint(lv_obj_t *kbd, const char *hint, bool error)
{
    kbd_t *k = get(kbd);
    if (!k) {
        return;
    }
    if (error) {
        snprintf(k->error, sizeof k->error, "%s", hint ? hint : "");
        lv_label_set_text(k->hint_l, k->error);
    } else {
        k->error[0] = 0;
        snprintf(k->hint, sizeof k->hint, "%s", hint ? hint : "");
        lv_label_set_text(k->hint_l, k->hint);
    }
    lv_obj_set_style_text_color(k->hint_l, lv_color_hex(error ? COL_ERR : COL_DIM), 0);
}

void kbd_type(lv_obj_t *kbd, const char *s)
{
    kbd_t *k = get(kbd);
    if (!k || !s) {
        return;
    }
    char last = 0;
    for (; *s; s++) {
        if (*s >= 0x20 && *s < 0x7F) {
            insert(k, *s);
            last = *s;
        }
    }
    // The eyes look at the last key, where the current page has it.
    for (int i = 0; last && i < k->nk; i++) {
        if (k->k[i].type == K_CHAR && k->k[i].ch == last) {
            key_cb(k, i);
            break;
        }
    }
}

bool kbd_done(lv_obj_t *kbd)
{
    kbd_t *k = get(kbd);
    if (!k) {
        return false;
    }
    if (!done_ok(k)) {
        char t[48];
        snprintf(t, sizeof t, "At least %u characters", k->cfg.min_len);
        kbd_set_hint(kbd, t, true);
        return false;
    }
    if (k->cfg.on_done) {
        k->cfg.on_done(lv_textarea_get_text(k->field), k->cfg.ctx);
    }
    return true;
}

void kbd_reveal(lv_obj_t *kbd, bool shown)
{
    kbd_t *k = get(kbd);
    if (k) {
        set_shown(k, shown);
    }
}
