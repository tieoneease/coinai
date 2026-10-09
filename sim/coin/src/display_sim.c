// The CO5300 panel and CST9217 touch for the simulator: LVGL renders into partial buffers as on
// the coin, and every flush goes to the host as a FRAME; touch arrives as TOUCH frames.
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "display.h"
#include "sim_esp.h"
#include "simlink.h"

#define BUF_LINES 24  // as display.c

static SemaphoreHandle_t s_lock;
static lv_display_t *s_disp;
static volatile bool s_pressed;
static volatile bool s_press_unread;  // a press LVGL has not read yet
static volatile int16_t s_x, s_y;
static uint8_t *s_frame;  // header + one buffer of pixels

esp_err_t bsp_display_lock(uint32_t timeout_ms)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTakeRecursive(s_lock, timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

void bsp_display_unlock(void)
{
    if (s_lock) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

static void send_panel(int brightness)
{
    uint8_t p[2] = {1, (uint8_t)brightness};
    sim_send(IPC_PANEL, p, 2);
}

esp_err_t display_set_brightness(int percent)
{
    if (!s_disp) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    // The panel takes 0-255 (register 0x51); report what that register would hold, as a percentage.
    send_panel((percent * 255 / 100) * 100 / 255);
    return ESP_OK;
}

void sim_touch_input(bool pressed, uint16_t x, uint16_t y)
{
    s_x = (int16_t)(x < BSP_LCD_H_RES ? x : BSP_LCD_H_RES - 1);
    s_y = (int16_t)(y < BSP_LCD_V_RES ? y : BSP_LCD_V_RES - 1);
    s_pressed = pressed;
    if (pressed) {
        s_press_unread = true;
    }
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = s_x;
    data->point.y = s_y;
    // A mouse click can press and release between two LVGL reads (every 30 ms); a finger on the
    // CST9217 never does. Report such a press once so it still counts as a tap.
    data->state = s_pressed || s_press_unread ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    s_press_unread = false;
}

static void flush(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    uint16_t w = (uint16_t)lv_area_get_width(a), h = (uint16_t)lv_area_get_height(a);
    sim_put16(s_frame, (uint16_t)a->x1);
    sim_put16(s_frame + 2, (uint16_t)a->y1);
    sim_put16(s_frame + 4, w);
    sim_put16(s_frame + 6, h);
    sim_send2(IPC_FRAME, s_frame, 8, px, (size_t)w * h * 2);
    lv_display_flush_ready(disp);
}

// The CO5300 takes windows that start on an even and end on an odd coordinate.
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t next = LV_DEF_REFR_PERIOD;
        if (bsp_display_lock(0) == ESP_OK) {
            next = lv_timer_handler();
            bsp_display_unlock();
        }
        if (next > 500) {
            next = 500;
        }
        vTaskDelay(pdMS_TO_TICKS(next ? next : 1));
    }
}

lv_display_t *display_start(void)
{
    // The CO5300 and CST9217 drivers pulse their reset lines and leave them released.
    gpio_set_level((gpio_num_t)1, 1);  // LCD reset
    gpio_set_level((gpio_num_t)2, 1);  // touch reset
    s_lock = xSemaphoreCreateRecursiveMutex();
    lv_init();
    lv_tick_set_cb(tick_ms);
    size_t buf_bytes = BSP_LCD_H_RES * BUF_LINES * 2;
    s_frame = malloc(8);
    void *buf1 = malloc(buf_bytes), *buf2 = malloc(buf_bytes);
    s_disp = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush);
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, s_disp);

    send_panel(100);  // the init sequence sets register 0x51 to 0xFF
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    return s_disp;
}
