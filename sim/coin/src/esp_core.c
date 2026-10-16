// ESP-IDF system services for the simulator: logging, errors, esp_timer, reset and deep sleep, the
// RTC clock and time of day, the app descriptor, heap figures, random numbers, CRC.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "sim_esp.h"
#include "simlink.h"

#ifndef PROJECT_VER
#define PROJECT_VER "sim"
#endif

// ---------- logging (ESP-IDF's format: "I (1234) tag: message") ----------

static esp_log_level_t s_log_level = CONFIG_LOG_DEFAULT_LEVEL;
static struct {
    char tag[24];
    esp_log_level_t level;
} s_tag_levels[16];
static int s_tag_count;
static CRITICAL_SECTION s_log_lock;
static INIT_ONCE s_log_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK log_init(PINIT_ONCE o, void *p, void **c)
{
    InitializeCriticalSection(&s_log_lock);
    return TRUE;
}

void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    InitOnceExecuteOnce(&s_log_once, log_init, NULL, NULL);
    EnterCriticalSection(&s_log_lock);
    if (strcmp(tag, "*") == 0) {
        s_log_level = level;
        s_tag_count = 0;
    } else {
        int i = 0;
        while (i < s_tag_count && strcmp(s_tag_levels[i].tag, tag) != 0) {
            i++;
        }
        if (i < 16) {
            snprintf(s_tag_levels[i].tag, sizeof s_tag_levels[i].tag, "%s", tag);
            s_tag_levels[i].level = level;
            if (i == s_tag_count) {
                s_tag_count++;
            }
        }
    }
    LeaveCriticalSection(&s_log_lock);
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{
    InitOnceExecuteOnce(&s_log_once, log_init, NULL, NULL);
    EnterCriticalSection(&s_log_lock);
    esp_log_level_t limit = s_log_level;
    for (int i = 0; i < s_tag_count; i++) {
        if (strcmp(s_tag_levels[i].tag, tag) == 0) {
            limit = s_tag_levels[i].level;
        }
    }
    if (level > limit || level == ESP_LOG_NONE) {
        LeaveCriticalSection(&s_log_lock);
        return;
    }
    static char line[1024];
    static const char letters[] = "?EWIDV";
    int n = snprintf(line, sizeof line, "%c (%" PRIu32 ") %s: ", letters[level], esp_log_timestamp(), tag);
    va_list ap;
    va_start(ap, format);
    n += vsnprintf(line + n, sizeof line - n - 1, format, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) {
        n = sizeof line - 2;
    }
    line[n++] = '\n';
    fwrite(line, 1, n, stdout);  // one write, so it cannot be split by other output
    LeaveCriticalSection(&s_log_lock);
}

// ---------- errors ----------

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
    default: return "UNKNOWN ERROR";
    }
}

void sim_error_check_failed(esp_err_t rc, const char *file, int line, const char *function, const char *expression)
{
    printf("ESP_ERROR_CHECK failed: esp_err_t 0x%x (%s)\nfile: \"%s\" line %d\nfunc: %s\nexpression: %s\n", rc,
           esp_err_to_name(rc), file, line, function, expression);
    printf("\nabort() was called\nRebooting...\n");
    sim_exit_reset(ESP_RST_PANIC);
}

// ---------- esp_timer: one dispatch thread, like ESP_TIMER_TASK ----------

struct sim_timer {
    esp_timer_create_args_t args;
    bool active;
    int64_t due_us;
    uint64_t period_us;
    struct sim_timer *next;
};

static CRITICAL_SECTION s_tm_lock;
static CONDITION_VARIABLE s_tm_cv;
static struct sim_timer *s_timers;
static INIT_ONCE s_tm_once = INIT_ONCE_STATIC_INIT;

static DWORD WINAPI timer_main(void *arg)
{
    EnterCriticalSection(&s_tm_lock);
    for (;;) {
        int64_t now = esp_timer_get_time();
        struct sim_timer *due = NULL;
        int64_t next = INT64_MAX;
        for (struct sim_timer *t = s_timers; t; t = t->next) {
            if (!t->active) {
                continue;
            }
            if (t->due_us <= now) {
                due = t;
                break;
            }
            if (t->due_us < next) {
                next = t->due_us;
            }
        }
        if (due) {
            if (due->period_us) {
                due->due_us += due->period_us;
            } else {
                due->active = false;
            }
            esp_timer_cb_t cb = due->args.callback;
            void *cb_arg = due->args.arg;
            LeaveCriticalSection(&s_tm_lock);
            cb(cb_arg);
            EnterCriticalSection(&s_tm_lock);
            continue;
        }
        DWORD wait = next == INT64_MAX ? INFINITE : (DWORD)((next - now + 999) / 1000);
        SleepConditionVariableCS(&s_tm_cv, &s_tm_lock, wait);
    }
    return 0;
}

static BOOL CALLBACK timer_init(PINIT_ONCE o, void *p, void **c)
{
    InitializeCriticalSection(&s_tm_lock);
    InitializeConditionVariable(&s_tm_cv);
    CreateThread(NULL, 1024 * 1024, timer_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    return TRUE;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    InitOnceExecuteOnce(&s_tm_once, timer_init, NULL, NULL);
    struct sim_timer *t = calloc(1, sizeof *t);
    t->args = *args;
    EnterCriticalSection(&s_tm_lock);
    t->next = s_timers;
    s_timers = t;
    LeaveCriticalSection(&s_tm_lock);
    *out = t;
    return ESP_OK;
}

static esp_err_t timer_start(esp_timer_handle_t t, uint64_t us, bool periodic)
{
    EnterCriticalSection(&s_tm_lock);
    if (t->active) {
        LeaveCriticalSection(&s_tm_lock);
        return ESP_ERR_INVALID_STATE;
    }
    t->active = true;
    t->due_us = esp_timer_get_time() + (int64_t)us;
    t->period_us = periodic ? us : 0;
    LeaveCriticalSection(&s_tm_lock);
    WakeConditionVariable(&s_tm_cv);
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us)
{
    return timer_start(t, timeout_us, false);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us)
{
    return timer_start(t, period_us, true);
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    EnterCriticalSection(&s_tm_lock);
    esp_err_t err = t->active ? ESP_OK : ESP_ERR_INVALID_STATE;
    t->active = false;
    LeaveCriticalSection(&s_tm_lock);
    return err;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    EnterCriticalSection(&s_tm_lock);
    t->active = false;
    for (struct sim_timer **p = &s_timers; *p; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            break;
        }
    }
    LeaveCriticalSection(&s_tm_lock);
    free(t);
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t t)
{
    return t->active;
}

// ---------- reset, deep sleep, RTC ----------

static uint64_t s_ext1_mask;
static int s_ext1_mode;
static uint64_t s_timer_wakeup_us;

esp_reset_reason_t esp_reset_reason(void)
{
    return (esp_reset_reason_t)sim_env_u64("COINSIM_RESET", ESP_RST_POWERON);
}

void esp_restart(void)
{
    printf("Rebooting...\n");
    sim_exit_reset(ESP_RST_SW);
}

void esp_system_abort(const char *details)
{
    printf("abort() was called: %s\nRebooting...\n", details);
    sim_exit_reset(ESP_RST_PANIC);
}

uint32_t esp_sleep_get_wakeup_causes(void)
{
    return (uint32_t)sim_env_u64("COINSIM_WAKE", 0);
}

esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();
    for (int i = ESP_SLEEP_WAKEUP_EXT0; i <= ESP_SLEEP_WAKEUP_UART; i++) {
        if (causes & (1u << i)) {
            return (esp_sleep_wakeup_cause_t)i;
        }
    }
    return ESP_SLEEP_WAKEUP_UNDEFINED;
}

uint64_t esp_sleep_get_ext1_wakeup_status(void)
{
    return sim_env_u64("COINSIM_EXT1", 0);
}

esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask, esp_sleep_ext1_wakeup_mode_t level_mode)
{
    s_ext1_mask |= io_mask;
    s_ext1_mode = level_mode;
    return ESP_OK;
}

esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t io_mask, esp_sleep_ext1_wakeup_mode_t level_mode)
{
    s_ext1_mask = io_mask;
    s_ext1_mode = level_mode;
    return ESP_OK;
}

esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us)
{
    s_timer_wakeup_us = time_in_us;
    return ESP_OK;
}

void esp_deep_sleep_disable_rom_logging(void)
{
}

void esp_deep_sleep_start(void)
{
    fflush(stdout);
    sim_exit_sleep(s_timer_wakeup_us, s_ext1_mask, s_ext1_mode);
}

uint64_t rtc_time_get(void)
{
    return sim_rtc_us();
}

uint64_t rtc_time_slowclk_to_us(uint64_t rtc_cycles, uint32_t period)
{
    return (rtc_cycles * period) >> 19;
}

uint32_t esp_clk_slowclk_cal_get(void)
{
    return 1u << 19;
}

// System time keeps counting through deep sleep (it runs from the RTC) and starts again at 0
// after power-off, as on the chip.
static RTC_DATA_ATTR int64_t s_utc_offset_us;

int sim_gettimeofday(struct timeval *tv, void *tz)
{
    int64_t us = s_utc_offset_us + (int64_t)sim_rtc_us();
    tv->tv_sec = (long)(us / 1000000);
    tv->tv_usec = (long)(us % 1000000);
    return 0;
}

int sim_settimeofday(const struct timeval *tv, const void *tz)
{
    s_utc_offset_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec - (int64_t)sim_rtc_us();
    return 0;
}

// ---------- app descriptor, heap, random, CRC ----------

const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d;
    if (!d.version[0]) {
        snprintf(d.version, sizeof d.version, "%s", PROJECT_VER);
        snprintf(d.project_name, sizeof d.project_name, "coinai");
        snprintf(d.idf_ver, sizeof d.idf_ver, "v5.5.5 (simulator)");
        snprintf(d.date, sizeof d.date, "%s", __DATE__);
        snprintf(d.time, sizeof d.time, "%s", __TIME__);
    }
    return &d;
}

// The coin's measured figures (HARDWARE.md, phase 4): these describe the device, not this process.
size_t heap_caps_get_free_size(uint32_t caps)
{
    return caps & MALLOC_CAP_SPIRAM ? 7 * 1024 * 1024 : caps & MALLOC_CAP_DMA ? 60 * 1024 : 93 * 1024;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    return caps & MALLOC_CAP_SPIRAM ? 7 * 1024 * 1024 : 54 * 1024;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return caps & MALLOC_CAP_SPIRAM ? 6 * 1024 * 1024 : 40 * 1024;
}

uint32_t esp_get_free_heap_size(void)
{
    return 93 * 1024;
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    return malloc(size);
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    return calloc(n, size);
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    return realloc(ptr, size);
}

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    return _aligned_malloc(size, alignment);
}

void heap_caps_free(void *ptr)
{
    free(ptr);
}

bool esp_ptr_internal(const void *p)
{
    return false;
}

bool esp_ptr_external_ram(const void *p)
{
    return true;
}

void esp_backtrace_get_start(uint32_t *pc, uint32_t *sp, uint32_t *next_pc)
{
    *pc = *sp = *next_pc = 0;
}

bool esp_backtrace_get_next_frame(esp_backtrace_frame_t *frame)
{
    return false;
}

uint32_t esp_cpu_process_stack_pc(uint32_t pc)
{
    return pc;
}

uint32_t esp_random(void)
{
    unsigned int v;
    rand_s(&v);
    return v;
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = (uint8_t)esp_random();
    }
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}

// The ROM's crc32_le: zlib's CRC-32 (reflected 0xEDB88320, inverted in and out), chainable.
uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}
