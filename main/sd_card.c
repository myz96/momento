/* microSD card over SPI for the XIAO ESP32S3 Sense expansion board. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

#include "sd_card.h"

static const char *TAG = "sd_card";

/* XIAO ESP32S3 Sense SD slot pins. GPIO21 is shared with the user LED. */
#define SD_PIN_SCK  7
#define SD_PIN_MISO 8
#define SD_PIN_MOSI 9
#define SD_PIN_CS   21

static sdmmc_card_t *s_card;

esp_err_t sd_card_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8192,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_PIN_CS;
    slot_cfg.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg,
                                  &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t sd_card_smoke_test(void)
{
    static const char msg[] = "momento sd card smoke test\n";
    const char *path = SD_MOUNT_POINT "/TEST.TXT";

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for write", path);
        return ESP_FAIL;
    }
    fputs(msg, f);
    fclose(f);

    char readback[sizeof(msg)] = {0};
    f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for read", path);
        return ESP_FAIL;
    }
    fread(readback, 1, sizeof(readback) - 1, f);
    fclose(f);

    if (strcmp(msg, readback) != 0) {
        ESP_LOGE(TAG, "Readback mismatch");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Smoke test OK: wrote and read %s", path);
    return ESP_OK;
}
