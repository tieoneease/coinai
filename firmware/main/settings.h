#pragma once
// Device settings the phone changes with CFG_SET (PROTOCOL.md), kept in NVS and cached in RAM.

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SET_REPLY_WAIT_S = 1,  // values are the CFG_SET keys
    SET_BRIGHTNESS = 2,    // percent, 1-100
    SET_IDLE_S = 3,        // idle time before standby (battery) or screen off (USB)
    SET_OFF_AFTER_H = 4,   // standby falls through to off after this long; 0 = never
    SET_UTC_OFFSET_MIN = 5,  // local time = UTC + this (int16, minutes, -720..840); for the face's clock
} setting_t;

// SET_UTC_OFFSET_MIN until the phone has sent one (read the value as int16_t).
#define SETTINGS_UTC_OFFSET_UNSET 0x7FFF

void settings_load(void);
uint16_t settings_get(setting_t key);
// ESP_ERR_INVALID_ARG for an unknown key or an out-of-range value.
esp_err_t settings_set(setting_t key, uint16_t value);
