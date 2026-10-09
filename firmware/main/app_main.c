// coinai firmware, phase 1: board bring-up.

#include "audio.h"
#include "bsp/esp-bsp.h"
#include "clipstore.h"
#include "diag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "input.h"
#include "nvs_flash.h"
#include "pmic.h"
#include "ui.h"

static const char *TAG = "coinai";

// Until the battery capacity is known (phase 9), charge gently.
#define CHARGE_CURRENT_MA 100
// A PWR release sooner than this is a tap: the audio is discarded.
#define TAP_MS 200

static volatile uint32_t s_pwr_presses;
static volatile uint32_t s_boot_presses;
static volatile int64_t s_rec_started_us;
static volatile uint16_t s_last_clip;
static volatile uint32_t s_last_clip_ms;

static void on_clip(const audio_clip_result_t *clip, void *ctx)
{
    if (!clip->discarded) {
        s_last_clip_ms = clip->duration_ms;
        s_last_clip = clip->id;
    }
}

static void on_input(input_event_t ev, uint32_t held_ms, void *ctx)
{
    switch (ev) {
    case INPUT_PWR_DOWN:
        s_pwr_presses++;
        s_rec_started_us = esp_timer_get_time();
        audio_rec_start();
        ESP_LOGI(TAG, "PWR down: recording");
        break;
    case INPUT_PWR_UP:
        audio_rec_stop(held_ms < TAP_MS);
        ESP_LOGI(TAG, "PWR up after %lu ms%s", (unsigned long)held_ms, held_ms < TAP_MS ? " (tap)" : "");
        break;
    case INPUT_BOOT_DOWN:
        s_boot_presses++;
        ESP_LOGI(TAG, "BOOT down");
        break;
    case INPUT_BOOT_UP:
        ESP_LOGI(TAG, "BOOT up after %lu ms", (unsigned long)held_ms);
        break;
    }
}

static void status_task(void *arg)
{
    ui_state_t st = {0};
    int64_t last_log = 0;
    for (;;) {
        st.pmic_ok = pmic_read_status(&st.pmic) == ESP_OK;
        st.imu_ok = imu_read(&st.imu) == ESP_OK;
        input_get(&st.pwr_pressed, &st.boot_pressed);
        st.pwr_presses = s_pwr_presses;
        st.boot_presses = s_boot_presses;
        int64_t now = esp_timer_get_time();
        st.uptime_s = (uint32_t)(now / 1000000);
        st.recording = audio_is_recording();
        st.mic_level = audio_level();
        st.rec_seconds = st.recording ? (uint32_t)((now - s_rec_started_us) / 1000000) : 0;
        st.last_clip = s_last_clip;
        st.last_clip_ms = s_last_clip_ms;

        if (bsp_display_lock(100) == ESP_OK) {
            ui_update(&st);
            bsp_display_unlock();
        }
        if (now - last_log >= 30 * 1000000LL) {
            last_log = now;
            ESP_LOGI(TAG, "battery %d%% %u mV, %s, VBUS %u mV, accel %.2f %.2f %.2f g", st.pmic.battery_pct,
                     st.pmic.vbat_mv, pmic_charger_state_name(st.pmic.charger_state), st.pmic.vbus_mv,
                     (double)st.imu.ax, (double)st.imu.ay, (double)st.imu.az);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot: reset reason %d", (int)esp_reset_reason());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    if (clipstore_init() != ESP_OK) {
        ESP_LOGE(TAG, "clip storage unavailable");
    }

    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    if (pmic_init(bus) == ESP_OK) {
        pmic_log_config();  // factory/previous configuration, before anything is changed
        pmic_set_charge_current_ma(CHARGE_CURRENT_MA);
    } else {
        ESP_LOGE(TAG, "PMIC init failed");
    }
    if (imu_init(bus) != ESP_OK) {
        ESP_LOGE(TAG, "IMU init failed");
    }
    if (audio_init(on_clip, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed");
    }

    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
    } else if (bsp_display_lock(1000) == ESP_OK) {
        ui_init();
        bsp_display_unlock();
    }

    ESP_ERROR_CHECK(input_init(on_input, NULL));
    xTaskCreatePinnedToCore(status_task, "status", 4096, NULL, 5, NULL, 0);
    ESP_ERROR_CHECK(diag_start(bus));
    ESP_LOGI(TAG, "ready after %lld ms", esp_timer_get_time() / 1000);
}
