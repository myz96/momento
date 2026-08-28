/* BLE Wi-Fi provisioning: NimBLE GATT server advertising as "Momento". */

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_prov.h"
#include "wifi_sync.h"

static const char *TAG = "ble_prov";

#define DEVICE_NAME "Momento"

/* Characteristic ids passed as the access-callback arg. */
enum { CHR_SSID = 2, CHR_PASS = 3, CHR_CONTROL = 4, CHR_STATUS = 5 };

/* UUID 6D6F6D65-6E74-6F00-0000-0000000000NN, bytes little-endian. */
#define PROV_UUID128(last)                                                 \
    BLE_UUID128_INIT(last, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
                     0x6f, 0x74, 0x6e, 0x65, 0x6d, 0x6f, 0x6d)

static const ble_uuid128_t svc_uuid = PROV_UUID128(0x01);
static const ble_uuid128_t ssid_uuid = PROV_UUID128(0x02);
static const ble_uuid128_t pass_uuid = PROV_UUID128(0x03);
static const ble_uuid128_t control_uuid = PROV_UUID128(0x04);
static const ble_uuid128_t status_uuid = PROV_UUID128(0x05);

static char s_ssid[33];
static char s_pass[65];
static bool s_started;   /* nimble_port started once, kept for the session */
static bool s_active;    /* advertising / accepting connections */
static uint8_t s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static void start_advertising(void);

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int id = (int)(uintptr_t)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && id == CHR_STATUS) {
        char status[128];
        wifi_sync_status_json(status, sizeof(status));
        return os_mbuf_append(ctxt->om, status, strlen(status)) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t buf[65] = { 0 };
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    buf[len] = '\0';

    switch (id) {
    case CHR_SSID:
        if (len == 0 || len > 32) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        strlcpy(s_ssid, (char *)buf, sizeof(s_ssid));
        ESP_LOGI(TAG, "SSID received: %s", s_ssid);
        return 0;
    case CHR_PASS:
        strlcpy(s_pass, (char *)buf, sizeof(s_pass));
        ESP_LOGI(TAG, "Password received (%u bytes)", len);
        return 0;
    case CHR_CONTROL:
        if (len == 1 && buf[0] == 0x01) {
            if (s_ssid[0] == '\0') {
                return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
            }
            ESP_LOGI(TAG, "Applying credentials for %s", s_ssid);
            wifi_sync_apply_credentials(s_ssid, s_pass);
            return 0;
        }
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &ssid_uuid.u, .access_cb = chr_access,
              .arg = (void *)CHR_SSID, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &pass_uuid.u, .access_cb = chr_access,
              .arg = (void *)CHR_PASS, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &control_uuid.u, .access_cb = chr_access,
              .arg = (void *)CHR_CONTROL, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &status_uuid.u, .access_cb = chr_access,
              .arg = (void *)CHR_STATUS, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "App connected");
        } else if (s_active) {
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "App disconnected");
        if (s_active) {
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_active) {
            start_advertising();
        }
        return 0;
    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv,
                           gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        s_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    if (s_active) {
        start_advertising();
    }
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_prov_start(void)
{
    s_active = true;
    if (s_started) {
        start_advertising();
        return ESP_OK;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        s_active = false;
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed: %d", rc);
        s_active = false;
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(DEVICE_NAME);

    nimble_port_freertos_init(host_task);
    s_started = true;
    ESP_LOGI(TAG, "BLE provisioning on, advertising as %s", DEVICE_NAME);
    return ESP_OK;
}

void ble_prov_stop(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    ble_gap_adv_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ESP_LOGI(TAG, "BLE provisioning off");
}
