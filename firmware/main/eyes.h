#pragma once
// The eyes bot: two eyes on black that move on springs, blink, glance and change mood, plus the rim
// (progress and "thinking" arcs) and small effects. A port of design/coin-ui/motion/coin-eyes.html.
//
// Each eye is one filled rounded rectangle; its lids are a black quad and a black circle clipped to the
// eye. Only the boxes that changed since the last frame are invalidated. All calls must hold the
// display lock (they run on the LVGL task, from ui.c).

#include <stdbool.h>
#include "lvgl.h"

typedef struct {
    float w;  // natural frequency, rad/s
    float z;  // damping ratio
} motion_t;

// Motion tokens (the study's table).
extern const motion_t M_SACCADE, M_MOOD, M_POP, M_LAYOUT, M_UI, M_SLOW, M_DROOP, M_SLEEPY;

typedef struct {
    float x, v, t, w, z;
} spring_t;

void spring_init(spring_t *s, float x, motion_t m);
void spring_to(spring_t *s, float target, motion_t m);
void spring_snap(spring_t *s, float x);
void spring_step(spring_t *s, float dt);

typedef enum {
    EXPR_NEUTRAL, EXPR_HAPPY, EXPR_JOY, EXPR_CONTENT, EXPR_LISTEN, EXPR_FOCUS, EXPR_THINK, EXPR_SAD,
    EXPR_WORRIED, EXPR_CONFUSED, EXPR_SURPRISED, EXPR_CURIOUS, EXPR_SLEEPY, EXPR_TIRED, EXPR_SQUEEZE,
    EXPR_CLOSED, EXPR_DOT, EXPR_COUNT,
} expr_t;

typedef enum { LAYOUT_FULL, LAYOUT_LISTEN, LAYOUT_HEADER, LAYOUT_FOOTER, LAYOUT_PEEK } layout_t;  // PEEK: small, at the top (above a keyboard)

typedef enum { RIM_OFF, RIM_PROGRESS, RIM_ORBIT } rim_mode_t;

// Per-eye shape parameters, for flows that animate one directly (the power-off squeeze).
typedef enum { EP_W, EP_H, EP_R, EP_LIDT, EP_LIDA, EP_LIDB, EP_DY, EP_DX, EP_COUNT } eye_param_t;

void eyes_init(lv_obj_t *parent);
// Advances springs, blinks and idle behaviour by dt seconds; then redraws what changed.
void eyes_step(float dt);

void eyes_reset(layout_t layout);  // neutral, centred in the layout, visible, no motion
void eyes_set_expr(expr_t e, motion_t m);
void eyes_snap_expr(expr_t e);
expr_t eyes_expr(void);
const char *eyes_expr_name(expr_t e);
expr_t eyes_expr_from_name(const char *name);  // EXPR_COUNT if unknown
void eyes_param_to(eye_param_t p, float value, motion_t m);  // both eyes
void eyes_look(float x, float y, motion_t m);  // gaze offset in eye units (about ±40)
void eyes_layout(layout_t l, motion_t m);
void eyes_kick_y(float v);         // a hop: px/s upwards is negative
void eyes_y_to(float y, motion_t m);
void eyes_scale_to(float s, motion_t m);
void eyes_spread_to(float spread, motion_t m);  // 1 = normal spacing, 0 = merged
void eyes_spread_snap(float spread);
void eyes_visible_to(float v, motion_t m);      // 0..1
void eyes_visible_snap(float v);
void eyes_blink(int n, float slow);
void eyes_auto(bool blink, bool saccade);
void eyes_blink_slow(float factor);  // slower, rarer blinks (tired, sleepy)
void eyes_quiet(float s);            // no idle blink or glance for s seconds
void eyes_set_level(float level);    // voice level 0..1 (the eyes swell)
void eyes_set_tilt(float x, float y);
void eyes_dizzy(void);
float eyes_y(void);
float eyes_layout_y(void);           // target y

void eyes_rim(rim_mode_t mode, lv_color_t color);
void eyes_rim_progress(float p);     // 0..1
void eyes_rim_track(bool on);
void eyes_ripple(int x, int y);
void eyes_zzz(bool on);

// Pixels invalidated by the eyes on the last frame (for `ui perf`).
uint32_t eyes_dirty_px(void);
