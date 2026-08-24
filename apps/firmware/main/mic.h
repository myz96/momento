#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MIC_SAMPLE_RATE 16000

/* Starts the PDM microphone at 16 kHz, 16-bit, mono. */
esp_err_t mic_init(void);

/* Reads raw samples. Blocks up to timeout_ms. */
esp_err_t mic_read(void *dst, size_t len, size_t *bytes_read, uint32_t timeout_ms);
