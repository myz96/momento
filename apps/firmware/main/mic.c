/* PDM microphone on the XIAO ESP32S3 Sense: clock GPIO42, data GPIO41. */

#include "driver/i2s_pdm.h"
#include "esp_log.h"

#include "mic.h"

static const char *TAG = "mic";

#define MIC_PIN_CLK  GPIO_NUM_42
#define MIC_PIN_DATA GPIO_NUM_41

static i2s_chan_handle_t s_rx;

esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_PIN_CLK,
            .din = MIC_PIN_DATA,
        },
    };
    err = i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PDM RX init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Channel enable failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "PDM microphone ready (%d Hz, 16-bit, mono)", MIC_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t mic_read(void *dst, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    return i2s_channel_read(s_rx, dst, len, bytes_read, timeout_ms);
}
