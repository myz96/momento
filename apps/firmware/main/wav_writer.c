/* Minimal WAV writer: 44-byte header + 16-bit mono PCM data. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "wav_writer.h"

static const char *TAG = "wav";

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void put_tag(FILE *f, const char *tag) { fwrite(tag, 4, 1, f); }

esp_err_t wav_write_file(const char *path, const int16_t *samples,
                         size_t sample_count, uint32_t sample_rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }

    uint32_t data_bytes = (uint32_t)(sample_count * 2);
    put_tag(f, "RIFF");
    put_u32(f, 36 + data_bytes);
    put_tag(f, "WAVE");
    put_tag(f, "fmt ");
    put_u32(f, 16);              /* fmt chunk size */
    put_u16(f, 1);               /* PCM */
    put_u16(f, 1);               /* mono */
    put_u32(f, sample_rate);
    put_u32(f, sample_rate * 2); /* byte rate */
    put_u16(f, 2);               /* block align */
    put_u16(f, 16);              /* bits per sample */
    put_tag(f, "data");
    put_u32(f, data_bytes);

    size_t written = fwrite(samples, 2, sample_count, f);
    fclose(f);

    if (written != sample_count) {
        ESP_LOGE(TAG, "Short write: %u of %u samples", (unsigned)written,
                 (unsigned)sample_count);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Wrote %s (%u bytes of audio)", path, (unsigned)data_bytes);
    return ESP_OK;
}
