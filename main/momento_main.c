/* Momento camera test for the Seeed XIAO ESP32S3 Sense.
 *
 * Initializes the OV2640 camera and streams JPEG frames over the
 * USB serial console as base64 text, framed by simple markers so a
 * host-side script can reassemble them into images.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "mbedtls/base64.h"

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

/* Encode in chunks whose input size is divisible by 3 so the
 * concatenated base64 output stays valid. */
#define B64_CHUNK_IN  3000
#define B64_CHUNK_OUT (((B64_CHUNK_IN + 2) / 3) * 4 + 1)

static esp_err_t camera_init(void)
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
        .frame_size = FRAMESIZE_QVGA, /* 320x240 keeps serial streaming fast */
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    return esp_camera_init(&config);
}

void app_main(void)
{
    esp_err_t err = camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Camera init OK");

    /* Let the sensor settle: auto exposure and gain need a few frames. */
    for (int i = 0; i < 8; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    static unsigned char b64_out[B64_CHUNK_OUT];
    uint32_t frame_no = 0;

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Frame capture failed");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        printf("\n<FRAME %lu %u>\n", (unsigned long)frame_no, (unsigned)fb->len);
        for (size_t off = 0; off < fb->len; off += B64_CHUNK_IN) {
            size_t in_len = fb->len - off;
            if (in_len > B64_CHUNK_IN) {
                in_len = B64_CHUNK_IN;
            }
            size_t olen = 0;
            if (mbedtls_base64_encode(b64_out, sizeof(b64_out), &olen,
                                      fb->buf + off, in_len) == 0) {
                
                                        (b64_out, 1, olen, stdout);
            }
        }
        printf("\n</FRAME>\n");
        fflush(stdout);

        esp_camera_fb_return(fb);
        frame_no++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
