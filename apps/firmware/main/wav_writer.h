#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

/* Streaming WAV writer: 16-bit mono PCM, header patched on finish. */

typedef struct {
    FILE *f;
    uint32_t data_bytes;
} wav_stream_t;

/* Opens the file and writes a placeholder header. */
esp_err_t wav_stream_open(wav_stream_t *s, const char *path,
                          uint32_t sample_rate);

/* Appends samples. Safe to call from a dedicated audio task. */
esp_err_t wav_stream_write(wav_stream_t *s, const int16_t *samples,
                           size_t sample_count);

/* Flushes buffered data to the card so a power loss keeps the audio so
 * far. The header still needs finish. Fails when the card does. */
esp_err_t wav_stream_sync(wav_stream_t *s);

/* Patches the header sizes and closes the file. */
esp_err_t wav_stream_finish(wav_stream_t *s);
