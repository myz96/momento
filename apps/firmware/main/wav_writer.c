/* Streaming WAV writer: 44-byte header + 16-bit mono PCM data. */

#include <string.h>
#include <unistd.h>

#include "esp_log.h"

#include "wav_writer.h"

static const char *TAG = "wav";

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void put_tag(FILE *f, const char *tag) { fwrite(tag, 4, 1, f); }

esp_err_t wav_stream_open(wav_stream_t *s, const char *path,
                          uint32_t sample_rate)
{
    memset(s, 0, sizeof(*s));
    s->f = fopen(path, "wb");
    if (!s->f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }

    put_tag(s->f, "RIFF");
    put_u32(s->f, 0); /* patched on finish */
    put_tag(s->f, "WAVE");
    put_tag(s->f, "fmt ");
    put_u32(s->f, 16);              /* fmt chunk size */
    put_u16(s->f, 1);               /* PCM */
    put_u16(s->f, 1);               /* mono */
    put_u32(s->f, sample_rate);
    put_u32(s->f, sample_rate * 2); /* byte rate */
    put_u16(s->f, 2);               /* block align */
    put_u16(s->f, 16);              /* bits per sample */
    put_tag(s->f, "data");
    put_u32(s->f, 0); /* patched on finish */
    return ESP_OK;
}

esp_err_t wav_stream_write(wav_stream_t *s, const int16_t *samples,
                           size_t sample_count)
{
    if (!s->f) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t written = fwrite(samples, 2, sample_count, s->f);
    s->data_bytes += (uint32_t)(written * 2);
    if (written != sample_count) {
        ESP_LOGE(TAG, "Short write: %u of %u samples", (unsigned)written,
                 (unsigned)sample_count);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wav_stream_sync(wav_stream_t *s)
{
    if (!s->f) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fflush(s->f) != 0 || fsync(fileno(s->f)) != 0) {
        ESP_LOGE(TAG, "Audio sync to card failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wav_stream_finish(wav_stream_t *s)
{
    if (!s->f) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = s->f;
    s->f = NULL;

    esp_err_t err = ESP_OK;
    if (fseek(f, 4, SEEK_SET) == 0) {
        put_u32(f, 36 + s->data_bytes);
    } else {
        err = ESP_FAIL;
    }
    if (fseek(f, 40, SEEK_SET) == 0) {
        put_u32(f, s->data_bytes);
    } else {
        err = ESP_FAIL;
    }
    if (ferror(f)) {
        err = ESP_FAIL;
    }
    if (fclose(f) != 0) {
        err = ESP_FAIL;
    }
    ESP_LOGI(TAG, "Wrote %u bytes of audio", (unsigned)s->data_bytes);
    return err;
}
