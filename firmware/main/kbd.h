#pragma once
// A full ASCII keyboard for the coin's 466 px round screen, as one reusable component: a title, a
// one-line field and four rows of keys fitted to the circle's widest band. Letters (one-shot shift,
// double-tap for caps lock), digits and two symbol pages cover all 95 printable ASCII characters.
//
// Keys commit on release, so a finger can slide onto the key it meant; a magnified preview shows the
// key under it. Backspace repeats while held. Passwords are hidden (each character shows briefly)
// with a show/hide toggle in the field; tapping the field moves the cursor.
//
// All calls must hold the display lock. Delete the returned object to close the keyboard; its text
// buffers are wiped then (best effort: LVGL frees earlier copies unwiped as the text grows).

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum { KBD_TEXT, KBD_PASSWORD } kbd_kind_t;

typedef struct {
    kbd_kind_t kind;
    const char *title;     // one line above the field
    const char *initial;   // starting text, or NULL
    const char *placeholder;
    uint16_t min_len;      // Done works from this many characters
    uint16_t max_len;      // at most this many (ASCII: bytes)
    const char *hint;      // the line under the keys, e.g. "BOOT cancels"
    void (*on_done)(const char *text, void *ctx);
    void (*on_key)(int x, int y, void *ctx);  // a key went down, or the finger slid onto one: its centre
    void (*on_reveal)(bool shown, void *ctx); // the password was shown or hidden
    void *ctx;
} kbd_cfg_t;

// A transparent full-screen container (not clickable itself) on `parent`, which must be the 466 x 466
// screen or an object of that size at 0,0. `cfg` and its strings are copied.
lv_obj_t *kbd_create(lv_obj_t *parent, const kbd_cfg_t *cfg);

const char *kbd_text(lv_obj_t *kbd);
// Replaces the hint line. error: shown in the error colour until the text changes, then the hint again.
void kbd_set_hint(lv_obj_t *kbd, const char *hint, bool error);
// Types `text` as if its keys were tapped (console tests and demos).
void kbd_type(lv_obj_t *kbd, const char *text);
// Taps Done; false when Done is not available yet (too short).
bool kbd_done(lv_obj_t *kbd);
// Shows or hides a password.
void kbd_reveal(lv_obj_t *kbd, bool shown);
