#pragma once
// The radio under the protocol layer in ble.c. On the coin this is NimBLE (ble_nimble.c); in the
// desktop simulator it is the host's Bluetooth bridge (sim/coin/src/ble_link_sim.c).
//
// ble.c calls link_*. The radio calls proto_* from its own task.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define LINK_CONN_NONE 0xFFFF
#define LINK_VALUE_MAX 244  // one ATT operation at MTU 247 (PROTOCOL.md rule 6)

typedef enum { LINK_OK = 0, LINK_NO_MEM, LINK_FAIL } link_rc_t;
typedef enum { PROTO_WRITE_OK = 0, PROTO_WRITE_AUTHEN, PROTO_WRITE_LEN, PROTO_WRITE_QUEUE_FULL } proto_write_t;

// ---------- radio ----------

// Starts the stack. proto_ready(true) follows once it can advertise.
esp_err_t link_init(void);
// Connectable advertising with the coinai service and name. Interval in 0.625 ms units; when
// duration_ms (>= 0) runs out, proto_adv_done() follows.
void link_adv_start(uint16_t interval, int32_t duration_ms);
void link_adv_stop(void);
bool link_authenticated(uint16_t conn);
// An indication on ctrl; proto_indicate_done() reports the confirmation.
link_rc_t link_indicate(uint16_t conn, const uint8_t *value, uint16_t len);
// A notification on data. LINK_NO_MEM: the radio's buffers are full; try again shortly.
link_rc_t link_notify(uint16_t conn, const uint8_t *value, uint16_t len);
uint16_t link_mtu(uint16_t conn);
// Connection parameters for a transfer (PROTOCOL.md "Link").
void link_transfer_params(uint16_t conn);
// Data length and PHY (PROTOCOL.md "Link"). Called from the protocol worker, never from a radio callback.
void link_setup(uint16_t conn);
void link_set_battery(uint8_t pct);
esp_err_t link_set_tx_power(uint16_t conn, int dbm);
int link_rssi(uint16_t conn);
esp_err_t link_forget_bonds(void);
void link_disconnect(uint16_t conn);

// ---------- protocol layer (ble.c), called by the radio ----------

void proto_ready(bool up);
void proto_connected(uint16_t conn);
void proto_disconnected(int reason);
void proto_subscribed(bool ctrl, bool on);
// A write to ctrl. The answer becomes the ATT response.
proto_write_t proto_ctrl_write(uint16_t conn, const uint8_t *value, uint16_t len);
void proto_indicate_done(bool confirmed);
// A timed advertising phase ended, or a connection attempt failed.
void proto_adv_done(void);
// The passkey to display for pairing (1..999999).
uint32_t proto_passkey(void);
void proto_security(bool authenticated);
