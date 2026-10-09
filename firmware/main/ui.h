#pragma once
// Phase 1 bring-up screen: battery, buttons, IMU and a dot that follows touch.

#include <stdbool.h>
#include <stdint.h>
#include "imu.h"
#include "pmic.h"

typedef struct {
    bool pmic_ok;
    pmic_status_t pmic;
    bool imu_ok;
    imu_sample_t imu;
    bool pwr_pressed;
    bool boot_pressed;
    uint32_t pwr_presses;
    uint32_t boot_presses;
    uint32_t uptime_s;
    bool recording;
    int mic_level;           // 0..100
    uint32_t rec_seconds;
    uint16_t last_clip;      // 0 = none yet
    uint32_t last_clip_ms;
} ui_state_t;

// Both must be called with the display lock held.
void ui_init(void);
void ui_update(const ui_state_t *st);
