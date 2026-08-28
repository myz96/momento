/* Wi-Fi sync mode: station-first HTTP file server over the SD card. */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"

#include "sd_card.h"
#include "wifi_sync.h"

static const char *TAG = "wifi_sync";

#define FILE_CHUNK_BYTES 16384
#define MAX_NAME_LEN     40

#define NVS_NAMESPACE "momento"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define STA_MAX_RETRIES 4
#define STA_JOIN_TIMEOUT_MS 20000

#define WIFI_EVT_GOT_IP BIT0
#define WIFI_EVT_FAILED BIT1

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static volatile wifi_sync_state_t s_state = WIFI_SYNC_OFF;
static char s_ip[16];
static char s_sta_ssid[33];
static EventGroupHandle_t s_wifi_events;
static int s_retries;
static bool s_netif_ready; /* esp_netif_init + event loop + handlers, once */
static bool s_wifi_ready;  /* esp_wifi_init runs once; toggles use start/stop */

/* ---------- credentials in NVS ---------- */

static bool creds_load(char *ssid, size_t ssid_len, char *pass,
                       size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t e1 = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    nvs_close(nvs);
    return e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0';
}

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* ---------- HTTP handlers ---------- */

/* Only media files are listed and served. TEST.TXT and friends stay hidden. */
static bool is_media_name(const char *name)
{
    if (strlen(name) > MAX_NAME_LEN) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    return strcasecmp(dot, ".JPG") == 0 || strcasecmp(dot, ".JPEG") == 0 ||
           strcasecmp(dot, ".WAV") == 0 || strcasecmp(dot, ".AVI") == 0;
}

static const char *content_type_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot) {
        if (strcasecmp(dot, ".JPG") == 0 || strcasecmp(dot, ".JPEG") == 0) {
            return "image/jpeg";
        }
        if (strcasecmp(dot, ".WAV") == 0) {
            return "audio/wav";
        }
        if (strcasecmp(dot, ".AVI") == 0) {
            return "video/x-msvideo";
        }
    }
    return "application/octet-stream";
}

/* Extracts and validates the file name after "/api/files/". Returns false on
 * anything that could escape the SD root. */
static bool uri_file_name(const httpd_req_t *req, char *out, size_t out_len)
{
    const char *prefix = "/api/files/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char *name = req->uri + strlen(prefix);
    size_t len = strlen(name);
    if (len == 0 || len >= out_len || len > MAX_NAME_LEN) {
        return false;
    }
    if (name[0] == '.' || strchr(name, '/') || strchr(name, '\\') ||
        strstr(name, "..")) {
        return false;
    }
    strcpy(out, name);
    return true;
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    int count = 0;
    long long total = 0;
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (!is_media_name(ent->d_name)) {
                continue;
            }
            char path[80];
            snprintf(path, sizeof(path), SD_MOUNT_POINT "/%.40s", ent->d_name);
            struct stat st;
            if (stat(path, &st) == 0) {
                count++;
                total += st.st_size;
            }
        }
        closedir(dir);
    }

    char body[128];
    snprintf(body, sizeof(body),
             "{\"device\":\"momento\",\"files\":%d,\"total_bytes\":%lld}",
             count, total);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t files_get_handler(httpd_req_t *req)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "SD card not readable");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    bool first = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_media_name(ent->d_name)) {
            continue;
        }
        char path[80];
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/%.40s", ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        char item[96];
        snprintf(item, sizeof(item), "%s{\"name\":\"%.40s\",\"size\":%lld}",
                 first ? "" : ",", ent->d_name, (long long)st.st_size);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t file_get_handler(httpd_req_t *req)
{
    char name[MAX_NAME_LEN + 1];
    if (!uri_file_name(req, name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad file name");
    }

    char path[80];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", name);
    struct stat st;
    if (stat(path, &st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Cannot open file");
    }

    char *buf = malloc(FILE_CHUNK_BYTES);
    if (!buf) {
        fclose(f);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }

    httpd_resp_set_type(req, content_type_for(name));
    char len_str[24];
    snprintf(len_str, sizeof(len_str), "%lld", (long long)st.st_size);
    httpd_resp_set_hdr(req, "X-File-Size", len_str);

    esp_err_t err = ESP_OK;
    size_t got;
    while ((got = fread(buf, 1, FILE_CHUNK_BYTES, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, got);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Send failed for %s: %s", name, esp_err_to_name(err));
            break;
        }
    }
    free(buf);
    fclose(f);

    if (err != ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        return err;
    }
    ESP_LOGI(TAG, "Served %s (%lld bytes)", name, (long long)st.st_size);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t file_delete_handler(httpd_req_t *req)
{
    char name[MAX_NAME_LEN + 1];
    if (!uri_file_name(req, name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad file name");
    }
    if (!is_media_name(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Not a media file");
    }

    char path[80];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", name);
    struct stat st;
    if (stat(path, &st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");
    }
    if (unlink(path) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Delete failed");
    }
    ESP_LOGI(TAG, "Deleted %s", name);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"deleted\":true}");
}

static esp_err_t server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t info_get = {
        .uri = "/api/info", .method = HTTP_GET, .handler = info_get_handler };
    const httpd_uri_t files_get = {
        .uri = "/api/files", .method = HTTP_GET, .handler = files_get_handler };
    const httpd_uri_t file_get = {
        .uri = "/api/files/*", .method = HTTP_GET, .handler = file_get_handler };
    const httpd_uri_t file_delete = {
        .uri = "/api/files/*", .method = HTTP_DELETE,
        .handler = file_delete_handler };

    httpd_register_uri_handler(s_server, &info_get);
    httpd_register_uri_handler(s_server, &files_get);
    httpd_register_uri_handler(s_server, &file_get);
    httpd_register_uri_handler(s_server, &file_delete);
    return ESP_OK;
}

/* ---------- Wi-Fi lifecycle ---------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state != WIFI_SYNC_CONNECTING) {
            return; /* deliberate stop or mode switch */
        }
        if (s_retries < STA_MAX_RETRIES) {
            s_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_EVT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_EVT_GOT_IP);
    }
}

static void netif_setup_once(void)
{
    if (s_netif_ready) {
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    s_wifi_events = xEventGroupCreate();
    s_netif_ready = true;
}

static esp_err_t wifi_init_once(void)
{
    if (s_wifi_ready) {
        return ESP_OK;
    }
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err == ESP_OK) {
        s_wifi_ready = true;
    }
    return err;
}

/* Tries to join the home network. Returns ESP_OK with an IP on success. */
static esp_err_t sta_try_join(const char *ssid, const char *pass)
{
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_config_t sta_cfg = { 0 };
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

    s_retries = 0;
    s_state = WIFI_SYNC_CONNECTING;
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    xEventGroupClearBits(s_wifi_events, WIFI_EVT_GOT_IP | WIFI_EVT_FAILED);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_EVT_GOT_IP | WIFI_EVT_FAILED, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(STA_JOIN_TIMEOUT_MS));

    if (bits & WIFI_EVT_GOT_IP) {
        ESP_LOGI(TAG, "Joined %s, ip=%s", ssid, s_ip);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Join %s failed (%s)", ssid,
             (bits & WIFI_EVT_FAILED) ? "rejected" : "timeout");
    s_state = WIFI_SYNC_OFF;
    esp_wifi_stop();
    return ESP_FAIL;
}

static esp_err_t ap_start(void)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WIFI_SYNC_SSID,
            .password = WIFI_SYNC_PASSWORD,
            .ssid_len = strlen(WIFI_SYNC_SSID),
            .channel = 6,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    strlcpy(s_ip, "192.168.4.1", sizeof(s_ip));
    s_state = WIFI_SYNC_AP;
    return ESP_OK;
}

static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed, momento.local not available");
        return;
    }
    mdns_hostname_set("momento");
    mdns_instance_name_set("Momento capture device");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t wifi_sync_start(void)
{
    if (s_state != WIFI_SYNC_OFF) {
        return ESP_OK;
    }

    netif_setup_once();
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    bool joined = false;
    if (creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Trying home network %s", ssid);
        joined = sta_try_join(ssid, pass) == ESP_OK;
    } else {
        ESP_LOGI(TAG, "No stored credentials, using SoftAP");
    }

    if (joined) {
        s_state = WIFI_SYNC_STA;
    } else {
        err = ap_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
            s_state = WIFI_SYNC_OFF;
            return err;
        }
    }

    err = server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        esp_wifi_stop();
        s_state = WIFI_SYNC_OFF;
        return err;
    }
    mdns_start();

    if (s_state == WIFI_SYNC_STA) {
        ESP_LOGI(TAG, "Sync mode on (station): http://%s and momento.local",
                 s_ip);
    } else {
        ESP_LOGI(TAG, "Sync mode on (SoftAP): SSID=%s pass=%s url=http://%s",
                 WIFI_SYNC_SSID, WIFI_SYNC_PASSWORD, s_ip);
    }
    return ESP_OK;
}

void wifi_sync_stop(void)
{
    if (s_state == WIFI_SYNC_OFF) {
        return;
    }
    s_state = WIFI_SYNC_OFF;
    mdns_free();
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    ESP_LOGI(TAG, "Sync mode off");
}

bool wifi_sync_is_running(void)
{
    return s_state != WIFI_SYNC_OFF;
}

wifi_sync_state_t wifi_sync_state(void)
{
    return s_state;
}

void wifi_sync_status_json(char *buf, size_t buf_len)
{
    const char *state;
    switch (s_state) {
    case WIFI_SYNC_STA:        state = "sta"; break;
    case WIFI_SYNC_AP:         state = "ap"; break;
    case WIFI_SYNC_CONNECTING: state = "connecting"; break;
    default:                   state = "off"; break;
    }
    snprintf(buf, buf_len, "{\"state\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\"}",
             state, s_state == WIFI_SYNC_OFF ? "" : s_ip,
             s_state == WIFI_SYNC_STA ? s_sta_ssid : "");
}

static void reconnect_task(void *arg)
{
    wifi_sync_stop();
    wifi_sync_start();
    vTaskDelete(NULL);
}

esp_err_t wifi_sync_apply_credentials(const char *ssid, const char *password)
{
    esp_err_t err = creds_save(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Credential save failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Credentials stored for %s", ssid);

    if (s_state != WIFI_SYNC_OFF) {
        /* Restart from a task: the BLE callback must not block for the
         * 20 s join timeout. */
        xTaskCreate(reconnect_task, "wifi_recfg", 4096, NULL, 5, NULL);
    }
    return ESP_OK;
}
