#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t offset; /* byte offset into the shared frame data buffer */
    uint32_t len;    /* JPEG size in bytes */
} avi_frame_ref_t;

/* Writes an MJPEG AVI file from JPEG frames stored in one buffer.
 * The frame rate comes from frame_count and duration_us. */
esp_err_t avi_write_mjpeg(const char *path, const uint8_t *frame_data,
                          const avi_frame_ref_t *frames, size_t frame_count,
                          uint16_t width, uint16_t height, uint64_t duration_us);
