#pragma once
// USB console for bench diagnostics: i2cscan, pmic, imu, stats, fbdump, reboot.

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t diag_start(i2c_master_bus_handle_t bus);
