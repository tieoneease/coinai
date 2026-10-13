// The simulator has no Wi-Fi: delivery is the phone's (net.h). Networks given over BLE WIFI_SET
// are accepted and kept in RAM, so the protocol behaves as on the coin.
//
// On-coin setup (net_ui_*) is faked with made-up networks, each step taking about as long as on
// the coin: "Home" takes the password "coinai-sim", "Office" never gives an address, open
// networks join, and a hidden "Lab" network can be typed in. A join needs no SamBot key here.
#include "net.h"

#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "net";
static char s_saved[NET_MAX_NETWORKS][33];
static char s_saved_pw[NET_MAX_NETWORKS][65];
static int s_nnets;
static int s_current = -1;

static net_ui_state_t s_ui_state;
static int64_t s_ui_due_us;  // the scan or join ends then
static char s_ui_ssid[33];
static char s_ui_pass[65];
static net_join_t s_ui_result;

static const net_ap_t FAKE_APS[] = {
    {"Home", -48, true}, {"Pixel_7421", -55, true}, {"Cafe Guest", -61, false}, {"Office", -63, true},
    {"TP-Link_3F2A", -70, true}, {"The neighbours' very long name", -74, true}, {"xfinitywifi", -78, false},
    {"DIRECT-roku-123", -81, true}, {"Garage", -86, true},
};
#define N_FAKE (int)(sizeof FAKE_APS / sizeof FAKE_APS[0])

esp_err_t net_init(void)
{
    ESP_LOGI(TAG, "simulator: no Wi-Fi, clips go to the phone");
    return ESP_OK;
}

void net_kick(uint16_t clip, bool force) {}
void net_prepare(uint16_t clip) {}
void net_note_power(uint16_t vbat_mv, bool usb) {}

void net_get_status(net_status_t *out)
{
    memset(out, 0, sizeof *out);
}

void net_overlay(ble_status_t *bs) {}
bool net_busy(void) { return false; }
bool net_sending(void) { return false; }
void net_shutdown(void) {}

esp_err_t net_wifi_add(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    int i = 0;
    while (i < s_nnets && strcmp(s_saved[i], ssid) != 0) {
        i++;
    }
    if (i == s_nnets) {
        if (s_nnets < NET_MAX_NETWORKS) {
            s_nnets++;
        }
        i = s_nnets - 1;
    }
    memmove(s_saved + 1, s_saved, i * sizeof s_saved[0]);
    memmove(s_saved_pw + 1, s_saved_pw, i * sizeof s_saved_pw[0]);
    snprintf(s_saved[0], sizeof s_saved[0], "%s", ssid);
    snprintf(s_saved_pw[0], sizeof s_saved_pw[0], "%s", pass ? pass : "");
    s_current = -1;
    ESP_LOGI(TAG, "Wi-Fi network \"%s\" stored (simulator: not used)", ssid);
    return ESP_OK;
}

esp_err_t net_wifi_forget(const char *ssid)
{
    int kept = 0;
    for (int i = 0; i < s_nnets; i++) {
        if (ssid && strcmp(s_saved[i], ssid) != 0) {
            memmove(s_saved_pw[kept], s_saved_pw[i], sizeof s_saved_pw[0]);
            memmove(s_saved[kept++], s_saved[i], sizeof s_saved[0]);
        }
    }
    bool changed = kept != s_nnets;
    s_nnets = kept;
    s_current = -1;
    return changed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t net_set_token(const char *token) { return ESP_OK; }
esp_err_t net_set_url(const char *url) { return ESP_OK; }

static void ui_poll(void)
{
    if ((s_ui_state != NET_UI_SCANNING && s_ui_state != NET_UI_JOINING) || esp_timer_get_time() < s_ui_due_us) {
        return;
    }
    if (s_ui_state == NET_UI_SCANNING) {
        s_ui_state = NET_UI_SCANNED;
        return;
    }
    const char *ss = s_ui_ssid;
    if (strcmp(ss, "Home") == 0) {
        s_ui_result = strcmp(s_ui_pass, "coinai-sim") == 0 ? NET_JOIN_OK : NET_JOIN_WRONG_PASSWORD;
    } else if (strcmp(ss, "Office") == 0) {
        s_ui_result = NET_JOIN_NO_IP;
    } else if (strcmp(ss, "Lab") == 0) {
        s_ui_result = NET_JOIN_OK;
    } else {
        s_ui_result = NET_JOIN_NOT_FOUND;
        for (int i = 0; i < N_FAKE; i++) {
            if (strcmp(FAKE_APS[i].ssid, ss) == 0) {
                s_ui_result = !FAKE_APS[i].secure ? NET_JOIN_OK : NET_JOIN_WRONG_PASSWORD;
            }
        }
    }
    if (s_ui_result == NET_JOIN_OK) {
        net_wifi_add(ss, s_ui_pass);
        s_current = 0;
    }
    ESP_LOGI(TAG, "setup join \"%s\": result %d (simulated)", ss, s_ui_result);
    memset(s_ui_pass, 0, sizeof s_ui_pass);
    s_ui_state = NET_UI_DONE;
}

void net_ui_scan(void)
{
    s_ui_state = NET_UI_SCANNING;
    s_ui_due_us = esp_timer_get_time() + 2000000;
}

void net_ui_join(const char *ssid, const char *pass)
{
    snprintf(s_ui_ssid, sizeof s_ui_ssid, "%s", ssid ? ssid : "");
    for (int i = 0; !pass && i < s_nnets; i++) {
        if (strcmp(s_saved[i], s_ui_ssid) == 0) {
            pass = s_saved_pw[i];
        }
    }
    snprintf(s_ui_pass, sizeof s_ui_pass, "%s", pass ? pass : "");
    s_ui_state = NET_UI_JOINING;
    s_ui_due_us = esp_timer_get_time() + 2500000;
}

net_ui_state_t net_ui_state(void)
{
    ui_poll();
    return s_ui_state;
}

int net_ui_aps(net_ap_t *out, int max)
{
    int n = N_FAKE < max ? N_FAKE : max;
    for (int i = 0; i < n; i++) {
        out[i] = FAKE_APS[i];
        out[i].saved = false;
        for (int k = 0; k < s_nnets; k++) {
            out[i].saved |= strcmp(s_saved[k], out[i].ssid) == 0;
        }
    }
    return n;
}

net_join_t net_ui_result(net_sambot_t *sambot)
{
    if (sambot) {
        *sambot = NET_SAMBOT_OK;
    }
    return s_ui_result;
}

bool net_ui_busy(void)
{
    net_ui_state_t st = net_ui_state();
    return st == NET_UI_SCANNING || st == NET_UI_JOINING || st == NET_UI_CHECKING;
}

int net_saved(char ssids[][33], int max, int *current)
{
    int n = s_nnets < max ? s_nnets : max;
    for (int i = 0; i < n; i++) {
        memcpy(ssids[i], s_saved[i], 33);
    }
    *current = s_current < n ? s_current : -1;
    return n;
}

void net_print(void)
{
    printf("simulator: no Wi-Fi; %d network%s stored\n", s_nnets, s_nnets == 1 ? "" : "s");
    for (int i = 0; i < s_nnets; i++) {
        printf("network %d: \"%s\"%s\n", i, s_saved[i], i == s_current ? ", connected (simulated)" : "");
    }
}

void net_scan_print(void)
{
    net_print();
}

static int cmd_wifi(int argc, char **argv)
{
    net_print();
    return 0;
}

esp_err_t net_console_register(void)
{
    const esp_console_cmd_t cmd = {.command = "wifi", .help = "Saved networks (simulator)", .func = cmd_wifi};
    return esp_console_cmd_register(&cmd);
}
