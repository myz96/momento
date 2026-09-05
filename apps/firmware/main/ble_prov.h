#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* BLE control channel (NimBLE GATT server).
 *
 * The device advertises as "Momento" whenever it is powered, so the
 * app can find it, provision Wi-Fi, and start or stop sync mode
 * remotely — no button holds.
 *
 * Service  6D6F6D65-6E74-6F00-0000-000000000001
 *   ssid     ...0002  write   UTF-8 network name (max 32 bytes)
 *   password ...0003  write   UTF-8 password (max 64 bytes)
 *   control  ...0004  write   0x01 = save credentials and reconnect
 *                             0x02 = enter sync mode
 *                             0x03 = leave sync mode
 *   status   ...0005  read    JSON {"state":"sta","ip":"...","ssid":"..."}
 */

/* Starts the NimBLE stack (first call) and advertising. */
esp_err_t ble_prov_start(void);

/* Stops advertising and drops any open connection. The stack stays
 * initialized for the next start. */
void ble_prov_stop(void);

/* True once when the app asked to enter sync mode; the flag clears. */
bool ble_prov_take_sync_request(void);

/* True once when the app asked to leave sync mode; the flag clears. */
bool ble_prov_take_exit_request(void);
