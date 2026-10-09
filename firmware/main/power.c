#include "power.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "ble.h"
#include "display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_private/esp_clk.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "nvs.h"
#include "pmic.h"
#include "soc/rtc.h"

static const char *TAG = "power";

#define PIN_PWR      GPIO_NUM_3
#define PIN_LCD_RST  GPIO_NUM_1
#define PIN_TP_RST   GPIO_NUM_2
#define TEST_SLEEP_S 1
#define LOG_SLOTS    16

// Survive deep sleep (not resets).
static RTC_DATA_ATTR uint64_t s_sleep_enter_ticks;
static RTC_DATA_ATTR uint64_t s_timer_us;
static RTC_DATA_ATTR bool s_test_record;
static RTC_DATA_ATTR int s_test_left;

static wake_kind_t s_kind = WAKE_COLD;
static uint64_t s_boot_ticks;
static int64_t s_ref_to_boot_us = -1;

const char *power_wake_name(wake_kind_t kind)
{
    static const char *names[] = {"cold", "cold-ptt", "ptt", "tap", "timer", "timer-test"};
    return kind <= WAKE_TIMER_TEST ? names[kind] : "?";
}

wake_kind_t power_wake_kind(void)
{
    return s_kind;
}

bool power_wake_records(wake_kind_t kind)
{
    return kind == WAKE_COLD_PTT || kind == WAKE_PTT || kind == WAKE_TIMER_TEST;
}

uint64_t power_rtc_ticks(void)
{
    return rtc_time_get();
}

int64_t power_ticks_to_us(uint64_t from, uint64_t to)
{
    if (to < from) {
        return -(int64_t)rtc_time_slowclk_to_us(from - to, esp_clk_slowclk_cal_get());
    }
    return (int64_t)rtc_time_slowclk_to_us(to - from, esp_clk_slowclk_cal_get());
}

uint64_t power_boot_ticks(void)
{
    return s_boot_ticks;
}

int64_t power_ref_to_boot_us(void)
{
    return s_ref_to_boot_us;
}

wake_kind_t power_boot(void)
{
    s_boot_ticks = power_rtc_ticks();
    uint32_t causes = esp_sleep_get_wakeup_causes();
    bool ext1 = causes & (1u << ESP_SLEEP_WAKEUP_EXT1);
    bool timer = causes & (1u << ESP_SLEEP_WAKEUP_TIMER);
    if (ext1 || timer) {
        gpio_hold_dis(PIN_LCD_RST);
        gpio_hold_dis(PIN_TP_RST);
        gpio_deep_sleep_hold_dis();
    }
    // ext1 left GPIO3 routed to the RTC domain; hand it back to the digital GPIO matrix.
    rtc_gpio_deinit(PIN_PWR);
    gpio_set_direction(PIN_PWR, GPIO_MODE_INPUT);
    bool pwr_held = gpio_get_level(PIN_PWR) == 1;

    if (ext1) {
        s_kind = pwr_held ? WAKE_PTT : WAKE_TAP;
    } else if (timer) {
        s_kind = s_test_record ? WAKE_TIMER_TEST : WAKE_TIMER;
        s_ref_to_boot_us = power_ticks_to_us(s_sleep_enter_ticks, s_boot_ticks) - (int64_t)s_timer_us;
    } else {
        s_kind = pwr_held ? WAKE_COLD_PTT : WAKE_COLD;
        if (esp_reset_reason() == ESP_RST_POWERON) {
            s_ref_to_boot_us = power_ticks_to_us(0, s_boot_ticks);  // the RTC timer starts at chip power-up
        }
    }
    if (s_kind != WAKE_TIMER_TEST) {
        s_test_left = 0;  // any other wake ends a test series
    }
    s_test_record = false;
    return s_kind;
}

void power_log_ptt(const ptt_record_t *rec)
{
    nvs_handle_t nvs;
    if (nvs_open("power", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    ptt_record_t log[LOG_SLOTS] = {0};
    size_t len = sizeof log;
    uint8_t next = 0;
    if (nvs_get_blob(nvs, "pttlog", log, &len) != ESP_OK || len != sizeof log) {
        memset(log, 0, sizeof log);
    }
    nvs_get_u8(nvs, "pttnext", &next);
    log[next % LOG_SLOTS] = *rec;
    nvs_set_blob(nvs, "pttlog", log, sizeof log);
    nvs_set_u8(nvs, "pttnext", (uint8_t)((next + 1) % LOG_SLOTS));
    nvs_commit(nvs);
    nvs_close(nvs);
}

void power_clear_ptt_log(void)
{
    nvs_handle_t nvs;
    if (nvs_open("power", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, "pttlog");
        nvs_erase_key(nvs, "pttnext");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void power_print_ptt_log(void)
{
    nvs_handle_t nvs;
    ptt_record_t log[LOG_SLOTS] = {0};
    size_t len = sizeof log;
    uint8_t next = 0;
    if (nvs_open("power", NVS_READONLY, &nvs) != ESP_OK) {
        printf("no records\n");
        return;
    }
    bool ok = nvs_get_blob(nvs, "pttlog", log, &len) == ESP_OK && len == sizeof log;
    nvs_get_u8(nvs, "pttnext", &next);
    nvs_close(nvs);
    if (!ok) {
        printf("no records\n");
        return;
    }
    printf("wake        clip  ref->boot  boot->1st frame  boot->file  ref->1st frame  clip ms  voiced ms\n");
    for (int i = 0; i < LOG_SLOTS; i++) {
        const ptt_record_t *r = &log[(next + i) % LOG_SLOTS];
        if (r->clip == 0) {
            continue;
        }
        char ref[16] = "unknown", total[16] = "unknown";
        if (r->ref_to_boot_us >= 0) {
            snprintf(ref, sizeof ref, "%.1f", r->ref_to_boot_us / 1000.0);
            snprintf(total, sizeof total, "%.1f", (r->ref_to_boot_us + r->first_frame_us) / 1000.0);
        }
        printf("%-10s %5u %10s %16.1f %11.1f %15s %8" PRIu32 " %10" PRIu32 "\n", power_wake_name(r->kind), r->clip, ref,
               r->first_frame_us / 1000.0, r->clip_open_us / 1000.0, total, r->clip_ms, r->voiced_ms);
    }
}

void power_test_series_begin(int n)
{
    s_test_left = n;
}

bool power_test_series_next(void)
{
    if (s_test_left > 0) {
        s_test_left--;
    }
    return s_test_left > 0;
}

static void peripherals_down(void)
{
    ble_shutdown();  // the phone sees the link end now instead of after the supervision timeout
    audio_shutdown();
    imu_sleep();
    display_set_brightness(0);
}

void power_enter_standby(uint32_t timer_s, bool test_record)
{
    ESP_LOGI(TAG, "standby: timer %" PRIu32 " s%s", timer_s, test_record ? ", records on wake" : "");
    esp_task_wdt_delete(NULL);  // the wait for PWR release below may be long; not subscribed is fine
    peripherals_down();

    // Hold the panel and touch controllers in reset: the panel stops scanning and touch stops
    // polling. The BSP re-initialises both on wake.
    gpio_set_direction(PIN_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_RST, 0);
    gpio_set_direction(PIN_TP_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_TP_RST, 0);
    gpio_hold_en(PIN_LCD_RST);
    gpio_hold_en(PIN_TP_RST);
    gpio_deep_sleep_hold_en();

    pmic_set_aldo1(false);  // audio analog rail

    // A PWR press still in progress would wake us at once.
    while (gpio_get_level(PIN_PWR) == 1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_PWR, ESP_EXT1_WAKEUP_ANY_HIGH);
    s_timer_us = (uint64_t)timer_s * 1000000ULL;
    if (timer_s) {
        esp_sleep_enable_timer_wakeup(s_timer_us);
    }
    s_test_record = test_record;
    esp_deep_sleep_disable_rom_logging();  // the ROM banner costs time on every wake
    s_sleep_enter_ticks = power_rtc_ticks();
    esp_deep_sleep_start();
}

void power_off(void)
{
    ESP_LOGW(TAG, "power off");
    esp_task_wdt_delete(NULL);
    peripherals_down();
    vTaskDelay(pdMS_TO_TICKS(50));  // let the log drain
    pmic_power_off();
    // The rails fall within milliseconds. If they do not (the PMIC refused), say so.
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGE(TAG, "still running 2 s after PMIC power-off");
    esp_restart();
}
