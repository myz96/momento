/* Wi-Fi sync mode: HTTP file server over the SD card.
 *
 * The radio runs AP and station together (APSTA): the SoftAP is up from
 * the first second as the reliable fallback, and the station keeps
 * retrying the stored home network until it joins — however late that
 * network appears. A successful join also starts SNTP, so capture
 * timestamps become real. */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
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

#define STA_RETRY_DELAY_MS 15000

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static volatile bool s_running;
static volatile bool s_sta_connected;
static volatile bool s_have_creds;
static char s_ip[16];
static char s_sta_ssid[33];
static bool s_netif_ready; /* esp_netif_init + event loop + handlers, once */
static bool s_wifi_ready;  /* esp_wifi_init runs once; toggles use start/stop */
static bool s_sntp_ready;  /* esp_netif_sntp_init runs once */
static bool s_retry_task_created; /* written only by wifi_sync_start (app task) */

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
        char item[128];
        snprintf(item, sizeof(item),
                 "%s{\"name\":\"%.40s\",\"size\":%lld,\"mtime\":%lld}",
                 first ? "" : ",", ent->d_name, (long long)st.st_size,
                 (long long)st.st_mtime);
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

static void sntp_start_once(void)
{
    if (s_sntp_ready) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_ready = true;
        ESP_LOGI(TAG, "SNTP started, capture timestamps become real");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "Joined %s, ip=%s", s_sta_ssid, s_ip);
        sntp_start_once();
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
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
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

/* Keeps trying the home network while sync mode runs. The task is
 * created once and lives forever; it idles when sync mode is off. A
 * join attempt scans off-channel and stalls SoftAP traffic, so no
 * attempt runs while a client is connected to the AP. */
static void sta_retry_task(void *arg)
{
    while (true) {
        if (s_running && s_have_creds && !s_sta_connected) {
            wifi_sta_list_t clients = { 0 };
            if (esp_wifi_ap_get_sta_list(&clients) != ESP_OK ||
                clients.num == 0) {
                esp_wifi_connect(); /* rejections surface as DISCONNECTED */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(STA_RETRY_DELAY_MS));
    }
}

static esp_err_t wifi_start_apsta(void)
{
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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    s_have_creds = creds_load(ssid, sizeof(ssid), pass, sizeof(pass));
    if (s_have_creds) {
        wifi_config_t sta_cfg = { 0 };
        strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, pass,
                sizeof(sta_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
        ESP_LOGI(TAG, "Will keep trying home network %s", ssid);
    } else {
        ESP_LOGI(TAG, "No stored credentials, SoftAP only");
    }

    return esp_wifi_start();
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
    if (s_running) {
        return ESP_OK;
    }

    netif_setup_once();
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_start_apsta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_running = true;

    err = server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        esp_wifi_stop();
        s_running = false;
        return err;
    }
    mdns_start();

    if (!s_retry_task_created) {
        if (xTaskCreate(sta_retry_task, "sta_retry", 3072, NULL, 4, NULL) ==
            pdPASS) {
            s_retry_task_created = true;
        } else {
            ESP_LOGE(TAG, "Retry task create failed; the home network only "
                          "joins on the next sync toggle");
        }
    }

    ESP_LOGI(TAG,
             "Sync mode on. SoftAP %s (pass %s) at http://192.168.4.1; "
             "momento.local once the home network joins.",
             WIFI_SYNC_SSID, WIFI_SYNC_PASSWORD);
    return ESP_OK;
}

void wifi_sync_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false; /* the retry task sees this and idles */
    s_sta_connected = false;
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
    return s_running;
}

wifi_sync_state_t wifi_sync_state(void)
{
    if (!s_running) {
        return WIFI_SYNC_OFF;
    }
    return s_sta_connected ? WIFI_SYNC_STA : WIFI_SYNC_AP;
}

void wifi_sync_status_json(char *buf, size_t buf_len)
{
    if (!s_running) {
        snprintf(buf, buf_len, "{\"state\":\"off\",\"ip\":\"\",\"ssid\":\"\"}");
        return;
    }
    if (s_sta_connected) {
        snprintf(buf, buf_len,
                 "{\"state\":\"sta\",\"ip\":\"%s\",\"ssid\":\"%s\"}", s_ip,
                 s_sta_ssid);
    } else {
        snprintf(buf, buf_len,
                 "{\"state\":\"ap\",\"ip\":\"192.168.4.1\",\"ssid\":\"\"}");
    }
}

esp_err_t wifi_sync_apply_credentials(const char *ssid, const char *password)
{
    esp_err_t err = creds_save(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Credential save failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Credentials stored for %s", ssid);

    if (s_running) {
        wifi_config_t sta_cfg = { 0 };
        strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, password,
                sizeof(sta_cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
        s_have_creds = true;
        s_sta_connected = false;
        esp_wifi_disconnect(); /* drop any old join */
        esp_wifi_connect();    /* immediate try; the retry task covers misses */
    }
    return ESP_OK;
}
