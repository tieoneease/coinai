#pragma once
// The coin process's connection to the simulator host (sim/IPC.md).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    IPC_HELLO = 0x01,
    IPC_I2C = 0x03,
    IPC_GPIO_OUT = 0x04,
    IPC_FRAME = 0x05,
    IPC_PANEL = 0x06,
    IPC_SPEAKER = 0x07,
    IPC_MIC_STATE = 0x08,
    IPC_SLEEP = 0x09,
    IPC_RESET = 0x0A,
    IPC_BLE_ADV = 0x10,
    IPC_BLE_INDICATE = 0x11,
    IPC_BLE_NOTIFY = 0x12,
    IPC_BLE_WRITE_RESULT = 0x13,
    IPC_BLE_PASSKEY = 0x14,
    IPC_BLE_DISCONNECT = 0x15,
    IPC_BLE_FORGET = 0x16,
    IPC_BLE_CONN_PARAMS = 0x17,
    IPC_BLE_LINK_SETUP = 0x18,
    IPC_BLE_TX_POWER = 0x19,
    IPC_BLE_BATTERY = 0x1A,

    IPC_I2C_RESULT = 0x83,
    IPC_GPIO_IN = 0x84,
    IPC_TOUCH = 0x85,
    IPC_MIC = 0x86,
    IPC_BLE_CONNECTED = 0x90,
    IPC_BLE_IND_DONE = 0x91,
    IPC_BLE_CREDIT = 0x92,
    IPC_BLE_DISCONNECTED = 0x93,
    IPC_BLE_SECURITY = 0x94,
    IPC_BLE_MTU = 0x95,
    IPC_BLE_SUBSCRIBE = 0x96,
    IPC_BLE_WRITE = 0x97,
    IPC_BLE_PASSKEY_REQ = 0x98,
    IPC_BLE_RSSI = 0x99,
    IPC_BLE_READY = 0x9A,
    IPC_BLE_ADV_DONE = 0x9B,
};

static inline void sim_put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void sim_put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static inline void sim_put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }
static inline uint16_t sim_get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t sim_get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Start-up (main.c): environment, RTC memory, connection.
void sim_link_start(void);
const char *sim_env(const char *name);
uint64_t sim_env_u64(const char *name, uint64_t def);
const char *sim_data_dir(void);

// One frame. Thread-safe; a frame is never interleaved with another.
void sim_send(uint8_t type, const void *payload, size_t len);
void sim_send2(uint8_t type, const void *head, size_t head_len, const void *body, size_t body_len);

// One I2C transaction through the host: 0 ACK, 1 NACK, -1 no answer.
int sim_i2c(uint8_t addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen, int timeout_ms);

// The RTC clock in microseconds: counts from power-on, through deep sleep.
uint64_t sim_rtc_us(void);

// Leaving the process the way the chip would.
void sim_exit_reset(int reason) __attribute__((noreturn));
void sim_exit_sleep(uint64_t timer_us, uint64_t ext1_mask, int ext1_mode) __attribute__((noreturn));

// Handlers for host frames, in the modules that own the hardware.
void sim_gpio_input(uint8_t pin, uint8_t level);
void sim_touch_input(bool pressed, uint16_t x, uint16_t y);
void sim_mic_input(const int16_t *pcm, size_t samples);
void sim_ble_input(uint8_t type, const uint8_t *p, size_t len);
