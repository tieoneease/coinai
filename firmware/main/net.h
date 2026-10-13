#pragma once
// Wi-Fi delivery (PLAN.md D10): with a known network in range and a SamBot token stored, the coin
// sends clips straight to SamBot (sambot.c) and shows the reply itself. Wi-Fi comes first; the
// phone path stays for everything else: when the phone is already connected, when no known network
// answers, when the battery is low, and for clips Wi-Fi could not deliver. Wi-Fi is on only while
// it delivers (and briefly after); off in standby.
//
// The UI sees a Wi-Fi delivery through the same ble_status_t fields as a phone transfer (see
// net_overlay()), and its replies through ble_note_reply().

#include <stdbool.h>
#include <stdint.h>
#include "ble.h"
#include "esp_err.h"

#define NET_MAX_NETWORKS 4

typedef enum {
    NET_IDLE,
    NET_JOINING,       // Wi-Fi on, waiting for an address
    NET_UPLOADING,     // the clip is going to /voice/transcribe
    NET_TRANSCRIBING,  // uploaded, waiting for the transcript
    NET_PROMPTING,     // the transcript is going to /prompt
    NET_WAITING,       // prompted: collecting the reply
} net_phase_t;

typedef struct {
    bool configured;   // at least one network and a token
    bool ready;        // a new clip would go over Wi-Fi now (configured, mode, battery, no back-off)
    net_phase_t phase;
    uint16_t clip;     // the clip in flight (phase > NET_JOINING)
    uint32_t sent, size;
    bool wifi_up;      // associated with an address
} net_status_t;

esp_err_t net_init(void);
// Deliver stored clips over Wi-Fi now (clip 0: all of them, oldest first). force: also when the
// mode is "phone", after a failure, or with the phone connected (console `net send`).
void net_kick(uint16_t clip, bool force);
// A recording started (audio callback; does not block), or is running at boot (after net_init).
// Wi-Fi first: the clip is claimed for Wi-Fi from its first byte and the network joined meanwhile,
// so the upload starts at release.
void net_prepare(uint16_t clip);
// Battery and USB power (vbat_mv 0: no battery, or no reading). Wi-Fi waits for the first call.
void net_note_power(uint16_t vbat_mv, bool usb);
void net_get_status(net_status_t *out);
// Shows a Wi-Fi delivery in the phone-transfer fields the UI already follows: upload progress as a
// transfer, then the clip as gone and a reply window while SamBot answers.
void net_overlay(ble_status_t *bs);
bool net_busy(void);             // delivering: stay awake
bool net_sending(void);          // the prompt is not out yet: what a PWR tap still waits for
void net_shutdown(void);         // standby or power-off: stop now (a reply still lands in SamBot)

// Settings, from the USB console and BLE WIFI_SET. Passwords and the token are never printed.
esp_err_t net_wifi_add(const char *ssid, const char *pass);  // pass "" = open network; at most 4, oldest dropped
esp_err_t net_wifi_forget(const char *ssid);                 // NULL: all
esp_err_t net_set_token(const char *token);                  // NULL or "": clear
esp_err_t net_set_url(const char *url);
// On-coin setup (ui.c, Settings > Wi-Fi): look for networks, then try one. Both run in the net task,
// after a delivery in progress; poll net_ui_state(). A network is saved (as the newest) only when it
// joined.
typedef enum {
    NET_UI_IDLE,
    NET_UI_SCANNING,
    NET_UI_SCANNED,   // net_ui_aps()
    NET_UI_JOINING,
    NET_UI_CHECKING,  // joined: asking SamBot
    NET_UI_DONE,      // net_ui_result()
} net_ui_state_t;

typedef enum {
    NET_JOIN_OK,
    NET_JOIN_WRONG_PASSWORD,
    NET_JOIN_NOT_FOUND,
    NET_JOIN_UNSUPPORTED,  // its security (WEP, WPA-TKIP only, enterprise)
    NET_JOIN_NO_IP,        // associated, no address from DHCP
    NET_JOIN_FAILED,
    NET_JOIN_LOW_BATTERY,  // on battery below 3.6 V: no Wi-Fi (as for delivery)
    NET_JOIN_RADIO,        // Wi-Fi did not start, or the scan failed
} net_join_t;

typedef enum {
    NET_SAMBOT_OK,
    NET_SAMBOT_NO_KEY,     // no token stored
    NET_SAMBOT_KEY_REFUSED,
    NET_SAMBOT_UNREACHED,
} net_sambot_t;

#define NET_UI_MAX_APS 12

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
    bool saved;
} net_ap_t;

void net_ui_scan(void);
void net_ui_join(const char *ssid, const char *pass);  // pass "": open, NULL: the saved one; copied, wiped after
net_ui_state_t net_ui_state(void);
int net_ui_aps(net_ap_t *out, int max);  // the last scan: one per SSID, strongest first
net_join_t net_ui_result(net_sambot_t *sambot);
bool net_ui_busy(void);                  // scanning or joining: keep the screen awake
// The saved networks, newest first; *current: the one Wi-Fi is on now (-1 none).
int net_saved(char ssids[][33], int max, int *current);

void net_print(void);       // console `net`
void net_scan_print(void);  // console `wifi scan`
esp_err_t net_console_register(void);  // `wifi` and `net` on the USB console
