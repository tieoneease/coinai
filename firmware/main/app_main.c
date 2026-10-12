// coinai firmware: push-to-talk from Active, Standby and Off; clips go to SamBot over Wi-Fi or to
// the phone over BLE (net.h).
//
// PWR hold = record. PWR tap = standby once the phone has its clips (from Active) or screen on
// (from standby). BOOT held, then PWR held 3 s = power off. A wake by PWR that is still held
// records at once. BOOT held for 1 s while recording discards the clip.

#include <inttypes.h>
#include "audio.h"
#include "battlog.h"
#include "ble.h"
#include "bsp/esp-bsp.h"
#include "clipstore.h"
#include "diag.h"
#include "display.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "imu.h"
#include "input.h"
#include "net.h"
#include "nvs_flash.h"
#include "pmic.h"
#include "power.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "coinai";

// Until the battery capacity is known (phase 9), charge gently.
#define CHARGE_CURRENT_MA   100
// A PWR release sooner than this is a tap: the audio is discarded.
#define TAP_MS              200
// A ptttest wake records this long, standing in for a PWR hold.
#define TEST_HOLD_MS        1500
#define TEST_SLEEP_S        1
// Idle time before standby after a wake that recorded, once the phone has the clip and the
// reply window is over.
#define LINGER_S            10
// How long the coin stays awake for the phone after a clip while clips are pending.
#define DELIVERY_S          60
// After a PWR tap, how long to wait for a phone that has not connected yet.
#define TAP_DELIVERY_S      10
#define POWEROFF_COMBO_MS   3000
// BOOT held this long while recording discards the clip (a phone streaming it gets ERROR 1).
#define DISCARD_HOLD_MS     1000
// The eyes get sleepy this long before the idle timeout.
#define DROWSY_MS           4000
// Provisional until phase 9 measures the cell: about 5% left on a Li-ion cell, and the 3.3 V
// buck has no headroom below this. 15 readings = 3 s, so radio bursts do not trigger it.
#define LOW_BATTERY_MV      3450
#define LOW_BATTERY_SAMPLES 15
// A wake by PWR counts as a tap (screen on, no clip) when the whole press is shorter than this.
#define WAKE_TAP_MS         300
// Deep-sleep wake to power_boot(), measured with ptttest (112-115 ms). PWR wakes have no
// timestamp of their own, so their press length is estimated with it.
#define WAKE_TO_BOOT_MS     113

typedef enum { CTL_CLIP, CTL_STANDBY, CTL_POWEROFF, CTL_POWEROFF_BATTERY, CTL_WAKE_WAS_TAP } ctl_type_t;
typedef struct {
    ctl_type_t type;
    audio_clip_result_t clip;
} ctl_msg_t;

static QueueHandle_t s_ctl;
static esp_timer_handle_t s_combo_timer;
static esp_timer_handle_t s_test_timer;
static esp_timer_handle_t s_discard_timer;
static volatile bool s_discard_armed;
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

static void on_clip(const audio_clip_result_t *clip, bool started, void *ctx)
{
    if (started) {
        net_prepare(clip->id);  // before BLE announces the clip: Wi-Fi first may claim it
    }
    ble_clip_event(clip, started);
    if (started) {
        return;
    }
    ui_event(UI_EV_CLIP_DONE, clip);
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

static void on_discard_timer(void *arg)
{
    bool pwr, boot;
    input_get(&pwr, &boot);
    if (s_discard_armed && boot && audio_is_recording()) {
        ESP_LOGI(TAG, "BOOT held while recording: clip discarded");
        ui_event(UI_EV_DISCARDED, NULL);
        audio_rec_stop(true);
    }
    s_discard_armed = false;
}

static void ui_standby_hook(void)
{
    post(CTL_STANDBY, NULL);
}

static void ui_poweroff_hook(void)
{
    post(CTL_POWEROFF, NULL);
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
            ui_event(UI_EV_COMBO_START, NULL);
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
            ui_event(UI_EV_COMBO_CANCEL, NULL);
            break;
        }
        if (s_wake_hold) {  // end of the press that woke us
            s_wake_hold = false;
            int64_t ref = power_ref_to_boot_us();
            int64_t pressed_ms = power_ticks_to_us(power_boot_ticks(), power_rtc_ticks()) / 1000 +
                                 (ref >= 0 ? ref / 1000 : WAKE_TO_BOOT_MS);
            bool tap = pressed_ms < WAKE_TAP_MS;
            audio_rec_stop(tap);
            ESP_LOGI(TAG, "PWR up: wake press of ~%lld ms%s", pressed_ms, tap ? " (tap: no recording)" : "");
            if (tap) {
                post(CTL_WAKE_WAS_TAP, NULL);
            }
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
        ui_event(UI_EV_BOOT_DOWN, NULL);
        if (audio_is_recording() && !s_combo) {
            s_discard_armed = true;
            esp_timer_start_once(s_discard_timer, DISCARD_HOLD_MS * 1000);
            ui_event(UI_EV_DISCARD_ARMED, NULL);
        }
        break;
    case INPUT_BOOT_UP:
        if (s_combo) {
            s_combo = false;
            esp_timer_stop(s_combo_timer);
            ui_event(UI_EV_COMBO_CANCEL, NULL);
        }
        if (s_discard_armed) {
            s_discard_armed = false;
            esp_timer_stop(s_discard_timer);
            ui_event(UI_EV_DISCARD_CANCEL, NULL);
        }
        ui_event(UI_EV_BOOT_UP, NULL);
        break;
    }
}

static void status_task(void *arg)
{
    ui_state_t st = {0};
    int64_t last_log = 0;
    int low_samples = 0;
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        st.pmic_ok = pmic_read_status(&st.pmic) == ESP_OK;
        // Low battery: below LOW_BATTERY_MV for LOW_BATTERY_SAMPLES readings in a row, off USB
        // power, the coin powers off (any recording is finalised first).
        if (st.pmic_ok && st.pmic.battery_present && !st.pmic.vbus_good && st.pmic.vbat_mv < LOW_BATTERY_MV) {
            if (++low_samples == LOW_BATTERY_SAMPLES) {
                ESP_LOGW(TAG, "battery at %u mV: powering off", st.pmic.vbat_mv);
                post(CTL_POWEROFF_BATTERY, NULL);
            }
        } else {
            low_samples = 0;
        }
        if (st.pmic_ok) {
            s_vbus = st.pmic.vbus_good;
            net_note_power(st.pmic.battery_present ? st.pmic.vbat_mv : 0, s_vbus);
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
        ble_note_device_status(st.pmic_ok && st.pmic.battery_present ? st.pmic.battery_pct : -1,
                               st.pmic_ok && st.pmic.charging,
                               s_vbus, st.recording);
        ble_get_status(&st.ble);
        net_status_t ns;
        net_get_status(&ns);
        if (st.pmic_ok) {
            battlog_sample(&st.pmic, (display_get_brightness() > 0 ? BATTLOG_SCREEN : 0) |
                                         (st.recording ? BATTLOG_REC : 0) | (st.ble.connected ? BATTLOG_LINK : 0) |
                                         (ns.wifi_up ? BATTLOG_WIFI : 0));
        }

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
//
// The coin stays awake while it records, while the phone is fetching clips or waiting for
// SamBot's reply (ble_busy), while Wi-Fi delivers (net_busy), and for DELIVERY_S after a clip
// while clips are pending. Otherwise:
// standby after the idle time on battery, screen off on USB power.
static void control_loop(lv_display_t *disp)
{
    bool wake_clip_pending = power_wake_records(s_wake);
    bool linger = wake_clip_pending;  // short idle timeout, then standby even on USB
    bool screen_off = false;
    bool standby_asap = false;        // PWR tap: standby as soon as the phone is served
    int64_t standby_req_us = 0;
    int64_t deliver_until_us = esp_timer_get_time() + DELIVERY_S * 1000000LL;
    int last_reply_state = -1;
    uint16_t last_reply_clip = 0;
    uint16_t last_pending = 0;
    bool drowsy = false;
    esp_task_wdt_add(NULL);  // standby and power-off decisions must not hang (D5)
    for (;;) {
        esp_task_wdt_reset();
        ctl_msg_t m;
        if (xQueueReceive(s_ctl, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
            switch (m.type) {
            case CTL_CLIP:
                s_last_activity_us = esp_timer_get_time();
                if (m.clip.id && !m.clip.discarded) {
                    deliver_until_us = esp_timer_get_time() + DELIVERY_S * 1000000LL;
                    ble_set_adv_mode(BLE_ADV_DELIVER);
                    net_kick(m.clip.id, false);
                }
                if (!wake_clip_pending) {
                    break;
                }
                wake_clip_pending = false;
                if (m.clip.id && !m.clip.discarded) {
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
                standby_asap = true;
                standby_req_us = esp_timer_get_time();
                if (!screen_off) {
                    ui_goodbye(UI_BYE_STANDBY);
                    display_set_brightness(0);
                    screen_off = true;
                }
                break;
            case CTL_POWEROFF:
            case CTL_POWEROFF_BATTERY:
                ui_set_drowsy(false);
                ui_goodbye(m.type == CTL_POWEROFF ? UI_BYE_POWEROFF : UI_BYE_BATTERY);
                power_off();
                break;
            case CTL_WAKE_WAS_TAP:
                linger = false;  // a screen-on wake: normal idle rules
                break;
            }
        }

        int64_t now = esp_timer_get_time();
        ble_status_t bs;
        ble_get_status(&bs);
        if (bs.reply_state != last_reply_state || bs.reply_clip != last_reply_clip) {
            last_reply_state = bs.reply_state;
            last_reply_clip = bs.reply_clip;
            s_last_activity_us = now;  // a reply arrived: give the user time to read it
            standby_asap = false;
        }
        if (bs.pending > last_pending && !wake_clip_pending) {
            // More for the phone: a new clip, or one Wi-Fi gave back.
            deliver_until_us = now + DELIVERY_S * 1000000LL;
            ble_set_adv_mode(BLE_ADV_DELIVER);
        }
        last_pending = bs.pending;
        if (bs.pending == 0 && !audio_is_recording()) {
            ble_set_adv_mode(BLE_ADV_IDLE);
        }
        int64_t idle_ms = (now - s_last_activity_us) / 1000;
        if (disp && bsp_display_lock(50) == ESP_OK) {
            int64_t lv_idle = lv_display_get_inactive_time(disp);
            bsp_display_unlock();
            idle_ms = lv_idle < idle_ms ? lv_idle : idle_ms;
        }
        if (standby_asap && idle_ms + 300 < (now - standby_req_us) / 1000) {
            standby_asap = false;  // touched or pressed again since the tap
            if (screen_off && !audio_is_recording()) {
                ui_screen_on();
                display_set_brightness(settings_get(SET_BRIGHTNESS));
                screen_off = false;
            }
        }
        bool delivering = bs.pending > 0 && now < deliver_until_us;
        if (standby_asap) {
            // After a tap only the transfer itself is waited for: no reply window, and at most
            // TAP_DELIVERY_S for a phone that has not connected yet.
            delivering = delivering && (bs.connected || now - standby_req_us < TAP_DELIVERY_S * 1000000LL);
            if (!(audio_is_recording() || s_wake_hold || bs.passkey || delivering || net_sending())) {
                power_enter_standby(0, false);
            }
            continue;
        }
        bool keep_awake = audio_is_recording() || s_wake_hold || ble_busy() || net_busy() || bs.passkey ||
                          delivering || ui_keep_awake();
        int64_t limit_ms = (linger ? LINGER_S : settings_get(SET_IDLE_S)) * 1000LL;
        bool want_drowsy = !keep_awake && !screen_off && idle_ms >= limit_ms - DROWSY_MS;
        if (want_drowsy != drowsy) {
            drowsy = want_drowsy;
            ui_set_drowsy(drowsy);
        }
        if (screen_off && idle_ms < 1000 && !audio_is_recording()) {
            ui_screen_on();  // touched or pressed: the eyes wake, then the light comes on
        }
        if (keep_awake) {
            if (screen_off && idle_ms < 1000) {
                display_set_brightness(settings_get(SET_BRIGHTNESS));
                screen_off = false;
            }
            continue;
        }
        if (idle_ms >= limit_ms) {
            if (linger || !s_vbus) {
                ui_goodbye(UI_BYE_STANDBY);
                if (audio_is_recording() || s_wake_hold) {
                    continue;  // PWR went down during the goodbye
                }
                power_enter_standby(0, false);
            } else if (!screen_off) {
                ESP_LOGI(TAG, "idle on USB power: screen off");
                ui_goodbye(UI_BYE_SCREEN_OFF);
                display_set_brightness(0);
                screen_off = true;
                drowsy = false;
                ui_set_drowsy(false);
            }
        } else if (screen_off && idle_ms < 1000) {
            display_set_brightness(settings_get(SET_BRIGHTNESS));
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
    ui_preinit();
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
    settings_load();
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
    diag_mem_mark("audio");
    clipstore_sweep();  // until then, tombstoned clips are only hidden

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
    esp_timer_create_args_t discard = {.callback = on_discard_timer, .name = "discard"};
    ESP_ERROR_CHECK(esp_timer_create(&combo, &s_combo_timer));
    ESP_ERROR_CHECK(esp_timer_create(&test, &s_test_timer));
    ESP_ERROR_CHECK(esp_timer_create(&discard, &s_discard_timer));
    if (s_wake == WAKE_TIMER_TEST) {
        esp_timer_start_once(s_test_timer, TEST_HOLD_MS * 1000);
    }

    diag_mem_mark("imu");
    lv_display_t *disp = display_start();
    diag_mem_mark("display");
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
    } else {
        display_set_brightness(settings_get(SET_BRIGHTNESS));
        if (bsp_display_lock(1000) == ESP_OK) {
            const ui_hooks_t hooks = {.standby = ui_standby_hook, .power_off = ui_poweroff_hook};
            ui_init(&hooks, s_wake);
            bsp_display_unlock();
        }
    }
    diag_mem_mark("ui");
    esp_err_t ble_err = ble_init();
    diag_mem_mark("ble");
    if (ble_err == ESP_OK) {
        ble_status_t bs;
        ble_get_status(&bs);
        ble_set_adv_mode(record || bs.pending ? BLE_ADV_DELIVER : BLE_ADV_IDLE);
    } else {
        ESP_LOGE(TAG, "BLE init failed");
    }
    if (net_init() == ESP_OK) {
        net_kick(0, false);  // clips left from before go now if a known network answers
    } else {
        ESP_LOGE(TAG, "Wi-Fi delivery init failed");
    }
    diag_mem_mark("net");
    s_last_activity_us = esp_timer_get_time();
    xTaskCreatePinnedToCoreWithCaps(status_task, "status", 4096, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(diag_start(bus));
    diag_mem_mark("diag");
    ESP_LOGI(TAG, "audio ready at %lld ms, all ready at %lld ms (esp_timer)", audio_ready_ms,
             esp_timer_get_time() / 1000);
    control_loop(disp);
}
