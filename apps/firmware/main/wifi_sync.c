/* Wi-Fi sync mode: SoftAP + HTTP file server over the SD card. */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "sd_card.h"
#include "wifi_sync.h"

static const char *TAG = "wifi_sync";

#define FILE_CHUNK_BYTES 16384
#define MAX_NAME_LEN     40

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static bool s_running;
static bool s_netif_ready; /* esp_netif_init + default event loop run once */
static bool s_wifi_ready;  /* esp_wifi_init runs once; toggles use start/stop */

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

esp_err_t wifi_sync_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    if (!s_netif_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_netif_ready = true;
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    if (!s_wifi_ready) {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t init_err = esp_wifi_init(&init_cfg);
        if (init_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s",
                     esp_err_to_name(init_err));
            return init_err;
        }
        s_wifi_ready = true;
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
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        esp_wifi_stop();
        return err;
    }

    s_running = true;
    ESP_LOGI(TAG, "Sync mode on. SSID=%s pass=%s url=http://192.168.4.1",
             WIFI_SYNC_SSID, WIFI_SYNC_PASSWORD);
    return ESP_OK;
}

void wifi_sync_stop(void)
{
    if (!s_running) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    esp_wifi_stop();
    s_running = false;
    ESP_LOGI(TAG, "Sync mode off");
}

bool wifi_sync_is_running(void)
{
    return s_running;
}
