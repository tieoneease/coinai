#pragma once
// NimBLE peripheral implementing PROTOCOL.md v1: the coinai service (ctrl + data), Device
// Information and Battery services, LE Secure Connections with a passkey shown on the screen.

#include <stdbool.h>
#include <stdint.h>
#include "audio.h"
#include "esp_err.h"

typedef enum {
    BLE_ADV_OFF,
    BLE_ADV_IDLE,     // 1022.5 ms: awake, nothing pending, so the app can reach status and settings
    BLE_ADV_DELIVER,  // 20 ms for 30 s, then 152.5 ms: a clip is waiting for the phone
} ble_adv_mode_t;

typedef struct {
    bool connected;
    bool authenticated;     // encrypted and MITM-authenticated (always false in bench mode)
    bool bench;             // pairing not required, until reboot (`ble bench`)
    uint32_t passkey;       // shown while pairing; 0 = none
    bool transferring;
    uint16_t tx_clip;
    uint32_t tx_offset;
    uint32_t tx_size;       // 0 while the clip is still being recorded
    uint16_t pending;       // clips stored on the device, including one being recorded
    bool reply_window;      // waiting for SamBot's answer after the last ACK
    uint16_t reply_clip;
    int reply_state;        // -1 = none, else PROTOCOL.md REPLY state
    char reply_text[201];
} ble_status_t;

esp_err_t ble_init(void);
void ble_set_adv_mode(ble_adv_mode_t mode);
void ble_get_status(ble_status_t *out);
// True while a connected phone is fetching clips or the reply window is open: keeps the coin awake.
bool ble_busy(void);
// From the status task; STATUS and the Battery service follow these values.
void ble_note_device_status(int battery_pct, bool charging, bool usb_power, bool recording);
// From the audio callback (must not block).
void ble_clip_event(const audio_clip_result_t *clip, bool started);
// Bench mode: protocol messages accepted without pairing, until the next reboot. USB console only.
void ble_set_bench(bool on);
esp_err_t ble_forget_bonds(void);
// Radio TX power for advertising and the connection, -24..+20 dBm (default +9).
esp_err_t ble_set_tx_power(int dbm);
// RSSI of the connected phone in dBm, or 0 when not connected.
int ble_conn_rssi(void);
// Disconnects and stops advertising before standby.
void ble_shutdown(void);
