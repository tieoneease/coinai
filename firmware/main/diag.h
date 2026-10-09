#pragma once
// USB console for bench diagnostics: i2cscan, pmic, imu, stats, fbdump, reboot.

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t diag_start(i2c_master_bus_handle_t bus);
// Records internal-RAM free and minimum at a named boot step; `stats` prints them.
void diag_mem_mark(const char *step);
