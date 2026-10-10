#include "eyes.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"

#define W 466
#define C 233
#define RIM_R 221
#define RIM_W 7

const motion_t M_SACCADE = {42, 0.92f};
const motion_t M_MOOD = {24, 0.68f};
const motion_t M_POP = {30, 0.42f};
const motion_t M_LAYOUT = {17, 0.82f};
const motion_t M_UI = {26, 0.9f};
const motion_t M_SLOW = {9, 0.95f};
const motion_t M_DROOP = {12, 0.85f};
const motion_t M_SLEEPY = {7, 0.95f};
static const motion_t M_TILT = {9, 0.72f};

// Blink keyframes, ms (scaled by the blink's slowness).
#define BLINK_CLOSE 70.0f
#define BLINK_HOLD 30.0f
#define BLINK_OPEN 110.0f
#define RIGHT_LAG 0.93f  // the right eye's springs run 7% slower: the eyes never move in lockstep

static lv_color_t s_eye_col, s_dim_col, s_track_col;

// ---------- springs ----------

void spring_init(spring_t *s, float x, motion_t m)
{
    s->x = s->t = x;
    s->v = 0;
    s->w = m.w;
    s->z = m.z;
}

void spring_to(spring_t *s, float target, motion_t m)
{
    s->t = target;
    s->w = m.w;
    s->z = m.z;
}

void spring_snap(spring_t *s, float x)
{
    s->x = s->t = x;
    s->v = 0;
}

void spring_step(spring_t *s, float dt)
{
    int n = (int)ceilf(dt / 0.002f);
    if (n < 1) {
        n = 1;
    }
    float h = dt / n;
    for (int i = 0; i < n; i++) {
        float a = -s->w * s->w * (s->x - s->t) - 2 * s->z * s->w * s->v;
        s->v += a * h;
        s->x += s->v * h;
    }
}

// ---------- state ----------

typedef struct {
    spring_t p[EP_COUNT];
    spring_t gx, gy;
} eye_t;

typedef struct {
    float st, slow, dur;
} blink_t;

typedef struct {  // what was drawn for one eye, in whole pixels: equal => nothing to redraw
    lv_area_t a;
    int32_t radius;
    lv_opa_t opa;
    bool top, bot;
    int32_t xl, xr, yl, yr, ytop;
    lv_area_t circ;
} eye_draw_t;

// The rim as drawn this frame. Angles are whole degrees clockwise from 12 o'clock, unwrapped
// (tail <= head; head - tail >= 360 is the whole ring).
typedef struct {
    bool on;
    lv_color_t color;
    int32_t tail, head;
    int32_t track;  // the track's span, centred on 12 o'clock: 0 off, 360 the whole ring
} rim_draw_t;

static lv_obj_t *s_obj;
static eye_t s_eye[2];
static spring_t s_bx, s_by, s_s, s_spread, s_vis, s_tilt_x, s_tilt_y;
static float s_t;
static blink_t s_blinks[4];
static int s_nblinks;
static float s_blink_end;
static float s_level, s_level_in, s_dizzy;
static bool s_auto_blink = true, s_auto_sac = true;
static float s_next_blink = 1.4f, s_next_sac = 0.8f, s_blink_slow = 1;
static expr_t s_expr;
static uint32_t s_rng = 0x9E3779B9u;
static eye_draw_t s_draw[2], s_prev[2];
static bool s_prev_valid;
static rim_mode_t s_rim_mode;
static lv_color_t s_rim_color;
static float s_rim_p;             // progress target, 0..1
static bool s_rim_track;
// The rim never appears or vanishes in one frame: the track sweeps out from 12 o'clock, a full
// progress ring unwinds into the orbit, and turning the arc off drains it. (A whole ring redrawn at
// once is 12 pieces, ~85 kpx, ~45 ms on the coin.) Each frame redraws only the moving ends.
static bool s_arc_on;
static float s_tail, s_head;      // degrees from 12 o'clock, unwrapped
static float s_track_span;
static float s_rim_last_t;
#define ORBIT_DPS 252.0f          // the orbit's speed
#define ORBIT_LEN 43.0f           // and length, degrees
#define CHASE_DPS 700.0f          // the tail's extra speed when catching up
#define DRAIN_DPS 1100.0f         // how fast an arc that was turned off drains
#define PROGRESS_DPS 1800.0f      // fastest a progress head moves
#define TRACK_DPS 1500.0f         // track sweep
static rim_draw_t s_rim_draw, s_rim_prev;
static int32_t s_rip_x, s_rip_y;
static float s_rip_t0 = -10;
static lv_area_t s_rip_prev;
static bool s_rip_prev_on;
static int32_t s_rip_r;
static lv_opa_t s_rip_opa;
static bool s_zzz;
static float s_zzz_t0;
static bool s_zzz_prev_on;
static uint32_t s_dirty_px;

static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng >> 8) / 16777216.0f;
}

static float clampf(float v, float a, float b)
{
    return v < a ? a : v > b ? b : v;
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float maxf(float a, float b)
{
    return a > b ? a : b;
}

// ---------- expressions ----------

static const char *EXPR_NAMES[EXPR_COUNT] = {
    "neutral", "happy", "joy", "content", "listen", "focus", "think", "sad", "worried",
    "confused", "surprised", "curious", "sleepy", "tired", "squeeze", "closed", "dot",
};

const char *eyes_expr_name(expr_t e)
{
    return e < EXPR_COUNT ? EXPR_NAMES[e] : "?";
}

expr_t eyes_expr_from_name(const char *name)
{
    for (int i = 0; i < EXPR_COUNT; i++) {
        if (strcmp(name, EXPR_NAMES[i]) == 0) {
            return (expr_t)i;
        }
    }
    return EXPR_COUNT;
}

// The study's expression table: overrides of the neutral eye (w 100, h 132, r 38), per eye where
// the two differ. lidT/lidB are fractions of the eye height, lidA is the brow angle in degrees.
static void expr_params(expr_t e, int eye, float p[EP_COUNT])
{
    static const float base[EP_COUNT] = {100, 132, 38, 0, 0, 0, 0, 0};
    memcpy(p, base, sizeof base);
    bool left = eye == 0;
    switch (e) {
    case EXPR_NEUTRAL: break;
    case EXPR_HAPPY: p[EP_H] = 120; p[EP_LIDB] = 0.40f; p[EP_DY] = -4; break;
    case EXPR_JOY: p[EP_W] = 110; p[EP_H] = 116; p[EP_LIDB] = 0.64f; p[EP_DY] = -10; break;
    case EXPR_CONTENT: p[EP_H] = 120; p[EP_LIDB] = 0.34f; break;
    case EXPR_LISTEN: p[EP_W] = 108; p[EP_H] = 148; p[EP_R] = 40; p[EP_DY] = -4; break;
    case EXPR_FOCUS: p[EP_W] = 106; p[EP_H] = 104; p[EP_R] = 30; p[EP_LIDT] = 0.16f; break;
    case EXPR_THINK:
        p[EP_H] = 122;
        p[EP_LIDT] = left ? 0.10f : 0.30f;
        p[EP_LIDA] = left ? -6 : 12;
        break;
    case EXPR_SAD: p[EP_H] = 118; p[EP_LIDT] = 0.34f; p[EP_LIDA] = -22; p[EP_DY] = 12; break;
    case EXPR_WORRIED: p[EP_W] = 92; p[EP_H] = 122; p[EP_LIDT] = 0.24f; p[EP_LIDA] = -16; p[EP_DY] = 4; break;
    case EXPR_CONFUSED:
        if (left) {
            p[EP_H] = 104; p[EP_LIDT] = 0.32f; p[EP_LIDA] = 14; p[EP_DY] = 8;
        } else {
            p[EP_W] = 106; p[EP_H] = 144; p[EP_DY] = -8;
        }
        break;
    case EXPR_SURPRISED: p[EP_W] = 114; p[EP_H] = 154; p[EP_R] = 52; p[EP_DY] = -8; break;
    case EXPR_CURIOUS:
        if (left) {
            p[EP_W] = 104; p[EP_H] = 142; p[EP_DY] = -6;
        } else {
            p[EP_H] = 118; p[EP_LIDT] = 0.16f; p[EP_LIDA] = 8; p[EP_DY] = 2;
        }
        break;
    case EXPR_SLEEPY: p[EP_H] = 122; p[EP_LIDT] = 0.56f; p[EP_LIDA] = 4; p[EP_DY] = 14; break;
    case EXPR_TIRED: p[EP_H] = 124; p[EP_LIDT] = 0.42f; p[EP_LIDA] = 2; p[EP_DY] = 8; break;
    case EXPR_SQUEEZE: p[EP_W] = 110; p[EP_H] = 24; p[EP_R] = 12; p[EP_DY] = 6; break;
    case EXPR_CLOSED: p[EP_W] = 104; p[EP_H] = 7; p[EP_R] = 3.5f; p[EP_DY] = 12; break;
    case EXPR_DOT: p[EP_W] = 14; p[EP_H] = 14; p[EP_R] = 7; break;
    default: break;
    }
}

typedef struct {
    float x, y, s;
} layout_pos_t;

static const layout_pos_t LAYOUTS[] = {
    [LAYOUT_FULL] = {233, 200, 1},
    [LAYOUT_LISTEN] = {233, 212, 1.06f},
    [LAYOUT_HEADER] = {233, 100, 0.42f},
    [LAYOUT_FOOTER] = {233, 388, 0.36f},
};

static motion_t lagged(motion_t m, int eye)
{
    if (eye) {
        m.w *= RIGHT_LAG;
    }
    return m;
}

void eyes_set_expr(expr_t e, motion_t m)
{
    s_expr = e;
    for (int i = 0; i < 2; i++) {
        float p[EP_COUNT];
        expr_params(e, i, p);
        for (int k = 0; k < EP_COUNT; k++) {
            spring_to(&s_eye[i].p[k], p[k], m);
        }
    }
}

void eyes_snap_expr(expr_t e)
{
    s_expr = e;
    for (int i = 0; i < 2; i++) {
        float p[EP_COUNT];
        expr_params(e, i, p);
        for (int k = 0; k < EP_COUNT; k++) {
            spring_snap(&s_eye[i].p[k], p[k]);
        }
    }
}

expr_t eyes_expr(void)
{
    return s_expr;
}

void eyes_param_to(eye_param_t p, float value, motion_t m)
{
    for (int i = 0; i < 2; i++) {
        spring_to(&s_eye[i].p[p], value, m);
    }
}

void eyes_look(float x, float y, motion_t m)
{
    for (int i = 0; i < 2; i++) {
        spring_to(&s_eye[i].gx, x, lagged(m, i));
        spring_to(&s_eye[i].gy, y, lagged(m, i));
    }
}

void eyes_layout(layout_t l, motion_t m)
{
    spring_to(&s_bx, LAYOUTS[l].x, m);
    spring_to(&s_by, LAYOUTS[l].y, m);
    spring_to(&s_s, LAYOUTS[l].s, m);
}

void eyes_kick_y(float v) { s_by.v += v; }
void eyes_y_to(float y, motion_t m) { spring_to(&s_by, y, m); }
void eyes_scale_to(float s, motion_t m) { spring_to(&s_s, s, m); }
void eyes_spread_to(float v, motion_t m) { spring_to(&s_spread, v, m); }
void eyes_spread_snap(float v) { spring_snap(&s_spread, v); }
void eyes_visible_to(float v, motion_t m) { spring_to(&s_vis, v, m); }
void eyes_visible_snap(float v) { spring_snap(&s_vis, v); }
float eyes_y(void) { return s_by.x; }
float eyes_layout_y(void) { return s_by.t; }
void eyes_auto(bool blink, bool saccade) { s_auto_blink = blink; s_auto_sac = saccade; }
void eyes_blink_slow(float f) { s_blink_slow = f; }
void eyes_set_level(float level) { s_level_in = clampf(level, 0, 1); }
void eyes_dizzy(void) { s_dizzy = 1; }

void eyes_set_tilt(float x, float y)
{
    spring_to(&s_tilt_x, x, M_TILT);
    spring_to(&s_tilt_y, y, M_TILT);
}

void eyes_quiet(float s)
{
    s_next_blink = maxf(s_next_blink, s);
    s_next_sac = maxf(s_next_sac, s);
}

void eyes_blink(int n, float slow)
{
    float st = maxf(s_t, s_blink_end);
    for (int k = 0; k < n && s_nblinks < 4; k++) {
        float dur = (BLINK_CLOSE + BLINK_HOLD + BLINK_OPEN) * slow / 1000;
        s_blinks[s_nblinks++] = (blink_t){st, slow, dur};
        st += dur + 0.07f;
    }
    s_blink_end = st;
}

static float blink_factor(void)
{
    float f = 0;
    for (int i = 0; i < s_nblinks; i++) {
        const blink_t *b = &s_blinks[i];
        float tt = (s_t - b->st) * 1000;
        if (tt < 0 || tt > b->dur * 1000) {
            continue;
        }
        float c = BLINK_CLOSE * b->slow, h = BLINK_HOLD * b->slow, o = BLINK_OPEN * b->slow, v;
        if (tt < c) {
            v = (tt / c) * (tt / c);
        } else if (tt < c + h) {
            v = 1;
        } else {
            float u = 1 - (tt - c - h) / o;
            v = u * u * u;
        }
        f = maxf(f, v);
    }
    return f;
}

static void random_look(void)
{
    float r = frand(), x = 0, y = 0;
    if (r < 0.3f) {
        // back to the centre
    } else if (r < 0.82f) {
        x = (frand() * 2 - 1) * 16;
        y = (frand() * 2 - 1) * 10;
    } else {  // a big glance, usually with a blink
        x = (frand() < 0.5f ? -1 : 1) * (30 + frand() * 16);
        y = (frand() * 2 - 1) * 14;
        if (frand() < 0.6f) {
            eyes_blink(1, s_blink_slow);
        }
    }
    eyes_look(x, y, M_SACCADE);
    s_next_sac = 0.6f + frand() * 2.2f;
}

void eyes_reset(layout_t layout)
{
    for (int i = 0; i < 2; i++) {
        float p[EP_COUNT];
        expr_params(EXPR_NEUTRAL, i, p);
        for (int k = 0; k < EP_COUNT; k++) {
            spring_init(&s_eye[i].p[k], p[k], M_MOOD);
        }
        spring_init(&s_eye[i].gx, 0, lagged(M_SACCADE, i));
        spring_init(&s_eye[i].gy, 0, lagged(M_SACCADE, i));
    }
    spring_init(&s_bx, LAYOUTS[layout].x, M_LAYOUT);
    spring_init(&s_by, LAYOUTS[layout].y, M_LAYOUT);
    spring_init(&s_s, LAYOUTS[layout].s, M_LAYOUT);
    spring_init(&s_spread, 1, M_MOOD);
    spring_init(&s_vis, 1, M_UI);
    spring_init(&s_tilt_x, 0, M_TILT);
    spring_init(&s_tilt_y, 0, M_TILT);
    s_nblinks = 0;
    s_blink_end = 0;
    s_level = s_level_in = s_dizzy = 0;
    s_auto_blink = s_auto_sac = true;
    s_next_blink = 1.4f;
    s_next_sac = 0.8f;
    s_blink_slow = 1;
    s_expr = EXPR_NEUTRAL;
}

// ---------- rim and effects ----------

void eyes_rim(rim_mode_t mode, lv_color_t color)
{
    if (mode != s_rim_mode) {
        s_rim_p = 0;
        if (mode == RIM_PROGRESS) {
            s_arc_on = true;  // a progress arc starts afresh at 12 o'clock
            s_tail = s_head = 0;
        } else if (mode == RIM_ORBIT && !s_arc_on) {
            s_arc_on = true;
            s_tail = s_head = 0;
        }
    }
    s_rim_mode = mode;
    if (mode != RIM_OFF) {
        s_rim_color = color;
    }
}

void eyes_rim_progress(float p) { s_rim_p = clampf(p, 0, 1); }
void eyes_rim_track(bool on) { s_rim_track = on; }

void eyes_ripple(int x, int y)
{
    s_rip_x = x;
    s_rip_y = y;
    s_rip_t0 = s_t;
}

void eyes_zzz(bool on)
{
    if (on && !s_zzz) {
        s_zzz_t0 = s_t;
    }
    s_zzz = on;
}

uint32_t eyes_dirty_px(void)
{
    return s_dirty_px;
}

// ---------- geometry and invalidation ----------

// (LVGL 9.5 keeps its area helpers private.)
static bool area_intersect(lv_area_t *out, const lv_area_t *a, const lv_area_t *b)
{
    out->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    out->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    out->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    out->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

static void area_join(lv_area_t *out, const lv_area_t *a, const lv_area_t *b)
{
    out->x1 = a->x1 < b->x1 ? a->x1 : b->x1;
    out->y1 = a->y1 < b->y1 ? a->y1 : b->y1;
    out->x2 = a->x2 > b->x2 ? a->x2 : b->x2;
    out->y2 = a->y2 > b->y2 ? a->y2 : b->y2;
}

static void invalidate(const lv_area_t *a)
{
    lv_area_t scr = {0, 0, W - 1, W - 1}, c;
    if (area_intersect(&c, a, &scr)) {
        lv_obj_invalidate_area(s_obj, &c);
        s_dirty_px += lv_area_get_size(&c);
    }
}

static void invalidate_union(const lv_area_t *a, const lv_area_t *b)
{
    lv_area_t u;
    area_join(&u, a, b);
    invalidate(&u);
}

static void compute_eyes(void)
{
    float s = s_s.x, bf = blink_factor(), L = s_level;
    float stretch = clampf(fabsf(s_by.v) / 2600, 0, 0.12f);
    float vis = clampf(s_vis.x, 0, 1);
    for (int i = 0; i < 2; i++) {
        eye_t *e = &s_eye[i];
        float side = i ? 1 : -1;
        float gx = e->gx.x + s_tilt_x.x * 42, gy = e->gy.x + s_tilt_y.x * 30;
        if (s_dizzy > 0) {
            float A = 24 * s_dizzy, ph = s_t * 10 + (i ? (float)M_PI : 0);
            gx += A * cosf(ph);
            gy += A * 0.7f * sinf(ph);
        }
        float smear = clampf(fabsf(e->gx.v) / 4000, 0, 0.08f);
        float w = e->p[EP_W].x, h = e->p[EP_H].x;
        float gn = clampf(gx / 50, -1, 1);
        // Parallax: the eye on the far side of the gaze gets narrower.
        w *= 1 - 0.12f * maxf(0, side * gn) + 0.04f * maxf(0, -side * gn);
        h *= 1 - 0.05f * maxf(0, side * gn);
        h *= (1 + 0.2f * L) * (1 + stretch) * (1 - smear * 0.5f);
        w *= (1 - 0.05f * L) * (1 - stretch * 0.6f) * (1 + smear);
        float h_open = h;
        h = lerpf(h, 7, bf);
        w *= 1 + 0.08f * bf;
        float spacing = 152 * s_spread.x * (1 - 0.06f * fabsf(gn));
        float cx = s_bx.x + (side * spacing / 2 + gx * 0.9f + e->p[EP_DX].x) * s;
        float cy = s_by.x + (gy + e->p[EP_DY].x - 8 * L) * s + (h_open - h) / 2 * 0.8f * s;
        float ew = w * s, eh = maxf(2, h * s);
        float r = fminf(fminf(e->p[EP_R].x * s, ew / 2), eh / 2);

        eye_draw_t *d = &s_draw[i];
        memset(d, 0, sizeof *d);
        d->opa = (lv_opa_t)lroundf(vis * 255);
        float x0 = cx - ew / 2, y0 = cy - eh / 2;
        d->a.x1 = lroundf(x0);
        d->a.y1 = lroundf(y0);
        d->a.x2 = lroundf(x0 + ew) - 1;
        d->a.y2 = lroundf(y0 + eh) - 1;
        if (d->a.x2 < d->a.x1) {
            d->a.x2 = d->a.x1;
        }
        if (d->a.y2 < d->a.y1) {
            d->a.y2 = d->a.y1;
        }
        d->radius = lroundf(r);
        float lid_t = clampf(e->p[EP_LIDT].x, 0, 1), lid_b = clampf(e->p[EP_LIDB].x, 0, 1);
        if (lid_t > 0.004f) {
            float yl = y0 + lid_t * eh;
            float sl = tanf(e->p[EP_LIDA].x * (float)M_PI / 180) * (i ? -1 : 1);
            d->top = true;
            // One triangle: its bottom edge is the lid line, the rest lies far outside the eye. (Two
            // triangles sharing a diagonal leave an anti-aliased seam the eye's colour shows through.)
            float reach = 3 * ew;
            d->xl = lroundf(cx - reach);
            d->xr = lroundf(cx + reach);
            d->yl = lroundf(yl - sl * reach);
            d->yr = lroundf(yl + sl * reach);
            d->ytop = lroundf(fminf(d->yl, d->yr) - 2 * reach);
        }
        if (lid_b > 0.004f) {
            float R = ew * 0.95f, top = y0 + eh * (1 - lid_b);
            d->bot = true;
            d->circ.x1 = lroundf(cx - R);
            d->circ.x2 = lroundf(cx + R);
            d->circ.y1 = lroundf(top);
            d->circ.y2 = lroundf(top + 2 * R);
        }
        if (d->opa == 0) {
            memset(d, 0, sizeof *d);  // invisible: nothing to draw, nothing changes
        }
    }
}

static void arc_area(int32_t start, int32_t end, lv_area_t *out)
{
    lv_draw_arc_get_area(C, C, RIM_R, start, end, RIM_W, true, out);
}

// Rim arcs are invalidated and clipped in pieces of at most 30 degrees. LVGL gives an arc's draw
// task the whole circle's bounding box (197 kpx, nearly the screen) and the software renderer
// evaluates its masks row by row over all of it that is dirty, the eyes included: measured 6-9 ms
// per arc per full frame on the coin. Drawing the same arc once per piece with the clip narrowed to
// the piece keeps that work on the band; the geometry is one arc, so there are no joins to show.
// A whole ring is 12 pieces, well inside LVGL's 32 dirty areas per frame.
#define RIM_PIECE_DEG 30

typedef void (*piece_fn)(int32_t start, int32_t end, void *ctx);

// Degrees as LVGL's: 0 is 3 o'clock, clockwise; end == start + 360 is the whole ring.
static void arc_pieces(int32_t start, int32_t end, piece_fn fn, void *ctx)
{
    int32_t span = end == start + 360 ? 360 : (end - start + 360) % 360;
    bool full = span == 360;
    for (int32_t a = 0; a < span; a += RIM_PIECE_DEG) {
        int32_t b = a + RIM_PIECE_DEG < span ? a + RIM_PIECE_DEG : span;
        int32_t s0 = a > 0 || full ? a - 1 : a, e0 = b < span || full ? b + 1 : b;
        fn((start + s0 + 360) % 360, (start + e0) % 360, ctx);
    }
}

static void invalidate_piece(int32_t start, int32_t end, void *ctx)
{
    lv_area_t r;
    arc_area(start, end, &r);
    invalidate(&r);
}

static void invalidate_arc(int32_t start, int32_t end)
{
    arc_pieces(start, end, invalidate_piece, NULL);
}

static float approachf(float x, float target, float max_step)
{
    return x < target ? fminf(target, x + max_step) : fmaxf(target, x - max_step);
}

static void compute_rim(void)
{
    float dt = clampf(s_t - s_rim_last_t, 0, 0.05f);
    s_rim_last_t = s_t;
    s_track_span = approachf(s_track_span, s_rim_track ? 360 : 0, TRACK_DPS * dt);
    if (s_arc_on) {
        switch (s_rim_mode) {
        case RIM_PROGRESS:
            s_head = approachf(s_head, s_tail + s_rim_p * 360, PROGRESS_DPS * dt);
            break;
        case RIM_ORBIT:
            s_head += ORBIT_DPS * dt;
            if (s_head - s_tail > ORBIT_LEN) {
                s_tail = fminf(s_head - ORBIT_LEN, s_tail + (ORBIT_DPS + CHASE_DPS) * dt);
            }
            break;
        default:
            s_tail = fminf(s_head, s_tail + DRAIN_DPS * dt);
            if (s_tail >= s_head) {
                s_arc_on = false;
            }
            break;
        }
        if (s_tail >= 360) {
            s_tail -= 360;
            s_head -= 360;
        }
    }
    rim_draw_t *d = &s_rim_draw;
    memset(d, 0, sizeof *d);
    d->track = lroundf(s_track_span);
    d->tail = lroundf(s_tail);
    d->head = lroundf(s_head);
    d->on = s_arc_on && d->head > d->tail;
    d->color = d->on ? s_rim_color : lv_color_black();
    if (!d->on) {
        d->tail = d->head = 0;
    }
}

static int32_t wrap180(int32_t a)
{
    a %= 360;
    return a > 180 ? a - 360 : a <= -180 ? a + 360 : a;
}

// Invalidates the band between two angles (degrees from 12 o'clock, unwrapped, lo <= hi), with
// room for the round ends.
static void invalidate_range(int32_t lo, int32_t hi)
{
    lo -= 3;
    hi += 3;
    if (hi - lo >= 360) {
        invalidate_arc(0, 360);
    } else {
        invalidate_arc(((270 + lo) % 360 + 360) % 360, ((270 + hi) % 360 + 360) % 360);
    }
}

static void invalidate_moved(int32_t from, int32_t to)
{
    int32_t d = wrap180(to - from);
    if (d) {
        invalidate_range(d > 0 ? from : from + d, d > 0 ? from + d : from);
    }
}

static void invalidate_rim(void)
{
    rim_draw_t *d = &s_rim_draw, *p = &s_rim_prev;
    if (memcmp(d, p, sizeof *d) == 0) {
        return;
    }
    if (d->track != p->track) {  // the two ends of the track
        int32_t a = p->track / 2, b = d->track / 2;
        invalidate_range(-(a > b ? a : b), -(a < b ? a : b));
        invalidate_range(a < b ? a : b, a > b ? a : b);
    }
    bool same = d->on && p->on && lv_color_eq(d->color, p->color) && abs(wrap180(d->tail - p->tail)) < 120 &&
                abs(wrap180(d->head - p->head)) < 120 && (d->head - d->tail >= 360) == (p->head - p->tail >= 360);
    if (same) {
        invalidate_moved(p->tail, d->tail);
        invalidate_moved(p->head, d->head);
        return;
    }
    if (p->on && (d->on != p->on || !lv_color_eq(d->color, p->color) || d->tail != p->tail || d->head != p->head)) {
        invalidate_range(p->tail, p->head);
    }
    if (d->on) {
        invalidate_range(d->tail, d->head);
    }
}

static void compute_effects(void)
{
    // Touch ripple: a ring that grows and fades in 420 ms.
    float u = (s_t - s_rip_t0) / 0.42f;
    bool on = u >= 0 && u < 1;
    lv_area_t a = {0};
    if (on) {
        float k = 1 - u;
        s_rip_r = lroundf(10 + 34 * (1 - k * k * k));
        s_rip_opa = (lv_opa_t)lroundf(k * 0.7f * 255);
        a = (lv_area_t){s_rip_x - s_rip_r - 3, s_rip_y - s_rip_r - 3, s_rip_x + s_rip_r + 3, s_rip_y + s_rip_r + 3};
    }
    if (on || s_rip_prev_on) {
        if (on && s_rip_prev_on) {
            invalidate_union(&a, &s_rip_prev);
        } else {
            invalidate(on ? &a : &s_rip_prev);
        }
    }
    s_rip_prev = a;
    s_rip_prev_on = on;

    // Floating z's: always redrawn while shown (a small area).
    lv_area_t z = {(int32_t)s_bx.x + 40, (int32_t)s_by.x - 130, (int32_t)s_bx.x + 120, (int32_t)s_by.x - 10};
    if (s_zzz || s_zzz_prev_on) {
        invalidate(&z);
    }
    s_zzz_prev_on = s_zzz;
}

// ---------- drawing ----------

static void draw_eye(lv_layer_t *layer, const eye_draw_t *d)
{
    if (d->opa < 2) {
        return;
    }
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = s_eye_col;
    r.bg_opa = d->opa;
    r.radius = d->radius;
    lv_draw_rect(layer, &r, &d->a);
    if (!d->top && !d->bot) {
        return;
    }
    // Lids are black shapes clipped to the eye, as an LVGL object clips to its area.
    lv_area_t saved = layer->_clip_area, clip;
    if (!area_intersect(&clip, &saved, &d->a)) {
        return;
    }
    layer->_clip_area = clip;
    if (d->top) {
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = lv_color_black();
        t.opa = LV_OPA_COVER;
        t.p[0] = (lv_point_precise_t){d->xl, d->yl};
        t.p[1] = (lv_point_precise_t){d->xr, d->yr};
        t.p[2] = (lv_point_precise_t){(d->xl + d->xr) / 2, d->ytop};
        lv_draw_triangle(layer, &t);
    }
    if (d->bot) {
        lv_draw_rect_dsc_t b;
        lv_draw_rect_dsc_init(&b);
        b.bg_color = lv_color_black();
        b.bg_opa = LV_OPA_COVER;
        b.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &b, &d->circ);
    }
    layer->_clip_area = saved;
}

typedef struct {
    lv_layer_t *layer;
    lv_area_t clip;  // the layer's clip area when drawing began
    lv_draw_arc_dsc_t dsc;
} arc_ctx_t;

static void draw_piece(int32_t start, int32_t end, void *ctx)
{
    arc_ctx_t *c = ctx;
    lv_area_t r, clip;
    arc_area(start, end, &r);
    if (area_intersect(&clip, &c->clip, &r)) {
        c->layer->_clip_area = clip;
        lv_draw_arc(c->layer, &c->dsc);
    }
}

static void draw_rim_arc(lv_layer_t *layer, int32_t start, int32_t end, int32_t width, lv_color_t color)
{
    arc_ctx_t c = {.layer = layer, .clip = layer->_clip_area};
    lv_draw_arc_dsc_init(&c.dsc);
    c.dsc.center = (lv_point_t){C, C};
    c.dsc.radius = RIM_R;
    c.dsc.width = width;
    c.dsc.color = color;
    c.dsc.opa = LV_OPA_COVER;
    c.dsc.rounded = end != start + 360;
    c.dsc.start_angle = start;
    c.dsc.end_angle = end;
    arc_pieces(start, end, draw_piece, &c);
    layer->_clip_area = c.clip;
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const rim_draw_t *rim = &s_rim_draw;
    if (rim->track >= 360) {
        draw_rim_arc(layer, 0, 360, RIM_W - 1, s_track_col);
    } else if (rim->track > 0) {
        draw_rim_arc(layer, ((270 - rim->track / 2) % 360 + 360) % 360, (270 + rim->track / 2) % 360, RIM_W - 1,
                     s_track_col);
    }
    if (rim->on) {
        if (rim->head - rim->tail >= 360) {
            draw_rim_arc(layer, 0, 360, RIM_W, rim->color);
        } else {
            draw_rim_arc(layer, ((270 + rim->tail) % 360 + 360) % 360, ((270 + rim->head) % 360 + 360) % 360, RIM_W,
                         rim->color);
        }
    }
    if (s_rip_prev_on) {
        lv_draw_arc_dsc_t a;
        lv_draw_arc_dsc_init(&a);
        a.center = (lv_point_t){s_rip_x, s_rip_y};
        a.radius = (uint16_t)s_rip_r;
        a.width = 3;
        a.start_angle = 0;
        a.end_angle = 360;
        a.color = lv_color_hex(0xF4ECDF);
        a.opa = s_rip_opa;
        lv_draw_arc(layer, &a);
    }
    for (int i = 0; i < 2; i++) {
        draw_eye(layer, &s_draw[i]);
    }
#if LV_FONT_MONTSERRAT_24
    if (s_zzz) {
        float el = s_t - s_zzz_t0;
        for (int k = 0; k < 3; k++) {
            if (el - k * 0.45f < 0) {
                continue;
            }
            float u = fmodf(el - k * 0.45f, 1.6f) / 1.6f;
            int32_t x = (int32_t)(s_bx.x + 63 + u * 34 + k * 4), y = (int32_t)(s_by.x - 40 - u * 70);
            lv_draw_label_dsc_t l;
            lv_draw_label_dsc_init(&l);
            l.font = &lv_font_montserrat_24;
            l.color = s_dim_col;
            l.opa = (lv_opa_t)lroundf(sinf(u * (float)M_PI) * 255);
            l.text = "z";
            lv_area_t a = {x - 10, y - 14, x + 14, y + 14};
            lv_draw_label(layer, &l, &a);
        }
    }
#endif
}

// ---------- the frame ----------

void eyes_step(float dt)
{
    s_t += dt;
    if (s_auto_blink) {
        s_next_blink -= dt;
        if (s_next_blink <= 0) {
            eyes_blink(frand() < 0.16f ? 2 : 1, s_blink_slow);
            s_next_blink = (2.2f + frand() * 3.4f) * s_blink_slow;
        }
    }
    if (s_auto_sac) {
        s_next_sac -= dt;
        if (s_next_sac <= 0) {
            random_look();
        }
    }
    int keep = 0;
    for (int i = 0; i < s_nblinks; i++) {
        if (s_t < s_blinks[i].st + s_blinks[i].dur + 0.02f) {
            s_blinks[keep++] = s_blinks[i];
        }
    }
    s_nblinks = keep;
    float tau = s_level_in > s_level ? 0.035f : 0.16f;  // fast attack, slow release
    s_level += (s_level_in - s_level) * (1 - expf(-dt / tau));
    if (s_dizzy > 0) {
        s_dizzy = maxf(0, s_dizzy - dt / 1.6f);
    }
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < EP_COUNT; k++) {
            spring_step(&s_eye[i].p[k], dt);
        }
        spring_step(&s_eye[i].gx, dt);
        spring_step(&s_eye[i].gy, dt);
    }
    spring_t *others[] = {&s_bx, &s_by, &s_s, &s_spread, &s_vis, &s_tilt_x, &s_tilt_y};
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        spring_step(others[i], dt);
    }

    s_dirty_px = 0;
    compute_eyes();
    for (int i = 0; i < 2; i++) {
        if (!s_prev_valid || memcmp(&s_draw[i], &s_prev[i], sizeof s_draw[i]) != 0) {
            bool now_on = s_draw[i].opa > 0, was_on = s_prev_valid && s_prev[i].opa > 0;
            lv_area_t a = s_draw[i].a, b = s_prev[i].a;
            lv_area_increase(&a, 1, 1);
            lv_area_increase(&b, 1, 1);
            if (now_on && was_on) {
                invalidate_union(&a, &b);
            } else if (now_on) {
                invalidate(&a);
            } else if (was_on) {
                invalidate(&b);
            }
        }
        s_prev[i] = s_draw[i];
    }
    s_prev_valid = true;
    compute_rim();
    invalidate_rim();
    s_rim_prev = s_rim_draw;
    compute_effects();
}

void eyes_init(lv_obj_t *parent)
{
    s_eye_col = lv_color_hex(0xFFF0D4);
    s_dim_col = lv_color_hex(0x8E867A);
    s_track_col = lv_color_hex(0x2E2A25);
    s_rng ^= (uint32_t)esp_timer_get_time();
    s_obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s_obj);
    lv_obj_set_size(s_obj, W, W);
    lv_obj_set_pos(s_obj, 0, 0);
    lv_obj_remove_flag(s_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    eyes_reset(LAYOUT_FULL);
    s_rim_mode = RIM_OFF;
}
