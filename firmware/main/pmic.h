#pragma once
// AXP2101 power management IC at I2C 0x34. Register map from XPowersLib (AXP2101Constants.h).
// Its IRQ line is not routed to the ESP32, so everything is polled.

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    bool vbus_good;
    bool battery_present;
    bool charging;
    bool discharging;
    uint8_t charger_state;  // STATUS2[2:0]; see pmic_charger_state_name()
    int battery_pct;        // fuel gauge, -1 without a battery
    uint16_t vbat_mv;
    uint16_t vbus_mv;
    uint16_t vsys_mv;
    float die_temp_c;
} pmic_status_t;

esp_err_t pmic_init(i2c_master_bus_handle_t bus);
esp_err_t pmic_read_status(pmic_status_t *out);
const char *pmic_charger_state_name(uint8_t state);

// Logs the raw register file plus decoded power-key timing, rails and charger settings.
esp_err_t pmic_log_config(void);

// Constant charge current: 100-200 mA in 25 mA steps, 300-1000 mA in 100 mA steps.
esp_err_t pmic_set_charge_current_ma(int ma);

esp_err_t pmic_read_reg(uint8_t reg, uint8_t *val);
esp_err_t pmic_write_reg(uint8_t reg, uint8_t val);
