#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "speaker-tone";

#define CORE_S3_I2C_SDA_GPIO 12
#define CORE_S3_I2C_SCL_GPIO 11
#define CORE_S3_AXP2101_I2C_ADDR 0x34
#define CORE_S3_AW88298_I2C_ADDR 0x36
#define CORE_S3_AW9523_I2C_ADDR 0x58

#define CORE_S3_SPEAKER_BCLK_GPIO 34
#define CORE_S3_SPEAKER_WS_GPIO 33
#define CORE_S3_SPEAKER_DOUT_GPIO 13
#define SPEAKER_SAMPLE_RATE 48000
#define SPEAKER_TONE_HZ 440
#define SPEAKER_FRAMES_PER_WRITE 480

static i2c_master_bus_handle_t i2c_bus;
static i2s_chan_handle_t speaker_tx;
static int active_sda_gpio = CORE_S3_I2C_SDA_GPIO;
static int active_scl_gpio = CORE_S3_I2C_SCL_GPIO;

static esp_err_t add_i2c_device(uint8_t addr, i2c_master_dev_handle_t *device)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(i2c_bus, &config, device);
}

static esp_err_t write_reg8(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };
    return i2c_master_transmit(device, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t read_reg8(i2c_master_dev_handle_t device, uint8_t reg, uint8_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(device, &reg, sizeof(reg), value, sizeof(*value), pdMS_TO_TICKS(100));
}

static esp_err_t write_reg16_be(i2c_master_dev_handle_t device, uint8_t reg, uint16_t value)
{
    uint8_t data[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xff) };
    return i2c_master_transmit(device, data, sizeof(data), pdMS_TO_TICKS(100));
}

static size_t i2c_scan(void)
{
    char line[256];
    size_t pos = 0;
    size_t found = 0;
    pos += snprintf(line + pos, sizeof(line) - pos, "i2c probe sda=%d scl=%d found:", active_sda_gpio, active_scl_gpio);
    for (uint8_t addr = 0x08; addr < 0x78 && pos < sizeof(line); ++addr) {
        esp_err_t ret = i2c_master_probe(i2c_bus, addr, pdMS_TO_TICKS(50));
        if (ret == ESP_OK) {
            ++found;
            pos += snprintf(line + pos, sizeof(line) - pos, " 0x%02x", addr);
        }
    }
    ESP_LOGI(TAG, "%s", line);
    return found;
}

static esp_err_t init_i2c_pair(int sda_gpio, int scl_gpio)
{
    if (i2c_bus) {
        i2c_del_master_bus(i2c_bus);
        i2c_bus = NULL;
    }
    active_sda_gpio = sda_gpio;
    active_scl_gpio = scl_gpio;
    i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&config, &i2c_bus);
    ESP_LOGI(TAG, "i2c init ret=%s sda=%d scl=%d", esp_err_to_name(ret), sda_gpio, scl_gpio);
    return ret;
}

static esp_err_t init_i2c(void)
{
    esp_err_t ret = init_i2c_pair(CORE_S3_I2C_SDA_GPIO, CORE_S3_I2C_SCL_GPIO);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    if (i2c_scan() > 0) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "no devices on default internal I2C pair; trying reversed pair for pin-map boundary check");
    ret = init_i2c_pair(CORE_S3_I2C_SCL_GPIO, CORE_S3_I2C_SDA_GPIO);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    i2c_scan();
    return ESP_OK;
}

static esp_err_t init_power_rails(void)
{
    i2c_master_dev_handle_t power = NULL;
    esp_err_t ret = add_i2c_device(CORE_S3_AXP2101_I2C_ADDR, &power);
    uint8_t axp90 = 0;
    if (ret == ESP_OK) ret = read_reg8(power, 0x90, &axp90);
    if (ret == ESP_OK) ret = write_reg8(power, 0x90, axp90 | 0xb4);
    if (ret == ESP_OK) ret = write_reg8(power, 0x97, 0x1c);
    if (ret == ESP_OK) ret = write_reg8(power, 0x69, 0x35);
    if (ret == ESP_OK) ret = write_reg8(power, 0x30, 0x3f);
    if (ret == ESP_OK) ret = write_reg8(power, 0x90, 0xbf);
    if (ret == ESP_OK) ret = write_reg8(power, 0x94, 0x1c);
    if (ret == ESP_OK) ret = write_reg8(power, 0x95, 0x1c);
    if (ret == ESP_OK) ret = write_reg8(power, 0x27, 0x00);
    uint8_t charge = 0;
    if (ret == ESP_OK) ret = read_reg8(power, 0x62, &charge);
    if (ret == ESP_OK) ret = write_reg8(power, 0x62, (charge & 0xe0) | 13);
    ESP_LOGI(TAG, "power rails ret=%s axp90_before=0x%02x charge_before=0x%02x", esp_err_to_name(ret), axp90, charge);
    if (power) i2c_master_bus_rm_device(power);
    return ret;
}

static esp_err_t enable_speaker_amp(void)
{
    i2c_master_dev_handle_t expander = NULL;
    i2c_master_dev_handle_t amp = NULL;
    esp_err_t expander_ret = add_i2c_device(CORE_S3_AW9523_I2C_ADDR, &expander);
    esp_err_t amp_ret = add_i2c_device(CORE_S3_AW88298_I2C_ADDR, &amp);

    uint8_t reg02 = 0;
    if (expander_ret == ESP_OK) expander_ret = read_reg8(expander, 0x02, &reg02);
    if (expander_ret == ESP_OK) expander_ret = write_reg8(expander, 0x02, reg02 | 0x04);

    // M5Unified CoreS3 speaker callback sequence for AW88298.
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x61, 0x0673); // boost mode disabled
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x04, 0x4040); // I2SEN=1 AMPPD=0 PWDN=0
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x05, 0x0008); // unmute
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x06, 0x14c8); // 48 kHz rate table + BCK mode 16*2
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x0c, 0x0064); // full volume

    ESP_LOGI(TAG,
             "amp enable expander_ret=%s aw9523_reg02_before=0x%02x aw88298_ret=%s",
             esp_err_to_name(expander_ret),
             reg02,
             esp_err_to_name(amp_ret));
    if (amp_ret != ESP_OK) {
        ESP_LOGW(TAG, "AW88298 writes did not ACK; continuing after AXP2101/AW9523 like M5Unified-style bring-up");
    }

    if (expander) i2c_master_bus_rm_device(expander);
    if (amp) i2c_master_bus_rm_device(amp);
    return expander_ret;
}

static esp_err_t start_i2s(void)
{
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_config.dma_desc_num = 8;
    chan_config.dma_frame_num = 240;
    esp_err_t ret = i2s_new_channel(&chan_config, &speaker_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel ret=%s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CORE_S3_SPEAKER_BCLK_GPIO,
            .ws = CORE_S3_SPEAKER_WS_GPIO,
            .dout = CORE_S3_SPEAKER_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(speaker_tx, &std_config);
    if (ret == ESP_OK) ret = i2s_channel_enable(speaker_tx);
    ESP_LOGI(TAG, "i2s start ret=%s sample_rate=%d bclk=%d ws=%d dout=%d",
             esp_err_to_name(ret),
             SPEAKER_SAMPLE_RATE,
             CORE_S3_SPEAKER_BCLK_GPIO,
             CORE_S3_SPEAKER_WS_GPIO,
             CORE_S3_SPEAKER_DOUT_GPIO);
    return ret;
}

static void speaker_task(void *arg)
{
    (void)arg;
    int16_t pcm[SPEAKER_FRAMES_PER_WRITE * 2];
    uint32_t phase = 0;
    const uint32_t half_period = SPEAKER_SAMPLE_RATE / (SPEAKER_TONE_HZ * 2);
    uint64_t writes = 0;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    uint64_t timeouts = 0;

    while (true) {
        for (size_t i = 0; i < SPEAKER_FRAMES_PER_WRITE; ++i) {
            int16_t sample = (phase++ / half_period) & 1 ? 12000 : -12000;
            pcm[i * 2] = sample;
            pcm[i * 2 + 1] = sample;
        }
        size_t written = 0;
        esp_err_t ret = i2s_channel_write(speaker_tx, pcm, sizeof(pcm), &written, pdMS_TO_TICKS(1000));
        if (ret == ESP_OK) {
            ++writes;
            frames += written / (sizeof(int16_t) * 2);
            bytes += written;
        } else {
            ++timeouts;
        }
        if ((writes % 100) == 0 || ret != ESP_OK) {
            ESP_LOGI(TAG, "wrote writes=%llu frames=%llu bytes=%llu last_ret=%s last_bytes=%u timeouts=%llu tone_hz=%d",
                     writes,
                     frames,
                     bytes,
                     esp_err_to_name(ret),
                     (unsigned)written,
                     timeouts,
                     SPEAKER_TONE_HZ);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "CoreS3 speaker-only tone sample start");
    ESP_ERROR_CHECK(init_i2c());
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t power_ret = init_power_rails();
    ESP_LOGI(TAG, "power init boundary ret=%s", esp_err_to_name(power_ret));
    i2c_scan();
    esp_err_t amp_ret = enable_speaker_amp();
    ESP_LOGI(TAG, "amp init boundary ret=%s", esp_err_to_name(amp_ret));
    ESP_ERROR_CHECK(start_i2s());
    xTaskCreatePinnedToCore(speaker_task, "speaker_tone", 4096, NULL, 5, NULL, 1);
}
