#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "audio.h"
#include "clipstore.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "plaintext.h"
#include "sambot.h"
#include "settings.h"

static const char *TAG = "net";

#define NO_SPEECH_MS    400     // D17, as ui.c and the app: quieter clips are left to the phone
#define MIN_VBAT_MV     3600    // on battery below this, Wi-Fi's TX peaks (~350 mA) are left to the phone
#define FAST_JOIN_MS    4000    // the last network, on its known channel and access point
#define JOIN_MS         10000
#define LINGER_MS       20000   // Wi-Fi stays up this long after the last delivery
#define UI_JOIN_MS      15000   // on-coin setup: a new network (no known channel; WPA3 is slower)
#define BACKOFF_MIN_S   30      // background retries after a failure; a new recording always tries
#define BACKOFF_MAX_S   600
#define DEFAULT_URL     "https://sambot.exe.xyz"
#define LANE            "web"   // as the app (ClipProcessor): replies show in SamBot's web lane
#define TOKEN_MAX       512
#define REPLY_MAX       8192
#define NO_SPEECH_TEXT  "No speech heard"
#define UNCERTAIN_TEXT  "Not sure SamBot got it."

#define EV_KICK     BIT0
#define EV_GOT_IP   BIT1
#define EV_DISCONN  BIT2
#define EV_IDLE     BIT3  // the task is between deliveries
#define EV_STOPPED  BIT4  // s_stop seen and Wi-Fi off (net_shutdown waits for it)

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_net_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    char ssid[33];
} last_ap_t;

typedef enum { D_DONE, D_RETRY, D_SKIP, D_AUTH, D_PHONE } dres_t;

static SemaphoreHandle_t s_lock;  // s_st, s_nets
static SemaphoreHandle_t s_radio;  // Wi-Fi on/off and its use: the net task, or a console scan
static EventGroupHandle_t s_ev;
static net_status_t s_st;
static wifi_net_t s_nets[NET_MAX_NETWORKS];
static int s_nnets;
static bool s_has_token;
static char s_url[128] = DEFAULT_URL;
static last_ap_t s_last;
static volatile uint16_t s_vbat_mv;
static volatile bool s_usb;
static volatile bool s_power_known;  // net_note_power ran: until then no Wi-Fi (TX peaks on a weak cell)
static volatile bool s_stop;
static volatile bool s_force;
static volatile uint16_t s_force_clip;
static volatile bool s_auth_failed;
static int64_t s_retry_at_us;
static int s_backoff_s;
static uint16_t s_skip[8];  // clips SamBot refused this boot: the phone's from now on
static esp_netif_t *s_netif;
static bool s_wifi_init, s_wifi_started, s_sntp;
static int64_t s_linger_until_us;
static volatile bool s_wifi_up;
static bool s_tried;  // Wi-Fi was switched on in this pass of the task
static volatile uint8_t s_disc_reason;  // the last disconnect's reason (wifi_err_reason_t)
static volatile bool s_assoc;           // associated since the last connect attempt began
static uint8_t s_fail_reason;           // try_connect's last failure: s_disc_reason then (before
static bool s_fail_assoc;               // its own esp_wifi_disconnect() adds one: reason 36)

// On-coin setup (net_ui_*): one request at a time, served by the net task.
typedef enum { UI_REQ_NONE, UI_REQ_SCAN, UI_REQ_JOIN } ui_req_t;
static ui_req_t s_ui_req;  // s_lock, as the other s_ui_* (but s_ui_net once the task has it)
static net_ui_state_t s_ui_state;
EXT_RAM_BSS_ATTR static net_ap_t s_ui_aps[NET_UI_MAX_APS];  // internal RAM is short with Wi-Fi on
static int s_ui_naps;
static net_join_t s_ui_result;
static net_sambot_t s_ui_sambot;
static wifi_net_t s_ui_net;  // the network to try; wiped once taken

// ---------- small helpers ----------

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

// Internal RAM is the scarce resource with Wi-Fi, BLE and LVGL all up (see sdkconfig.defaults). Not
// heap_caps_get_minimum_free_size(): it sums each region's own low point, reached at different times.
static void mem_log(const char *step)
{
    ESP_LOGI(TAG, "mem %s: internal free %u, largest block %u", step,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void set_phase(net_phase_t ph, uint16_t clip)
{
    lock();
    s_st.phase = ph;
    s_st.clip = clip;
    if (ph != NET_UPLOADING) {
        s_st.sent = s_st.size = 0;
    }
    unlock();
}

static bool skipped(uint16_t id)
{
    for (int i = 0; i < 8; i++) {
        if (s_skip[i] == id) {
            return true;
        }
    }
    return false;
}

static void skip(uint16_t id)
{
    memmove(s_skip + 1, s_skip, 7 * sizeof s_skip[0]);
    s_skip[0] = id;
}

static bool configured(void)
{
    lock();
    bool c = s_nnets > 0 && s_has_token;
    unlock();
    return c;
}

static bool power_ok(void)
{
    return s_power_known && (s_usb || s_vbat_mv == 0 || s_vbat_mv >= MIN_VBAT_MV);
}

static bool phone_connected(void)
{
    ble_status_t bs;
    ble_get_status(&bs);
    return bs.connected && (bs.authenticated || bs.bench);
}

// May a clip go over Wi-Fi now? at_end: the clip just ended (phone-first looks at the phone);
// background: retries respect the back-off.
static bool eligible(bool background)
{
    int mode = settings_get(SET_DELIVERY);
    if (!configured() || mode == DELIVERY_PHONE_ONLY || s_auth_failed || !power_ok() || s_stop) {
        return false;
    }
    if (mode == DELIVERY_PHONE_FIRST && phone_connected()) {
        return false;
    }
    return !background || now_us() >= s_retry_at_us;
}

static void backoff(void)
{
    s_backoff_s = s_backoff_s ? s_backoff_s * 2 : BACKOFF_MIN_S;
    if (s_backoff_s > BACKOFF_MAX_S) {
        s_backoff_s = BACKOFF_MAX_S;
    }
    s_retry_at_us = now_us() + s_backoff_s * 1000000LL;
}

// ---------- settings in NVS ----------

static void load_networks(void)
{
    nvs_handle_t nvs;
    wifi_net_t nets[NET_MAX_NETWORKS] = {0};
    int n = 0;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        for (int i = 0; i < NET_MAX_NETWORKS; i++) {
            char ks[4] = {'s', (char)('0' + i)}, kp[4] = {'p', (char)('0' + i)};
            size_t ls = sizeof nets[n].ssid, lp = sizeof nets[n].pass;
            if (nvs_get_str(nvs, ks, nets[n].ssid, &ls) == ESP_OK && nets[n].ssid[0]) {
                if (nvs_get_str(nvs, kp, nets[n].pass, &lp) != ESP_OK) {
                    nets[n].pass[0] = 0;
                }
                n++;
            }
        }
        if (n == 0) {  // stored by WIFI_SET before the network list existed
            size_t ls = sizeof nets[0].ssid, lp = sizeof nets[0].pass;
            if (nvs_get_str(nvs, "ssid", nets[0].ssid, &ls) == ESP_OK && nets[0].ssid[0]) {
                if (nvs_get_str(nvs, "pass", nets[0].pass, &lp) != ESP_OK) {
                    nets[0].pass[0] = 0;
                }
                n = 1;
            }
        }
        size_t ll = sizeof s_last;
        if (nvs_get_blob(nvs, "last", &s_last, &ll) != ESP_OK || ll != sizeof s_last) {
            memset(&s_last, 0, sizeof s_last);
        }
        nvs_close(nvs);
    }
    lock();
    memcpy(s_nets, nets, sizeof nets);
    s_nnets = n;
    unlock();
    memset(nets, 0, sizeof nets);
}

static esp_err_t save_networks(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("wifi", NVS_READWRITE, &nvs), TAG, "nvs");
    lock();
    for (int i = 0; i < NET_MAX_NETWORKS; i++) {
        char ks[4] = {'s', (char)('0' + i)}, kp[4] = {'p', (char)('0' + i)};
        if (i < s_nnets) {
            nvs_set_str(nvs, ks, s_nets[i].ssid);
            nvs_set_str(nvs, kp, s_nets[i].pass);
        } else {
            nvs_erase_key(nvs, ks);
            nvs_erase_key(nvs, kp);
        }
    }
    unlock();
    nvs_erase_key(nvs, "ssid");
    nvs_erase_key(nvs, "pass");
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void save_last(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "last", &s_last, sizeof s_last);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// The token goes from NVS straight to sambot.c; nothing here keeps or prints it.
static void load_token(void)
{
    nvs_handle_t nvs;
    char *token = calloc(1, TOKEN_MAX + 1);
    if (!token) {
        return;
    }
    size_t lt = TOKEN_MAX + 1, lu = sizeof s_url;
    bool has = false;
    if (nvs_open("net", NVS_READONLY, &nvs) == ESP_OK) {
        has = nvs_get_str(nvs, "token", token, &lt) == ESP_OK && token[0];
        if (nvs_get_str(nvs, "url", s_url, &lu) != ESP_OK || !s_url[0]) {
            snprintf(s_url, sizeof s_url, "%s", DEFAULT_URL);
        }
        nvs_close(nvs);
    }
    sambot_configure(s_url, has ? token : "");
    lock();
    s_has_token = has;
    unlock();
    memset(token, 0, TOKEN_MAX + 1);
    free(token);
}

// ---------- Wi-Fi ----------

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_disc_reason = d ? d->reason : 0;
        s_wifi_up = false;
        xEventGroupClearBits(s_ev, EV_GOT_IP);
        xEventGroupSetBits(s_ev, EV_DISCONN);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_assoc = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_up = true;
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

static esp_err_t wifi_on(void)
{
    if (!s_netif) {
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_netif, ESP_ERR_NO_MEM, TAG, "sta netif");
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, NULL);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_wifi, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    }
    if (!s_wifi_init) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = false;  // the networks live in our NVS keys, not the driver's
        mem_log("before Wi-Fi");
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_wifi_init = true;
        mem_log("Wi-Fi init");
    }
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_wifi_started = true;
        mem_log("Wi-Fi start");
    }
    return ESP_OK;
}

static void wifi_off(void)
{
    if (s_sntp) {
        esp_netif_sntp_deinit();
        s_sntp = false;
    }
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_init) {
        esp_wifi_deinit();  // gives its buffers back
        s_wifi_init = false;
        ESP_LOGI(TAG, "Wi-Fi off");
    }
    s_wifi_up = false;
    xEventGroupClearBits(s_ev, EV_GOT_IP);
}

static bool try_connect(const wifi_net_t *n, const uint8_t *bssid, uint8_t channel, int timeout_ms)
{
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, n->ssid, strnlen(n->ssid, 32));
    memcpy(wc.sta.password, n->pass, strnlen(n->pass, 64));
    wc.sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (bssid) {
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, bssid, 6);
        wc.sta.channel = channel;
    }
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISCONN);
    s_disc_reason = 0;
    s_assoc = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        memset(&wc, 0, sizeof wc);
        return false;
    }
    memset(&wc, 0, sizeof wc);
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_DISCONN, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(b & EV_GOT_IP)) {
        s_fail_reason = (b & EV_DISCONN) ? s_disc_reason : 0;  // 0: timed out
        s_fail_assoc = s_assoc;
        esp_wifi_disconnect();
        return false;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        (memcmp(s_last.bssid, ap.bssid, 6) != 0 || s_last.channel != ap.primary || strcmp(s_last.ssid, n->ssid) != 0)) {
        memcpy(s_last.bssid, ap.bssid, 6);
        s_last.channel = ap.primary;
        snprintf(s_last.ssid, sizeof s_last.ssid, "%s", n->ssid);
        save_last();
    }
    return true;
}

// Joins a known network: the last one directly, else the strongest known one a scan finds.
static bool join(int64_t *join_ms)
{
    int64_t t0 = now_us();
    if (s_wifi_up) {
        *join_ms = 0;
        return true;
    }
    s_tried = true;
    if (wifi_on() != ESP_OK) {
        return false;
    }
    wifi_net_t *nets = heap_caps_calloc(NET_MAX_NETWORKS, sizeof(wifi_net_t), MALLOC_CAP_SPIRAM);
    wifi_ap_record_t *aps = heap_caps_calloc(24, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    bool ok = false;
    if (!nets || !aps) {
        goto out;
    }
    lock();
    int n = s_nnets;
    memcpy(nets, s_nets, sizeof(wifi_net_t) * NET_MAX_NETWORKS);
    unlock();
    for (int i = 0; i < n && s_last.channel; i++) {
        if (strcmp(nets[i].ssid, s_last.ssid) == 0) {
            ok = try_connect(&nets[i], s_last.bssid, s_last.channel, FAST_JOIN_MS);
            ESP_LOGI(TAG, "fast join \"%s\" ch %u: %s", nets[i].ssid, s_last.channel, ok ? "ok" : "failed");
            break;
        }
    }
    if (!ok) {
        wifi_scan_config_t sc = {.scan_type = WIFI_SCAN_TYPE_ACTIVE};  // default times: required with BLE on
        uint16_t found = 24;
        if (esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_records(&found, aps) == ESP_OK) {
            // Records come strongest first.
            for (int a = 0; a < found && !ok && !s_stop; a++) {
                for (int i = 0; i < n; i++) {
                    if (strcmp((const char *)aps[a].ssid, nets[i].ssid) == 0) {
                        ok = try_connect(&nets[i], aps[a].bssid, aps[a].primary, JOIN_MS);
                        ESP_LOGI(TAG, "join \"%s\" (%d dBm, ch %u): %s", nets[i].ssid, aps[a].rssi, aps[a].primary,
                                 ok ? "ok" : "failed");
                        break;
                    }
                }
            }
            if (!ok) {
                ESP_LOGI(TAG, "no known network among %u found", found);
            }
        }
    }
    if (ok && !s_sntp) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec < 1700000000) {  // the phone has not set the clock this boot
            esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            s_sntp = esp_netif_sntp_init(&c) == ESP_OK;
        }
    }
out:
    if (nets) {
        memset(nets, 0, sizeof(wifi_net_t) * NET_MAX_NETWORKS);
        free(nets);
    }
    free(aps);
    *join_ms = (now_us() - t0) / 1000;
    lock();
    s_st.wifi_up = ok;
    unlock();
    mem_log(ok ? "joined" : "no network");
    return ok;
}

// ---------- on-coin setup ----------

static void ui_set(net_ui_state_t st)
{
    lock();
    s_ui_state = st;
    unlock();
}

static void ui_done(net_join_t result, net_sambot_t sambot)
{
    lock();
    s_ui_result = result;
    s_ui_sambot = sambot;
    s_ui_state = NET_UI_DONE;
    unlock();
}

static net_join_t join_failure(uint8_t reason, bool assoc)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return NET_JOIN_WRONG_PASSWORD;  // or a signal too weak to finish the handshake
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return NET_JOIN_NOT_FOUND;
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return NET_JOIN_UNSUPPORTED;
    case 0:
        return assoc ? NET_JOIN_NO_IP : NET_JOIN_FAILED;  // timed out
    default:
        return NET_JOIN_FAILED;
    }
}

static void ui_scan(void)
{
    if (!power_ok()) {
        ui_done(NET_JOIN_LOW_BATTERY, NET_SAMBOT_OK);
        return;
    }
    s_tried = true;
    wifi_ap_record_t *aps = heap_caps_calloc(24, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    uint16_t found = 24;
    wifi_scan_config_t sc = {.scan_type = WIFI_SCAN_TYPE_ACTIVE};  // default times: required with BLE on
    int64_t t0 = now_us();
    if (!aps || wifi_on() != ESP_OK || esp_wifi_scan_start(&sc, true) != ESP_OK ||
        esp_wifi_scan_get_ap_records(&found, aps) != ESP_OK) {
        free(aps);
        ui_done(NET_JOIN_RADIO, NET_SAMBOT_OK);
        return;
    }
    // One entry per SSID (its strongest access point), hidden ones left out, strongest first.
    net_ap_t list[NET_UI_MAX_APS];
    int n = 0;
    lock();
    for (int a = 0; a < found; a++) {
        const char *ssid = (const char *)aps[a].ssid;
        if (!ssid[0]) {
            continue;
        }
        int i = 0;
        while (i < n && strcmp(list[i].ssid, ssid) != 0) {
            i++;
        }
        if (i < n) {
            if (aps[a].rssi > list[i].rssi) {
                list[i].rssi = aps[a].rssi;
            }
            continue;
        }
        if (n == NET_UI_MAX_APS) {
            continue;
        }
        net_ap_t *e = &list[n++];
        memset(e, 0, sizeof *e);
        snprintf(e->ssid, sizeof e->ssid, "%s", ssid);
        e->rssi = aps[a].rssi;
        e->secure = aps[a].authmode != WIFI_AUTH_OPEN && aps[a].authmode != WIFI_AUTH_OWE;
        for (int k = 0; k < s_nnets; k++) {
            e->saved |= strcmp(s_nets[k].ssid, ssid) == 0;
        }
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && list[j].rssi > list[j - 1].rssi; j--) {
            net_ap_t t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }
    }
    memcpy(s_ui_aps, list, n * sizeof list[0]);
    s_ui_naps = n;
    s_ui_state = NET_UI_SCANNED;
    unlock();
    ESP_LOGI(TAG, "setup scan: %u access points, %d networks in %lld ms", found, n, (now_us() - t0) / 1000);
    free(aps);
    s_linger_until_us = now_us() + LINGER_MS * 1000LL;
}

static void ui_join(void)
{
    wifi_net_t *n = heap_caps_calloc(1, sizeof *n, MALLOC_CAP_SPIRAM);
    lock();
    if (n) {
        *n = s_ui_net;
    }
    memset(&s_ui_net, 0, sizeof s_ui_net);
    unlock();
    if (!n) {
        ui_done(NET_JOIN_FAILED, NET_SAMBOT_OK);
        return;
    }
    if (!power_ok()) {
        ui_done(NET_JOIN_LOW_BATTERY, NET_SAMBOT_OK);
        goto out;
    }
    s_tried = true;
    if (wifi_on() != ESP_OK) {
        ui_done(NET_JOIN_RADIO, NET_SAMBOT_OK);
        goto out;
    }
    if (s_wifi_up) {
        // On a network already (maybe this one): leave it first, so that its disconnect is not
        // taken for this attempt's failure.
        xEventGroupClearBits(s_ev, EV_DISCONN);
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_ev, EV_DISCONN, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    }
    int64_t t0 = now_us();
    bool ok = try_connect(n, NULL, 0, UI_JOIN_MS);
    uint8_t reason = ok ? 0 : s_fail_reason;
    bool assoc = !ok && s_fail_assoc;
    ESP_LOGI(TAG, "setup join \"%s\": %s after %lld ms (reason %u%s)", n->ssid, ok ? "ok" : "failed",
             (now_us() - t0) / 1000, reason, assoc ? ", associated" : "");
    lock();
    s_st.wifi_up = ok;
    unlock();
    s_linger_until_us = now_us() + LINGER_MS * 1000LL;
    if (!ok) {
        esp_wifi_disconnect();
        ui_done(join_failure(reason, assoc), NET_SAMBOT_OK);
        goto out;
    }
    net_wifi_add(n->ssid, n->pass);  // the newest: tried first from now on
    net_sambot_t sb = NET_SAMBOT_NO_KEY;
    if (s_has_token) {
        ui_set(NET_UI_CHECKING);
        sambot_begin();
        sambot_err_t err;
        sambot_rc_t rc = sambot_check(&err);
        ESP_LOGI(TAG, "setup: SamBot %s (status %d)", sambot_rc_name(rc), err.status);
        sb = rc == SAMBOT_OK ? NET_SAMBOT_OK : rc == SAMBOT_AUTH ? NET_SAMBOT_KEY_REFUSED : NET_SAMBOT_UNREACHED;
        if (rc == SAMBOT_OK) {
            s_auth_failed = false;
        }
    }
    ui_done(NET_JOIN_OK, sb);
out:
    memset(n, 0, sizeof *n);
    free(n);
}

// A setup request, if any (net task, holding s_radio).
static void ui_step(void)
{
    lock();
    ui_req_t req = s_ui_req;
    s_ui_req = UI_REQ_NONE;
    unlock();
    if (s_stop) {
        return;
    }
    if (req == UI_REQ_SCAN) {
        ui_scan();
    } else if (req == UI_REQ_JOIN) {
        ui_join();
    }
}

// ---------- one clip ----------

typedef struct {
    uint16_t clip;
    bool told;      // REPLY 0 went to the screen
    bool hidden;    // tombstoned: POST /prompt may have gone out
    bool prompted;  // SamBot accepted the prompt: the clip is gone
    int64_t prompting_us, prompted_us;
} job_t;

static void on_progress(uint32_t sent, uint32_t total, void *ctx)
{
    job_t *j = ctx;
    lock();
    s_st.sent = sent;
    s_st.size = total;
    unlock();
    if (sent >= total && !j->told) {
        j->told = true;
        set_phase(NET_TRANSCRIBING, j->clip);
        ble_note_reply(j->clip, 0, "");  // PROTOCOL.md REPLY 0, as the app sends it
    }
}

// The prompt goes out at most once (PLAN.md "Duplicates"): the tombstone comes first, so a reset
// from here on deletes the clip instead of sending it again. Until here a reset loses nothing.
static bool before_prompt(void *ctx)
{
    job_t *j = ctx;
    if (s_stop) {
        return false;
    }
    clipstore_hide(j->clip);
    j->hidden = true;
    j->prompting_us = now_us();
    set_phase(NET_PROMPTING, j->clip);
    return true;
}

static void on_prompted(void *ctx)
{
    job_t *j = ctx;
    j->prompted = true;
    j->prompted_us = now_us();
    clipstore_remove(j->clip);  // also drops the tombstone: SamBot has it
    set_phase(NET_WAITING, j->clip);
}

static void say(uint16_t clip, int state, const char *markdown)
{
    char text[201];
    plaintext_for_coin(markdown, text, sizeof text);
    ble_note_reply(clip, state, text);
}

static void fail_text(uint16_t clip, const char *prefix, const sambot_err_t *err)
{
    char t[160];
    snprintf(t, sizeof t, "%s%s", prefix, err->detail[0] ? err->detail : "no detail");
    say(clip, 3, t);
}

static dres_t deliver(uint16_t id, int64_t join_ms)
{
    clip_meta_t meta;
    bool complete = false;
    if (clipstore_get_meta(id, audio_recording_clip(), &meta, &complete) != ESP_OK) {
        return D_DONE;  // discarded meanwhile
    }
    if (!complete) {
        return D_PHONE;
    }
    if (meta.voiced_ms != CLIP_VOICED_UNKNOWN && meta.voiced_ms < NO_SPEECH_MS) {
        return D_PHONE;  // the phone keeps it as "no speech"; the coin already said so
    }
    char path[32];
    clipstore_path(id, path, sizeof path);
    char *text = NULL;  // the transcript, from sambot_transcribe
    char *reply = heap_caps_calloc(1, REPLY_MAX, MALLOC_CAP_SPIRAM);
    if (!reply) {
        return D_RETRY;
    }
    job_t job = {.clip = id};
    sambot_err_t err = {0};
    dres_t res = D_RETRY;
    int64_t t0 = now_us();
    set_phase(NET_UPLOADING, id);
    lock();
    s_st.size = meta.size;
    unlock();
    sambot_rc_t rc = sambot_transcribe(path, &text, on_progress, &job, &err);
    int64_t t_tr = now_us();
    mem_log("transcribed");
    switch (rc) {
    case SAMBOT_OK:
        break;
    case SAMBOT_AUTH:
        s_auth_failed = true;
        say(id, 3, "SamBot refused the coin's key. The phone will send this one.");
        res = D_AUTH;
        goto out;
    case SAMBOT_REFUSED:
        fail_text(id, "Failed: transcribe: ", &err);
        res = D_SKIP;
        goto out;
    default:
        if (job.told) {
            ble_note_reply(id, -1, "");
        }
        res = D_RETRY;
        goto out;
    }
    char *s = text;  // trim
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
        s++;
    }
    size_t tl = strlen(s);
    while (tl && (s[tl - 1] == ' ' || s[tl - 1] == '\n' || s[tl - 1] == '\r' || s[tl - 1] == '\t')) {
        s[--tl] = 0;
    }
    if (!tl) {
        clipstore_remove(id);
        ble_note_reply(id, 3, NO_SPEECH_TEXT);
        ESP_LOGI(TAG, "clip %u: blank transcript, deleted", id);
        res = D_DONE;
        goto out;
    }
    say(id, 1, s);  // what SamBot heard, while the coin waits
    uint32_t wait_ms = settings_get(SET_REPLY_WAIT_S) * 1000u;
    bool settled = false;
    const sambot_ask_hooks_t hooks = {.before_prompt = before_prompt, .prompted = on_prompted, .ctx = &job};
    rc = sambot_ask(s, LANE, wait_ms, reply, REPLY_MAX, &settled, &hooks, &err);
    int64_t t_end = now_us();
    mem_log("asked");
    ESP_LOGI(TAG, "net task stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    switch (rc) {
    case SAMBOT_OK:
        if (settled) {
            say(id, 2, reply);
        } else {
            ble_note_reply(id, -1, "");  // the rest lands in SamBot's web lane
        }
        ESP_LOGI(TAG, "clip %u delivered over Wi-Fi: join %lld ms, upload+transcribe %lld ms, to prompt %lld ms, "
                      "reply %lld ms%s", id, join_ms, (t_tr - t0) / 1000, (job.prompted_us - t_tr) / 1000,
                 (t_end - job.prompted_us) / 1000, settled ? "" : " (not settled)");
        ESP_LOGI(TAG, "clip %u: events live after %lld ms, POST /prompt %lld ms", id,
                 (job.prompting_us - t_tr) / 1000, (job.prompted_us - job.prompting_us) / 1000);
        res = D_DONE;
        break;
    case SAMBOT_UNKNOWN:
        clipstore_remove(id);  // never sent twice
        say(id, 3, UNCERTAIN_TEXT);
        ESP_LOGW(TAG, "clip %u: prompt outcome unknown (%s); deleted", id, err.detail);
        res = D_DONE;
        break;
    case SAMBOT_AUTH:
        if (job.hidden) {
            clipstore_unhide(id);
        }
        s_auth_failed = true;
        say(id, 3, "SamBot refused the coin's key. The phone will send this one.");
        res = D_AUTH;
        break;
    case SAMBOT_REFUSED:
        if (job.hidden) {
            clipstore_unhide(id);
        }
        fail_text(id, "SamBot: ", &err);
        res = D_SKIP;
        break;
    default:  // not sent: network, or standby
        if (job.hidden) {
            clipstore_unhide(id);
        }
        ble_note_reply(id, -1, "");
        res = D_RETRY;
        break;
    }
out:
    if (res != D_DONE) {
        ESP_LOGW(TAG, "clip %u: %s (HTTP %d: %s)", id, sambot_rc_name(rc), err.status, err.detail);
    }
    if (text) {
        memset(text, 0, strlen(text));
        free(text);
    }
    free(reply);
    return res;
}

// Claims the oldest stored clip Wi-Fi may take; one the phone holds (fetching, or received and
// not yet ACKed) is passed over for the next. Returns the claimed clip, or 0.
static uint16_t claim_next(void)
{
    static clip_info_t list[64];  // net task only
    int n = clipstore_list(list, 64);
    uint16_t recording = audio_recording_clip();
    for (int i = 0; i < n; i++) {
        uint16_t id = list[i].id;
        if (id == recording || skipped(id)) {
            continue;
        }
        clip_meta_t meta;
        bool complete;
        if (clipstore_get_meta(id, 0, &meta, &complete) != ESP_OK || !complete ||
            (meta.voiced_ms != CLIP_VOICED_UNKNOWN && meta.voiced_ms < NO_SPEECH_MS)) {
            continue;
        }
        if (ble_claim_clip(id)) {
            return id;
        }
    }
    return 0;
}

static void net_step(void);

static void net_task(void *arg)
{
    for (;;) {
        xEventGroupSetBits(s_ev, EV_IDLE);
        xEventGroupWaitBits(s_ev, EV_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        xSemaphoreTake(s_radio, portMAX_DELAY);
        bool was_on = s_wifi_init;
        ui_step();
        net_step();
        if (!s_wifi_init && (was_on || s_tried)) {
            mem_log("Wi-Fi off");
        }
        s_tried = false;
        xSemaphoreGive(s_radio);
    }
}

// One pass of the net task, holding s_radio.
static void net_step(void)
{
    {
        if (s_stop) {
            wifi_off();
            xEventGroupSetBits(s_ev, EV_STOPPED);
            return;
        }
        bool force = s_force && configured();
        s_force = false;
        uint16_t claim = ble_claimed_clip();
        if (!claim && force && s_force_clip) {
            if (ble_claim_clip(s_force_clip)) {
                claim = s_force_clip;
            } else {
                ESP_LOGW(TAG, "clip %u: the phone has it (fetching or not yet ACKed)", s_force_clip);
            }
        } else if (!claim && (force || eligible(true))) {
            claim = claim_next();  // eligible() already left phone first to a connected phone
        }
        if (!claim) {
            if (s_wifi_init && now_us() > s_linger_until_us) {
                wifi_off();
            }
            return;
        }
        xEventGroupClearBits(s_ev, EV_IDLE);
        sambot_begin();  // from here an abort (net_shutdown) holds for the whole delivery
        set_phase(NET_JOINING, claim);
        int64_t join_ms = 0;
        if (!join(&join_ms)) {
            ESP_LOGW(TAG, "clip %u: no Wi-Fi after %lld ms; the phone may have it", claim, join_ms);
            set_phase(NET_IDLE, 0);
            ble_release_clip(true);
            backoff();
            wifi_off();
            return;
        }
        if (audio_recording_clip() == claim) {
            // Joined during the recording: wait for its end.
            set_phase(NET_JOINING, claim);
            while (audio_recording_clip() == claim && !s_stop) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        dres_t r = s_stop ? D_RETRY : deliver(claim, join_ms);
        set_phase(NET_IDLE, 0);
        switch (r) {
        case D_DONE:
            ble_release_clip(false);
            s_backoff_s = 0;
            s_retry_at_us = 0;
            xEventGroupSetBits(s_ev, EV_KICK);  // the next one, if any
            break;
        case D_SKIP:
            skip(claim);
            ble_release_clip(true);
            break;
        case D_PHONE:
            skip(claim);
            ble_release_clip(true);
            break;
        case D_AUTH:
        case D_RETRY:
            ble_release_clip(true);
            backoff();
            break;
        }
        s_linger_until_us = now_us() + LINGER_MS * 1000LL;
    }
}

// ---------- public ----------

// How the last net_shutdown before standby went (its log is lost when the USB console drops):
// -1 none, else ms until Wi-Fi was off, or SHUTDOWN_BUSY when it was still on after 2 s.
#define SHUTDOWN_BUSY 0x7FFFFFFF
static RTC_DATA_ATTR int32_t s_rtc_shutdown_ms = -1;

esp_err_t net_init(void)
{
    if (s_rtc_shutdown_ms >= 0) {
        if (s_rtc_shutdown_ms == SHUTDOWN_BUSY) {
            ESP_LOGW(TAG, "before standby: Wi-Fi was still busy after 2 s");
        } else {
            ESP_LOGI(TAG, "before standby: Wi-Fi off after %ld ms", (long)s_rtc_shutdown_ms);
        }
        s_rtc_shutdown_ms = -1;
    }
    s_lock = xSemaphoreCreateMutex();
    s_radio = xSemaphoreCreateMutex();
    s_ev = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_lock && s_radio && s_ev, ESP_ERR_NO_MEM, TAG, "rtos objects");
    load_networks();
    load_token();
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(net_task, "net", 10240, NULL, 4, NULL, 0,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    lock();
    ESP_LOGI(TAG, "%d network%s, %s token, delivery mode %u", s_nnets, s_nnets == 1 ? "" : "s",
             s_has_token ? "a" : "no", settings_get(SET_DELIVERY));
    unlock();
    return ESP_OK;
}

void net_kick(uint16_t clip, bool force)
{
    if (!s_ev) {
        return;
    }
    if (force) {
        s_force_clip = clip;
        s_force = true;
        s_auth_failed = false;  // `net send`: try the key again
    }
    xEventGroupSetBits(s_ev, EV_KICK);
}

void net_prepare(uint16_t clip)
{
    if (!clip) {
        return;
    }
    // Wi-Fi first: the new clip is Wi-Fi's from its first byte, so the phone does not start it too.
    if (!s_ev || settings_get(SET_DELIVERY) != DELIVERY_WIFI_FIRST || !eligible(false) || ble_claimed_clip()) {
        return;
    }
    if (ble_claim_clip(clip)) {
        set_phase(NET_JOINING, clip);
        xEventGroupSetBits(s_ev, EV_KICK);
    }
}

void net_note_power(uint16_t vbat_mv, bool usb)
{
    s_vbat_mv = vbat_mv;
    s_usb = usb;
    s_power_known = true;
}

void net_get_status(net_status_t *out)
{
    if (!s_lock) {
        memset(out, 0, sizeof *out);
        return;
    }
    bool ready = eligible(false);
    lock();
    *out = s_st;
    out->configured = s_nnets > 0 && s_has_token;
    out->wifi_up = s_wifi_up;
    unlock();
    out->ready = ready;
}

void net_overlay(ble_status_t *bs)
{
    if (!s_lock) {
        return;
    }
    lock();
    net_status_t st = s_st;
    unlock();
    uint16_t claim = ble_claimed_clip();
    if (claim) {
        char path[32];
        struct stat sb;
        clipstore_path(claim, path, sizeof path);
        if (stat(path, &sb) == 0 && !clipstore_discarded(claim)) {
            bs->pending++;  // still on the coin
        }
    }
    if (st.phase == NET_UPLOADING && !bs->transferring && st.size) {
        bs->transferring = true;
        bs->tx_clip = st.clip;
        bs->tx_offset = st.sent;
        bs->tx_size = st.size;
    }
}

bool net_busy(void)
{
    if (!s_lock) {
        return false;
    }
    lock();
    bool busy = s_st.phase != NET_IDLE;
    unlock();
    return busy;
}

bool net_sending(void)
{
    if (!s_lock) {
        return false;
    }
    lock();
    bool sending = s_st.phase != NET_IDLE && s_st.phase != NET_WAITING;
    unlock();
    return sending;
}

void net_shutdown(void)
{
    if (!s_ev) {
        return;
    }
    s_stop = true;
    sambot_abort();
    xEventGroupSetBits(s_ev, EV_KICK);
    // The task finishes the call in progress (aborted within about a second, or when a blocking
    // connect gives up) and switches Wi-Fi off before deep sleep or power-off.
    int64_t t0 = now_us();
    if (xEventGroupWaitBits(s_ev, EV_STOPPED, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000)) & EV_STOPPED) {
        s_rtc_shutdown_ms = (int32_t)((now_us() - t0) / 1000);
    } else {
        s_rtc_shutdown_ms = SHUTDOWN_BUSY;
        ESP_LOGW(TAG, "shutdown: Wi-Fi still busy after 2 s");
    }
}

void net_ui_scan(void)
{
    if (!s_lock) {
        return;
    }
    lock();
    s_ui_req = UI_REQ_SCAN;
    s_ui_state = NET_UI_SCANNING;
    unlock();
    xEventGroupSetBits(s_ev, EV_KICK);
}

void net_ui_join(const char *ssid, const char *pass)
{
    if (!s_lock) {
        return;
    }
    lock();
    memset(&s_ui_net, 0, sizeof s_ui_net);
    snprintf(s_ui_net.ssid, sizeof s_ui_net.ssid, "%s", ssid ? ssid : "");
    for (int i = 0; !pass && i < s_nnets; i++) {
        if (strcmp(s_nets[i].ssid, s_ui_net.ssid) == 0) {
            pass = s_nets[i].pass;  // a saved network: its password
        }
    }
    snprintf(s_ui_net.pass, sizeof s_ui_net.pass, "%s", pass ? pass : "");
    s_ui_req = UI_REQ_JOIN;
    s_ui_state = NET_UI_JOINING;
    unlock();
    xEventGroupSetBits(s_ev, EV_KICK);
}

net_ui_state_t net_ui_state(void)
{
    if (!s_lock) {
        return NET_UI_IDLE;
    }
    lock();
    net_ui_state_t st = s_ui_state;
    unlock();
    return st;
}

int net_ui_aps(net_ap_t *out, int max)
{
    if (!s_lock) {
        return 0;
    }
    lock();
    int n = s_ui_naps < max ? s_ui_naps : max;
    memcpy(out, s_ui_aps, n * sizeof *out);
    unlock();
    return n;
}

net_join_t net_ui_result(net_sambot_t *sambot)
{
    if (!s_lock) {
        return NET_JOIN_FAILED;
    }
    lock();
    net_join_t r = s_ui_result;
    if (sambot) {
        *sambot = s_ui_sambot;
    }
    unlock();
    return r;
}

bool net_ui_busy(void)
{
    net_ui_state_t st = net_ui_state();
    return st == NET_UI_SCANNING || st == NET_UI_JOINING || st == NET_UI_CHECKING;
}

int net_saved(char ssids[][33], int max, int *current)
{
    *current = -1;
    if (!s_lock) {
        return 0;
    }
    lock();
    int n = s_nnets < max ? s_nnets : max;
    for (int i = 0; i < n; i++) {
        memcpy(ssids[i], s_nets[i].ssid, 33);
        ssids[i][32] = 0;
        if (s_wifi_up && strcmp(s_nets[i].ssid, s_last.ssid) == 0) {
            *current = i;
        }
    }
    unlock();
    return n;
}

esp_err_t net_wifi_add(const char *ssid, const char *pass)
{
    if (!s_lock || !ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64) ||
        (pass && pass[0] && strlen(pass) < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    int i = 0;
    while (i < s_nnets && strcmp(s_nets[i].ssid, ssid) != 0) {
        i++;
    }
    if (i == s_nnets) {  // new: in front, the oldest drops off the end
        if (s_nnets < NET_MAX_NETWORKS) {
            s_nnets++;
        }
        i = s_nnets - 1;
    }
    memmove(s_nets + 1, s_nets, i * sizeof s_nets[0]);
    memset(&s_nets[0], 0, sizeof s_nets[0]);
    snprintf(s_nets[0].ssid, sizeof s_nets[0].ssid, "%s", ssid);
    snprintf(s_nets[0].pass, sizeof s_nets[0].pass, "%s", pass ? pass : "");
    unlock();
    ESP_LOGI(TAG, "Wi-Fi network \"%s\" stored (%s)", ssid, pass && pass[0] ? "with password" : "open");
    s_backoff_s = 0;
    s_retry_at_us = 0;
    return save_networks();
}

esp_err_t net_wifi_forget(const char *ssid)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    int kept = 0;
    for (int i = 0; i < s_nnets; i++) {
        if (ssid && strcmp(s_nets[i].ssid, ssid) != 0) {
            s_nets[kept++] = s_nets[i];
        }
    }
    bool changed = kept != s_nnets;
    for (int i = kept; i < NET_MAX_NETWORKS; i++) {
        memset(&s_nets[i], 0, sizeof s_nets[i]);
    }
    s_nnets = kept;
    unlock();
    if (!changed) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!ssid || strcmp(ssid, s_last.ssid) == 0) {
        memset(&s_last, 0, sizeof s_last);
        save_last();
    }
    return save_networks();
}

esp_err_t net_set_token(const char *token)
{
    if (token && strlen(token) > TOKEN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("net", NVS_READWRITE, &nvs), TAG, "nvs");
    esp_err_t err = token && token[0] ? nvs_set_str(nvs, "token", token) : nvs_erase_key(nvs, "token");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_auth_failed = false;
        load_token();
    }
    return err;
}

esp_err_t net_set_url(const char *url)
{
    if (!url || strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof s_url) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("net", NVS_READWRITE, &nvs), TAG, "nvs");
    esp_err_t err = nvs_set_str(nvs, "url", url);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        load_token();
    }
    return err;
}

static const char *phase_name(net_phase_t p)
{
    static const char *const names[] = {"idle", "joining", "uploading", "transcribing", "prompting", "waiting"};
    return p <= NET_WAITING ? names[p] : "?";
}

void net_print(void)
{
    net_status_t st;
    net_get_status(&st);
    static const char *const modes[] = {"wifi-first", "phone-first", "phone-only"};
    unsigned mode = settings_get(SET_DELIVERY);
    printf("delivery: %s; %s; Wi-Fi %s; %s\n", mode < 3 ? modes[mode] : "?", st.ready ? "ready" : "not ready",
           st.wifi_up ? "up" : "off", phase_name(st.phase));
    if (st.clip) {
        printf("clip %u: %lu of %lu bytes\n", st.clip, (unsigned long)st.sent, (unsigned long)st.size);
    }
    lock();
    printf("SamBot: %s, token %s%s\n", s_url, s_has_token ? "stored" : "missing", s_auth_failed ? " (refused)" : "");
    for (int i = 0; i < s_nnets; i++) {
        printf("network %d: \"%s\" (%s)%s\n", i, s_nets[i].ssid, s_nets[i].pass[0] ? "password" : "open",
               strcmp(s_nets[i].ssid, s_last.ssid) == 0 ? ", last used" : "");
    }
    unlock();
    if (s_wifi_up) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            printf("connected: \"%s\" %d dBm ch %u\n", (const char *)ap.ssid, ap.rssi, ap.primary);
        }
    }
    printf("power: %u mV%s; back-off %d s\n", s_vbat_mv, s_usb ? " (USB)" : "",
           s_retry_at_us > now_us() ? (int)((s_retry_at_us - now_us()) / 1000000) : 0);
}

void net_scan_print(void)
{
    if (ble_claimed_clip() || net_busy() || xSemaphoreTake(s_radio, pdMS_TO_TICKS(1000)) != pdTRUE) {
        printf("busy delivering\n");
        return;
    }
    if (wifi_on() != ESP_OK) {
        xSemaphoreGive(s_radio);
        printf("Wi-Fi start failed\n");
        return;
    }
    wifi_ap_record_t *aps = heap_caps_calloc(24, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    uint16_t n = 24;
    wifi_scan_config_t sc = {.scan_type = WIFI_SCAN_TYPE_ACTIVE};
    int64_t t0 = now_us();
    if (aps && esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
        printf("%u networks in %lld ms\n", n, (now_us() - t0) / 1000);
        for (int i = 0; i < n; i++) {
            printf("%4d dBm ch %2u %s \"%s\"\n", aps[i].rssi, aps[i].primary,
                   aps[i].authmode == WIFI_AUTH_OPEN ? "open" : "auth", (const char *)aps[i].ssid);
        }
    } else {
        printf("scan failed\n");
    }
    free(aps);
    s_linger_until_us = now_us() + LINGER_MS * 1000LL;  // the task switches Wi-Fi off later
    xSemaphoreGive(s_radio);
}

// ---------- USB console ----------

// A line that may hold a secret (well-formed or not): wipe its words from `from` on and drop the
// console history. The REPL's own copy of the line is freed unwiped (esp_console), not reachable here.
static void scrub(int argc, char **argv, int from)
{
    for (int i = from; i < argc; i++) {
        memset(argv[i], 0, strlen(argv[i]));
    }
    linenoiseHistoryFree();
}

static int cmd_wifi(int argc, char **argv)
{
    esp_err_t err;
    if (argc == 1) {
        net_print();
        return 0;
    }
    if (strcmp(argv[1], "add") == 0) {
        err = argc == 3 || argc == 4 ? net_wifi_add(argv[2], argc == 4 ? argv[3] : "") : ESP_ERR_INVALID_ARG;
        scrub(argc, argv, 3);  // the password
        if (argc != 3 && argc != 4) {
            printf("usage: wifi add <ssid> [password] (quote an SSID or password with spaces)\n");
            return 1;
        }
        printf("%s\n", err == ESP_OK ? "stored" : esp_err_to_name(err));
        return err != ESP_OK;
    }
    if (argc == 3 && strcmp(argv[1], "forget") == 0) {
        err = net_wifi_forget(strcmp(argv[2], "all") == 0 ? NULL : argv[2]);
        printf("%s\n", err == ESP_OK ? "forgotten" : esp_err_to_name(err));
        return err != ESP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        net_scan_print();
        return 0;
    }
    printf("usage: wifi | wifi add <ssid> [password] | wifi forget <ssid|all> | wifi scan\n");
    return 1;
}

static int cmd_net(int argc, char **argv)
{
    esp_err_t err;
    if (argc == 1) {
        net_print();
        return 0;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "send") == 0) {
        net_kick(argc == 3 ? (uint16_t)atoi(argv[2]) : 0, true);
        printf("sending%s\n", configured() ? "" : ": not configured (wifi add, net token)");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "mode") == 0) {
        int m = strcmp(argv[2], "wifi") == 0          ? DELIVERY_WIFI_FIRST
                : strcmp(argv[2], "phone-first") == 0 ? DELIVERY_PHONE_FIRST
                : strcmp(argv[2], "phone") == 0       ? DELIVERY_PHONE_ONLY
                                                      : -1;
        err = m < 0 ? ESP_ERR_INVALID_ARG : settings_set(SET_DELIVERY, (uint16_t)m);
        printf("%s\n", err == ESP_OK ? "ok" : "usage: net mode wifi|phone-first|phone");
        return err != ESP_OK;
    }
    if (strcmp(argv[1], "token") == 0) {
        err = argc == 3 ? net_set_token(strcmp(argv[2], "clear") == 0 ? NULL : argv[2]) : ESP_ERR_INVALID_ARG;
        scrub(argc, argv, 2);  // the token
        if (argc != 3) {
            printf("usage: net token <token>|clear\n");
            return 1;
        }
        printf("%s\n", err == ESP_OK ? "token stored" : esp_err_to_name(err));
        return err != ESP_OK;
    }
    if (argc == 3 && strcmp(argv[1], "url") == 0) {
        err = net_set_url(argv[2]);
        printf("%s\n", err == ESP_OK ? "ok" : "usage: net url https://host");
        return err != ESP_OK;
    }
    printf("usage: net | net send [clip] | net mode wifi|phone-first|phone | net token <token>|clear | net url <url>\n");
    return 1;
}

esp_err_t net_console_register(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "wifi", .help = "Wi-Fi networks: wifi | wifi add <ssid> [password] | wifi forget <ssid|all> | wifi scan",
         .func = cmd_wifi},
        {.command = "net", .help = "Wi-Fi delivery: net | net send [clip] | net mode wifi|phone-first|phone | "
                                    "net token <token>|clear | net url <url>",
         .func = cmd_net},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return ESP_OK;
}
