// coinai firmware, phase 3: push-to-talk from Active, Standby and Off.
//
// PWR hold = record. PWR tap = standby (from Active) or screen on (from standby).
// BOOT held, then PWR held 3 s = power off. A wake by PWR that is still held records at once.

#include <inttypes.h>
#include "audio.h"
#include "bsp/esp-bsp.h"
#include "clipstore.h"
#include "diag.h"
#include "display.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "imu.h"
#include "input.h"
#include "nvs_flash.h"
#include "pmic.h"
#include "power.h"
#include "ui.h"

static const char *TAG = "coinai";

// Until the battery capacity is known (phase 9), charge gently.
#define CHARGE_CURRENT_MA   100
// A PWR release sooner than this is a tap: the audio is discarded.
#define TAP_MS              200
// A ptttest wake records this long, standing in for a PWR hold.
#define TEST_HOLD_MS        1500
#define TEST_SLEEP_S        1
// Idle time before standby after a wake that recorded (phase 4 turns this into the BLE reply window).
#define LINGER_S            10
// Idle time before standby on battery, or before the screen goes off on USB power.
#define IDLE_S              30
#define POWEROFF_COMBO_MS   3000

typedef enum { CTL_CLIP, CTL_STANDBY, CTL_POWEROFF } ctl_type_t;
typedef struct {
    ctl_type_t type;
    audio_clip_result_t clip;
} ctl_msg_t;

static QueueHandle_t s_ctl;
static esp_timer_handle_t s_combo_timer;
static esp_timer_handle_t s_test_timer;
static wake_kind_t s_wake;
static volatile bool s_wake_hold;        // the PWR hold that woke us has not been released yet
static volatile bool s_combo;            // PWR went down with BOOT held: power-off gesture
static volatile bool s_vbus;
static volatile int64_t s_last_activity_us;
static volatile uint32_t s_pwr_presses;
static volatile uint32_t s_boot_presses;
static volatile int64_t s_rec_started_us;
static volatile uint16_t s_last_clip;
static volatile uint32_t s_last_clip_ms;

static void post(ctl_type_t type, const audio_clip_result_t *clip)
{
    ctl_msg_t m = {.type = type};
    if (clip) {
        m.clip = *clip;
    }
    xQueueSend(s_ctl, &m, 0);
}

static void on_clip(const audio_clip_result_t *clip, void *ctx)
{
    if (!clip->discarded) {
        s_last_clip_ms = clip->duration_ms;
        s_last_clip = clip->id;
    }
    post(CTL_CLIP, clip);
}

static void on_combo_timer(void *arg)
{
    bool pwr, boot;
    input_get(&pwr, &boot);
    if (s_combo && pwr && boot) {
        post(CTL_POWEROFF, NULL);
    }
}

static void on_test_timer(void *arg)
{
    audio_rec_stop(false);
}

static void on_input(input_event_t ev, uint32_t held_ms, void *ctx)
{
    s_last_activity_us = esp_timer_get_time();
    bool pwr, boot;
    switch (ev) {
    case INPUT_PWR_DOWN:
        s_pwr_presses++;
        input_get(&pwr, &boot);
        if (boot) {
            s_combo = true;
            esp_timer_start_once(s_combo_timer, POWEROFF_COMBO_MS * 1000);
            ESP_LOGI(TAG, "BOOT+PWR: keep holding %d s to power off", POWEROFF_COMBO_MS / 1000);
            break;
        }
        s_rec_started_us = esp_timer_get_time();
        audio_rec_start();
        ESP_LOGI(TAG, "PWR down: recording");
        break;
    case INPUT_PWR_UP:
        if (s_combo) {
            s_combo = false;
            esp_timer_stop(s_combo_timer);
            break;
        }
        if (s_wake_hold) {  // end of the hold that woke us: always a recording, never a tap
            s_wake_hold = false;
            audio_rec_stop(false);
            ESP_LOGI(TAG, "PWR up: wake recording ends");
            break;
        }
        audio_rec_stop(held_ms < TAP_MS);
        ESP_LOGI(TAG, "PWR up after %" PRIu32 " ms%s", held_ms, held_ms < TAP_MS ? " (tap: standby)" : "");
        if (held_ms < TAP_MS) {
            post(CTL_STANDBY, NULL);
        }
        break;
    case INPUT_BOOT_DOWN:
        s_boot_presses++;
        break;
    case INPUT_BOOT_UP:
        if (s_combo) {
            s_combo = false;
            esp_timer_stop(s_combo_timer);
        }
        break;
    }
}

static void status_task(void *arg)
{
    ui_state_t st = {0};
    int64_t last_log = 0;
    for (;;) {
        st.pmic_ok = pmic_read_status(&st.pmic) == ESP_OK;
        if (st.pmic_ok) {
            s_vbus = st.pmic.vbus_good;
        }
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
            ESP_LOGI(TAG, "battery %d%% %u mV, %s, VBUS %u mV", st.pmic.battery_pct, st.pmic.vbat_mv,
                     pmic_charger_state_name(st.pmic.charger_state), st.pmic.vbus_mv);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static int32_t clamp_us(int64_t us)
{
    return us > INT32_MAX ? INT32_MAX : us < -1 ? -1 : (int32_t)us;
}

static void log_wake_recording(const audio_clip_result_t *clip)
{
    ptt_record_t r = {
        .kind = (uint8_t)s_wake,
        .clip = clip->id,
        .ref_to_boot_us = clamp_us(power_ref_to_boot_us()),
        .first_frame_us = clamp_us(power_ticks_to_us(power_boot_ticks(), audio_first_frame_ticks())),
        .clip_open_us = clamp_us(power_ticks_to_us(power_boot_ticks(), clip->open_ticks)),
        .clip_ms = clip->duration_ms,
        .voiced_ms = clip->voiced_ms,
    };
    power_log_ptt(&r);
    ESP_LOGI(TAG, "%s wake: ref->boot %.1f ms, boot->first frame %.1f ms, boot->file %.1f ms", power_wake_name(s_wake),
             r.ref_to_boot_us / 1000.0, r.first_frame_us / 1000.0, r.clip_open_us / 1000.0);
}

// Owns standby and power-off decisions; runs on the main task after start-up.
static void control_loop(lv_display_t *disp)
{
    bool wake_clip_pending = power_wake_records(s_wake);
    bool linger = wake_clip_pending;  // short idle timeout, then standby even on USB
    bool screen_off = false;
    for (;;) {
        ctl_msg_t m;
        if (xQueueReceive(s_ctl, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
            switch (m.type) {
            case CTL_CLIP:
                s_last_activity_us = esp_timer_get_time();
                if (!wake_clip_pending) {
                    break;
                }
                wake_clip_pending = false;
                if (m.clip.id) {
                    log_wake_recording(&m.clip);
                }
                if (s_wake == WAKE_TIMER_TEST) {
                    if (power_test_series_next()) {
                        power_enter_standby(TEST_SLEEP_S, true);
                    }
                    ESP_LOGI(TAG, "ptttest series done; see pttlog");
                    linger = false;
                }
                break;
            case CTL_STANDBY:
                power_enter_standby(0, false);
                break;
            case CTL_POWEROFF:
                power_off();
                break;
            }
        }

        int64_t now = esp_timer_get_time();
        int64_t idle_ms = (now - s_last_activity_us) / 1000;
        if (disp && bsp_display_lock(50) == ESP_OK) {
            int64_t lv_idle = lv_display_get_inactive_time(disp);
            bsp_display_unlock();
            idle_ms = lv_idle < idle_ms ? lv_idle : idle_ms;
        }
        if (audio_is_recording() || s_wake_hold) {
            continue;
        }
        int64_t limit_ms = (linger ? LINGER_S : IDLE_S) * 1000LL;
        if (idle_ms >= limit_ms) {
            if (linger || !s_vbus) {
                power_enter_standby(0, false);
            } else if (!screen_off) {
                ESP_LOGI(TAG, "idle on USB power: screen off");
                display_set_brightness(0);
                screen_off = true;
            }
        } else if (screen_off && idle_ms < 1000) {
            display_set_brightness(100);
            screen_off = false;
        }
    }
}

void app_main(void)
{
    // Latency-critical part: from here to audio_init(), only what recording needs.
    s_wake = power_boot();
    bool record = power_wake_records(s_wake);
    s_ctl = xQueueCreate(8, sizeof(ctl_msg_t));
    if (record) {
        s_rec_started_us = esp_timer_get_time();
        audio_rec_start();
    }
    s_wake_hold = s_wake == WAKE_PTT || s_wake == WAKE_COLD_PTT;
    ESP_ERROR_CHECK(input_init(on_input, NULL, s_wake_hold));

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
    bool pmic_ok = pmic_init(bus) == ESP_OK;
    if (pmic_ok) {
        pmic_set_aldo1(true);  // standby switched the audio rail off
    } else {
        ESP_LOGE(TAG, "PMIC init failed");
    }
    if (audio_init(on_clip, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed");
    }
    int64_t audio_ready_ms = esp_timer_get_time() / 1000;

    // Everything else.
    ESP_LOGI(TAG, "wake: %s, reset reason %d", power_wake_name(s_wake), (int)esp_reset_reason());
    if (pmic_ok) {
        if (s_wake == WAKE_COLD) {
            pmic_log_config();  // the configuration as found, before anything below changes it
        }
        pmic_set_charge_current_ma(CHARGE_CURRENT_MA);
    }
    if (imu_init(bus) != ESP_OK) {
        ESP_LOGE(TAG, "IMU init failed");
    }
    esp_timer_create_args_t combo = {.callback = on_combo_timer, .name = "combo"};
    esp_timer_create_args_t test = {.callback = on_test_timer, .name = "ptttest"};
    ESP_ERROR_CHECK(esp_timer_create(&combo, &s_combo_timer));
    ESP_ERROR_CHECK(esp_timer_create(&test, &s_test_timer));
    if (s_wake == WAKE_TIMER_TEST) {
        esp_timer_start_once(s_test_timer, TEST_HOLD_MS * 1000);
    }

    lv_display_t *disp = display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
    } else if (bsp_display_lock(1000) == ESP_OK) {
        ui_init();
        bsp_display_unlock();
    }
    s_last_activity_us = esp_timer_get_time();
    xTaskCreatePinnedToCore(status_task, "status", 4096, NULL, 5, NULL, 0);
    ESP_ERROR_CHECK(diag_start(bus));
    ESP_LOGI(TAG, "audio ready at %lld ms, all ready at %lld ms (esp_timer)", audio_ready_ms,
             esp_timer_get_time() / 1000);
    control_loop(disp);
}
