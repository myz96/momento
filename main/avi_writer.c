/* Minimal MJPEG AVI writer.
 *
 * Layout: RIFF('AVI ' LIST('hdrl' avih LIST('strl' strh strf))
 *                     LIST('movi' '00dc'...) 'idx1').
 * All values are little-endian; the ESP32-S3 is little-endian, so plain
 * fwrite of integers is correct.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "avi_writer.h"

static const char *TAG = "avi";

#define AVIF_HASINDEX  0x00000010
#define AVIIF_KEYFRAME 0x00000010

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void put_tag(FILE *f, const char *tag) { fwrite(tag, 4, 1, f); }

static uint32_t padded(uint32_t len) { return (len + 1) & ~1u; }

esp_err_t avi_write_mjpeg(const char *path, const uint8_t *frame_data,
                          const avi_frame_ref_t *frames, size_t frame_count,
                          uint16_t width, uint16_t height, uint64_t duration_us)
{
    if (frame_count == 0 || duration_us == 0) {
        ESP_LOGE(TAG, "No frames to write");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t movi_size = 4; /* 'movi' type tag */
    uint32_t max_frame = 0;
    for (size_t i = 0; i < frame_count; i++) {
        movi_size += 8 + padded(frames[i].len);
        if (frames[i].len > max_frame) {
            max_frame = frames[i].len;
        }
    }
    uint32_t idx1_size = 16 * (uint32_t)frame_count;
    /* hdrl list data: type(4) + avih chunk(8+56) + strl list(8+116) */
    uint32_t hdrl_size = 4 + 64 + 124;
    uint32_t riff_size = 4 + (8 + hdrl_size) + (8 + movi_size) + (8 + idx1_size);

    uint32_t usec_per_frame = (uint32_t)(duration_us / frame_count);
    /* fps = rate / scale */
    uint32_t rate = (uint32_t)frame_count * 1000u;
    uint32_t scale = (uint32_t)(duration_us / 1000u);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }

    put_tag(f, "RIFF");
    put_u32(f, riff_size);
    put_tag(f, "AVI ");

    put_tag(f, "LIST");
    put_u32(f, hdrl_size);
    put_tag(f, "hdrl");

    put_tag(f, "avih");
    put_u32(f, 56);
    put_u32(f, usec_per_frame);
    put_u32(f, max_frame * 15); /* rough max bytes per second */
    put_u32(f, 0);              /* padding granularity */
    put_u32(f, AVIF_HASINDEX);
    put_u32(f, (uint32_t)frame_count);
    put_u32(f, 0); /* initial frames */
    put_u32(f, 1); /* streams */
    put_u32(f, max_frame);
    put_u32(f, width);
    put_u32(f, height);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0);

    put_tag(f, "LIST");
    put_u32(f, 116);
    put_tag(f, "strl");

    put_tag(f, "strh");
    put_u32(f, 56);
    put_tag(f, "vids");
    put_tag(f, "MJPG");
    put_u32(f, 0); /* flags */
    put_u16(f, 0); /* priority */
    put_u16(f, 0); /* language */
    put_u32(f, 0); /* initial frames */
    put_u32(f, scale);
    put_u32(f, rate);
    put_u32(f, 0); /* start */
    put_u32(f, (uint32_t)frame_count);
    put_u32(f, max_frame);
    put_u32(f, 0xFFFFFFFF); /* quality: default */
    put_u32(f, 0);          /* sample size */
    put_u16(f, 0);          /* rcFrame */
    put_u16(f, 0);
    put_u16(f, width);
    put_u16(f, height);

    put_tag(f, "strf");
    put_u32(f, 40);
    put_u32(f, 40); /* BITMAPINFOHEADER size */
    put_u32(f, width);
    put_u32(f, height);
    put_u16(f, 1);  /* planes */
    put_u16(f, 24); /* bit count */
    put_tag(f, "MJPG");
    put_u32(f, (uint32_t)width * height * 3);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0);

    put_tag(f, "LIST");
    put_u32(f, movi_size);
    put_tag(f, "movi");

    for (size_t i = 0; i < frame_count; i++) {
        put_tag(f, "00dc");
        put_u32(f, frames[i].len);
        fwrite(frame_data + frames[i].offset, 1, frames[i].len, f);
        if (frames[i].len & 1) {
            fputc(0, f);
        }
    }

    put_tag(f, "idx1");
    put_u32(f, idx1_size);
    uint32_t chunk_offset = 4; /* relative to the 'movi' type tag */
    for (size_t i = 0; i < frame_count; i++) {
        put_tag(f, "00dc");
        put_u32(f, AVIIF_KEYFRAME);
        put_u32(f, chunk_offset);
        put_u32(f, frames[i].len);
        chunk_offset += 8 + padded(frames[i].len);
    }

    long total = ftell(f);
    fclose(f);

    ESP_LOGI(TAG, "Wrote %s: %u frames, %ux%u, %.1f fps, %ld bytes", path,
             (unsigned)frame_count, width, height,
             1000000.0f * frame_count / duration_us, total);
    return ESP_OK;
}
