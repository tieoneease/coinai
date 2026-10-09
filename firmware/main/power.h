#pragma once
// Standby = ESP32 deep sleep, woken by PWR (GPIO3 high, ext1) or a timer. Off = PMIC off.

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WAKE_COLD,        // power-on or reset, PWR not held
    WAKE_COLD_PTT,    // power-on with PWR still held (pressed from Off): record now
    WAKE_PTT,         // PWR woke us from standby and is still held: record now
    WAKE_TAP,         // PWR woke us but was already released: screen on, no recording
    WAKE_TIMER,       // timer wake
    WAKE_TIMER_TEST,  // timer wake from `sleep <s> rec` / `ptttest`: behaves like WAKE_PTT
} wake_kind_t;

// Call first thing in app_main: reads the wake cause, releases pins held through sleep.
wake_kind_t power_boot(void);
wake_kind_t power_wake_kind(void);
const char *power_wake_name(wake_kind_t kind);
bool power_wake_records(wake_kind_t kind);  // the kinds that start a recording at boot

// RTC slow-clock ticks since the chip powered up. Keeps counting through deep sleep.
uint64_t power_rtc_ticks(void);
// Microseconds from `from` to `to` ticks with the current slow-clock calibration.
int64_t power_ticks_to_us(uint64_t from, uint64_t to);
// Microseconds from this boot's reference moment to power_boot(): the timer alarm for timer
// wakes, chip power-up for power-on boots. -1 when unknown (PWR wakes from standby, resets).
int64_t power_ref_to_boot_us(void);
uint64_t power_boot_ticks(void);

// Latency records for recordings started by a wake, kept in NVS (last 16).
typedef struct {
    uint8_t kind;           // wake_kind_t
    uint16_t clip;
    int32_t ref_to_boot_us; // -1 when unknown
    int32_t first_frame_us; // from power_boot()
    int32_t clip_open_us;   // from power_boot()
    uint32_t clip_ms;
    uint32_t voiced_ms;
} ptt_record_t;
void power_log_ptt(const ptt_record_t *rec);
void power_print_ptt_log(void);
void power_clear_ptt_log(void);

// `ptttest n`: n cycles of standby -> timer wake -> record like a PWR hold.
void power_test_series_begin(int n);
// Called after a test recording: true if another cycle should run (and counts it).
bool power_test_series_next(void);

// Shuts the peripherals down and enters deep sleep. timer_s = 0 means PWR-only wake.
// test_record makes the timer wake behave like a PWR hold. Does not return.
void power_enter_standby(uint32_t timer_s, bool test_record);
// Finalises any recording and switches the PMIC off. Does not return.
void power_off(void);
