#include "ui.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui";

#define DOT_SIZE 36

static lv_obj_t *s_batt;
static lv_obj_t *s_power;
static lv_obj_t *s_keys;
static lv_obj_t *s_imu;
static lv_obj_t *s_touch;
static lv_obj_t *s_uptime;
static lv_obj_t *s_rec;
static lv_obj_t *s_dot;
static uint32_t s_touches;

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    return label;
}

static void on_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_RELEASED) {
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "touch up at %d,%d", (int)p.x, (int)p.y);
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        s_touches++;
        ESP_LOGI(TAG, "touch down at %d,%d", (int)p.x, (int)p.y);
    }
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_dot, p.x - DOT_SIZE / 2, p.y - DOT_SIZE / 2);
    lv_label_set_text_fmt(s_touch, "touch %d, %d  (#%lu)", (int)p.x, (int)p.y, (unsigned long)s_touches);
}

void ui_init(void)
{
    const lv_color_t fg = lv_color_hex(0xE8E8E8);
    const lv_color_t dim = lv_color_hex(0x8A8A8A);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_touch, LV_EVENT_ALL, NULL);

    // Everything stays inside the round panel's central square.
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(col, 330, LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);

    add_label(col, &lv_font_montserrat_28, fg, "coinai");
    add_label(col, &lv_font_montserrat_16, dim, "bring-up");
    s_batt = add_label(col, &lv_font_montserrat_20, fg, "battery --");
    s_power = add_label(col, &lv_font_montserrat_16, dim, "");
    s_keys = add_label(col, &lv_font_montserrat_20, fg, "PWR -  BOOT -");
    s_imu = add_label(col, &lv_font_montserrat_16, dim, "imu --");
    s_touch = add_label(col, &lv_font_montserrat_16, dim, "touch the screen");
    s_rec = add_label(col, &lv_font_montserrat_20, fg, "hold PWR to record");
    s_uptime = add_label(col, &lv_font_montserrat_16, dim, "");

    s_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dot);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x35C2FF), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

void ui_update(const ui_state_t *st)
{
    if (st->pmic_ok) {
        const pmic_status_t *p = &st->pmic;
        if (p->battery_present) {
            lv_label_set_text_fmt(s_batt, "battery %d%%  %u mV", p->battery_pct, p->vbat_mv);
        } else {
            lv_label_set_text(s_batt, "no battery");
        }
        lv_label_set_text_fmt(s_power, "%s  |  USB %s  |  %.0f C", pmic_charger_state_name(p->charger_state),
                              p->vbus_good ? "in" : "out", (double)p->die_temp_c);
    } else {
        lv_label_set_text(s_batt, "PMIC not responding");
    }

    lv_label_set_text_fmt(s_keys, "PWR %s (%lu)   BOOT %s (%lu)", st->pwr_pressed ? "DOWN" : "up",
                          (unsigned long)st->pwr_presses, st->boot_pressed ? "DOWN" : "up",
                          (unsigned long)st->boot_presses);
    lv_obj_set_style_text_color(s_keys, (st->pwr_pressed || st->boot_pressed) ? lv_color_hex(0x35C2FF) : lv_color_hex(0xE8E8E8), 0);

    if (st->imu_ok) {
        lv_label_set_text_fmt(s_imu, "accel %.2f %.2f %.2f g", (double)st->imu.ax, (double)st->imu.ay, (double)st->imu.az);
    } else {
        lv_label_set_text(s_imu, "IMU not responding");
    }
    lv_label_set_text_fmt(s_uptime, "up %lus", (unsigned long)st->uptime_s);

    if (st->recording) {
        lv_label_set_text_fmt(s_rec, "REC %lus  level %d", (unsigned long)st->rec_seconds, st->mic_level);
        lv_obj_set_style_text_color(s_rec, lv_color_hex(0xFF4040), 0);
    } else {
        if (st->last_clip) {
            lv_label_set_text_fmt(s_rec, "clip %u saved, %lu.%lus", st->last_clip, (unsigned long)(st->last_clip_ms / 1000),
                                  (unsigned long)(st->last_clip_ms % 1000 / 100));
        } else {
            lv_label_set_text_fmt(s_rec, "hold PWR to record  (mic %d)", st->mic_level);
        }
        lv_obj_set_style_text_color(s_rec, lv_color_hex(0xE8E8E8), 0);
    }
}
