/**
 * Bluetooth BLE serial transport.
 *
 * Uses a custom bridge_profile (non-secured) instead of the stock
 * ble_profile_serial, which requires MITM-authenticated pairing that
 * the host bridge's NoInputNoOutput agent cannot satisfy.  The custom
 * profile drops authentication (ATTR_PERMISSION_NONE + GapPairingNone).
 *
 * Flow:
 *   1. Open the Bt service record
 *   2. Start the bridge profile (replaces default RPC profile)
 *   3. Set RX callback and start advertising
 *   4. On exit, restore the default profile
 */

#include "transport.h"
#include "protocol.h"
#include "bridge_profile.h"
#include <furi.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt.h>

#define TAG "BtTransport"

typedef struct {
    Transport            base;   /* MUST be first */
    Bt*                  bt;
    FuriHalBleProfileBase* profile;
    TransportRxCallback  callback;
    void*                callback_ctx;
    TransportConnectCallback connect_cb;
    void*                connect_ctx;
    bool                 connected;
    bool                 hello_sent; /* track per-connection hello */
    /* Line-buffered RX */
    char                 rx_buf[PROTOCOL_MAX_MSG_LEN];
    int                  rx_pos;
} BtTransport;

/* ── BLE event callbacks (BLE GAP thread — NOT GUI thread) ──── */

static void bridge_rx_cb(const uint8_t* data, uint16_t len, void* context) {
    BtTransport* bt = context;
    if(!bt || !data) return;

    FURI_LOG_D(TAG, "RX chunk: %u bytes", len);

    /* Line-buffer incoming data, dispatch complete lines.
     *
     * IMPORTANT: Do NOT call bridge_profile_tx() from inside this
     * callback — the BLE stack holds a mutex when calling us, and
     * bridge_profile_tx() needs the same mutex, causing a deadlock
     * on Momentum firmware.  All TX (hello, pong, etc.) goes through
     * the GUI thread via the app callback → message queue path. */
    for(uint16_t i = 0; i < len; i++) {
        if(data[i] == '\n') {
            if(bt->rx_pos > 0) {
                bt->rx_buf[bt->rx_pos] = '\0';

                FURI_LOG_D(TAG, "RX line: %s", bt->rx_buf);

                if(bt->callback) bt->callback(bt->rx_buf, bt->callback_ctx);
                bt->rx_pos = 0;
            }
        } else if(bt->rx_pos < (int)sizeof(bt->rx_buf) - 1) {
            bt->rx_buf[bt->rx_pos++] = (char)data[i];
        }
    }
}

static void bt_status_changed_cb(BtStatus status, void* context) {
    BtTransport* bt = context;
    if(!bt) return;
    bool new_connected = (status == BtStatusConnected);
    bool changed = (new_connected != bt->connected);
    FURI_LOG_I(TAG, "bt_status_changed: status=%d cur_connected=%d new=%d",
               (int)status, (int)bt->connected, (int)new_connected);
    if(new_connected) {
        bt->connected = true;
        bt->hello_sent = false;
    } else {
        bt->connected = false;
        bt->hello_sent = false;
    }
    /* Notify the app of the transition so it can reset its own
     * hello_sent (re-handshake on bridge restart).  Callback runs on
     * the BT stack thread — implementation must not touch UI or call
     * transport_send; dispatch a custom event instead. */
    if(changed && bt->connect_cb) {
        bt->connect_cb(new_connected, bt->connect_ctx);
    }
}

/* ── vtable implementations ────────────────────────────────── */

static void bt_start(Transport* t, TransportRxCallback cb, void* ctx) {
    if(!t) return;
    BtTransport* bt = (BtTransport*)t;
    bt->callback = cb;
    bt->callback_ctx = ctx;
    bt->rx_pos = 0;
    bt->connected = false;
    bt->hello_sent = false;

    bt->bt = furi_record_open(RECORD_BT);

    /* Disconnect any existing connection before switching profiles */
    bt_disconnect(bt->bt);
    furi_delay_ms(200);

    /* Start the bridge profile (restarts BLE core2) */
    bt->profile = bt_profile_start(bt->bt, ble_profile_bridge, NULL);
    if(!bt->profile) {
        FURI_LOG_E(TAG, "Failed to start BLE bridge profile");
        furi_record_close(RECORD_BT);
        bt->bt = NULL;
        return;
    }

    /* Register RX callback and connection-status observer BEFORE
     * advertising so neither data nor the Connected event can race
     * past us.  Without this, a fast-connecting central leaves
     * bt->connected=false which silently drops every TX. */
    bridge_profile_set_rx_callback(bt->profile, bridge_rx_cb, bt);
    bt_set_status_changed_callback(bt->bt, bt_status_changed_cb, bt);

    /* Start advertising */
    furi_hal_bt_start_advertising();

    FURI_LOG_I(TAG, "Bridge BLE started, advertising");
}

static void bt_stop(Transport* t) {
    if(!t) return;
    BtTransport* bt = (BtTransport*)t;
    if(!bt->bt) return;

    bt_set_status_changed_callback(bt->bt, NULL, NULL);
    bt_disconnect(bt->bt);
    furi_delay_ms(200);

    /* Restore default profile so built-in BLE RPC works again */
    bt_profile_restore_default(bt->bt);

    furi_record_close(RECORD_BT);
    bt->bt = NULL;
    bt->profile = NULL;
    bt->connected = false;

    FURI_LOG_I(TAG, "Bridge BLE stopped");
}

static void bt_send(Transport* t, const char* data, int len) {
    if(!t || !data) return;
    BtTransport* bt = (BtTransport*)t;
    if(!bt->profile || !bt->connected) {
        FURI_LOG_D(TAG, "bt_send dropped %d bytes (profile=%d connected=%d)",
                   len, !!bt->profile, !!bt->connected);
        return;
    }

    int offset = 0;
    while(offset < len) {
        int chunk = len - offset;
        if(chunk > BRIDGE_PROFILE_TX_CHUNK) chunk = BRIDGE_PROFILE_TX_CHUNK;
        bridge_profile_tx(
            bt->profile, (uint8_t*)(data + offset), (uint16_t)chunk);
        offset += chunk;
    }
}

static void bt_free(Transport* t) {
    if(!t) return;
    free(t);
}

/* ── public factory ────────────────────────────────────────── */

Transport* transport_bt_alloc(void) {
    BtTransport* bt = malloc(sizeof(BtTransport));
    furi_check(bt != NULL);
    memset(bt, 0, sizeof(BtTransport));
    bt->base.start = bt_start;
    bt->base.stop = bt_stop;
    bt->base.send = bt_send;
    bt->base.free_fn = bt_free;
    return (Transport*)bt;
}

/* Optional observer — only the Bridge-mode BT transport fires this.
 * Callers should set it after alloc and before start; we match on the
 * start/stop vtable entries to be safe if called on a non-BT transport
 * (e.g. NUS or USB), in which case we just no-op. */
void transport_bt_set_connect_callback(
    Transport* t,
    TransportConnectCallback cb,
    void* context) {
    if(!t || t->start != bt_start) return;
    BtTransport* bt = (BtTransport*)t;
    bt->connect_cb = cb;
    bt->connect_ctx = context;
}
