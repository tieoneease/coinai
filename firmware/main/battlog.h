#pragma once

// Battery log on flash, for estimating drain and battery life without a current sensor (the
// AXP2101 has no battery-current ADC). One CSV line per record:
//
//     rtc_s,event,vbat_mv,soc_pct,flags
//
// rtc_s: seconds on the RTC timer, which runs since the PMIC powered the chip and through deep
// sleep (not through Off). event: B boot/wake, P periodic, C state change, S standby entry,
// O power off. flags: U USB power, C charging, D screen on, R recording, L phone connected.
// Periodic records come every minute on battery and every 5 minutes on USB.

#include <stdbool.h>
#include <stdint.h>
#include "pmic.h"

#define BATTLOG_USB    0x01
#define BATTLOG_CHG    0x02
#define BATTLOG_SCREEN 0x04
#define BATTLOG_REC    0x08
#define BATTLOG_LINK   0x10

// Called by the status task with every PMIC reading; writes when a record is due.
void battlog_sample(const pmic_status_t *p, uint8_t flags);
// Writes one record now (reads the PMIC itself when p is NULL).
void battlog_event(char event, const pmic_status_t *p, uint8_t flags);
// Console: prints the log (older file first); clears it.
void battlog_print(void);
void battlog_clear(void);
