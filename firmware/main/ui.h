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
} ui_state_t;

// Both must be called with the display lock held.
void ui_init(void);
void ui_update(const ui_state_t *st);
