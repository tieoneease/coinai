#include "settings.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

typedef struct {
    const char *nvs_key;
    uint16_t def, min, max;
} spec_t;

static const spec_t SPECS[] = {
    [SET_REPLY_WAIT_S] = {"reply_wait", 45, 0, 600},
    [SET_BRIGHTNESS] = {"brightness", 100, 1, 100},
    [SET_IDLE_S] = {"idle_s", 30, 5, 3600},
    [SET_OFF_AFTER_H] = {"off_h", 0, 0, 24 * 30},
};
#define NSPECS (sizeof SPECS / sizeof SPECS[0])

static uint16_t s_val[NSPECS];

static bool valid_key(setting_t key)
{
    return key > 0 && key < NSPECS && SPECS[key].nvs_key;
}

void settings_load(void)
{
    nvs_handle_t nvs;
    bool open = nvs_open("cfg", NVS_READONLY, &nvs) == ESP_OK;
    for (setting_t k = 1; k < NSPECS; k++) {
        uint16_t v = SPECS[k].def;
        if (open && nvs_get_u16(nvs, SPECS[k].nvs_key, &v) == ESP_OK && (v < SPECS[k].min || v > SPECS[k].max)) {
            v = SPECS[k].def;
        }
        s_val[k] = v;
    }
    if (open) {
        nvs_close(nvs);
    }
}

uint16_t settings_get(setting_t key)
{
    return valid_key(key) ? s_val[key] : 0;
}

esp_err_t settings_set(setting_t key, uint16_t value)
{
    if (!valid_key(key) || value < SPECS[key].min || value > SPECS[key].max) {
        return ESP_ERR_INVALID_ARG;
    }
    s_val[key] = value;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("cfg", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, SPECS[key].nvs_key, value);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "%s = %u%s", SPECS[key].nvs_key, value, err == ESP_OK ? "" : " (not saved)");
    return err;
}
