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
#define CORE_S3_SPEAKER_MCLK_GPIO 0
#define SPEAKER_SAMPLE_RATE 24000
#define SPEAKER_TONE_HZ 440
#define SPEAKER_TONE_AMPLITUDE 3000
#define SPEAKER_FRAMES_PER_WRITE 480

static i2c_master_bus_handle_t i2c_bus;
static i2s_chan_handle_t speaker_tx;
static int active_sda_gpio = CORE_S3_I2C_SDA_GPIO;
static int active_scl_gpio = CORE_S3_I2C_SCL_GPIO;
static i2c_port_num_t active_i2c_port = I2C_NUM_1;

static const int16_t sine_table_64[64] = {
    0, 294, 585, 871, 1148, 1414, 1667, 1903,
    2121, 2319, 2494, 2646, 2772, 2871, 2942, 2986,
    3000, 2986, 2942, 2871, 2772, 2646, 2494, 2319,
    2121, 1903, 1667, 1414, 1148, 871, 585, 294,
    0, -294, -585, -871, -1148, -1414, -1667, -1903,
    -2121, -2319, -2494, -2646, -2772, -2871, -2942, -2986,
    -3000, -2986, -2942, -2871, -2772, -2646, -2494, -2319,
    -2121, -1903, -1667, -1414, -1148, -871, -585, -294,
};

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

static size_t i2c_probe_known_devices(void)
{
    const struct {
        uint8_t addr;
        const char *name;
    } devices[] = {
        { CORE_S3_AXP2101_I2C_ADDR, "AXP2101" },
        { CORE_S3_AW88298_I2C_ADDR, "AW88298" },
        { CORE_S3_AW9523_I2C_ADDR, "AW9523" },
        { 0x40, "ES7210" },
    };
    size_t found = 0;
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i) {
        esp_err_t ret = i2c_master_probe(i2c_bus, devices[i].addr, pdMS_TO_TICKS(50));
        if (ret == ESP_OK) {
            ++found;
        }
        ESP_LOGI(TAG, "i2c probe port=%d sda=%d scl=%d addr=0x%02x name=%s ret=%s",
                 active_i2c_port,
                 active_sda_gpio,
                 active_scl_gpio,
                 devices[i].addr,
                 devices[i].name,
                 esp_err_to_name(ret));
    }
    return found;
}

static bool i2c_candidate_can_read_axp(void)
{
    i2c_master_dev_handle_t power = NULL;
    uint8_t axp90 = 0;
    esp_err_t ret = add_i2c_device(CORE_S3_AXP2101_I2C_ADDR, &power);
    if (ret == ESP_OK) {
        ret = read_reg8(power, 0x90, &axp90);
    }
    if (power) {
        i2c_master_bus_rm_device(power);
    }
    ESP_LOGI(TAG,
             "i2c axp-read-test port=%d sda=%d scl=%d reg90=0x%02x ret=%s",
             active_i2c_port,
             active_sda_gpio,
             active_scl_gpio,
             axp90,
             esp_err_to_name(ret));
    return ret == ESP_OK;
}

static esp_err_t init_i2c_pair(i2c_port_num_t port, int sda_gpio, int scl_gpio)
{
    if (i2c_bus) {
        i2c_del_master_bus(i2c_bus);
        i2c_bus = NULL;
    }
    active_i2c_port = port;
    active_sda_gpio = sda_gpio;
    active_scl_gpio = scl_gpio;
    i2c_master_bus_config_t config = {
        .i2c_port = port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&config, &i2c_bus);
    ESP_LOGI(TAG, "i2c init ret=%s port=%d sda=%d scl=%d", esp_err_to_name(ret), port, sda_gpio, scl_gpio);
    return ret;
}

static esp_err_t init_i2c(void)
{
    // Moddable m5stack_cores3 provider.js uses the internal I2C bus as port 0,
    // SDA=12, SCL=11. M5Unified variants are kept only as diagnostics.
    const struct {
        i2c_port_num_t port;
        int sda;
        int scl;
        const char *label;
    } candidates[] = {
        { I2C_NUM_0, CORE_S3_I2C_SDA_GPIO, CORE_S3_I2C_SCL_GPIO, "moddable-internal" },
        { I2C_NUM_1, CORE_S3_I2C_SDA_GPIO, CORE_S3_I2C_SCL_GPIO, "m5unified-internal" },
        { I2C_NUM_1, CORE_S3_I2C_SCL_GPIO, CORE_S3_I2C_SDA_GPIO, "port1-reversed-pins" },
        { I2C_NUM_0, CORE_S3_I2C_SCL_GPIO, CORE_S3_I2C_SDA_GPIO, "port0-reversed-pins" },
    };

    esp_err_t last_ret = ESP_FAIL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        ESP_LOGI(TAG, "trying I2C candidate %s", candidates[i].label);
        last_ret = init_i2c_pair(candidates[i].port, candidates[i].sda, candidates[i].scl);
        if (last_ret != ESP_OK) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        size_t probes = i2c_probe_known_devices();
        if (probes > 0 || i2c_candidate_can_read_axp()) {
            ESP_LOGI(TAG, "selected I2C candidate %s", candidates[i].label);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG,
             "no devices found on any CoreS3 internal I2C candidate; reverting to Moddable internal bus for power/amp writes");
    return init_i2c_pair(I2C_NUM_0, CORE_S3_I2C_SDA_GPIO, CORE_S3_I2C_SCL_GPIO);
}

static esp_err_t core_s3_aw9523_bit_on(uint8_t reg, uint8_t mask)
{
    i2c_master_dev_handle_t expander = NULL;
    esp_err_t ret = add_i2c_device(CORE_S3_AW9523_I2C_ADDR, &expander);
    uint8_t value = 0;
    if (ret == ESP_OK) ret = read_reg8(expander, reg, &value);
    if (ret == ESP_OK) ret = write_reg8(expander, reg, value | mask);
    ESP_LOGI(TAG,
             "aw9523 bit_on reg=0x%02x mask=0x%02x ret=%s before=0x%02x",
             reg,
             mask,
             esp_err_to_name(ret),
             value);
    if (expander) i2c_master_bus_rm_device(expander);
    return ret;
}

static esp_err_t init_power_rails(void)
{
    i2c_master_dev_handle_t power = NULL;
    esp_err_t ret = add_i2c_device(CORE_S3_AXP2101_I2C_ADDR, &power);
    uint8_t axp90 = 0;
    if (ret == ESP_OK) ret = read_reg8(power, 0x90, &axp90);
    // Match M5Unified CoreS3 AXP2101 setup as closely as possible.
    // See M5Unified Power_Class.cpp: CoreS3 sets ALDO1=1.8V for AW88298,
    // ALDO2=3.3V for ES7210, and enables the LDO bank.
    if (ret == ESP_OK) ret = write_reg8(power, 0x90, 0xbf);
    if (ret == ESP_OK) ret = write_reg8(power, 0x92, 18 - 5);
    if (ret == ESP_OK) ret = write_reg8(power, 0x93, 33 - 5);
    if (ret == ESP_OK) ret = write_reg8(power, 0x94, 33 - 5);
    if (ret == ESP_OK) ret = write_reg8(power, 0x95, 33 - 5);
    if (ret == ESP_OK) ret = write_reg8(power, 0x27, 0x00);
    if (ret == ESP_OK) ret = write_reg8(power, 0x69, 0x11);
    if (ret == ESP_OK) ret = write_reg8(power, 0x10, 0x30);
    if (ret == ESP_OK) ret = write_reg8(power, 0x30, 0x0f);
    ESP_LOGI(TAG, "power rails ret=%s axp90_before=0x%02x", esp_err_to_name(ret), axp90);
    if (power) i2c_master_bus_rm_device(power);
    return ret;
}

static uint16_t aw88298_rate_reg_value(uint32_t sample_rate)
{
    static const uint8_t rate_tbl[] = { 4, 5, 6, 8, 10, 11, 15, 20, 22, 44 };
    size_t reg_value = 0;
    size_t rate = (sample_rate + 1102) / 2205;
    while (rate > rate_tbl[reg_value] && ++reg_value < sizeof(rate_tbl)) {
    }
    return (uint16_t)(reg_value | 0x14c0);
}

static esp_err_t init_aw9523_like_moddable(void)
{
    i2c_master_dev_handle_t expander = NULL;
    esp_err_t ret = add_i2c_device(CORE_S3_AW9523_I2C_ADDR, &expander);
    if (ret == ESP_OK) ret = write_reg8(expander, 0x02, 0b00000111);
    if (ret == ESP_OK) ret = write_reg8(expander, 0x03, 0b10000011);
    if (ret == ESP_OK) ret = write_reg8(expander, 0x04, 0b00011000);
    if (ret == ESP_OK) ret = write_reg8(expander, 0x05, 0b00001100);
    if (ret == ESP_OK) ret = write_reg8(expander, 0x11, 0b00010000);
    if (ret == ESP_OK) ret = write_reg8(expander, 0x12, 0b11111111);
    if (ret == ESP_OK) ret = write_reg8(expander, 0x13, 0b11111111);

    // Match Moddable resetLcd(): clear P0_5 briefly, then set it again.
    uint8_t reg03 = 0;
    if (ret == ESP_OK) ret = read_reg8(expander, 0x03, &reg03);
    if (ret == ESP_OK) ret = write_reg8(expander, 0x03, reg03 & 0b11011111);
    if (ret == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    if (ret == ESP_OK) ret = write_reg8(expander, 0x03, (reg03 & 0b11011111) | 0b00100000);

    ESP_LOGI(TAG, "aw9523 Moddable setup ret=%s reg03_before=0x%02x", esp_err_to_name(ret), reg03);
    if (expander) i2c_master_bus_rm_device(expander);
    return ret;
}

static esp_err_t enable_speaker_amp(void)
{
    i2c_master_dev_handle_t amp = NULL;
    esp_err_t amp_ret = add_i2c_device(CORE_S3_AW88298_I2C_ADDR, &amp);

    uint16_t rate_reg = aw88298_rate_reg_value(SPEAKER_SAMPLE_RATE);
    // Match Moddable m5stack_cores3/setup-target.js AW88298 constructor.
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x05, 0x0008); // RMSE=0 HAGCE=0 HDCCE=0 HMUTE=0
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x61, 0x0673); // boost mode disabled
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x04, 0x4040); // I2SEN=1 AMPPD=0 PWDN=0
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x0c, 0x0664); // volume=250 like Moddable
    if (amp_ret == ESP_OK) amp_ret = write_reg16_be(amp, 0x06, rate_reg); // sample-rate table

    ESP_LOGI(TAG,
             "amp enable aw88298_ret=%s rate_reg=0x%04x sample_rate=%d",
             esp_err_to_name(amp_ret),
             rate_reg,
             SPEAKER_SAMPLE_RATE);
    if (amp_ret != ESP_OK) {
        ESP_LOGW(TAG, "AW88298 writes did not ACK; speaker will likely remain silent");
    }

    if (amp) i2c_master_bus_rm_device(amp);
    return amp_ret;
}

static esp_err_t start_i2s(void)
{
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_config.dma_desc_num = 8;
    chan_config.dma_frame_num = 240;
    chan_config.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_config, &speaker_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel ret=%s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_config = {0};
    std_config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_config.clk_cfg.sample_rate_hz = SPEAKER_SAMPLE_RATE;
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_config.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_config.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_config.slot_cfg.ws_pol = false;
    std_config.slot_cfg.bit_shift = false;
    std_config.slot_cfg.left_align = false;
    std_config.slot_cfg.big_endian = false;
    std_config.slot_cfg.bit_order_lsb = false;
    std_config.gpio_cfg.mclk = CORE_S3_SPEAKER_MCLK_GPIO;
    std_config.gpio_cfg.bclk = CORE_S3_SPEAKER_BCLK_GPIO;
    std_config.gpio_cfg.ws = CORE_S3_SPEAKER_WS_GPIO;
    std_config.gpio_cfg.dout = CORE_S3_SPEAKER_DOUT_GPIO;
    std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.invert_flags.mclk_inv = false;
    std_config.gpio_cfg.invert_flags.bclk_inv = false;
    std_config.gpio_cfg.invert_flags.ws_inv = false;
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
    int16_t pcm[SPEAKER_FRAMES_PER_WRITE];
    uint32_t phase_q16 = 0;
    const uint32_t phase_step_q16 = (uint32_t)(((uint64_t)SPEAKER_TONE_HZ * 64U * 65536U) / SPEAKER_SAMPLE_RATE);
    uint64_t writes = 0;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    uint64_t timeouts = 0;

    while (true) {
        for (size_t i = 0; i < SPEAKER_FRAMES_PER_WRITE; ++i) {
            pcm[i] = sine_table_64[(phase_q16 >> 16) & 63];
            phase_q16 += phase_step_q16;
        }
        size_t written = 0;
        esp_err_t ret = i2s_channel_write(speaker_tx, pcm, sizeof(pcm), &written, pdMS_TO_TICKS(1000));
        if (ret == ESP_OK) {
            ++writes;
            frames += written / sizeof(int16_t);
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
    esp_err_t aw9523_ret = init_aw9523_like_moddable();
    ESP_LOGI(TAG, "aw9523 init boundary ret=%s", esp_err_to_name(aw9523_ret));
    i2c_probe_known_devices();
    esp_err_t amp_ret = enable_speaker_amp();
    ESP_LOGI(TAG, "amp init boundary ret=%s", esp_err_to_name(amp_ret));
    ESP_ERROR_CHECK(start_i2s());
    xTaskCreatePinnedToCore(speaker_task, "speaker_tone", 4096, NULL, 5, NULL, 1);
}
