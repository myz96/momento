/* Momento SD card recording test for the Seeed XIAO ESP32S3 Sense.
 *
 * Boot sequence:
 *   1. Mount the microSD card and run a write/read smoke test.
 *   2. Record 5 seconds of VGA MJPEG video and 16 kHz mono audio into PSRAM.
 *   3. Write VIDEO.AVI and AUDIO.WAV to the card.
 *   4. Take one photo at each resolution up to UXGA 1600x1200.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_camera.h"

#include "avi_writer.h"
#include "mic.h"
#include "sd_card.h"
#include "wav_writer.h"

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

#define RECORD_US         (5LL * 1000 * 1000)
#define TARGET_FPS        15
#define FRAME_INTERVAL_US (1000000 / TARGET_FPS)
#define MAX_FRAMES        90
#define VIDEO_BUF_BYTES   (6 * 1024 * 1024)

#define AUDIO_MAX_BYTES (MIC_SAMPLE_RATE * 2 * 6) /* room for 6 seconds */
#define AUDIO_GAIN      2

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
        size_t chunk = 3200; /* 100 ms of samples */
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
        /* Init at the largest size so the frame buffers fit every
         * resolution the photo pass uses later. */
        .frame_size = FRAMESIZE_UXGA,
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

static void log_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        ESP_LOGI(TAG, "  %s: %ld bytes", path, (long)st.st_size);
    } else {
        ESP_LOGW(TAG, "  %s: missing", path);
    }
}

static void record_av(void)
{
    uint8_t *video_buf = heap_caps_malloc(VIDEO_BUF_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *audio_buf = heap_caps_malloc(AUDIO_MAX_BYTES, MALLOC_CAP_SPIRAM);
    static avi_frame_ref_t frames[MAX_FRAMES];
    if (!video_buf || !audio_buf) {
        ESP_LOGE(TAG, "PSRAM allocation failed");
        return;
    }

    audio_job_t job = {
        .buf = audio_buf,
        .cap = AUDIO_MAX_BYTES,
        .done = xSemaphoreCreateBinary(),
    };

    ESP_LOGI(TAG, "Recording %d seconds...", (int)(RECORD_US / 1000000));
    xTaskCreate(audio_task, "audio", 4096, &job, 5, NULL);

    int64_t t0 = esp_timer_get_time();
    int64_t next_store = t0;
    size_t used = 0;
    size_t frame_count = 0;

    while (esp_timer_get_time() - t0 < RECORD_US) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_store) {
            if (frame_count >= MAX_FRAMES ||
                used + fb->len > VIDEO_BUF_BYTES) {
                ESP_LOGW(TAG, "Video buffer full, capture stops early");
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
    }
    int64_t duration_us = esp_timer_get_time() - t0;

    job.stop = true;
    xSemaphoreTake(job.done, pdMS_TO_TICKS(1000));

    size_t sample_count = job.captured / 2;
    ESP_LOGI(TAG, "Captured %u frames and %u audio samples in %.2f s",
             (unsigned)frame_count, (unsigned)sample_count,
             duration_us / 1000000.0f);

    /* Software gain with saturation. */
    int16_t *samples = (int16_t *)audio_buf;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t v = samples[i] * AUDIO_GAIN;
        if (v > INT16_MAX) v = INT16_MAX;
        if (v < INT16_MIN) v = INT16_MIN;
        samples[i] = (int16_t)v;
    }

    wav_write_file(SD_MOUNT_POINT "/AUDIO.WAV", samples, sample_count,
                   MIC_SAMPLE_RATE);
    avi_write_mjpeg(SD_MOUNT_POINT "/VIDEO.AVI", video_buf, frames,
                    frame_count, 640, 480, (uint64_t)duration_us);

    heap_caps_free(video_buf);
    heap_caps_free(audio_buf);
    vSemaphoreDelete(job.done);
}

static void photo_pass(void)
{
    static const struct {
        framesize_t size;
        const char *name;
        const char *path;
    } shots[] = {
        { FRAMESIZE_QVGA, "QVGA 320x240", SD_MOUNT_POINT "/PHOTO_QVGA.JPG" },
        { FRAMESIZE_VGA, "VGA 640x480", SD_MOUNT_POINT "/PHOTO_VGA.JPG" },
        { FRAMESIZE_SVGA, "SVGA 800x600", SD_MOUNT_POINT "/PHOTO_SVGA.JPG" },
        { FRAMESIZE_XGA, "XGA 1024x768", SD_MOUNT_POINT "/PHOTO_XGA.JPG" },
        { FRAMESIZE_HD, "HD 1280x720", SD_MOUNT_POINT "/PHOTO_HD.JPG" },
        { FRAMESIZE_SXGA, "SXGA 1280x1024", SD_MOUNT_POINT "/PHOTO_SXGA.JPG" },
        { FRAMESIZE_UXGA, "UXGA 1600x1200", SD_MOUNT_POINT "/PHOTO_UXGA.JPG" },
    };

    sensor_t *s = esp_camera_sensor_get();
    for (size_t i = 0; i < sizeof(shots) / sizeof(shots[0]); i++) {
        s->set_framesize(s, shots[i].size);
        vTaskDelay(pdMS_TO_TICKS(300));
        skip_frames(2);

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Photo capture failed at %s", shots[i].name);
            continue;
        }
        FILE *f = fopen(shots[i].path, "wb");
        if (f) {
            fwrite(fb->buf, 1, fb->len, f);
            fclose(f);
            ESP_LOGI(TAG, "Photo %s: %u bytes -> %s", shots[i].name,
                     (unsigned)fb->len, shots[i].path);
        } else {
            ESP_LOGE(TAG, "Cannot open %s", shots[i].path);
        }
        esp_camera_fb_return(fb);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Momento SD recording test starts in 3 seconds");
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (sd_card_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed, test stops");
        return;
    }
    if (sd_card_smoke_test() != ESP_OK) {
        ESP_LOGE(TAG, "SD card smoke test failed, test stops");
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

    /* Let auto exposure settle. */
    skip_frames(10);

    record_av();
    photo_pass();

    ESP_LOGI(TAG, "Files on the card:");
    log_file_size(SD_MOUNT_POINT "/TEST.TXT");
    log_file_size(SD_MOUNT_POINT "/AUDIO.WAV");
    log_file_size(SD_MOUNT_POINT "/VIDEO.AVI");
    log_file_size(SD_MOUNT_POINT "/PHOTO_QVGA.JPG");
    log_file_size(SD_MOUNT_POINT "/PHOTO_VGA.JPG");
    log_file_size(SD_MOUNT_POINT "/PHOTO_SVGA.JPG");
    log_file_size(SD_MOUNT_POINT "/PHOTO_XGA.JPG");
    log_file_size(SD_MOUNT_POINT "/PHOTO_HD.JPG");
    log_file_size(SD_MOUNT_POINT "/PHOTO_SXGA.JPG");
    log_file_size(SD_MOUNT_POINT "/PHOTO_UXGA.JPG");
    ESP_LOGI(TAG, "DONE - power off and move the card to the computer");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
