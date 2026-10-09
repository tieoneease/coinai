#pragma once
// PWR reaches GPIO3 as SYS_OUT (high while pressed); BOOT shorts GPIO0 to ground.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    INPUT_PWR_DOWN,
    INPUT_PWR_UP,
    INPUT_BOOT_DOWN,
    INPUT_BOOT_UP,
} input_event_t;

// held_ms is the press duration for *_UP events and 0 otherwise. Runs on the input task.
typedef void (*input_cb_t)(input_event_t ev, uint32_t held_ms, void *ctx);

// assume_pwr_pressed: the caller believes PWR is held (a PTT wake). If it was released before the
// edge interrupt existed, an INPUT_PWR_UP is still delivered.
esp_err_t input_init(input_cb_t cb, void *ctx, bool assume_pwr_pressed);
void input_get(bool *pwr_pressed, bool *boot_pressed);
