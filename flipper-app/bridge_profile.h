/**
 * Bridge-mode BLE Serial profile — non-secured clone of the stock
 * ble_profile_serial.
 *
 * The stock Flipper serial profile requires MITM-authenticated pairing
 * (GapPairingPinCodeVerify), which the host bridge's NoInputNoOutput
 * agent cannot satisfy.  This custom profile drops the authentication
 * requirement (ATTR_PERMISSION_NONE + GapPairingNone), matching what
 * the Desktop / NUS transport already does.
 *
 * Service:  00003082-0000-1000-8000-00805f9b34fb  (0x3082)
 * RX char:  19ed82ae-ed21-4c9d-4145-228e62fe0000  (host→Flipper, write)
 * TX char:  19ed82ae-ed21-4c9d-4145-228e61fe0000  (Flipper→host, notify)
 */

#pragma once

#include <furi_ble/profile_interface.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max payload per TX notification chunk (capped below ATT MTU-3). */
#define BRIDGE_PROFILE_TX_CHUNK 240

typedef void (*BridgeProfileRxCallback)(const uint8_t* data, uint16_t len, void* context);

/* Profile descriptor.  Pass to bt_profile_start(bt, ble_profile_bridge, NULL). */
extern const FuriHalBleProfileTemplate* const ble_profile_bridge;

/* Send bytes to host via TX characteristic notification.  Splits into
 * chunks <= BRIDGE_PROFILE_TX_CHUNK automatically.  Safe to call from
 * the GUI thread only (not from the BLE RX event callback). */
bool bridge_profile_tx(FuriHalBleProfileBase* profile, const uint8_t* data, uint16_t size);

/* Register RX callback.  Callback is invoked on the BLE event thread —
 * DO NOT call bridge_profile_tx or other UI functions from inside it;
 * defer to the GUI thread. */
void bridge_profile_set_rx_callback(
    FuriHalBleProfileBase* profile,
    BridgeProfileRxCallback callback,
    void* context);

#ifdef __cplusplus
}
#endif