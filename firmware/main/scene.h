#pragma once
// Generative UI, experiment 1: a "scene" is one card from a fixed catalog (coin/1) that a model
// fills in, plus the eyes' mood and a spoken line. The coin owns the catalog and lays the card out
// itself; experiments/genui/catalog.md is the contract a model is given.
//
//   {"say": "...", "mood": "joy", "card": {"kind": "stat", "value": "12.60", "unit": "$", ...}}
//
// scene_load() parses, validates and lays out a scene; the layout is what ui.c draws, so the
// issues it reports (text that does not fit, unknown fields, non-ASCII) are exactly what the
// screen would show. No LVGL objects are touched here: fonts are only measured.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum { CARD_ANSWER, CARD_STAT, CARD_LIST, CARD_STEPS, CARD_TIMER, CARD_CONFIRM, CARD_LATER, CARD_COUNT } card_kind_t;

#define SCENE_ITEMS 6        // list items, steps
#define SCENE_LINES 40       // laid-out text lines, all steps included
#define SCENE_ISSUES 400

typedef enum { EYES_HEADER, EYES_PEEK } scene_eyes_t;

typedef struct {
    int16_t y;               // centre of the line
    int8_t page;             // steps: the step it belongs to; -1 on every page
    int8_t row;              // list: the item; -1 otherwise
    uint8_t dim;             // drawn in the dim colour
    const lv_font_t *font;
    char text[100];
} scene_line_t;

typedef struct {
    card_kind_t kind;
    int expr;                // expr_t
    char say[200];
    scene_eyes_t eyes;
    int n_lines;
    scene_line_t lines[SCENE_LINES];
    // stat value / timer clock
    int16_t value_y;
    const lv_font_t *value_font;
    char value[24];
    char unit[12];
    // list rows and steps
    int n_items;
    int16_t row_y[SCENE_ITEMS];
    bool checked[SCENE_ITEMS];
    // confirm
    char yes[24], no[24];
    int16_t pills_y;
    // timer
    int32_t seconds;
    // later: the fixed caption under the text
    int16_t caption_y;
    // validation
    int n_issues;
    char issues[SCENE_ISSUES];
} scene_t;

// Parses and lays out a scene; false when it cannot be shown at all (not JSON, no card, unknown
// kind). Either way scene_get()->issues says what was wrong.
bool scene_load(const char *json, size_t len);
scene_t *scene_get(void);
const char *scene_kind_name(card_kind_t k);

// UTF-8 to the ASCII the built-in fonts cover: common punctuation is mapped, the rest dropped.
// Returns how many characters were dropped.
int scene_ascii(char *dst, size_t n, const char *src);

// Console: ui scene begin | + <hex> | end | last. Returns the console status.
int scene_console(int argc, char **argv, bool (*show)(void));
