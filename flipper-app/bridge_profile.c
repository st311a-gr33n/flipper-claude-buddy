/**
 * Bridge-mode BLE Serial profile — implementation.
 *
 * A non-secured drop-in replacement for the stock ble_profile_serial.
 * The stock profile's characteristics require ATTR_PERMISSION_AUTHEN_*
 * which forces MITM pairing; this profile uses ATTR_PERMISSION_NONE so
 * a host bridge can connect without pairing (matching what the Desktop /
 * NUS transport already does).
 *
 * Structure follows the upstream ble_profile_serial / nus_profile.c
 * patterns:
 *   - Register a custom primary service (UUID 0x3082)
 *   - RX characteristic (host→device, write / write-without-response)
 *   - TX characteristic (device→host, notify)
 *   - Hook the BLE event dispatcher to receive writes
 *   - Use ble_gatt_characteristic_update() for TX notifications
 */

#include "bridge_profile.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>
#include <gap.h>

#include <ble/core/ble_defs.h>
#include <ble/core/ble_std.h>
#include <ble/core/auto/ble_types.h>
#include <compiler.h>

#define TAG "BridgeProfile"

/* ── ST BlueNRG event wrappers (not in FAP SDK; redeclared locally) ── */

typedef __PACKED_STRUCT {
    uint8_t type;
    uint8_t data[1];
}
hci_uart_pckt_local;

typedef __PACKED_STRUCT {
    uint8_t evt;
    uint8_t plen;
    uint8_t data[1];
}
hci_event_pckt_local;

typedef __PACKED_STRUCT {
    uint16_t ecode;
    uint8_t data[1];
}
evt_blecore_aci_local;

#ifndef ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE
#define ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE 0x0C01
#endif

/* ── BLE UUIDs (little-endian 128-bit for ST BlueNRG) ────────────── */
/* Svc:     00003082-0000-1000-8000-00805f9b34fb  (0x3082) */
/* TX char: 19ed82ae-ed21-4c9d-4145-228e61fe0000 */
/* RX char: 19ed82ae-ed21-4c9d-4145-228e62fe0000 */

static const uint8_t BRIDGE_SVC_UUID[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x82, 0x30, 0x00, 0x00};

static const uint8_t BRIDGE_RX_CHAR_UUID[16] = {
    0x00, 0x00, 0xfe, 0x62, 0x8e, 0x22, 0x45, 0x41,
    0x9d, 0x4c, 0x21, 0xed, 0xae, 0x82, 0xed, 0x19};

static const uint8_t BRIDGE_TX_CHAR_UUID[16] = {
    0x00, 0x00, 0xfe, 0x61, 0x8e, 0x22, 0x45, 0x41,
    0x9d, 0x4c, 0x21, 0xed, 0xae, 0x82, 0xed, 0x19};

/* ── characteristic descriptors ─────────────────────────────────── */

typedef enum {
    BridgeCharRx = 0,
    BridgeCharTx,
    BridgeCharCount,
} BridgeCharId;

/* No authentication — matching the NUS transport's NUS_CHAR_SEC. */
#define BRIDGE_CHAR_SEC ATTR_PERMISSION_NONE

/* Forward decl — callback-backed TX data source (see bridge_tx_read_cb). */
static bool bridge_tx_read_cb(const void* context, const uint8_t** data, uint16_t* data_len);

static const BleGattCharacteristicParams bridge_chars[BridgeCharCount] = {
    [BridgeCharRx] =
        {.name = "Bridge RX",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = BRIDGE_PROFILE_TX_CHUNK,
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_WRITE,
         .security_permissions = BRIDGE_CHAR_SEC,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [BridgeCharTx] = {
        .name = "Bridge TX",
        .data_prop_type = FlipperGattCharacteristicDataCallback,
        .data.callback.fn = bridge_tx_read_cb,
        .data.callback.context = NULL,
        .uuid_type = UUID_TYPE_128,
        .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
        .security_permissions = BRIDGE_CHAR_SEC,
        .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
        .is_variable = CHAR_VALUE_LEN_VARIABLE}};

/* ── profile state ──────────────────────────────────────────────── */

typedef struct {
    FuriHalBleProfileBase base;

    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[BridgeCharCount];
    GapSvcEventHandler* event_handler;

    BridgeProfileRxCallback rx_cb;
    void* rx_ctx;
} BleProfileBridge;

_Static_assert(offsetof(BleProfileBridge, base) == 0, "Wrong layout");

/* ── callback-backed TX ──────────────────────────────────────────── */

typedef struct {
    const uint8_t* data;
    uint16_t len;
} BridgeTxPayload;

static bool
    bridge_tx_read_cb(const void* context, const uint8_t** data, uint16_t* data_len) {
    if(!context || !data) {
        if(data_len) *data_len = BRIDGE_PROFILE_TX_CHUNK;
        return false;
    }
    const BridgeTxPayload* p = context;
    *data = p->data;
    *data_len = p->len;
    return false;
}

/* Fill UUID fields that must be set at runtime (BleGattCharacteristicParams
 * stores UUID by value, so copy before init). */
static void bridge_fill_char_uuids(BleGattCharacteristicParams out[BridgeCharCount]) {
    memcpy(out, bridge_chars, sizeof(bridge_chars));
    memcpy(out[BridgeCharRx].uuid.Char_UUID_128, BRIDGE_RX_CHAR_UUID, 16);
    memcpy(out[BridgeCharTx].uuid.Char_UUID_128, BRIDGE_TX_CHAR_UUID, 16);
}

/* ── event dispatcher callback (BLE thread) ─────────────────────── */

static BleEventAckStatus bridge_event_handler(void* event, void* context) {
    BleProfileBridge* bp = context;
    BleEventAckStatus ret = BleEventNotAck;

    hci_event_pckt_local* pckt =
        (hci_event_pckt_local*)(((hci_uart_pckt_local*)event)->data);
    if(pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return ret;

    evt_blecore_aci_local* core_evt = (evt_blecore_aci_local*)pckt->data;
    if(core_evt->ecode != ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) return ret;

    aci_gatt_attribute_modified_event_rp0* mod =
        (aci_gatt_attribute_modified_event_rp0*)core_evt->data;

    if(mod->Attr_Handle == bp->chars[BridgeCharRx].handle + 1) {
        if(bp->rx_cb) {
            bp->rx_cb(mod->Attr_Data, mod->Attr_Data_Length, bp->rx_ctx);
        }
        ret = BleEventAckFlowEnable;
    }
    return ret;
}

/* ── template hooks ─────────────────────────────────────────────── */

static FuriHalBleProfileBase* bridge_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);

    BleProfileBridge* bp = malloc(sizeof(BleProfileBridge));
    memset(bp, 0, sizeof(*bp));
    bp->base.config = ble_profile_bridge;

    bp->event_handler =
        ble_event_dispatcher_register_svc_handler(bridge_event_handler, bp);

    Service_UUID_t svc_uuid = {0};
    memcpy(svc_uuid.Service_UUID_128, BRIDGE_SVC_UUID, 16);
    if(!ble_gatt_service_add(
           UUID_TYPE_128, &svc_uuid, PRIMARY_SERVICE, 8, &bp->svc_handle)) {
        FURI_LOG_E(TAG, "ble_gatt_service_add failed");
        ble_event_dispatcher_unregister_svc_handler(bp->event_handler);
        free(bp);
        return NULL;
    }

    BleGattCharacteristicParams chars[BridgeCharCount];
    bridge_fill_char_uuids(chars);
    for(uint8_t i = 0; i < BridgeCharCount; i++) {
        ble_gatt_characteristic_init(bp->svc_handle, &chars[i], &bp->chars[i]);
    }

    FURI_LOG_I(TAG, "Bridge profile started (svc=%u)", bp->svc_handle);
    return &bp->base;
}

static void bridge_profile_stop(FuriHalBleProfileBase* profile) {
    furi_check(profile);
    furi_check(profile->config == ble_profile_bridge);

    BleProfileBridge* bp = (BleProfileBridge*)profile;
    ble_event_dispatcher_unregister_svc_handler(bp->event_handler);
    for(uint8_t i = 0; i < BridgeCharCount; i++) {
        ble_gatt_characteristic_delete(bp->svc_handle, &bp->chars[i]);
    }
    ble_gatt_service_delete(bp->svc_handle);
    free(bp);
}

#define CONN_INTERVAL_MIN 0x06 /* 7.5 ms */
#define CONN_INTERVAL_MAX 0x24 /* 45 ms   */

static void bridge_profile_get_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    memset(config, 0, sizeof(*config));

    /* Adv layout: 16-bit UUID placeholder (4 B AD) so we stay under
     * the 31 B PDU limit.  The real 128-bit UUID is in the GATT
     * service table — the host bridge scans for this 16-bit UUID. */
    config->adv_service.UUID_Type = UUID_TYPE_16;
    config->adv_service.Service_UUID_16 = 0x3082;
    config->appearance_char = 0x8600;

    /* Pairing DISABLED — matching Desktop/NUS behaviour.  The host
     * bridge connects without pairing; characteristics use
     * ATTR_PERMISSION_NONE so unencrypted access works. */
    config->bonding_mode = false;
    config->pairing_method = GapPairingNone;
    config->conn_param.conn_int_min = CONN_INTERVAL_MIN;
    config->conn_param.conn_int_max = CONN_INTERVAL_MAX;
    config->conn_param.slave_latency = 0;
    config->conn_param.supervisor_timeout = 0;

    /* Use a distinct MAC so macOS CoreBluetooth doesn't collide with
     * the stock Flipper firmware's cached CBPeripheral.name (factory MAC)
     * or the NUS / Desktop profile (XOR 0x01). */
    memcpy(
        config->mac_address,
        furi_hal_version_get_ble_mac(),
        sizeof(config->mac_address));
    config->mac_address[0] ^= 0x02;

    /* Device name: "Flipper" by default, overridable via app settings. */
    const char* name = "Flipper";
    config->adv_name[0] = 0x09; /* AD_TYPE_COMPLETE_LOCAL_NAME */
    strlcpy(
        config->adv_name + 1, name, FURI_HAL_VERSION_DEVICE_NAME_LENGTH - 1);
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = bridge_profile_start,
    .stop = bridge_profile_stop,
    .get_gap_config = bridge_profile_get_gap_config,
};

const FuriHalBleProfileTemplate* const ble_profile_bridge = &profile_callbacks;

/* ── public API ─────────────────────────────────────────────────── */

bool bridge_profile_tx(
    FuriHalBleProfileBase* profile,
    const uint8_t* data,
    uint16_t size) {
    furi_check(profile && profile->config == ble_profile_bridge);
    furi_check(data);

    BleProfileBridge* bp = (BleProfileBridge*)profile;

    uint16_t sent = 0;
    while(sent < size) {
        uint16_t chunk = size - sent;
        if(chunk > BRIDGE_PROFILE_TX_CHUNK) chunk = BRIDGE_PROFILE_TX_CHUNK;
        BridgeTxPayload payload = {.data = data + sent, .len = chunk};
        if(!ble_gatt_characteristic_update(
               bp->svc_handle, &bp->chars[BridgeCharTx], &payload)) {
            FURI_LOG_E(TAG, "TX update failed at offset %u", sent);
            return false;
        }
        sent += chunk;
    }
    return true;
}

void bridge_profile_set_rx_callback(
    FuriHalBleProfileBase* profile,
    BridgeProfileRxCallback callback,
    void* context) {
    furi_check(profile && profile->config == ble_profile_bridge);
    BleProfileBridge* bp = (BleProfileBridge*)profile;
    bp->rx_cb = callback;
    bp->rx_ctx = context;
}