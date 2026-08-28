#pragma once

#include "esp_err.h"

/* BLE Wi-Fi provisioning (NimBLE GATT server).
 *
 * The device advertises as "Momento" while sync mode is on. The app
 * writes the home network credentials and then reads the status until
 * the device reports a state.
 *
 * Service  6D6F6D65-6E74-6F00-0000-000000000001
 *   ssid     ...0002  write   UTF-8 network name (max 32 bytes)
 *   password ...0003  write   UTF-8 password (max 64 bytes)
 *   control  ...0004  write   0x01 = save credentials and reconnect
 *   status   ...0005  read    JSON {"state":"sta","ip":"...","ssid":"..."}
 */

/* Starts the NimBLE stack (first call) and advertising. */
esp_err_t ble_prov_start(void);

/* Stops advertising and drops any open connection. The stack stays
 * initialized for the next sync session. */
void ble_prov_stop(void);
