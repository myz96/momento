#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Writes 16-bit mono PCM samples to a WAV file. */
esp_err_t wav_write_file(const char *path, const int16_t *samples,
                         size_t sample_count, uint32_t sample_rate);
