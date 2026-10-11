#pragma once
// CO5300 AMOLED panel, CST9217 touch and LVGL, set up here instead of bsp_display_start() so the
// draw buffers, LVGL task placement and (phase 8) panel timing are ours. See BUF_LINES in display.c.
// bsp_display_lock()/unlock() still apply.

#include "esp_err.h"
#include "lvgl.h"

lv_display_t *display_start(void);
// Panel brightness 0-100 (0 = dark; the panel and LVGL keep running).
esp_err_t display_set_brightness(int percent);
int display_get_brightness(void);  // the last value set
