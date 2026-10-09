#pragma once
// QMI8658 6-axis IMU at I2C 0x6B.

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    float ax, ay, az;  // g
    float gx, gy, gz;  // degrees per second
    float temp_c;
} imu_sample_t;

esp_err_t imu_init(i2c_master_bus_handle_t bus);
esp_err_t imu_read(imu_sample_t *out);
// Power-down for standby; imu_init() brings it back.
esp_err_t imu_sleep(void);
