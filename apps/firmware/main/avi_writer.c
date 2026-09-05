/* Streaming MJPEG AVI writer.
 *
 * Layout: RIFF('AVI ' LIST('hdrl' avih LIST('strl' strh strf))
 *                     LIST('movi' '00dc'...) 'idx1').
 * The header is written with placeholder sizes at open and patched at
 * finish. All values are little-endian, matching the ESP32-S3.
 */

#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "avi_writer.h"

static const char *TAG = "avi";

#define AVIF_HASINDEX  0x00000010
#define AVIIF_KEYFRAME 0x00000010

#define INDEX_INITIAL_CAP 1024

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void put_tag(FILE *f, const char *tag) { fwrite(tag, 4, 1, f); }

static uint32_t padded(uint32_t len) { return (len + 1) & ~1u; }

esp_err_t avi_stream_open(avi_stream_t *s, const char *path, uint16_t width,
                          uint16_t height)
{
    memset(s, 0, sizeof(*s));
    s->width = width;
    s->height = height;
    s->movi_bytes = 4; /* the 'movi' type tag */

    s->frame_lens = heap_caps_malloc(INDEX_INITIAL_CAP * sizeof(uint32_t),
                                     MALLOC_CAP_SPIRAM);
    if (!s->frame_lens) {
        ESP_LOGE(TAG, "Index allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s->frame_cap = INDEX_INITIAL_CAP;

    s->f = fopen(path, "wb");
    if (!s->f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        heap_caps_free(s->frame_lens);
        s->frame_lens = NULL;
        return ESP_FAIL;
    }
    FILE *f = s->f;

    put_tag(f, "RIFF");
    s->pos_riff_size = ftell(f);
    put_u32(f, 0); /* patched on finish */
    put_tag(f, "AVI ");

    put_tag(f, "LIST");
    put_u32(f, 4 + 64 + 124); /* hdrl: type + avih chunk + strl list */
    put_tag(f, "hdrl");

    put_tag(f, "avih");
    put_u32(f, 56);
    s->pos_avih = ftell(f);
    put_u32(f, 0); /* usec per frame, patched */
    put_u32(f, 0); /* max bytes per second, patched */
    put_u32(f, 0); /* padding granularity */
    put_u32(f, AVIF_HASINDEX);
    put_u32(f, 0); /* total frames, patched */
    put_u32(f, 0); /* initial frames */
    put_u32(f, 1); /* streams */
    put_u32(f, 0); /* buffer size, patched */
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
    s->pos_strh = ftell(f);
    put_tag(f, "vids");
    put_tag(f, "MJPG");
    put_u32(f, 0); /* flags */
    put_u16(f, 0); /* priority */
    put_u16(f, 0); /* language */
    put_u32(f, 0); /* initial frames */
    put_u32(f, 0); /* scale, patched */
    put_u32(f, 0); /* rate, patched */
    put_u32(f, 0); /* start */
    put_u32(f, 0); /* length, patched */
    put_u32(f, 0); /* buffer size, patched */
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
    s->pos_movi_size = ftell(f);
    put_u32(f, 0); /* patched on finish */
    put_tag(f, "movi");
    return ESP_OK;
}

esp_err_t avi_stream_add_frame(avi_stream_t *s, const uint8_t *jpeg,
                               uint32_t len)
{
    if (!s->f) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s->frame_count >= s->frame_cap) {
        size_t new_cap = s->frame_cap * 2;
        uint32_t *grown = heap_caps_realloc(
            s->frame_lens, new_cap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        if (!grown) {
            ESP_LOGE(TAG, "Index growth failed at %u frames",
                     (unsigned)s->frame_count);
            return ESP_ERR_NO_MEM;
        }
        s->frame_lens = grown;
        s->frame_cap = new_cap;
    }

    long chunk_start = ftell(s->f);
    put_tag(s->f, "00dc");
    put_u32(s->f, len);
    size_t written = fwrite(jpeg, 1, len, s->f);
    if (len & 1) {
        fputc(0, s->f);
    }
    if (written != len) {
        ESP_LOGE(TAG, "Short frame write (%u of %u bytes)", (unsigned)written,
                 (unsigned)len);
        /* Roll back so finish writes the index where this frame began;
         * the RIFF size then hides any trailing garbage from players.
         * clearerr keeps the sticky error flag from failing the later
         * finish, whose own writes are checked separately. */
        if (chunk_start >= 0 && fseek(s->f, chunk_start, SEEK_SET) == 0) {
            clearerr(s->f);
        }
        return ESP_FAIL;
    }

    s->frame_lens[s->frame_count] = len;
    s->frame_count++;
    s->movi_bytes += 8 + padded(len);
    if (len > s->max_frame) {
        s->max_frame = len;
    }
    return ESP_OK;
}

esp_err_t avi_stream_sync(avi_stream_t *s)
{
    if (!s->f) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fflush(s->f) != 0 || fsync(fileno(s->f)) != 0) {
        ESP_LOGE(TAG, "Video sync to card failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t avi_stream_finish(avi_stream_t *s, uint64_t duration_us)
{
    if (!s->f) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = s->f;
    s->f = NULL;

    if (s->frame_count == 0 || duration_us == 0) {
        fclose(f);
        heap_caps_free(s->frame_lens);
        s->frame_lens = NULL;
        ESP_LOGE(TAG, "No frames recorded");
        return ESP_ERR_INVALID_ARG;
    }

    put_tag(f, "idx1");
    put_u32(f, 16 * s->frame_count);
    uint32_t chunk_offset = 4; /* relative to the 'movi' type tag */
    for (uint32_t i = 0; i < s->frame_count; i++) {
        put_tag(f, "00dc");
        put_u32(f, AVIIF_KEYFRAME);
        put_u32(f, chunk_offset);
        put_u32(f, s->frame_lens[i]);
        chunk_offset += 8 + padded(s->frame_lens[i]);
    }
    long end = ftell(f);

    uint32_t usec_per_frame = (uint32_t)(duration_us / s->frame_count);
    uint32_t rate = s->frame_count * 1000u;
    uint32_t scale = (uint32_t)(duration_us / 1000u);
    uint32_t riff_size = (uint32_t)end - 8;

    fseek(f, s->pos_riff_size, SEEK_SET);
    put_u32(f, riff_size);

    fseek(f, s->pos_avih, SEEK_SET);
    put_u32(f, usec_per_frame);
    put_u32(f, s->max_frame * 15); /* rough max bytes per second */
    fseek(f, s->pos_avih + 16, SEEK_SET);
    put_u32(f, s->frame_count);
    fseek(f, s->pos_avih + 28, SEEK_SET);
    put_u32(f, s->max_frame);

    fseek(f, s->pos_strh + 20, SEEK_SET);
    put_u32(f, scale);
    put_u32(f, rate);
    fseek(f, s->pos_strh + 32, SEEK_SET);
    put_u32(f, s->frame_count);
    put_u32(f, s->max_frame);

    fseek(f, s->pos_movi_size, SEEK_SET);
    put_u32(f, s->movi_bytes);

    esp_err_t err = ferror(f) ? ESP_FAIL : ESP_OK;
    if (fclose(f) != 0) {
        err = ESP_FAIL;
    }
    heap_caps_free(s->frame_lens);
    s->frame_lens = NULL;

    ESP_LOGI(TAG, "Wrote %u frames, %ux%u, %.1f fps, %ld bytes",
             (unsigned)s->frame_count, s->width, s->height,
             1000000.0f * s->frame_count / duration_us, end);
    return err;
}
