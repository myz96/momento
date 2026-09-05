#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

/* Streaming MJPEG AVI writer. Frames go straight to the SD card; the
 * header and index are patched on finish, so recording length is bound
 * by the card, not by RAM. */

typedef struct {
    FILE *f;
    uint32_t *frame_lens; /* PSRAM-backed index of frame sizes */
    size_t frame_cap;
    uint32_t frame_count;
    uint32_t max_frame;
    uint32_t movi_bytes; /* movi list data bytes, including the type tag */
    uint16_t width;
    uint16_t height;
    long pos_riff_size;
    long pos_avih;
    long pos_strh;
    long pos_movi_size;
} avi_stream_t;

/* Opens the file and writes a placeholder header. */
esp_err_t avi_stream_open(avi_stream_t *s, const char *path, uint16_t width,
                          uint16_t height);

/* Appends one JPEG frame. On a failed write the file position rolls
 * back to the frame start, so the stream stays consistent. */
esp_err_t avi_stream_add_frame(avi_stream_t *s, const uint8_t *jpeg,
                               uint32_t len);

/* Flushes buffered frames to the card so a power loss keeps the video
 * so far. The header still needs finish. Fails when the card does. */
esp_err_t avi_stream_sync(avi_stream_t *s);

/* Writes the index, patches the header, and closes the file. Frees the
 * frame index. duration_us sets the frame rate. */
esp_err_t avi_stream_finish(avi_stream_t *s, uint64_t duration_us);
