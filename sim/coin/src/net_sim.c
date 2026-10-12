// The simulator has no Wi-Fi: delivery is the phone's (net.h). Networks given over BLE WIFI_SET
// are accepted and kept in RAM, so the protocol behaves as on the coin.
#include "net.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "net";
static int s_nnets;

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
    if (s_nnets < NET_MAX_NETWORKS) {
        s_nnets++;
    }
    ESP_LOGI(TAG, "Wi-Fi network \"%s\" stored (simulator: not used)", ssid);
    return ESP_OK;
}

esp_err_t net_wifi_forget(const char *ssid)
{
    s_nnets = 0;
    return ESP_OK;
}

esp_err_t net_set_token(const char *token) { return ESP_OK; }
esp_err_t net_set_url(const char *url) { return ESP_OK; }

void net_print(void)
{
    printf("simulator: no Wi-Fi; %d network%s stored\n", s_nnets, s_nnets == 1 ? "" : "s");
}

void net_scan_print(void)
{
    net_print();
}

esp_err_t net_console_register(void)
{
    return ESP_OK;
}
