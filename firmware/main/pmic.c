#include "pmic.h"

#include <math.h>
#include <stdio.h>
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pmic";

#define AXP2101_ADDR    0x34
#define AXP2101_IC_TYPE 0x4A

enum {
    REG_STATUS1 = 0x00,
    REG_STATUS2 = 0x01,
    REG_IC_TYPE = 0x03,
    REG_COMMON_CFG = 0x10,
    REG_PWRON_STATUS = 0x20,
    REG_PWROFF_STATUS = 0x21,
    REG_PWROFF_EN = 0x22,
    REG_KEY_TIMING = 0x27,
    REG_ADC_CTRL = 0x30,
    REG_VBAT_H = 0x34,
    REG_VBUS_H = 0x38,
    REG_VSYS_H = 0x3A,
    REG_TDIE_H = 0x3C,
    REG_TS_CTRL = 0x50,
    REG_IPRECHG = 0x61,
    REG_ICC_CHG = 0x62,
    REG_CV_CHG = 0x64,
    REG_DC_ONOFF = 0x80,
    REG_DCDC1_VOL = 0x82,
    REG_LDO_ONOFF0 = 0x90,
    REG_ALDO1_VOL = 0x92,
    REG_BAT_PERCENT = 0xA4,
};

static i2c_master_dev_handle_t s_dev;

esp_err_t pmic_read_reg(uint8_t reg, uint8_t *val)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

esp_err_t pmic_write_reg(uint8_t reg, uint8_t val)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof buf, 100);
}

// ADC results are split across a high register (5 or 6 valid bits) and the next low register.
static esp_err_t read_adc(uint8_t reg_h, uint8_t mask_h, uint16_t *out)
{
    uint8_t h, l;
    ESP_RETURN_ON_ERROR(pmic_read_reg(reg_h, &h), TAG, "adc high");
    ESP_RETURN_ON_ERROR(pmic_read_reg(reg_h + 1, &l), TAG, "adc low");
    *out = (uint16_t)(((h & mask_h) << 8) | l);
    return ESP_OK;
}

esp_err_t pmic_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");

    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(pmic_read_reg(REG_IC_TYPE, &id), TAG, "no AXP2101 at 0x34");
    if (id != AXP2101_IC_TYPE) {
        ESP_LOGW(TAG, "unexpected IC type 0x%02x (AXP2101 is 0x%02x)", id, AXP2101_IC_TYPE);
    }

    // Measure VBAT (bit 0), VBUS (bit 2), VSYS (bit 3) and die temperature (bit 4).
    // The TS channel (bit 1) is left as found: it can gate charging on boards without a thermistor.
    uint8_t adc;
    ESP_RETURN_ON_ERROR(pmic_read_reg(REG_ADC_CTRL, &adc), TAG, "adc ctrl");
    if ((adc & 0x1D) != 0x1D) {
        ESP_RETURN_ON_ERROR(pmic_write_reg(REG_ADC_CTRL, adc | 0x1D), TAG, "adc enable");
    }
    return ESP_OK;
}

const char *pmic_charger_state_name(uint8_t state)
{
    static const char *names[] = {"trickle", "precharge", "constant current", "constant voltage", "done", "not charging"};
    return state < sizeof names / sizeof names[0] ? names[state] : "unknown";
}

esp_err_t pmic_read_status(pmic_status_t *s)
{
    uint8_t st1, st2, pct;
    ESP_RETURN_ON_ERROR(pmic_read_reg(REG_STATUS1, &st1), TAG, "status1");
    ESP_RETURN_ON_ERROR(pmic_read_reg(REG_STATUS2, &st2), TAG, "status2");
    s->vbus_good = st1 & (1u << 5);
    s->battery_present = st1 & (1u << 3);
    s->charging = (st2 >> 5) == 0x01;
    s->discharging = (st2 >> 5) == 0x02;
    s->charger_state = st2 & 0x07;

    uint16_t raw;
    s->vbat_mv = (s->battery_present && read_adc(REG_VBAT_H, 0x1F, &raw) == ESP_OK) ? raw : 0;
    s->vbus_mv = (s->vbus_good && read_adc(REG_VBUS_H, 0x3F, &raw) == ESP_OK) ? raw : 0;
    s->vsys_mv = read_adc(REG_VSYS_H, 0x3F, &raw) == ESP_OK ? raw : 0;
    s->die_temp_c = read_adc(REG_TDIE_H, 0x3F, &raw) == ESP_OK ? 22.0f + (7274 - (int)raw) / 20.0f : NAN;
    s->battery_pct = (s->battery_present && pmic_read_reg(REG_BAT_PERCENT, &pct) == ESP_OK) ? pct : -1;
    return ESP_OK;
}

static int charge_current_ma(uint8_t reg)
{
    uint8_t code = reg & 0x1F;
    return code <= 8 ? code * 25 : 200 + (code - 8) * 100;
}

esp_err_t pmic_log_config(void)
{
    static const uint8_t regs[] = {
        0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x30, 0x40, 0x41, 0x42, 0x50, 0x61, 0x62, 0x63,
        0x64, 0x68, 0x69, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
        0x96, 0x97, 0x98, 0x99, 0x9A,
    };
    uint8_t v[256] = {0};
    char line[128];
    int pos = 0;
    for (size_t i = 0; i < sizeof regs; i++) {
        ESP_RETURN_ON_ERROR(pmic_read_reg(regs[i], &v[regs[i]]), TAG, "read 0x%02x", regs[i]);
        pos += snprintf(line + pos, sizeof line - pos, " %02x=%02x", regs[i], v[regs[i]]);
        if (i % 8 == 7 || i == sizeof regs - 1) {
            ESP_LOGI(TAG, "regs%s", line);
            pos = 0;
        }
    }

    static const int on_ms[] = {128, 512, 1000, 2000};
    static const int off_s[] = {4, 6, 8, 10};
    static const int irq_ms[] = {1000, 1500, 2000, 2500};
    static const char *cv[] = {"?", "4.0 V", "4.1 V", "4.2 V", "4.35 V", "4.4 V", "?", "?"};
    uint8_t key = v[REG_KEY_TIMING];
    ESP_LOGI(TAG, "power-on source 0x%02x, power-off source 0x%02x", v[REG_PWRON_STATUS], v[REG_PWROFF_STATUS]);
    ESP_LOGI(TAG, "PWR key: on-time %d ms, off-time %d s, irq-time %d ms; long-press shutdown %s (action: %s)",
             on_ms[key & 3], off_s[(key >> 2) & 3], irq_ms[(key >> 4) & 3],
             (v[REG_PWROFF_EN] & 0x02) ? "enabled" : "disabled", (v[REG_PWROFF_EN] & 0x01) ? "restart" : "power off");
    ESP_LOGI(TAG, "DCDC1 %s %d mV; ALDO1 %s %d mV",
             (v[REG_DC_ONOFF] & 1) ? "on" : "off", 1500 + (v[REG_DCDC1_VOL] & 0x1F) * 100,
             (v[REG_LDO_ONOFF0] & 1) ? "on" : "off", 500 + (v[REG_ALDO1_VOL] & 0x1F) * 100);
    ESP_LOGI(TAG, "charger: constant current %d mA, precharge %d mA, target %s; TS control 0x%02x",
             charge_current_ma(v[REG_ICC_CHG]), (v[REG_IPRECHG] & 0x0F) * 25, cv[v[REG_CV_CHG] & 7], v[REG_TS_CTRL]);
    return ESP_OK;
}

esp_err_t pmic_set_charge_current_ma(int ma)
{
    uint8_t code;
    if (ma >= 100 && ma <= 200 && ma % 25 == 0) {
        code = ma / 25;
    } else if (ma >= 300 && ma <= 1000 && ma % 100 == 0) {
        code = 8 + (ma - 200) / 100;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t old;
    ESP_RETURN_ON_ERROR(pmic_read_reg(REG_ICC_CHG, &old), TAG, "read charge current");
    uint8_t val = (old & ~0x1F) | code;
    if (val != old) {
        ESP_RETURN_ON_ERROR(pmic_write_reg(REG_ICC_CHG, val), TAG, "write charge current");
    }
    ESP_LOGI(TAG, "charge current %d mA -> %d mA (reg 0x62: 0x%02x -> 0x%02x)", charge_current_ma(old), ma, old, val);
    return ESP_OK;
}

esp_err_t pmic_set_aldo1(bool on)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(pmic_read_reg(REG_LDO_ONOFF0, &v), TAG, "ldo onoff");
    uint8_t nv = on ? (v | 0x01) : (v & ~0x01);
    return nv == v ? ESP_OK : pmic_write_reg(REG_LDO_ONOFF0, nv);
}

esp_err_t pmic_set_long_press_off(bool enable)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(pmic_read_reg(REG_PWROFF_EN, &v), TAG, "pwroff en");
    uint8_t nv = enable ? (v | 0x02) : (v & ~0x02);
    return nv == v ? ESP_OK : pmic_write_reg(REG_PWROFF_EN, nv);
}

esp_err_t pmic_power_off(void)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(pmic_read_reg(REG_COMMON_CFG, &v), TAG, "common cfg");
    return pmic_write_reg(REG_COMMON_CFG, v | 0x01);  // soft power-off: every rail drops
}
