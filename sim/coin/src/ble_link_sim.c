// The radio under ble.c for the simulator: ble_link.h over the host's Bluetooth bridge
// (sim/IPC.md "BLE"). Host events run on their own task, as NimBLE's run on its host task.
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "ble_link.h"
#include "sim_esp.h"
#include "simlink.h"

#define START_CREDITS 12

typedef struct {
    uint8_t type;
    uint16_t len;
    uint8_t data[];
} msg_t;

static QueueHandle_t s_inbox;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static volatile bool s_started;  // link_init ran
static volatile bool s_ready;    // BLE_READY arrived
static volatile uint16_t s_conn = LINK_CONN_NONE;
static volatile bool s_authenticated;
static volatile uint16_t s_mtu = 23;
static volatile int s_rssi;
static volatile LONG s_credits;
static volatile uint16_t s_ind_seq;
static SemaphoreHandle_t s_ready_once;

static BOOL CALLBACK init_once(PINIT_ONCE o, void *p, void **c)
{
    s_inbox = xQueueCreate(64, sizeof(msg_t *));
    return TRUE;
}

void sim_ble_input(uint8_t type, const uint8_t *data, size_t len)
{
    InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
    msg_t *m = malloc(sizeof *m + len);
    m->type = type;
    m->len = (uint16_t)len;
    memcpy(m->data, data, len);
    if (xQueueSend(s_inbox, &m, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE("ble_sim", "radio task stuck: dropped event 0x%02x", type);
        free(m);
    }
}

static void handle(const msg_t *m)
{
    const uint8_t *p = m->data;
    switch (m->type) {
    case IPC_BLE_READY:
        s_ready = true;
        proto_ready(true);
        break;
    case IPC_BLE_CONNECTED:
        s_mtu = 23;
        s_authenticated = false;
        InterlockedExchange(&s_credits, START_CREDITS);
        s_conn = sim_get16(p);
        proto_connected(s_conn);
        break;
    case IPC_BLE_DISCONNECTED:
        s_conn = LINK_CONN_NONE;
        s_authenticated = false;
        proto_disconnected(m->len ? p[0] : 0x13);
        break;
    case IPC_BLE_IND_DONE:
        if (m->len >= 3 && sim_get16(p) == s_ind_seq) {
            proto_indicate_done(p[2] != 0);
        }
        break;
    case IPC_BLE_CREDIT:
        InterlockedExchangeAdd(&s_credits, sim_get16(p));
        break;
    case IPC_BLE_SECURITY:
        s_authenticated = m->len >= 2 && p[0] && p[1];
        proto_security(s_authenticated);
        break;
    case IPC_BLE_MTU:
        s_mtu = sim_get16(p);
        break;
    case IPC_BLE_SUBSCRIBE:
        proto_subscribed(p[0] == 1, p[1] != 0);
        break;
    case IPC_BLE_WRITE: {
        uint8_t ans[3];
        sim_put16(ans, sim_get16(p));
        proto_write_t r = proto_ctrl_write(s_conn, p + 2, (uint16_t)(m->len - 2));
        ans[2] = r == PROTO_WRITE_OK       ? 0
                 : r == PROTO_WRITE_AUTHEN ? 1
                 : r == PROTO_WRITE_LEN    ? 2
                                           : 3;
        sim_send(IPC_BLE_WRITE_RESULT, ans, 3);
        break;
    }
    case IPC_BLE_PASSKEY_REQ: {
        uint8_t pk[4];
        sim_put32(pk, proto_passkey());
        sim_send(IPC_BLE_PASSKEY, pk, 4);
        break;
    }
    case IPC_BLE_RSSI:
        s_rssi = (int8_t)p[0];
        break;
    case IPC_BLE_ADV_DONE:
        proto_adv_done();
        break;
    }
}

static void radio_task(void *arg)
{
    for (;;) {
        msg_t *m;
        if (xQueueReceive(s_inbox, &m, portMAX_DELAY) == pdTRUE) {
            handle(m);
            free(m);
        }
    }
}

esp_err_t link_init(void)
{
    InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
    if (!s_started) {
        s_started = true;
        xTaskCreatePinnedToCore(radio_task, "nimble_host", 4096, NULL, 5, NULL, 0);
    }
    return ESP_OK;
}

void link_adv_start(uint16_t interval, int32_t duration_ms)
{
    uint8_t m[7] = {1};
    sim_put16(m + 1, interval);
    sim_put32(m + 3, (uint32_t)(duration_ms < 0 ? -1 : duration_ms));
    sim_send(IPC_BLE_ADV, m, sizeof m);
}

void link_adv_stop(void)
{
    uint8_t m[7] = {0};
    sim_put32(m + 3, (uint32_t)-1);
    sim_send(IPC_BLE_ADV, m, sizeof m);
}

bool link_authenticated(uint16_t conn)
{
    return conn != LINK_CONN_NONE && conn == s_conn && s_authenticated;
}

link_rc_t link_indicate(uint16_t conn, const uint8_t *value, uint16_t len)
{
    if (conn != s_conn || s_conn == LINK_CONN_NONE) {
        return LINK_FAIL;
    }
    uint8_t seq[2];
    sim_put16(seq, ++s_ind_seq);
    sim_send2(IPC_BLE_INDICATE, seq, 2, value, len);
    return LINK_OK;
}

link_rc_t link_notify(uint16_t conn, const uint8_t *value, uint16_t len)
{
    if (conn != s_conn || s_conn == LINK_CONN_NONE) {
        return LINK_FAIL;
    }
    if (InterlockedDecrement(&s_credits) < 0) {
        InterlockedIncrement(&s_credits);
        return LINK_NO_MEM;
    }
    sim_send(IPC_BLE_NOTIFY, value, len);
    return LINK_OK;
}

uint16_t link_mtu(uint16_t conn)
{
    return s_mtu;
}

void link_transfer_params(uint16_t conn)
{
    // As ble_nimble.c: 15 ms interval, latency 0, 6 s supervision timeout.
    uint8_t m[8];
    sim_put16(m, 12);
    sim_put16(m + 2, 12);
    sim_put16(m + 4, 0);
    sim_put16(m + 6, 600);
    sim_send(IPC_BLE_CONN_PARAMS, m, sizeof m);
}

void link_setup(uint16_t conn)
{
    sim_send(IPC_BLE_LINK_SETUP, NULL, 0);
}

void link_set_battery(uint8_t pct)
{
    sim_send(IPC_BLE_BATTERY, &pct, 1);
}

esp_err_t link_set_tx_power(uint16_t conn, int dbm)
{
    int8_t v = (int8_t)dbm;
    sim_send(IPC_BLE_TX_POWER, &v, 1);
    return ESP_OK;
}

int link_rssi(uint16_t conn)
{
    return conn == s_conn && s_conn != LINK_CONN_NONE ? s_rssi : 0;
}

esp_err_t link_forget_bonds(void)
{
    sim_send(IPC_BLE_FORGET, NULL, 0);
    return ESP_OK;
}

void link_disconnect(uint16_t conn)
{
    sim_send(IPC_BLE_DISCONNECT, NULL, 0);
}
