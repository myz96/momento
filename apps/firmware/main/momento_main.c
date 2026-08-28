/* Momento button capture for the Seeed XIAO ESP32S3 Sense.
 *
 * Breadboard:
 *   GPIO1 = REC button (to GND, internal pull-up)
 *   GPIO2 = CAM button (to GND, internal pull-up)
 *   GPIO4 = red LED (on = capture or write in progress)
 *   GPIO5/6 = I2C (DRV2605L haptics at 0x5A, scanned at boot)
 *
 * CAM press: one UXGA photo -> /sdcard/PHOTO_NNN.JPG
 * CAM hold (>= 1.5 s): toggle Wi-Fi sync mode (SoftAP + HTTP file
 *   server, LED blinks). Hold CAM again to leave sync mode.
 * REC press: start recording; press REC again to stop.
 *   Saves /sdcard/VID_NNN.AVI + AUD_NNN.WAV. Auto-stop at 30 s or
 *   when the PSRAM buffer is full (~10-12 s of a typical scene).
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "nvs_flash.h"

#include "avi_writer.h"
#include "ble_prov.h"
#include "mic.h"
#include "sd_card.h"
#include "wav_writer.h"
#include "wifi_sync.h"

static const char *TAG = "momento";

/* XIAO ESP32S3 Sense camera pin map (from the Seeed wiki) */
#define CAM_PIN_PWDN  (-1)
#define CAM_PIN_RESET (-1)
#define CAM_PIN_XCLK  10
#define CAM_PIN_SIOD  40
#define CAM_PIN_SIOC  39
#define CAM_PIN_D7    48 /* Y9 */
#define CAM_PIN_D6    11 /* Y8 */
#define CAM_PIN_D5    12 /* Y7 */
#define CAM_PIN_D4    14 /* Y6 */
#define CAM_PIN_D3    16 /* Y5 */
#define CAM_PIN_D2    18 /* Y4 */
#define CAM_PIN_D1    17 /* Y3 */
#define CAM_PIN_D0    15 /* Y2 */
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF  47
#define CAM_PIN_PCLK  13

/* Breadboard pins */
#define PIN_BTN_REC GPIO_NUM_1
#define PIN_BTN_CAM GPIO_NUM_2
#define PIN_LED     GPIO_NUM_4
#define PIN_I2C_SDA GPIO_NUM_5
#define PIN_I2C_SCL GPIO_NUM_6

#define MAX_RECORD_US     (30LL * 1000 * 1000)
#define TARGET_FPS        15
#define FRAME_INTERVAL_US (1000000 / TARGET_FPS)
#define MAX_FRAMES        512
#define VIDEO_BUF_BYTES   (5 * 1024 * 1024)

#define AUDIO_MAX_BYTES (MIC_SAMPLE_RATE * 2 * 32) /* 32 seconds */
#define AUDIO_GAIN      2

#define SYNC_HOLD_MS 1500 /* CAM held this long toggles sync mode */

typedef struct {
    uint8_t *buf;
    size_t cap;
    volatile size_t captured;
    volatile bool stop;
    SemaphoreHandle_t done;
} audio_job_t;

static void audio_task(void *arg)
{
    audio_job_t *job = arg;
    while (!job->stop && job->captured < job->cap) {
        size_t chunk = 3200;
        if (chunk > job->cap - job->captured) {
            chunk = job->cap - job->captured;
        }
        size_t got = 0;
        if (mic_read(job->buf + job->captured, chunk, &got, 200) == ESP_OK) {
            job->captured += got;
        }
    }
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static esp_err_t camera_start(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_UXGA, /* buffers sized for the largest shot */
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_VGA);
    return ESP_OK;
}

static void skip_frames(int n)
{
    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void gpio_setup(void)
{
    gpio_config_t btns = {
        .pin_bit_mask = (1ULL << PIN_BTN_REC) | (1ULL << PIN_BTN_CAM),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btns);

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led);
    gpio_set_level(PIN_LED, 0);
}

static void i2c_scan(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed, scan skipped");
        return;
    }
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X%s", addr,
                     addr == 0x5A ? " (DRV2605L haptics)" : "");
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "I2C scan: no devices found");
    }
    i2c_del_master_bus(bus);
}

static int next_index(const char *pattern)
{
    char path[64];
    struct stat st;
    for (int i = 1; i < 1000; i++) {
        snprintf(path, sizeof(path), pattern, i);
        if (stat(path, &st) != 0) {
            return i;
        }
    }
    return 999;
}

static void take_photo(void)
{
    gpio_set_level(PIN_LED, 1);
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_UXGA);
    vTaskDelay(pdMS_TO_TICKS(300));
    skip_frames(2);

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        char path[64];
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/PHOTO_%03d.JPG",
                 next_index(SD_MOUNT_POINT "/PHOTO_%03d.JPG"));
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(fb->buf, 1, fb->len, f);
            fclose(f);
            ESP_LOGI(TAG, "Photo saved: %s (%u bytes)", path, (unsigned)fb->len);
        } else {
            ESP_LOGE(TAG, "Cannot open %s", path);
        }
        esp_camera_fb_return(fb);
    } else {
        ESP_LOGE(TAG, "Photo capture failed");
    }

    s->set_framesize(s, FRAMESIZE_VGA);
    skip_frames(1);
    gpio_set_level(PIN_LED, 0);
}

static void record_clip(void)
{
    uint8_t *video_buf = heap_caps_malloc(VIDEO_BUF_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *audio_buf = heap_caps_malloc(AUDIO_MAX_BYTES, MALLOC_CAP_SPIRAM);
    static avi_frame_ref_t frames[MAX_FRAMES];
    if (!video_buf || !audio_buf) {
        ESP_LOGE(TAG, "PSRAM allocation failed");
        heap_caps_free(video_buf);
        heap_caps_free(audio_buf);
        return;
    }

    gpio_set_level(PIN_LED, 1);

    audio_job_t job = {
        .buf = audio_buf,
        .cap = AUDIO_MAX_BYTES,
        .done = xSemaphoreCreateBinary(),
    };
    ESP_LOGI(TAG, "Recording... press REC again to stop (max %d s)",
             (int)(MAX_RECORD_US / 1000000));
    xTaskCreate(audio_task, "audio", 4096, &job, 5, NULL);

    int64_t t0 = esp_timer_get_time();
    int64_t next_store = t0;
    size_t used = 0;
    size_t frame_count = 0;
    int rec_high_streak = 0; /* REC must read released before a stop press counts */
    int rec_low_streak = 0;
    bool stop_requested = false;
    const char *stop_reason = "time limit";

    while (esp_timer_get_time() - t0 < MAX_RECORD_US) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_store) {
            if (frame_count >= MAX_FRAMES ||
                used + fb->len > VIDEO_BUF_BYTES) {
                stop_reason = "buffer full";
                esp_camera_fb_return(fb);
                break;
            }
            memcpy(video_buf + used, fb->buf, fb->len);
            frames[frame_count].offset = used;
            frames[frame_count].len = fb->len;
            used += fb->len;
            frame_count++;
            next_store += FRAME_INTERVAL_US;
            if (next_store < now) {
                next_store = now;
            }
        }
        esp_camera_fb_return(fb);

        if (gpio_get_level(PIN_BTN_REC) == 1) {
            rec_high_streak++;
            rec_low_streak = 0;
        } else if (rec_high_streak >= 3) {
            rec_low_streak++;
            if (rec_low_streak >= 2) {
                stop_requested = true;
                stop_reason = "REC pressed";
                break;
            }
        }
    }
    (void)stop_requested;
    int64_t duration_us = esp_timer_get_time() - t0;

    job.stop = true;
    xSemaphoreTake(job.done, pdMS_TO_TICKS(1000));

    size_t sample_count = job.captured / 2;
    ESP_LOGI(TAG, "Stopped (%s): %u frames, %u audio samples, %.2f s",
             stop_reason, (unsigned)frame_count, (unsigned)sample_count,
             duration_us / 1000000.0f);

    int16_t *samples = (int16_t *)audio_buf;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t v = samples[i] * AUDIO_GAIN;
        if (v > INT16_MAX) v = INT16_MAX;
        if (v < INT16_MIN) v = INT16_MIN;
        samples[i] = (int16_t)v;
    }

    int idx = next_index(SD_MOUNT_POINT "/VID_%03d.AVI");
    char wav_path[64], avi_path[64];
    snprintf(wav_path, sizeof(wav_path), SD_MOUNT_POINT "/AUD_%03d.WAV", idx);
    snprintf(avi_path, sizeof(avi_path), SD_MOUNT_POINT "/VID_%03d.AVI", idx);

    wav_write_file(wav_path, samples, sample_count, MIC_SAMPLE_RATE);
    avi_write_mjpeg(avi_path, video_buf, frames, frame_count, 640, 480,
                    (uint64_t)duration_us);

    heap_caps_free(video_buf);
    heap_caps_free(audio_buf);
    vSemaphoreDelete(job.done);
    gpio_set_level(PIN_LED, 0);
}

static bool button_pressed(gpio_num_t pin)
{
    if (gpio_get_level(pin) != 0) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    if (gpio_get_level(pin) != 0) {
        return false;
    }
    return true;
}

static void wait_release(gpio_num_t pin)
{
    while (gpio_get_level(pin) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* Returns true when CAM stays pressed for SYNC_HOLD_MS. Returns false as
 * soon as the button is released earlier (a short press). */
static bool cam_hold_is_long(void)
{
    int held_ms = 0;
    while (gpio_get_level(PIN_BTN_CAM) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        held_ms += 20;
        if (held_ms >= SYNC_HOLD_MS) {
            return true;
        }
    }
    return false;
}

/* Runs sync mode until CAM is held again. The LED blinks the whole time. */
static void sync_mode(void)
{
    ble_prov_start(); /* provisioning stays available even when Wi-Fi fails */
    if (wifi_sync_start() != ESP_OK) {
        ble_prov_stop();
        for (int i = 0; i < 5; i++) { /* fast error blink */
            gpio_set_level(PIN_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(60));
            gpio_set_level(PIN_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(60));
        }
        return;
    }

    bool led_on = false;
    while (true) {
        led_on = !led_on;
        gpio_set_level(PIN_LED, led_on);
        if (button_pressed(PIN_BTN_CAM)) {
            if (cam_hold_is_long()) {
                wait_release(PIN_BTN_CAM);
                break;
            }
            wait_release(PIN_BTN_CAM);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    ble_prov_stop();
    wifi_sync_stop();
    gpio_set_level(PIN_LED, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Momento button capture");
    gpio_setup();
    i2c_scan();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    if (sd_card_mount() != ESP_OK || sd_card_smoke_test() != ESP_OK) {
        ESP_LOGE(TAG, "SD card not ready, test stops");
        return;
    }
    esp_err_t err = camera_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return;
    }
    if (mic_init() != ESP_OK) {
        ESP_LOGE(TAG, "Microphone init failed, test stops");
        return;
    }
    skip_frames(10);

    for (int i = 0; i < 2; i++) {
        gpio_set_level(PIN_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(PIN_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    ESP_LOGI(TAG, "Ready. CAM = photo, hold CAM = sync mode. REC = start/stop recording.");

    while (true) {
        if (button_pressed(PIN_BTN_CAM)) {
            if (cam_hold_is_long()) {
                ESP_LOGI(TAG, "CAM held, sync mode toggles on");
                wait_release(PIN_BTN_CAM);
                sync_mode();
            } else {
                ESP_LOGI(TAG, "CAM button pressed");
                take_photo();
                wait_release(PIN_BTN_CAM);
            }
        } else if (button_pressed(PIN_BTN_REC)) {
            ESP_LOGI(TAG, "REC button pressed, recording starts");
            wait_release(PIN_BTN_REC);
            record_clip();
            wait_release(PIN_BTN_REC);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
