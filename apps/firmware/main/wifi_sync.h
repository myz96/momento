#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Wi-Fi sync mode: SoftAP + HTTP file server over the SD card.
 *
 * The phone joins the AP (SSID WIFI_SYNC_SSID) and talks to
 * http://192.168.4.1. Endpoints:
 *   GET    /api/info          device + storage summary
 *   GET    /api/files         JSON list of media files
 *   GET    /api/files/NAME    file download
 *   DELETE /api/files/NAME    remove a file after the app stored it
 */

#define WIFI_SYNC_SSID     "Momento"
#define WIFI_SYNC_PASSWORD "momento123"

/* Starts the SoftAP and the HTTP server. Safe to call again after stop. */
esp_err_t wifi_sync_start(void);

/* Stops the HTTP server and the SoftAP. */
void wifi_sync_stop(void);

bool wifi_sync_is_running(void);
