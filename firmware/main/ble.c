#include "ble.h"

#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "ble_link.h"
#include "clipstore.h"
#include "diag.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "ble";

enum {
    MSG_HELLO = 0x01, MSG_LIST = 0x02, MSG_GET = 0x03, MSG_ACK = 0x04, MSG_WIFI_SET = 0x05, MSG_CFG_SET = 0x06,
    MSG_REPLY = 0x07,
    MSG_STATUS = 0x81, MSG_CLIP_STARTED = 0x82, MSG_CLIP_ENDED = 0x83, MSG_LIST_RESULT = 0x84, MSG_ERROR = 0x85,
};
enum { ERR_UNKNOWN_CLIP = 1, ERR_BAD_OFFSET = 2, ERR_BAD_LENGTH = 3, ERR_BUSY = 4, ERR_INVALID = 5 };

#define TLV_MAX        LINK_VALUE_MAX
#define DATA_HDR       6
#define LIST_PER_MSG   34
#define MAX_LIST       256
#define FAST_ADV_MS    30000

typedef enum { EV_RX, EV_CLIP_STARTED, EV_CLIP_FINISHED, EV_STATUS, EV_CLIPS_CHANGED, EV_CONNECTED, EV_DISCONNECTED } ev_type_t;
typedef struct {
    ev_type_t type;
    uint16_t len;
    uint8_t buf[TLV_MAX];
} ev_t;

static QueueHandle_t s_ev;
static TaskHandle_t s_worker;
static SemaphoreHandle_t s_ind_done;
static SemaphoreHandle_t s_lock;  // guards s_st and the device-status snapshot
static volatile bool s_ind_ok;
static volatile bool s_radio_up;
static volatile uint16_t s_conn = LINK_CONN_NONE;
static volatile bool s_ctrl_sub, s_data_sub;
static volatile bool s_bench;
static volatile bool s_console;  // ble_inject(): answers go to the USB console while no phone is connected
static ble_adv_mode_t s_adv_mode = BLE_ADV_OFF;
static int64_t s_fast_adv_until_us;
static ble_status_t s_st = {.reply_state = -1};
static int64_t s_reply_deadline_us;
static struct {
    int battery_pct;
    bool charging, usb, recording;
    uint16_t pending;
} s_dev = {.battery_pct = -1};

// Owned by the worker task.
static struct {
    bool active;
    uint16_t clip;
    uint32_t offset;
    bool complete;
    clip_meta_t meta;
    FILE *f;
} s_tx;

// ---------- helpers ----------

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }
static uint16_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t get64(const uint8_t *p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }

static uint64_t utc_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1700000000 ? (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
}

static void post(ev_type_t type, const void *buf, size_t len)
{
    ev_t ev = {.type = type, .len = (uint16_t)len};
    if (len) {
        memcpy(ev.buf, buf, len > TLV_MAX ? TLV_MAX : len);
    }
    if (xQueueSend(s_ev, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped event %d", type);
    }
}

static void refresh_pending(void)
{
    uint16_t n = (uint16_t)clipstore_count();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dev.pending = n;
    xSemaphoreGive(s_lock);
}

// ---------- outgoing ctrl messages (worker task only) ----------

static bool console_link(void)
{
    return s_console && s_conn == LINK_CONN_NONE;
}

// One "BLE-<kind> <hex>" line in a single write(). Each task has its own stdout stream, flushed in
// 128-byte pieces, so a printf of this line could be split by another task's log line.
static void console_print(const char *kind, const uint8_t *b, size_t len)
{
    static char line[16 + 2 * (DATA_HDR + 238) + 2];  // worker task only
    static const char hex[] = "0123456789abcdef";
    int n = snprintf(line, sizeof line, "BLE-%s ", kind);
    for (size_t i = 0; i < len && n + 3 < (int)sizeof line; i++) {
        line[n++] = hex[b[i] >> 4];
        line[n++] = hex[b[i] & 15];
    }
    line[n++] = '\n';
    fflush(stdout);
    write(STDOUT_FILENO, line, n);
}

static esp_err_t send_ctrl(uint8_t type, const uint8_t *value, uint16_t len)
{
    uint8_t tlv[TLV_MAX];
    if (len + 3 > TLV_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    tlv[0] = type;
    put16(tlv + 1, len);
    if (len) {
        memcpy(tlv + 3, value, len);
    }
    if (console_link()) {
        console_print("CTRL", tlv, len + 3);
        return ESP_OK;
    }
    uint16_t conn = s_conn;
    if (conn == LINK_CONN_NONE || !s_ctrl_sub || !(s_bench || link_authenticated(conn))) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ind_done, 0);
    s_ind_ok = false;
    link_rc_t rc = link_indicate(conn, tlv, len + 3);
    if (rc != LINK_OK) {
        ESP_LOGW(TAG, "indicate 0x%02x failed (%d)", type, rc);
        return rc == LINK_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (xSemaphoreTake(s_ind_done, pdMS_TO_TICKS(3000)) != pdTRUE || !s_ind_ok) {
        ESP_LOGW(TAG, "indication 0x%02x not confirmed", type);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void send_error(uint8_t request, uint8_t code, uint16_t clip)
{
    uint8_t v[4] = {request, code};
    put16(v + 2, clip);
    ESP_LOGW(TAG, "ERROR for 0x%02x: code %u clip %u", request, code, clip);
    send_ctrl(MSG_ERROR, v, sizeof v);
}

static void send_status(void)
{
    uint8_t v[4 + 32];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v[0] = s_dev.battery_pct < 0 ? 255 : (uint8_t)s_dev.battery_pct;
    v[1] = (s_dev.charging ? 1 : 0) | (s_dev.usb ? 2 : 0) | (s_dev.recording ? 4 : 0);
    put16(v + 2, s_dev.pending);
    xSemaphoreGive(s_lock);
    char ver[40];  // fw_version is at most 32 bytes on the wire
    snprintf(ver, sizeof ver, "%s%s", esp_app_get_description()->version, s_bench ? "-bench" : "");
    size_t n = strnlen(ver, 32);
    memcpy(v + 4, ver, n);
    send_ctrl(MSG_STATUS, v, 4 + n);
}

static void send_list(void)
{
    EXT_RAM_BSS_ATTR static clip_info_t list[MAX_LIST];
    int n = clipstore_list(list, MAX_LIST);
    uint16_t recording = audio_recording_clip();
    int i = 0;
    do {
        uint8_t v[1 + LIST_PER_MSG * 7];
        int k = 0;
        for (; k < LIST_PER_MSG && i < n; k++, i++) {
            clip_meta_t meta;
            bool complete = false;
            uint32_t size = list[i].size;
            if (clipstore_get_meta(list[i].id, recording, &meta, &complete) == ESP_OK && complete) {
                size = meta.size;
            }
            uint8_t *e = v + 1 + k * 7;
            put16(e, list[i].id);
            put32(e + 2, size);
            e[6] = complete ? 1 : 0;
        }
        v[0] = i < n ? 1 : 0;  // more messages follow
        send_ctrl(MSG_LIST_RESULT, v, 1 + k * 7);
    } while (i < n);
}

// ---------- transfers ----------

static void tx_stop(void)
{
    if (s_tx.f) {
        fclose(s_tx.f);
        s_tx.f = NULL;
    }
    if (s_tx.active && clipstore_discarded(s_tx.clip)) {
        clipstore_remove(s_tx.clip);  // discarded while this transfer held its file open
    }
    s_tx.active = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.transferring = false;
    xSemaphoreGive(s_lock);
}

static void handle_get(uint16_t clip, uint32_t offset)
{
    tx_stop();  // a new GET replaces the current one
    clip_meta_t meta;
    bool complete;
    if (clipstore_get_meta(clip, audio_recording_clip(), &meta, &complete) != ESP_OK) {
        send_error(MSG_GET, ERR_UNKNOWN_CLIP, clip);
        return;
    }
    if (offset > meta.size) {
        send_error(MSG_GET, ERR_BAD_OFFSET, clip);
        return;
    }
    s_tx = (typeof(s_tx)){.active = true, .clip = clip, .offset = offset, .complete = complete, .meta = meta};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.transferring = true;
    s_st.tx_clip = clip;
    s_st.tx_offset = offset;
    s_st.tx_size = complete ? meta.size : 0;
    xSemaphoreGive(s_lock);
    if (s_conn != LINK_CONN_NONE) {
        link_transfer_params(s_conn);
    }
    ESP_LOGI(TAG, "GET clip %u from %lu (%s, %lu bytes)", clip, (unsigned long)offset,
             complete ? "complete" : "recording", (unsigned long)meta.size);
}

static void send_clip_ended(void)
{
    uint8_t v[26];
    put16(v, s_tx.clip);
    put32(v + 2, s_tx.meta.size);
    put32(v + 6, s_tx.meta.duration_ms);
    put32(v + 10, s_tx.meta.crc32);
    put32(v + 14, s_tx.meta.voiced_ms);
    put64(v + 18, s_tx.meta.start_utc_ms);
    ESP_LOGI(TAG, "clip %u sent: %lu bytes, crc %08lx", s_tx.clip, (unsigned long)s_tx.meta.size,
             (unsigned long)s_tx.meta.crc32);
    tx_stop();
    send_ctrl(MSG_CLIP_ENDED, v, sizeof v);
}

// Sends notifications until the controller's buffers are full, the file runs dry or a burst is done.
// Returns true when it should run again soon.
static bool tx_step(void)
{
    uint16_t conn = s_conn;
    bool console = console_link();
    if (!s_tx.active || (!console && (conn == LINK_CONN_NONE || !s_data_sub))) {
        return false;
    }
    if (clipstore_discard_pending(s_tx.clip)) {
        // Discarded while streaming: stop with ERROR 1 (tx_stop deletes it once our handle is closed).
        uint16_t clip = s_tx.clip;
        tx_stop();
        send_error(MSG_GET, ERR_UNKNOWN_CLIP, clip);
        refresh_pending();
        send_status();
        return false;
    }
    if (!s_tx.complete && audio_recording_clip() != s_tx.clip) {
        // Recording ended since the GET: pick up the final size and CRC.
        bool complete;
        if (clipstore_get_meta(s_tx.clip, 0, &s_tx.meta, &complete) == ESP_OK) {
            s_tx.complete = complete;
        }
    }
    if (!s_tx.f) {
        char path[32];
        clipstore_path(s_tx.clip, path, sizeof path);
        s_tx.f = fopen(path, "rb");
        if (!s_tx.f || fseek(s_tx.f, s_tx.offset, SEEK_SET) != 0) {
            send_error(MSG_GET, ERR_UNKNOWN_CLIP, s_tx.clip);
            tx_stop();
            return false;
        }
    }
    int payload_max = console ? 238 : link_mtu(conn) - 3 - DATA_HDR;
    if (payload_max > 238) {
        payload_max = 238;
    }
    uint8_t pkt[DATA_HDR + 238];
    for (int burst = 0; burst < 16; burst++) {
        uint32_t limit = s_tx.complete ? s_tx.meta.size : UINT32_MAX;
        if (s_tx.offset >= limit) {
            send_clip_ended();
            return false;
        }
        size_t want = payload_max;
        if (s_tx.complete && limit - s_tx.offset < want) {
            want = limit - s_tx.offset;
        }
        size_t n = fread(pkt + DATA_HDR, 1, want, s_tx.f);
        if (n == 0) {
            // A clip still being recorded: reopen later to see the next flushed page.
            fclose(s_tx.f);
            s_tx.f = NULL;
            vTaskDelay(pdMS_TO_TICKS(100));
            return true;
        }
        put16(pkt, s_tx.clip);
        put32(pkt + 2, s_tx.offset);
        link_rc_t rc = LINK_OK;
        if (console) {
            console_print("DATA", pkt, DATA_HDR + n);
        } else {
            rc = link_notify(conn, pkt, DATA_HDR + n);
        }
        if (rc == LINK_NO_MEM) {
            // Host buffers full: the controller drains them at the link's pace.
            fseek(s_tx.f, s_tx.offset, SEEK_SET);
            vTaskDelay(pdMS_TO_TICKS(5));
            return true;
        }
        if (rc != LINK_OK) {
            ESP_LOGW(TAG, "notify failed; transfer stopped");
            tx_stop();
            return false;
        }
        s_tx.offset += n;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.tx_offset = s_tx.offset;
        xSemaphoreGive(s_lock);
    }
    return true;
}

// ---------- incoming ctrl messages ----------

static void set_reply_window(bool open)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reply_deadline_us = open ? esp_timer_get_time() + (int64_t)settings_get(SET_REPLY_WAIT_S) * 1000000 : 0;
    s_st.reply_window = open;
    xSemaphoreGive(s_lock);
}

static void handle_ack(uint16_t clip, uint32_t crc)
{
    clip_meta_t meta;
    bool complete;
    if (clipstore_get_meta(clip, audio_recording_clip(), &meta, &complete) != ESP_OK) {
        send_error(MSG_ACK, ERR_UNKNOWN_CLIP, clip);
        return;
    }
    if (!complete || meta.crc32 != crc) {
        send_error(MSG_ACK, ERR_INVALID, clip);
        return;
    }
    if (s_tx.active && s_tx.clip == clip) {
        tx_stop();
    }
    clipstore_remove(clip);
    refresh_pending();
    uint16_t pending = s_dev.pending;
    ESP_LOGI(TAG, "clip %u acknowledged and deleted; %u pending", clip, pending);
    if (pending == 0) {
        set_reply_window(true);  // PROTOCOL.md rule 7
    }
    send_status();
}

static void handle_cfg(const uint8_t *v, uint16_t len)
{
    if (len < 2) {
        send_error(MSG_CFG_SET, ERR_BAD_LENGTH, 0xFFFF);
        return;
    }
    bool is_u8 = v[0] == SET_BRIGHTNESS;
    if (len != (is_u8 ? 2 : 3)) {
        send_error(MSG_CFG_SET, ERR_BAD_LENGTH, 0xFFFF);
        return;
    }
    uint16_t value = is_u8 ? v[1] : get16(v + 1);
    if (settings_set((setting_t)v[0], value) == ESP_ERR_INVALID_ARG) {
        send_error(MSG_CFG_SET, ERR_INVALID, 0xFFFF);
        return;
    }
    if (v[0] == SET_BRIGHTNESS) {
        display_set_brightness(value);
    }
}

static void handle_wifi(const uint8_t *v, uint16_t len)
{
    if (!link_authenticated(s_conn)) {  // never over an unencrypted bench link
        send_error(MSG_WIFI_SET, ERR_INVALID, 0xFFFF);
        return;
    }
    if (len < 2 || v[0] > 32 || 1 + v[0] + 1 > len || v[1 + v[0]] > 64 || 2 + v[0] + v[1 + v[0]] != len) {
        send_error(MSG_WIFI_SET, ERR_BAD_LENGTH, 0xFFFF);
        return;
    }
    char ssid[33] = {0}, pass[65] = {0};
    memcpy(ssid, v + 1, v[0]);
    memcpy(pass, v + 2 + v[0], v[1 + v[0]]);
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "pass", pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Wi-Fi credentials stored for \"%s\"", ssid);  // never the password
}

static void handle_reply(const uint8_t *v, uint16_t len)
{
    if (len < 3 || len > 3 + 200 || v[2] > 3) {
        send_error(MSG_REPLY, len < 3 || len > 203 ? ERR_BAD_LENGTH : ERR_INVALID, len >= 2 ? get16(v) : 0xFFFF);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.reply_clip = get16(v);
    s_st.reply_state = v[2];
    memcpy(s_st.reply_text, v + 3, len - 3);
    s_st.reply_text[len - 3] = 0;
    xSemaphoreGive(s_lock);
    if (v[2] >= 2) {
        set_reply_window(false);  // done or failed: the normal idle timeout takes over
    }
    ESP_LOGI(TAG, "REPLY clip %u state %u: %.40s", get16(v), v[2], (const char *)v + 3);
}

static void handle_rx(const uint8_t *b, uint16_t len)
{
    if (len < 3 || get16(b + 1) != len - 3) {
        send_error(len ? b[0] : 0, ERR_BAD_LENGTH, 0xFFFF);
        return;
    }
    uint8_t type = b[0];
    const uint8_t *v = b + 3;
    uint16_t vlen = len - 3;
    switch (type) {
    case MSG_HELLO:
        if (vlen != 9) {
            send_error(type, ERR_BAD_LENGTH, 0xFFFF);
            return;
        }
        if (get64(v + 1) > 1700000000000ULL) {
            uint64_t ms = get64(v + 1);
            struct timeval tv = {.tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)(ms % 1000) * 1000};
            settimeofday(&tv, NULL);
        }
        ESP_LOGI(TAG, "HELLO v%u", v[0]);
        send_status();
        break;
    case MSG_LIST:
        send_list();
        break;
    case MSG_GET:
        if (vlen != 6) {
            send_error(type, ERR_BAD_LENGTH, 0xFFFF);
            return;
        }
        handle_get(get16(v), get32(v + 2));
        break;
    case MSG_ACK:
        if (vlen != 6) {
            send_error(type, ERR_BAD_LENGTH, 0xFFFF);
            return;
        }
        handle_ack(get16(v), get32(v + 2));
        break;
    case MSG_WIFI_SET:
        handle_wifi(v, vlen);
        break;
    case MSG_CFG_SET:
        handle_cfg(v, vlen);
        break;
    case MSG_REPLY:
        handle_reply(v, vlen);
        break;
    default:
        break;  // rule 5: unknown types are ignored
    }
}

// ---------- worker ----------

static void worker_task(void *arg)
{
    ev_t ev;
    bool more = false;
    for (;;) {
        if (xQueueReceive(s_ev, &ev, more ? 0 : pdMS_TO_TICKS(200)) == pdTRUE) {
            switch (ev.type) {
            case EV_RX:
                handle_rx(ev.buf, ev.len);
                break;
            case EV_CLIP_STARTED: {
                refresh_pending();
                s_dev.recording = true;  // the status task notices only up to 200 ms later
                uint8_t v[15];
                memcpy(v, ev.buf, 2);
                v[2] = 1;  // Ogg Opus
                put32(v + 3, 16000);
                put64(v + 7, utc_now_ms());
                send_ctrl(MSG_CLIP_STARTED, v, sizeof v);
                send_status();
                break;
            }
            case EV_CLIP_FINISHED:
                refresh_pending();
                s_dev.recording = false;
                send_status();
                break;
            case EV_STATUS:
                send_status();
                break;
            case EV_CLIPS_CHANGED:
                refresh_pending();
                send_status();
                break;
            case EV_CONNECTED: {
                tx_stop();  // nothing from an earlier link carries over
                uint16_t conn = s_conn;
                if (conn != LINK_CONN_NONE) {
                    link_setup(conn);
                }
                break;
            }
            case EV_DISCONNECTED:
                tx_stop();
                break;
            }
        }
        more = tx_step();
        if (s_reply_deadline_us && esp_timer_get_time() > s_reply_deadline_us) {
            set_reply_window(false);
        }
    }
}

// ---------- advertising ----------

static void adv_apply(void)
{
    if (!s_radio_up || s_conn != LINK_CONN_NONE || s_adv_mode == BLE_ADV_OFF) {
        return;
    }
    int64_t now = esp_timer_get_time();
    bool fast = s_adv_mode == BLE_ADV_DELIVER && now < s_fast_adv_until_us;
    uint16_t itvl = s_adv_mode == BLE_ADV_IDLE ? 1636 : fast ? 32 : 244;  // 0.625 ms units
    link_adv_start(itvl, fast ? (int32_t)((s_fast_adv_until_us - now) / 1000) : -1);
}

// ---------- radio events (ble_link.h; the radio's task) ----------

void proto_ready(bool up)
{
    s_radio_up = up;
    if (up) {
        adv_apply();
    }
}

void proto_connected(uint16_t conn)
{
    s_conn = conn;
    s_ctrl_sub = s_data_sub = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.connected = true;
    s_st.authenticated = false;
    xSemaphoreGive(s_lock);
    // HCI commands issued from inside NimBLE's GAP callback time out (seen on IDF 5.5.5: LE Set Data
    // Length never acked, host reset). The worker sets up the link instead.
    post(EV_CONNECTED, NULL, 0);
}

void proto_disconnected(int reason)
{
    s_conn = LINK_CONN_NONE;
    s_ctrl_sub = s_data_sub = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.connected = s_st.authenticated = false;
    s_st.passkey = 0;
    xSemaphoreGive(s_lock);
    s_ind_ok = false;
    xSemaphoreGive(s_ind_done);  // release a worker waiting on a confirmation
    post(EV_DISCONNECTED, NULL, 0);
    adv_apply();
}

void proto_subscribed(bool ctrl, bool on)
{
    if (ctrl) {
        s_ctrl_sub = on;
    } else {
        s_data_sub = on;
    }
}

proto_write_t proto_ctrl_write(uint16_t conn, const uint8_t *value, uint16_t len)
{
    // Checked here rather than with GATT permissions so bench mode can skip it at run time. Android
    // answers this error by pairing (PROTOCOL.md "Security").
    if (!s_bench && !link_authenticated(conn)) {
        return PROTO_WRITE_AUTHEN;
    }
    if (len > TLV_MAX) {
        return PROTO_WRITE_LEN;
    }
    ev_t ev = {.type = EV_RX, .len = len};
    memcpy(ev.buf, value, len);
    return xQueueSend(s_ev, &ev, 0) == pdTRUE ? PROTO_WRITE_OK : PROTO_WRITE_QUEUE_FULL;
}

void proto_indicate_done(bool confirmed)
{
    s_ind_ok = confirmed;
    xSemaphoreGive(s_ind_done);
}

void proto_adv_done(void)
{
    adv_apply();
}

uint32_t proto_passkey(void)
{
    uint32_t p;
    do {
        p = esp_random() % 1000000;
    } while (p == 0);  // 0 means "no passkey" in ble_status_t
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.passkey = p;
    xSemaphoreGive(s_lock);
    return p;
}

void proto_security(bool authenticated)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.authenticated = authenticated;
    s_st.passkey = 0;
    xSemaphoreGive(s_lock);
}

// ---------- public ----------

esp_err_t ble_init(void)
{
    s_ev = xQueueCreate(8, sizeof(ev_t));
    s_ind_done = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ev && s_ind_done && s_lock, ESP_ERR_NO_MEM, TAG, "rtos objects");

    s_dev.pending = (uint16_t)clipstore_count();

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(worker_task, "ble_proto", 6144, NULL, 8, &s_worker, 0,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "worker");
    ESP_RETURN_ON_ERROR(link_init(), TAG, "radio");
    ESP_LOGI(TAG, "BLE up: %u clips pending", s_dev.pending);
    return ESP_OK;
}

void ble_set_adv_mode(ble_adv_mode_t mode)
{
    if (!s_ev || mode == s_adv_mode) {
        return;
    }
    s_adv_mode = mode;
    if (mode == BLE_ADV_DELIVER) {
        s_fast_adv_until_us = esp_timer_get_time() + FAST_ADV_MS * 1000LL;
    }
    if (mode == BLE_ADV_OFF) {
        link_adv_stop();
    } else {
        adv_apply();
    }
}

void ble_get_status(ble_status_t *out)
{
    if (!s_lock) {
        memset(out, 0, sizeof *out);
        out->reply_state = -1;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    out->bench = s_bench;
    out->pending = s_dev.pending;
    xSemaphoreGive(s_lock);
}

bool ble_busy(void)
{
    if (!s_lock || s_conn == LINK_CONN_NONE) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_st.transferring || s_st.reply_window;
    xSemaphoreGive(s_lock);
    return busy;
}

void ble_note_device_status(int battery_pct, bool charging, bool usb_power, bool recording)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = battery_pct != s_dev.battery_pct || charging != s_dev.charging || usb_power != s_dev.usb ||
                   recording != s_dev.recording;
    s_dev.battery_pct = battery_pct;
    s_dev.charging = charging;
    s_dev.usb = usb_power;
    s_dev.recording = recording;
    xSemaphoreGive(s_lock);
    if (changed) {
        if (battery_pct >= 0) {
            link_set_battery((uint8_t)battery_pct);
        }
        if (s_conn != LINK_CONN_NONE) {
            post(EV_STATUS, NULL, 0);
        }
    }
}

void ble_clip_event(const audio_clip_result_t *clip, bool started)
{
    if (!s_ev || (!started && clip->id == 0)) {
        return;
    }
    // The worker recounts the stored clips: STATUS carries the pending count.
    uint8_t id[2];
    put16(id, clip->id);
    post(started ? EV_CLIP_STARTED : EV_CLIP_FINISHED, id, 2);
}

void ble_clips_changed(void)
{
    if (s_ev) {
        post(EV_CLIPS_CHANGED, NULL, 0);
    }
}

void ble_inject(const uint8_t *tlv, size_t len)
{
    if (!s_ev || len > TLV_MAX) {
        return;
    }
    s_console = true;
    post(EV_RX, tlv, len);
}

void ble_console_link(bool on)
{
    s_console = on;
    if (!on && s_ev) {
        post(EV_DISCONNECTED, NULL, 0);  // ends a transfer the console link started
    }
}

void ble_set_bench(bool on)
{
    s_bench = on;
    ESP_LOGW(TAG, "bench mode %s", on ? "ON: protocol messages accepted without pairing until reboot" : "off");
}

esp_err_t ble_set_tx_power(int dbm)
{
    return link_set_tx_power(s_conn, dbm);
}

int ble_conn_rssi(void)
{
    return link_rssi(s_conn);
}

esp_err_t ble_forget_bonds(void)
{
    return link_forget_bonds();
}

void ble_shutdown(void)
{
    if (!s_ev) {
        return;
    }
    s_adv_mode = BLE_ADV_OFF;
    link_adv_stop();
    uint16_t conn = s_conn;
    if (conn != LINK_CONN_NONE) {
        link_disconnect(conn);
        for (int i = 0; i < 50 && s_conn != LINK_CONN_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
