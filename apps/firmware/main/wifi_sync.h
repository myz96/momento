#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Wi-Fi sync mode: HTTP file server over the SD card.
 *
 * Connection strategy:
 *   1. Station mode: join the home network with credentials stored in
 *      NVS (provisioned over BLE, see ble_prov.h). The device announces
 *      itself as momento.local via mDNS.
 *   2. Fallback SoftAP: no credentials, or the join fails. The phone
 *      joins WIFI_SYNC_SSID and talks to http://192.168.4.1.
 *
 * HTTP endpoints (see packages/device-protocol/README.md):
 *   GET    /api/info          device + storage summary
 *   GET    /api/files         JSON list of media files
 *   GET    /api/files/NAME    file download
 *   DELETE /api/files/NAME    remove a file after the app stored it
 */

#define WIFI_SYNC_SSID     "Momento"
#define WIFI_SYNC_PASSWORD "momento123"

typedef enum {
    WIFI_SYNC_OFF,        /* sync mode not running */
    WIFI_SYNC_CONNECTING, /* station join in progress */
    WIFI_SYNC_STA,        /* on the home network */
    WIFI_SYNC_AP,         /* fallback SoftAP */
} wifi_sync_state_t;

/* Starts Wi-Fi (station first, SoftAP fallback) and the HTTP server. */
esp_err_t wifi_sync_start(void);

/* Stops the HTTP server, mDNS, and Wi-Fi. */
void wifi_sync_stop(void);

bool wifi_sync_is_running(void);

wifi_sync_state_t wifi_sync_state(void);

/* Fills buf with a status JSON: {"state":"sta","ip":"192.168.1.7"}. */
void wifi_sync_status_json(char *buf, size_t buf_len);

/* Saves credentials to NVS and, when sync mode runs, reconnects in
 * station mode. Called from the BLE provisioning service. */
esp_err_t wifi_sync_apply_credentials(const char *ssid, const char *password);
