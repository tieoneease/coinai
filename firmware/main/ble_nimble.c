// NimBLE under the protocol layer in ble.c (see ble_link.h): GATT table, GAP events, advertising,
// pairing and bonds.
#include "ble_link.h"

#include <string.h>
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/bas/ble_svc_bas.h"
#include "services/dis/ble_svc_dis.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

void ble_store_config_init(void);

// c0a1XXXX-f569-41ae-8676-7f5a60d835fc, in NimBLE's little-endian byte order.
#define COINAI_UUID(n) \
    BLE_UUID128_INIT(0xfc, 0x35, 0xd8, 0x60, 0x5a, 0x7f, 0x76, 0x86, 0xae, 0x41, 0x69, 0xf5, (n), 0x00, 0xa1, 0xc0)

static const ble_uuid128_t s_svc_uuid = COINAI_UUID(0x00);
static const ble_uuid128_t s_ctrl_uuid = COINAI_UUID(0x01);
static const ble_uuid128_t s_data_uuid = COINAI_UUID(0x02);
static uint16_t s_ctrl_handle, s_data_handle;
static uint8_t s_own_addr_type;

static int gap_event(struct ble_gap_event *event, void *arg);

// ---------- GATT ----------

static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[LINK_VALUE_MAX];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= sizeof buf && ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, NULL) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    switch (proto_ctrl_write(conn, buf, len)) {
    case PROTO_WRITE_OK:
        return 0;
    case PROTO_WRITE_AUTHEN:
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    case PROTO_WRITE_LEN:
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    default:
        return BLE_ATT_ERR_PREPARE_QUEUE_FULL;
    }
}

static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_UNLIKELY;  // notify only
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &s_ctrl_uuid.u, .access_cb = ctrl_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_INDICATE,
             .val_handle = &s_ctrl_handle},
            {.uuid = &s_data_uuid.u, .access_cb = data_access, .flags = BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &s_data_handle},
            {0},
        },
    },
    {0},
};

// ---------- radio operations ----------

void link_adv_start(uint16_t interval, int32_t duration_ms)
{
    ble_gap_adv_stop();
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    f.name = (uint8_t *)"coinai";
    f.name_len = 6;
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc %d", rc);
        return;
    }
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = interval,
        .itvl_max = interval,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, duration_ms < 0 ? BLE_HS_FOREVER : duration_ms, &p, gap_event,
                           NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc %d", rc);
    }
}

void link_adv_stop(void)
{
    ble_gap_adv_stop();
}

bool link_authenticated(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    return ble_gap_conn_find(conn, &d) == 0 && d.sec_state.encrypted && d.sec_state.authenticated;
}

link_rc_t link_indicate(uint16_t conn, const uint8_t *value, uint16_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(value, len);
    if (!om) {
        return LINK_NO_MEM;
    }
    int rc = ble_gatts_indicate_custom(conn, s_ctrl_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "indicate rc %d", rc);
        return LINK_FAIL;
    }
    return LINK_OK;
}

link_rc_t link_notify(uint16_t conn, const uint8_t *value, uint16_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(value, len);
    int rc = om ? ble_gatts_notify_custom(conn, s_data_handle, om) : BLE_HS_ENOMEM;
    if (rc == BLE_HS_ENOMEM) {
        return LINK_NO_MEM;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "notify rc %d", rc);
        return LINK_FAIL;
    }
    return LINK_OK;
}

uint16_t link_mtu(uint16_t conn)
{
    return ble_att_mtu(conn);
}

void link_transfer_params(uint16_t conn)
{
    // PROTOCOL.md "Link": 15 ms interval, latency 0, 6 s supervision timeout while transferring.
    struct ble_gap_upd_params p = {
        .itvl_min = 12, .itvl_max = 12, .latency = 0, .supervision_timeout = 600,
        .min_ce_len = 0, .max_ce_len = 0,
    };
    ble_gap_update_params(conn, &p);
}

void link_setup(uint16_t conn)
{
    // PROTOCOL.md "Link". The phone, as GATT client, asks for the 247-byte MTU.
    int rc1 = ble_gap_set_data_len(conn, 251, 2120);
    int rc2 = ble_gap_set_prefered_le_phy(conn, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK,
                                          BLE_GAP_LE_PHY_CODED_ANY);
    if (rc1 || rc2) {
        ESP_LOGW(TAG, "link setup: data length rc %d, 2M PHY rc %d", rc1, rc2);
    }
}

void link_set_battery(uint8_t pct)
{
    ble_svc_bas_battery_level_set(pct);
}

esp_err_t link_set_tx_power(uint16_t conn, int dbm)
{
    int level = dbm >= 20 ? ESP_PWR_LVL_P20 : dbm <= -24 ? ESP_PWR_LVL_N24 : (dbm + 24) / 3;
    esp_err_t err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, level);
    if (err == ESP_OK) {
        err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, level);
    }
    if (err == ESP_OK && conn != LINK_CONN_NONE) {
        err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, level);
    }
    ESP_LOGI(TAG, "TX power level %d (%d dBm requested): %s", level, dbm, esp_err_to_name(err));
    return err;
}

int link_rssi(uint16_t conn)
{
    int8_t rssi = 0;
    return conn != LINK_CONN_NONE && ble_gap_conn_rssi(conn, &rssi) == 0 ? rssi : 0;
}

esp_err_t link_forget_bonds(void)
{
    return ble_store_clear() == 0 ? ESP_OK : ESP_FAIL;
}

void link_disconnect(uint16_t conn)
{
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

// ---------- GAP ----------

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            proto_adv_done();
            return 0;
        }
        ESP_LOGI(TAG, "connected");
        proto_connected(event->connect.conn_handle);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason 0x%x", event->disconnect.reason);
        proto_disconnected(event->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        proto_adv_done();  // end of the fast phase: continue at the slower interval
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ctrl_handle) {
            proto_subscribed(true, event->subscribe.cur_indicate);
        } else if (event->subscribe.attr_handle == s_data_handle) {
            proto_subscribed(false, event->subscribe.cur_notify);
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        // Status 0 = indication sent; BLE_HS_EDONE = confirmed; anything else ends it too.
        if (event->notify_tx.indication && event->notify_tx.status != 0) {
            proto_indicate_done(event->notify_tx.status == BLE_HS_EDONE);
        }
        return 0;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY tx %d rx %d (status %d)", event->phy_updated.tx_phy, event->phy_updated.rx_phy,
                 event->phy_updated.status);
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "connection interval %.2f ms, latency %d, timeout %d ms", desc.conn_itvl * 1.25,
                     desc.conn_latency, desc.supervision_timeout * 10);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %u", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = proto_passkey()};
            ESP_LOGI(TAG, "pairing: passkey shown on screen");
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            proto_security(desc.sec_state.encrypted && desc.sec_state.authenticated);
            ESP_LOGI(TAG, "encryption %s (status %d, authenticated %d, bonded %d)",
                     desc.sec_state.encrypted ? "on" : "off", event->enc_change.status, desc.sec_state.authenticated,
                     desc.sec_state.bonded);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The phone lost its bond: forget ours and pair again.
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    proto_ready(true);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
    proto_ready(false);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t link_init(void)
{
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init");
    diag_mem_mark("nimble_port");
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_bas_init();
    ble_svc_dis_init();
    ble_svc_dis_manufacturer_name_set("coinai");
    ble_svc_dis_model_number_set("ESP32-S3-Touch-AMOLED-1.75C");
    ble_svc_dis_firmware_revision_set(esp_app_get_description()->version);
    ESP_RETURN_ON_FALSE(ble_gatts_count_cfg(s_svcs) == 0 && ble_gatts_add_svcs(s_svcs) == 0, ESP_FAIL, TAG,
                        "gatt services");
    ble_svc_gap_device_name_set("coinai");
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
