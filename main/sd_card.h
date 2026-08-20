#pragma once

#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

/* Mounts the microSD card over SPI. Formats the card if the mount fails. */
esp_err_t sd_card_mount(void);

/* Writes TEST.TXT, reads it back, and compares the content. */
esp_err_t sd_card_smoke_test(void);
