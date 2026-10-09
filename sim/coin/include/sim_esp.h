#pragma once
// ESP-IDF API subset for the desktop simulator. The ESP-IDF header names in this directory
// (esp_log.h, driver/gpio.h, ...) include this file. Implementations: src/*.c.

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "sim_freertos.h"

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

// ---------- esp_err.h ----------
typedef int esp_err_t;
#define ESP_OK                     0
#define ESP_FAIL                   -1
#define ESP_ERR_NO_MEM             0x101
#define ESP_ERR_INVALID_ARG        0x102
#define ESP_ERR_INVALID_STATE      0x103
#define ESP_ERR_INVALID_SIZE       0x104
#define ESP_ERR_NOT_FOUND          0x105
#define ESP_ERR_NOT_SUPPORTED      0x106
#define ESP_ERR_TIMEOUT            0x107
#define ESP_ERR_INVALID_RESPONSE   0x108
#define ESP_ERR_INVALID_CRC        0x109
#define ESP_ERR_INVALID_VERSION    0x10A
#define ESP_ERR_INVALID_MAC        0x10B
#define ESP_ERR_NOT_FINISHED       0x10C
#define ESP_ERR_NOT_ALLOWED        0x10D
#define ESP_ERR_NVS_BASE           0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED 0x1101
#define ESP_ERR_NVS_NOT_FOUND      0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH  0x1103
#define ESP_ERR_NVS_READ_ONLY      0x1104
#define ESP_ERR_NVS_INVALID_NAME   0x1108
#define ESP_ERR_NVS_INVALID_HANDLE 0x1109
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES  0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
const char *esp_err_to_name(esp_err_t code);
void sim_error_check_failed(esp_err_t rc, const char *file, int line, const char *function, const char *expression)
    __attribute__((noreturn));
#define ESP_ERROR_CHECK(x)                                                         \
    do {                                                                           \
        esp_err_t err_rc_ = (x);                                                   \
        if (unlikely(err_rc_ != ESP_OK)) {                                         \
            sim_error_check_failed(err_rc_, __FILE__, __LINE__, __func__, #x);     \
        }                                                                          \
    } while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x)                                                                       \
    ({                                                                                                         \
        esp_err_t err_rc_ = (x);                                                                               \
        if (unlikely(err_rc_ != ESP_OK)) {                                                                     \
            esp_log_write(ESP_LOG_ERROR, "err", "ESP_ERROR_CHECK_WITHOUT_ABORT failed: %s at %s:%d", \
                          esp_err_to_name(err_rc_), __FILE__, __LINE__);                                       \
        }                                                                                                      \
        err_rc_;                                                                                               \
    })

// ---------- esp_log.h ----------
typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;
void esp_log_level_set(const char *tag, esp_log_level_t level);
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
uint32_t esp_log_timestamp(void);
#define ESP_LOGE(tag, fmt, ...) esp_log_write(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_write(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_write(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)                                        \
    do {                                                               \
        if (0) esp_log_write(ESP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGV(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
#define ESP_EARLY_LOGD ESP_LOGD
#define ESP_DRAM_LOGE ESP_LOGE
#define ESP_DRAM_LOGW ESP_LOGW

// ---------- esp_check.h ----------
#define ESP_RETURN_ON_ERROR(x, log_tag, format, ...)                                      \
    do {                                                                                  \
        esp_err_t err_rc_ = (x);                                                          \
        if (unlikely(err_rc_ != ESP_OK)) {                                                \
            ESP_LOGE(log_tag, "%s(%d): " format, __func__, __LINE__, ##__VA_ARGS__);      \
            return err_rc_;                                                               \
        }                                                                                 \
    } while (0)
#define ESP_RETURN_ON_FALSE(a, err_code, log_tag, format, ...)                            \
    do {                                                                                  \
        if (unlikely(!(a))) {                                                             \
            ESP_LOGE(log_tag, "%s(%d): " format, __func__, __LINE__, ##__VA_ARGS__);      \
            return err_code;                                                              \
        }                                                                                 \
    } while (0)
#define ESP_RETURN_VOID_ON_ERROR(x, log_tag, format, ...)                                 \
    do {                                                                                  \
        esp_err_t err_rc_ = (x);                                                          \
        if (unlikely(err_rc_ != ESP_OK)) {                                                \
            ESP_LOGE(log_tag, "%s(%d): " format, __func__, __LINE__, ##__VA_ARGS__);      \
            return;                                                                       \
        }                                                                                 \
    } while (0)
#define ESP_RETURN_VOID_ON_FALSE(a, log_tag, format, ...)                                 \
    do {                                                                                  \
        if (unlikely(!(a))) {                                                             \
            ESP_LOGE(log_tag, "%s(%d): " format, __func__, __LINE__, ##__VA_ARGS__);      \
            return;                                                                       \
        }                                                                                 \
    } while (0)
#define ESP_GOTO_ON_ERROR(x, goto_tag, log_tag, format, ...)                              \
    do {                                                                                  \
        esp_err_t err_rc_ = (x);                                                          \
        if (unlikely(err_rc_ != ESP_OK)) {                                                \
            ESP_LOGE(log_tag, "%s(%d): " format, __func__, __LINE__, ##__VA_ARGS__);      \
            ret = err_rc_;                                                                \
            goto goto_tag;                                                                \
        }                                                                                 \
    } while (0)
#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, log_tag, format, ...)                    \
    do {                                                                                  \
        if (unlikely(!(a))) {                                                             \
            ESP_LOGE(log_tag, "%s(%d): " format, __func__, __LINE__, ##__VA_ARGS__);      \
            ret = err_code;                                                               \
            goto goto_tag;                                                                \
        }                                                                                 \
    } while (0)

// ---------- esp_attr.h ----------
// RTC slow memory survives deep sleep: the simulator saves this section in SLEEP and restores it.
#define RTC_DATA_ATTR __attribute__((section(".rtcdat")))
#define RTC_NOINIT_ATTR RTC_DATA_ATTR
#define RTC_SLOW_ATTR RTC_DATA_ATTR
#define RTC_FAST_ATTR RTC_DATA_ATTR
#define RTC_RODATA_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define EXT_RAM_ATTR
#define NOINIT_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#define FORCE_INLINE_ATTR static inline __attribute__((always_inline))
#define NOINLINE_ATTR __attribute__((noinline))
#define DMA_ATTR
#define IRAM_BSS_ATTR

// ---------- esp_timer.h ----------
typedef struct sim_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef enum { ESP_TIMER_TASK, ESP_TIMER_ISR } esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
bool esp_timer_is_active(esp_timer_handle_t timer);
int64_t esp_timer_get_time(void);

// ---------- esp_task_wdt.h ----------
esp_err_t esp_task_wdt_add(TaskHandle_t task);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_delete(TaskHandle_t task);

// ---------- esp_system.h ----------
typedef enum {
    ESP_RST_UNKNOWN,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
    ESP_RST_USB,
    ESP_RST_JTAG,
    ESP_RST_EFUSE,
    ESP_RST_PWR_GLITCH,
    ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void) __attribute__((noreturn));
uint32_t esp_get_free_heap_size(void);

// ---------- esp_random.h ----------
uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);

// ---------- esp_rom_crc.h ----------
uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len);

// ---------- esp_app_desc.h ----------
typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);

// ---------- esp_sleep.h ----------
typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_TOUCHPAD,
    ESP_SLEEP_WAKEUP_ULP,
    ESP_SLEEP_WAKEUP_GPIO,
    ESP_SLEEP_WAKEUP_UART,
} esp_sleep_source_t;
typedef esp_sleep_source_t esp_sleep_wakeup_cause_t;
typedef enum { ESP_EXT1_WAKEUP_ANY_LOW = 0, ESP_EXT1_WAKEUP_ANY_HIGH = 1 } esp_sleep_ext1_wakeup_mode_t;
uint32_t esp_sleep_get_wakeup_causes(void);
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void);
uint64_t esp_sleep_get_ext1_wakeup_status(void);
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask, esp_sleep_ext1_wakeup_mode_t level_mode);
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t io_mask, esp_sleep_ext1_wakeup_mode_t level_mode);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us);
void esp_deep_sleep_disable_rom_logging(void);
void esp_deep_sleep_start(void) __attribute__((noreturn));

// ---------- soc/rtc.h, esp_private/esp_clk.h ----------
// The simulated RTC slow clock ticks once per microsecond (calibration 1.0 in Q13.19).
uint64_t rtc_time_get(void);
uint64_t rtc_time_slowclk_to_us(uint64_t rtc_cycles, uint32_t period);
uint32_t esp_clk_slowclk_cal_get(void);

// ---------- driver/gpio.h, driver/rtc_io.h ----------
typedef enum {
    GPIO_NUM_NC = -1,
    GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
    GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15,
    GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_21,
    GPIO_NUM_26 = 26, GPIO_NUM_27, GPIO_NUM_28, GPIO_NUM_29, GPIO_NUM_30, GPIO_NUM_31, GPIO_NUM_32,
    GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_35, GPIO_NUM_36, GPIO_NUM_37, GPIO_NUM_38, GPIO_NUM_39, GPIO_NUM_40,
    GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_43, GPIO_NUM_44, GPIO_NUM_45, GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_48,
    GPIO_NUM_MAX,
} gpio_num_t;
typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
    GPIO_MODE_OUTPUT_OD = 6,
    GPIO_MODE_INPUT_OUTPUT_OD = 7,
    GPIO_MODE_INPUT_OUTPUT = 3,
} gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE = 1,
    GPIO_INTR_NEGEDGE = 2,
    GPIO_INTR_ANYEDGE = 3,
    GPIO_INTR_LOW_LEVEL = 4,
    GPIO_INTR_HIGH_LEVEL = 5,
} gpio_int_type_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;
typedef void (*gpio_isr_t)(void *arg);
esp_err_t gpio_config(const gpio_config_t *cfg);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t isr, void *arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
esp_err_t gpio_hold_en(gpio_num_t pin);
esp_err_t gpio_hold_dis(gpio_num_t pin);
void gpio_deep_sleep_hold_en(void);
void gpio_deep_sleep_hold_dis(void);
esp_err_t rtc_gpio_deinit(gpio_num_t pin);

// ---------- driver/i2c_master.h ----------
typedef struct sim_i2c_bus *i2c_master_bus_handle_t;
typedef struct sim_i2c_dev *i2c_master_dev_handle_t;
typedef enum { I2C_ADDR_BIT_LEN_7 = 0, I2C_ADDR_BIT_LEN_10 = 1 } i2c_addr_bit_len_t;
typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    uint32_t scl_wait_us;
    struct {
        uint32_t disable_ack_check : 1;
    } flags;
} i2c_device_config_t;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *write_buffer, size_t write_size,
                              int xfer_timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *read_buffer, size_t read_size,
                             int xfer_timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *write_buffer, size_t write_size,
                                      uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int xfer_timeout_ms);

// ---------- nvs.h, nvs_flash.h ----------
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_set_i8(nvs_handle_t h, const char *key, int8_t v);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_set_i16(nvs_handle_t h, const char *key, int16_t v);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v);
esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t v);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);
esp_err_t nvs_get_i8(nvs_handle_t h, const char *key, int8_t *v);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v);
esp_err_t nvs_get_i16(nvs_handle_t h, const char *key, int16_t *v);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v);
esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *v);
esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *v);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);

// ---------- esp_heap_caps.h, esp_memory_utils.h ----------
#define MALLOC_CAP_EXEC     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT  (1 << 12)
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps);
bool esp_ptr_internal(const void *p);
bool esp_ptr_external_ram(const void *p);

// ---------- esp_debug_helpers.h, esp_cpu_utils.h ----------
typedef struct {
    uint32_t pc;
    uint32_t sp;
    uint32_t next_pc;
    const void *exc_frame;
} esp_backtrace_frame_t;
void esp_backtrace_get_start(uint32_t *pc, uint32_t *sp, uint32_t *next_pc);
bool esp_backtrace_get_next_frame(esp_backtrace_frame_t *frame);
uint32_t esp_cpu_process_stack_pc(uint32_t pc);

// ---------- esp_console.h ----------
typedef int (*esp_console_cmd_func_t)(int argc, char **argv);
typedef int (*esp_console_cmd_func_with_context_t)(void *context, int argc, char **argv);
typedef struct {
    const char *command;
    const char *help;
    const char *hint;
    esp_console_cmd_func_t func;
    void *argtable;
    esp_console_cmd_func_with_context_t func_w_context;
    void *context;
} esp_console_cmd_t;
typedef struct esp_console_repl_s esp_console_repl_t;
struct esp_console_repl_s {
    esp_err_t (*del)(esp_console_repl_t *repl);
};
typedef struct {
    uint32_t max_history_len;
    const char *history_save_path;
    uint32_t task_stack_size;
    uint32_t task_priority;
    int task_core_id;
    const char *prompt;
    size_t max_cmdline_length;
} esp_console_repl_config_t;
#define ESP_CONSOLE_REPL_CONFIG_DEFAULT()                                                                   \
    {                                                                                                       \
        .max_history_len = 32, .history_save_path = NULL, .task_stack_size = 4096, .task_priority = 2,      \
        .task_core_id = tskNO_AFFINITY, .prompt = NULL, .max_cmdline_length = 0,                            \
    }
typedef struct {
    int unused;
} esp_console_dev_usb_serial_jtag_config_t;
#define ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT() {0}
typedef esp_console_dev_usb_serial_jtag_config_t esp_console_dev_uart_config_t;
#define ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT() {0}
esp_err_t esp_console_new_repl_usb_serial_jtag(const esp_console_dev_usb_serial_jtag_config_t *dev,
                                               const esp_console_repl_config_t *cfg, esp_console_repl_t **out);
esp_err_t esp_console_new_repl_uart(const esp_console_dev_uart_config_t *dev, const esp_console_repl_config_t *cfg,
                                    esp_console_repl_t **out);
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd);
esp_err_t esp_console_register_help_command(void);
esp_err_t esp_console_start_repl(esp_console_repl_t *repl);

// ---------- esp_littlefs.h ----------
typedef struct {
    const char *base_path;
    const char *partition_label;
    const void *partition;
    uint8_t format_if_mount_failed : 1;
    uint8_t read_only : 1;
    uint8_t dont_mount : 1;
    uint8_t grow_on_mount : 1;
} esp_vfs_littlefs_conf_t;
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf);
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label);
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);
