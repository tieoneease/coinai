#pragma once
// The firmware's own configuration (firmware/build/config/sdkconfig.h, written by `idf.py build`),
// so the simulator runs with the coin's settings. Only what cannot apply on a desktop changes.
#include COINSIM_FW_SDKCONFIG

// LVGL runs single-threaded under bsp_display_lock(), with one software draw unit.
#undef CONFIG_LV_OS_FREERTOS
#define CONFIG_LV_OS_NONE 1
#undef CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT
#define CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT 1
#undef CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM
