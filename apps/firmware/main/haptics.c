#include "haptics.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "haptics";

#define DRV2605_ADDR 0x5A
#define PIN_I2C_SDA GPIO_NUM_5
#define PIN_I2C_SCL GPIO_NUM_6

/* Registers (DRV2605L datasheet §8.6). */
#define REG_STATUS   0x00
#define REG_MODE     0x01
#define REG_LIBRARY  0x03
#define REG_WAVESEQ1 0x04
#define REG_WAVESEQ2 0x05
#define REG_GO       0x0C
#define REG_FEEDBACK 0x1A

#define MODE_INTERNAL_TRIGGER 0x00

/* ROM library effects (datasheet §11.2). */
#define EFFECT_STRONG_CLICK 1
#define EFFECT_DOUBLE_CLICK 10

static i2c_master_dev_handle_t s_dev; /* NULL = chip absent, all no-ops */

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

void haptics_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed, haptics off");
        return;
    }
    if (i2c_master_probe(bus, DRV2605_ADDR, 50) != ESP_OK) {
        ESP_LOGI(TAG, "No DRV2605L at 0x5A, haptics off");
        i2c_del_master_bus(bus);
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DRV2605_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605L attach failed, haptics off");
        i2c_del_master_bus(bus);
        return;
    }

    /* Exit standby, ERM open loop, ROM library A — the Adafruit-breakout
     * default; a different motor only needs a library/feedback change. */
    esp_err_t err = ESP_OK;
    err |= write_reg(REG_MODE, MODE_INTERNAL_TRIGGER);
    err |= write_reg(REG_FEEDBACK, 0x36); /* ERM mode, datasheet default gains */
    err |= write_reg(REG_LIBRARY, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605L config failed, haptics off");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        i2c_del_master_bus(bus);
        return;
    }
    ESP_LOGI(TAG, "DRV2605L ready");
}

static void play(uint8_t effect)
{
    if (!s_dev) {
        return;
    }
    /* Fire-and-forget: the sequencer plays while capture continues. */
    write_reg(REG_WAVESEQ1, effect);
    write_reg(REG_WAVESEQ2, 0); /* end of sequence */
    write_reg(REG_GO, 1);
}

void haptics_click(void)
{
    play(EFFECT_STRONG_CLICK);
}

void haptics_double_click(void)
{
    play(EFFECT_DOUBLE_CLICK);
}
