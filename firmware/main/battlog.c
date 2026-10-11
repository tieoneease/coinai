#include "battlog.h"

#include <inttypes.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "power.h"

#define LOG_PATH "/c/batt.csv"
#define OLD_PATH "/c/batt0.csv"
#define ROTATE_BYTES (48 * 1024)  // about a day of minutes on battery per file; two files kept
#define BATTERY_PERIOD_US (60 * 1000000LL)
#define USB_PERIOD_US (300 * 1000000LL)

static const char *TAG = "battlog";
static SemaphoreHandle_t s_lock;
static int64_t s_last_us = -1;
static uint8_t s_last_flags = 0xFF;

static uint32_t rtc_seconds(void)
{
    return (uint32_t)(power_ticks_to_us(0, power_rtc_ticks()) / 1000000);
}

static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void write_record(char event, const pmic_status_t *p, uint8_t flags)
{
    char f[8];
    int n = 0;
    const char letters[] = "UCDRL";
    for (int i = 0; i < 5; i++) {
        if (flags & (1 << i)) {
            f[n++] = letters[i];
        }
    }
    f[n] = 0;
    lock();
    FILE *fp = fopen(LOG_PATH, "a");
    if (fp) {
        fprintf(fp, "%" PRIu32 ",%c,%u,%d,%s\n", rtc_seconds(), event, p->vbat_mv, p->battery_pct, f);
        long size = ftell(fp);
        fclose(fp);
        if (size > ROTATE_BYTES) {
            remove(OLD_PATH);
            rename(LOG_PATH, OLD_PATH);
        }
    }
    xSemaphoreGive(s_lock);
}

void battlog_event(char event, const pmic_status_t *p, uint8_t flags)
{
    pmic_status_t st;
    if (!p) {
        if (pmic_read_status(&st) != ESP_OK) {
            return;
        }
        p = &st;
    }
    if (!p->battery_present) {
        return;
    }
    if (p->vbus_good) {
        flags |= BATTLOG_USB;
    }
    if (p->charging) {
        flags |= BATTLOG_CHG;
    }
    write_record(event, p, flags);
    s_last_us = esp_timer_get_time();
    s_last_flags = flags & (BATTLOG_USB | BATTLOG_CHG);
}

void battlog_sample(const pmic_status_t *p, uint8_t flags)
{
    if (!p->battery_present) {
        return;
    }
    uint8_t power = (p->vbus_good ? BATTLOG_USB : 0) | (p->charging ? BATTLOG_CHG : 0);
    int64_t now = esp_timer_get_time();
    if (s_last_us < 0) {
        battlog_event('B', p, flags);
    } else if (power != s_last_flags) {
        battlog_event('C', p, flags);
    } else if (now - s_last_us >= (p->vbus_good ? USB_PERIOD_US : BATTERY_PERIOD_US)) {
        battlog_event('P', p, flags);
    }
}

static void print_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    char line[64];
    while (fgets(line, sizeof line, fp)) {
        fputs(line, stdout);
    }
    fclose(fp);
}

void battlog_print(void)
{
    lock();
    printf("rtc_s,event,vbat_mv,soc_pct,flags\n");
    print_file(OLD_PATH);
    print_file(LOG_PATH);
    printf("battlog end\n");
    xSemaphoreGive(s_lock);
}

void battlog_clear(void)
{
    lock();
    remove(OLD_PATH);
    remove(LOG_PATH);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "cleared");
}
