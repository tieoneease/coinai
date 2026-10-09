#include "imu.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

#define QMI8658_ADDR 0x6B
#define QMI8658_ID   0x05

enum {
    REG_WHO_AM_I = 0x00,
    REG_REVISION = 0x01,
    REG_CTRL1 = 0x02,
    REG_CTRL2 = 0x03,
    REG_CTRL3 = 0x04,
    REG_CTRL7 = 0x08,
    REG_TEMP_L = 0x33,
    REG_RESET = 0x60,
};

#define ACCEL_LSB_PER_G   8192.0f  // +-4 g
#define GYRO_LSB_PER_DPS  64.0f    // +-512 dps

static i2c_master_dev_handle_t s_dev;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof buf, 100);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, 100);
}

esp_err_t imu_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");

    uint8_t id[2] = {0};
    ESP_RETURN_ON_ERROR(read_regs(REG_WHO_AM_I, id, 2), TAG, "no QMI8658 at 0x6B");
    ESP_RETURN_ON_FALSE(id[0] == QMI8658_ID, ESP_ERR_NOT_FOUND, TAG, "WHO_AM_I 0x%02x, expected 0x%02x", id[0], QMI8658_ID);

    ESP_RETURN_ON_ERROR(write_reg(REG_RESET, 0xB0), TAG, "soft reset");
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL1, 0x40), TAG, "ctrl1");  // register auto-increment, little endian
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL2, 0x16), TAG, "ctrl2");  // accel +-4 g, 125 Hz
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL3, 0x56), TAG, "ctrl3");  // gyro +-512 dps, 125 Hz
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL7, 0x03), TAG, "ctrl7");  // accel + gyro on
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I 0x%02x, revision 0x%02x", id[0], id[1]);
    return ESP_OK;
}

esp_err_t imu_read(imu_sample_t *s)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    uint8_t b[14];
    ESP_RETURN_ON_ERROR(read_regs(REG_TEMP_L, b, sizeof b), TAG, "read");
    int16_t raw[7];
    for (int i = 0; i < 7; i++) {
        raw[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    }
    s->temp_c = raw[0] / 256.0f;
    s->ax = raw[1] / ACCEL_LSB_PER_G;
    s->ay = raw[2] / ACCEL_LSB_PER_G;
    s->az = raw[3] / ACCEL_LSB_PER_G;
    s->gx = raw[4] / GYRO_LSB_PER_DPS;
    s->gy = raw[5] / GYRO_LSB_PER_DPS;
    s->gz = raw[6] / GYRO_LSB_PER_DPS;
    return ESP_OK;
}

esp_err_t imu_sleep(void)
{
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL7, 0x00), TAG, "ctrl7");  // accel + gyro off
    return write_reg(REG_CTRL1, 0x41);                              // and the internal 2 MHz oscillator
}
