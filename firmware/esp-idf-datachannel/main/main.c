#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define HTTP_BUF_SIZE 8192
#define URL_BUF_SIZE 512

#define CORE_S3_ES7210_I2C_ADDR 0x40
#define CORE_S3_AW88298_I2C_ADDR 0x36
#define CORE_S3_AW9523_I2C_ADDR 0x58
#define CORE_S3_AXP2101_I2C_ADDR 0x34
#define CORE_S3_INTERNAL_I2C_SDA_GPIO 12
#define CORE_S3_INTERNAL_I2C_SCL_GPIO 11
#define CORE_S3_MIC_SAMPLE_RATE 8000
#define CORE_S3_MIC_TDM_CHANNELS 4
#define CORE_S3_SPEAKER_SAMPLE_RATE 8000
#define CORE_S3_SPEAKER_CHANNELS 1
#define CORE_S3_SPEAKER_MAX_FRAME_SAMPLES 320
#define CORE_S3_MIC_FRAME_DURATION_MS ((CONFIG_STACKCHAN_CORE_S3_MIC_FRAME_SAMPLES * 1000U) / CORE_S3_MIC_SAMPLE_RATE)
#define AUDIO_TEST_TONE_HZ 440U
#define AUDIO_TEST_TONE_AMPLITUDE 10000


static const char *TAG = "stackchan_dc";
static EventGroupHandle_t wifi_events;
static int wifi_retry_count;
static esp_peer_default_cfg_t peer_default_cfg;

typedef struct {
    char base_url[URL_BUF_SIZE];
    char room[96];
    char client_id[96];
    char wss_url[URL_BUF_SIZE];
    char ws_url[URL_BUF_SIZE];
    char ice_url[URL_BUF_SIZE];
    char ice_user[128];
    char ice_password[128];
    esp_peer_ice_server_cfg_t ice_servers[1];
    bool is_initiator;
    esp_websocket_client_handle_t ws;
    esp_peer_handle_t peer;
    bool peer_loop_running;
    bool peer_connected;
    bool local_offer_started;
    bool audio_task_running;
    bool speaker_amp_enabled;
    i2s_chan_handle_t speaker_tx;
    uint32_t audio_tx_frames;
    uint32_t audio_tx_bytes;
    uint32_t audio_tx_drops;
    uint32_t audio_tx_last_pts;
    uint32_t audio_tx_last_pts_delta;
    uint32_t audio_tx_last_size;
    uint32_t audio_tx_min_size;
    uint32_t audio_tx_max_size;
    uint32_t audio_tx_ret_ok;
    uint32_t audio_tx_ret_fail;
    uint32_t mic_samples;
    uint32_t mic_acquire_frames;
    uint32_t mic_read_failures;
    uint32_t mic_short_reads;
    uint32_t mic_conversion_clips;
    uint32_t audio_rx_frames;
    uint32_t audio_rx_bytes;
    uint32_t audio_rx_empty_frames;
    uint32_t audio_rx_last_pts;
    uint32_t audio_rx_last_pts_delta;
    uint32_t audio_rx_last_size;
    uint32_t audio_rx_pts_discontinuities;
    uint32_t audio_rx_decode_samples;
    uint32_t audio_rx_decode_invalid;
    uint32_t audio_rx_decode_peak;
    int16_t audio_rx_decode_min;
    int16_t audio_rx_decode_max;
    uint32_t audio_rx_decode_rms;
    uint32_t audio_rx_speaker_write_frames;
    uint32_t audio_rx_speaker_write_samples;
    uint32_t audio_rx_speaker_write_bytes;
    uint32_t audio_rx_speaker_drops;
    uint32_t audio_rx_speaker_short_writes;
    esp_err_t audio_rx_speaker_last_ret;
    uint32_t data_rx_frames;
    uint16_t data_stream_id;
} app_ctx_t;

static app_ctx_t app;

static esp_err_t format_into(char *dest, size_t dest_size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(dest, dest_size, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= dest_size) {
        ESP_LOGE(TAG, "formatted string truncated: need=%d cap=%u", written, (unsigned)dest_size);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

typedef struct {
    char *data;
    int len;
    int cap;
} http_buffer_t;

static void log_heap(const char *label)
{
    ESP_LOGI(TAG, "heap[%s] free=%u min=%u psram_free=%u",
             label,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_buffer_t *buffer = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !buffer || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }

    int copy_len = event->data_len;
    if (buffer->len + copy_len >= buffer->cap) {
        copy_len = buffer->cap - buffer->len - 1;
    }
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->len, event->data, copy_len);
        buffer->len += copy_len;
        buffer->data[buffer->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_json(const char *url, esp_http_client_method_t method, const char *body, char *out, int out_len)
{
    http_buffer_t buffer = {
        .data = out,
        .len = 0,
        .cap = out_len,
    };
    out[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, method);
    if (body) {
        esp_http_client_set_header(client, "content-type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "http %s status=%d ret=%s bytes=%d", url, status, esp_err_to_name(ret), buffer.len);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        return ret;
    }
    return status >= 200 && status < 300 ? ESP_OK : ESP_FAIL;
}

static const char *json_string(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_bool_string(cJSON *object, const char *name)
{
    const char *value = json_string(object, name);
    return value && strcmp(value, "true") == 0;
}

static const char *json_first_url(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsString(item)) {
        return item->valuestring;
    }
    if (cJSON_IsArray(item)) {
        cJSON *first = cJSON_GetArrayItem(item, 0);
        if (cJSON_IsString(first)) {
            return first->valuestring;
        }
    }
    return NULL;
}

static void parse_first_ice_server(cJSON *params)
{
    cJSON *pc_config = cJSON_GetObjectItemCaseSensitive(params, "pc_config");
    cJSON *ice_servers = cJSON_GetObjectItemCaseSensitive(pc_config, "iceServers");
    cJSON *first = cJSON_GetArrayItem(ice_servers, 0);
    const char *url = json_first_url(first, "urls");
    if (!url) {
        app.ice_url[0] = '\0';
        ESP_LOGW(TAG, "join response did not include pc_config.iceServers[0].urls");
        return;
    }

    const char *user = json_string(first, "username");
    const char *credential = json_string(first, "credential");
    strlcpy(app.ice_url, url, sizeof(app.ice_url));
    strlcpy(app.ice_user, user ? user : "", sizeof(app.ice_user));
    strlcpy(app.ice_password, credential ? credential : "", sizeof(app.ice_password));
    app.ice_servers[0].stun_url = app.ice_url;
    app.ice_servers[0].user = app.ice_user;
    app.ice_servers[0].psw = app.ice_password;
    ESP_LOGI(TAG, "ice server[0] url=%s user_set=%d credential_len=%u",
             app.ice_url,
             app.ice_user[0] != '\0',
             (unsigned)strlen(app.ice_password));
}

static void send_signaling_json(cJSON *message)
{
    if (!app.ws || !message) {
        return;
    }

    char *payload = cJSON_PrintUnformatted(message);
    if (!payload) {
        ESP_LOGE(TAG, "signaling json allocation failed");
        return;
    }

    int ret = esp_websocket_client_send_text(app.ws, payload, strlen(payload), pdMS_TO_TICKS(2000));
    const char *type = json_string(message, "type");
    ESP_LOGI(TAG, "ws send type=%s bytes=%u ret=%d", type ? type : "unknown", (unsigned)strlen(payload), ret);
    free(payload);
}

static bool bytes_contain(const uint8_t *haystack, size_t haystack_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (!haystack || needle_len == 0 || haystack_len < needle_len) {
        return false;
    }
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static int peer_msg_callback(esp_peer_msg_t *msg, void *ctx)
{
    const char *type = "candidate";
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
#if CONFIG_STACKCHAN_PEER_ROLE_ESP_OFFERER
        type = "offer";
#else
        type = "answer";
#endif
    }
    ESP_LOGI(TAG, "peer on_msg(%s) bytes=%u", type, (unsigned)msg->size);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    cJSON_AddStringToObject(root, "type", type);
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        cJSON_AddStringToObject(root, "sdp", (const char *)msg->data);
    } else {
        cJSON_AddStringToObject(root, "candidate", (const char *)msg->data);
        cJSON_AddStringToObject(root, "id", "0");
        cJSON_AddNumberToObject(root, "label", 0);
    }
    send_signaling_json(root);
    cJSON_Delete(root);
    return 0;
}

static int peer_state_callback(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "peer state=%d", state);
    app.peer_connected = state == ESP_PEER_STATE_CONNECTED ||
                         state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED ||
                         state == ESP_PEER_STATE_DATA_CHANNEL_OPENED;
    if (state == ESP_PEER_STATE_CONNECTED) {
        log_heap("peer-connected");
    }
    return 0;
}

static int peer_audio_info_callback(esp_peer_audio_stream_info_t *info, void *ctx)
{
    ESP_LOGI(TAG, "audio info codec=%d sample_rate=%u channel=%u",
             info->codec,
             (unsigned)info->sample_rate,
             (unsigned)info->channel);
    return 0;
}

static uint32_t isqrt_u64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    while (bit > value) {
        bit >>= 2;
    }
    uint32_t result = 0;
    while (bit) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static int16_t alaw_to_linear16(uint8_t sample)
{
    sample ^= 0x55;
    int16_t value = (sample & 0x0f) << 4;
    uint8_t segment = (sample & 0x70) >> 4;

    switch (segment) {
    case 0:
        value += 8;
        break;
    case 1:
        value += 0x108;
        break;
    default:
        value += 0x108;
        value <<= segment - 1;
        break;
    }
    return (sample & 0x80) ? value : -value;
}

#if CONFIG_STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
static esp_err_t core_s3_i2c_write_reg8(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };
    return i2c_master_transmit(device, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t core_s3_i2c_read_reg8(i2c_master_dev_handle_t device, uint8_t reg, uint8_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(device, &reg, sizeof(reg), value, sizeof(*value), pdMS_TO_TICKS(100));
}

static esp_err_t core_s3_aw88298_write_reg(i2c_master_dev_handle_t amp, uint8_t reg, uint16_t value)
{
    uint8_t data[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xff) };
    return i2c_master_transmit(amp, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t core_s3_speaker_enable_amp(bool enable)
{
    if (app.speaker_amp_enabled == enable) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t amp = NULL;
    i2c_master_dev_handle_t expander = NULL;
    i2c_master_dev_handle_t power = NULL;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CORE_S3_INTERNAL_I2C_SDA_GPIO,
        .scl_io_num = CORE_S3_INTERNAL_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "core-s3 speaker amp i2c bus ret=%s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t amp_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CORE_S3_AW88298_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_device_config_t expander_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CORE_S3_AW9523_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_device_config_t power_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CORE_S3_AXP2101_I2C_ADDR,
        .scl_speed_hz = 400000,
    };

    ret = i2c_master_bus_add_device(bus, &amp_config, &amp);
    if (ret == ESP_OK) {
        ret = i2c_master_bus_add_device(bus, &expander_config, &expander);
    }
    if (ret == ESP_OK) {
        ret = i2c_master_bus_add_device(bus, &power_config, &power);
    }

    esp_err_t power_ret = ESP_OK;
    esp_err_t expander_ret = ESP_ERR_INVALID_STATE;
    esp_err_t amp_ret = ESP_ERR_INVALID_STATE;
    uint8_t reg02 = 0;
    if (ret == ESP_OK) {
        if (enable) {
            uint8_t axp90 = 0;
            power_ret = core_s3_i2c_read_reg8(power, 0x90, &axp90);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x90, 0xbf);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x92, 18 - 5);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x93, 33 - 5);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x94, 33 - 5);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x95, 33 - 5);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x27, 0x00);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x69, 0x11);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x10, 0x30);
            if (power_ret == ESP_OK) power_ret = core_s3_i2c_write_reg8(power, 0x30, 0x0f);
            reg02 = axp90;
        }
        if (enable) {
            expander_ret = core_s3_i2c_write_reg8(expander, 0x02, 0b00000111);
            if (expander_ret == ESP_OK) expander_ret = core_s3_i2c_write_reg8(expander, 0x03, 0b10000011);
            if (expander_ret == ESP_OK) expander_ret = core_s3_i2c_write_reg8(expander, 0x04, 0b00011000);
            if (expander_ret == ESP_OK) expander_ret = core_s3_i2c_write_reg8(expander, 0x05, 0b00001100);
            if (expander_ret == ESP_OK) expander_ret = core_s3_i2c_write_reg8(expander, 0x11, 0b00010000);
            if (expander_ret == ESP_OK) expander_ret = core_s3_i2c_write_reg8(expander, 0x12, 0b11111111);
            if (expander_ret == ESP_OK) expander_ret = core_s3_i2c_write_reg8(expander, 0x13, 0b11111111);
        } else {
            expander_ret = ESP_OK;
        }
        if (enable) {
            if (power_ret == ESP_OK && expander_ret == ESP_OK) {
                amp_ret = core_s3_aw88298_write_reg(amp, 0x05, 0x0008); // RMSE=0 HAGCE=0 HDCCE=0 HMUTE=0
            }
            if (amp_ret == ESP_OK) {
                amp_ret = core_s3_aw88298_write_reg(amp, 0x61, 0x0673); // boost mode disabled
            }
            if (amp_ret == ESP_OK) {
                amp_ret = core_s3_aw88298_write_reg(amp, 0x04, 0x4040); // I2SEN=1 AMPPD=0 PWDN=0
            }
            if (amp_ret == ESP_OK) {
                amp_ret = core_s3_aw88298_write_reg(amp, 0x0c, 0x0664); // Moddable volume=250
            }
            if (amp_ret == ESP_OK) {
                amp_ret = core_s3_aw88298_write_reg(amp, 0x06, 0x14c3); // 8 kHz sample-rate table
            }
        } else {
            amp_ret = core_s3_aw88298_write_reg(amp, 0x04, 0x4000); // I2SEN=0, keep amp/power domains settled
        }
    }

    if (expander) {
        esp_err_t del_ret = i2c_master_bus_rm_device(expander);
        if (del_ret != ESP_OK) {
            ESP_LOGW(TAG, "core-s3 speaker aw9523 remove ret=%s", esp_err_to_name(del_ret));
        }
    }
    if (power) {
        esp_err_t del_ret = i2c_master_bus_rm_device(power);
        if (del_ret != ESP_OK) {
            ESP_LOGW(TAG, "core-s3 speaker axp2101 remove ret=%s", esp_err_to_name(del_ret));
        }
    }
    if (amp) {
        esp_err_t del_ret = i2c_master_bus_rm_device(amp);
        if (del_ret != ESP_OK) {
            ESP_LOGW(TAG, "core-s3 speaker aw88298 remove ret=%s", esp_err_to_name(del_ret));
        }
    }
    esp_err_t bus_del_ret = i2c_del_master_bus(bus);
    if (bus_del_ret != ESP_OK) {
        ESP_LOGW(TAG, "core-s3 speaker amp i2c bus cleanup ret=%s", esp_err_to_name(bus_del_ret));
    }

    esp_err_t effective_ret = ret;
    if (effective_ret == ESP_OK && power_ret != ESP_OK) {
        effective_ret = power_ret;
    }
    if (effective_ret == ESP_OK && expander_ret != ESP_OK) {
        effective_ret = expander_ret;
    }
    if (effective_ret == ESP_OK) {
        app.speaker_amp_enabled = enable;
        if (amp_ret != ESP_OK) {
            ESP_LOGW(TAG, "core-s3 speaker aw88298 register writes did not ACK; continuing like M5Unified after power/expander enable");
        }
    }
    ESP_LOGI(TAG, "core-s3 speaker amp enable=%d ret=%s axp90_before=0x%02x power_ret=%s expander_ret=%s aw88298_ret=%s",
             enable,
             esp_err_to_name(effective_ret),
             reg02,
             esp_err_to_name(power_ret),
             esp_err_to_name(expander_ret),
             esp_err_to_name(amp_ret));
    return effective_ret;
}

static esp_err_t core_s3_speaker_i2s_start(void)
{
    if (app.speaker_tx) {
        return ESP_OK;
    }

    esp_err_t ret = core_s3_speaker_enable_amp(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "core-s3 speaker amp start ret=%s", esp_err_to_name(ret));
        return ret;
    }

    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_config.dma_desc_num = 8;
    chan_config.dma_frame_num = CORE_S3_SPEAKER_MAX_FRAME_SAMPLES;
    chan_config.auto_clear = true;

    ret = i2s_new_channel(&chan_config, &app.speaker_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "core-s3 speaker i2s new channel ret=%s", esp_err_to_name(ret));
        app.speaker_tx = NULL;
        return ret;
    }

    i2s_std_config_t std_config = {0};
    std_config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_config.clk_cfg.sample_rate_hz = CORE_S3_SPEAKER_SAMPLE_RATE;
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_config.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_config.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_config.slot_cfg.ws_pol = false;
    std_config.slot_cfg.bit_shift = true;
    std_config.slot_cfg.left_align = false;
    std_config.slot_cfg.big_endian = false;
    std_config.slot_cfg.bit_order_lsb = false;
    std_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.bclk = CONFIG_STACKCHAN_CORE_S3_SPEAKER_I2S_BCLK_GPIO;
    std_config.gpio_cfg.ws = CONFIG_STACKCHAN_CORE_S3_SPEAKER_I2S_WS_GPIO;
    std_config.gpio_cfg.dout = CONFIG_STACKCHAN_CORE_S3_SPEAKER_I2S_DOUT_GPIO;
    std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.invert_flags.mclk_inv = false;
    std_config.gpio_cfg.invert_flags.bclk_inv = false;
    std_config.gpio_cfg.invert_flags.ws_inv = false;

    ret = i2s_channel_init_std_mode(app.speaker_tx, &std_config);
    if (ret == ESP_OK) {
        ret = i2s_channel_enable(app.speaker_tx);
    }
    ESP_LOGI(TAG, "core-s3 speaker i2s start ret=%s sample_rate=%u channels=%u pins bclk=%d ws=%d dout=%d",
             esp_err_to_name(ret),
             (unsigned)CORE_S3_SPEAKER_SAMPLE_RATE,
             (unsigned)CORE_S3_SPEAKER_CHANNELS,
             CONFIG_STACKCHAN_CORE_S3_SPEAKER_I2S_BCLK_GPIO,
             CONFIG_STACKCHAN_CORE_S3_SPEAKER_I2S_WS_GPIO,
             CONFIG_STACKCHAN_CORE_S3_SPEAKER_I2S_DOUT_GPIO);
    if (ret != ESP_OK) {
        i2s_del_channel(app.speaker_tx);
        app.speaker_tx = NULL;
    }
    log_heap("speaker-start");
    return ret;
}

static void core_s3_speaker_i2s_stop(void)
{
    if (!app.speaker_tx) {
        return;
    }
    esp_err_t disable_ret = i2s_channel_disable(app.speaker_tx);
    if (disable_ret != ESP_OK) {
        ESP_LOGW(TAG, "core-s3 speaker i2s disable ret=%s", esp_err_to_name(disable_ret));
    }
    esp_err_t del_ret = i2s_del_channel(app.speaker_tx);
    if (del_ret != ESP_OK) {
        ESP_LOGW(TAG, "core-s3 speaker i2s delete ret=%s", esp_err_to_name(del_ret));
    }
    app.speaker_tx = NULL;
    esp_err_t amp_ret = core_s3_speaker_enable_amp(false);
    if (amp_ret != ESP_OK) {
        ESP_LOGW(TAG, "core-s3 speaker amp stop ret=%s", esp_err_to_name(amp_ret));
    }
    ESP_LOGI(TAG, "core-s3 speaker i2s stopped writes=%u samples=%u bytes=%u drops=%u short_writes=%u",
             (unsigned)app.audio_rx_speaker_write_frames,
             (unsigned)app.audio_rx_speaker_write_samples,
             (unsigned)app.audio_rx_speaker_write_bytes,
             (unsigned)app.audio_rx_speaker_drops,
             (unsigned)app.audio_rx_speaker_short_writes);
}

static void core_s3_speaker_write_pcm(const int16_t *samples, uint32_t sample_count)
{
    if (!samples || sample_count == 0) {
        return;
    }
    if (sample_count > CORE_S3_SPEAKER_MAX_FRAME_SAMPLES) {
        sample_count = CORE_S3_SPEAKER_MAX_FRAME_SAMPLES;
        app.audio_rx_speaker_drops++;
    }
    esp_err_t ret = core_s3_speaker_i2s_start();
    if (ret != ESP_OK || !app.speaker_tx) {
        app.audio_rx_speaker_drops++;
        app.audio_rx_speaker_last_ret = ret;
        if (app.audio_rx_speaker_drops == 1 || app.audio_rx_speaker_drops % 50 == 0) {
            ESP_LOGW(TAG, "audio rx speaker drop count=%u start_ret=%s",
                     (unsigned)app.audio_rx_speaker_drops,
                     esp_err_to_name(ret));
        }
        return;
    }

    size_t bytes_written = 0;
    size_t write_bytes = sample_count * sizeof(samples[0]);
    ret = i2s_channel_write(app.speaker_tx, samples, write_bytes, &bytes_written, 0);
    app.audio_rx_speaker_last_ret = ret;
    if (ret == ESP_OK && bytes_written > 0) {
        app.audio_rx_speaker_write_frames++;
        app.audio_rx_speaker_write_samples += sample_count;
        app.audio_rx_speaker_write_bytes += bytes_written;
        if (bytes_written < write_bytes) {
            app.audio_rx_speaker_short_writes++;
        }
    } else {
        app.audio_rx_speaker_drops++;
    }

    if (app.audio_rx_speaker_write_frames == 1 || app.audio_rx_speaker_write_frames % 50 == 0 || ret != ESP_OK || bytes_written < write_bytes) {
        ESP_LOGI(TAG, "audio rx speaker writes=%u samples=%u bytes=%u drops=%u short_writes=%u last_ret=%s last_bytes=%u requested_bytes=%u",
                 (unsigned)app.audio_rx_speaker_write_frames,
                 (unsigned)app.audio_rx_speaker_write_samples,
                 (unsigned)app.audio_rx_speaker_write_bytes,
                 (unsigned)app.audio_rx_speaker_drops,
                 (unsigned)app.audio_rx_speaker_short_writes,
                 esp_err_to_name(ret),
                 (unsigned)bytes_written,
                 (unsigned)write_bytes);
    }
}
#endif

static int peer_audio_data_callback(esp_peer_audio_frame_t *frame, void *ctx)
{
    app.audio_rx_frames++;
    uint32_t size = frame->size > 0 ? (uint32_t)frame->size : 0;
    app.audio_rx_bytes += size;
    app.audio_rx_last_size = size;
    uint32_t pts_delta = app.audio_rx_frames > 1 ? frame->pts - app.audio_rx_last_pts : 0;
    app.audio_rx_last_pts = frame->pts;
    app.audio_rx_last_pts_delta = pts_delta;
    bool pts_discontinuity = app.audio_rx_frames > 1 && pts_delta != 20;
    if (pts_discontinuity) {
        app.audio_rx_pts_discontinuities++;
    }

    if (!frame->data || frame->size <= 0) {
        app.audio_rx_empty_frames++;
        if (app.audio_rx_empty_frames == 1 || app.audio_rx_empty_frames % 10 == 0) {
            ESP_LOGW(TAG, "audio rx empty frames=%u total_frames=%u pts=%u size=%d",
                     (unsigned)app.audio_rx_empty_frames,
                     (unsigned)app.audio_rx_frames,
                     (unsigned)frame->pts,
                     frame->size);
        }
        return 0;
    }

    int16_t decoded_min = INT16_MAX;
    int16_t decoded_max = INT16_MIN;
    uint32_t peak = 0;
    uint64_t sum_squares = 0;
#if CONFIG_STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
    int16_t speaker_pcm[CORE_S3_SPEAKER_MAX_FRAME_SAMPLES];
    uint32_t speaker_samples = size < CORE_S3_SPEAKER_MAX_FRAME_SAMPLES ? size : CORE_S3_SPEAKER_MAX_FRAME_SAMPLES;
#endif
    for (int i = 0; i < frame->size; i++) {
        int16_t sample = alaw_to_linear16(frame->data[i]);
#if CONFIG_STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
        if ((uint32_t)i < speaker_samples) {
            speaker_pcm[i] = sample;
        }
#endif
        if (sample < decoded_min) {
            decoded_min = sample;
        }
        if (sample > decoded_max) {
            decoded_max = sample;
        }
        int32_t abs_sample = sample < 0 ? -(int32_t)sample : sample;
        if ((uint32_t)abs_sample > peak) {
            peak = (uint32_t)abs_sample;
        }
        sum_squares += (uint64_t)abs_sample * (uint64_t)abs_sample;
    }

    app.audio_rx_decode_samples += size;
    app.audio_rx_decode_min = decoded_min;
    app.audio_rx_decode_max = decoded_max;
    app.audio_rx_decode_peak = peak;
    app.audio_rx_decode_rms = isqrt_u64(sum_squares / size);

#if CONFIG_STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
    core_s3_speaker_write_pcm(speaker_pcm, speaker_samples);
#endif

    if (app.audio_rx_frames == 1 || app.audio_rx_frames % 50 == 0 || pts_discontinuity) {
        ESP_LOGI(TAG, "audio rx frames=%u bytes=%u empty=%u pts=%u pts_delta=%u pts_discont=%u last_size=%u",
                 (unsigned)app.audio_rx_frames,
                 (unsigned)app.audio_rx_bytes,
                 (unsigned)app.audio_rx_empty_frames,
                 (unsigned)frame->pts,
                 (unsigned)pts_delta,
                 (unsigned)app.audio_rx_pts_discontinuities,
                 (unsigned)app.audio_rx_last_size);
        ESP_LOGI(TAG, "audio rx decode samples=%u invalid=%u rms=%u peak=%u min=%d max=%d",
                 (unsigned)app.audio_rx_decode_samples,
                 (unsigned)app.audio_rx_decode_invalid,
                 (unsigned)app.audio_rx_decode_rms,
                 (unsigned)app.audio_rx_decode_peak,
                 app.audio_rx_decode_min,
                 app.audio_rx_decode_max);
    }
    return 0;
}

static int peer_video_info_callback(esp_peer_video_stream_info_t *info, void *ctx)
{
    ESP_LOGI(TAG, "video info codec=%d width=%d height=%d fps=%d",
             info->codec, info->width, info->height, info->fps);
    return 0;
}

static int peer_video_data_callback(esp_peer_video_frame_t *frame, void *ctx)
{
    ESP_LOGI(TAG, "video rx frame pts=%u bytes=%d", (unsigned)frame->pts, frame->size);
    return 0;
}

static int peer_channel_open_callback(esp_peer_data_channel_info_t *ch, void *ctx)
{
    app.data_stream_id = ch->stream_id;
    ESP_LOGI(TAG, "datachannel open label=%s stream_id=%u",
             ch->label ? ch->label : "(none)",
             (unsigned)ch->stream_id);
    return 0;
}

static int peer_channel_close_callback(esp_peer_data_channel_info_t *ch, void *ctx)
{
    ESP_LOGI(TAG, "datachannel close label=%s stream_id=%u",
             ch->label ? ch->label : "(none)",
             (unsigned)ch->stream_id);
    return 0;
}

static int peer_data_callback(esp_peer_data_frame_t *frame, void *ctx)
{
    app.data_rx_frames++;
    app.data_stream_id = frame->stream_id;
    ESP_LOGI(TAG, "datachannel message stream_id=%u type=%d bytes=%u count=%u",
             (unsigned)frame->stream_id,
             frame->type,
             (unsigned)frame->size,
             (unsigned)app.data_rx_frames);

    int prefix = frame->size < CONFIG_STACKCHAN_LOG_PAYLOAD_PREFIX ? frame->size : CONFIG_STACKCHAN_LOG_PAYLOAD_PREFIX;
    ESP_LOGI(TAG, "datachannel prefix=%.*s", prefix, (const char *)frame->data);

    if (frame->type == ESP_PEER_DATA_CHANNEL_STRING && bytes_contain(frame->data, frame->size, "\"ping\"")) {
        const char pong[] = "{\"type\":\"pong\",\"from\":\"cores3\"}";
        esp_peer_data_frame_t response = {
            .type = ESP_PEER_DATA_CHANNEL_STRING,
            .stream_id = frame->stream_id,
            .data = (uint8_t *)pong,
            .size = strlen(pong),
        };
        int ret = esp_peer_send_data(app.peer, &response);
        ESP_LOGI(TAG, "datachannel pong ret=%d bytes=%u", ret, (unsigned)response.size);
    }
    return 0;
}

static void peer_loop_task(void *arg)
{
    while (app.peer_loop_running && app.peer) {
        esp_peer_main_loop(app.peer);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}


static void log_audio_tx_result(const char *source, uint32_t sent_bytes, uint32_t pts, int ret)
{
    uint32_t pts_delta = app.audio_tx_frames ? pts - app.audio_tx_last_pts : 0;
    app.audio_tx_last_pts = pts;
    app.audio_tx_last_pts_delta = pts_delta;
    app.audio_tx_last_size = sent_bytes;
    if (app.audio_tx_min_size == 0 || sent_bytes < app.audio_tx_min_size) {
        app.audio_tx_min_size = sent_bytes;
    }
    if (sent_bytes > app.audio_tx_max_size) {
        app.audio_tx_max_size = sent_bytes;
    }
    if (ret == 0) {
        app.audio_tx_frames++;
        app.audio_tx_bytes += sent_bytes;
        app.audio_tx_ret_ok++;
    } else {
        app.audio_tx_drops++;
        app.audio_tx_ret_fail++;
    }
    if (app.audio_tx_frames == 1 || app.audio_tx_frames % 50 == 0 || ret != 0) {
        ESP_LOGI(TAG, "%s audio tx frames=%u bytes=%u drops=%u ret_ok=%u ret_fail=%u last_ret=%d pts=%u pts_delta=%u last_size=%u size_min=%u size_max=%u",
                 source,
                 (unsigned)app.audio_tx_frames,
                 (unsigned)app.audio_tx_bytes,
                 (unsigned)app.audio_tx_drops,
                 (unsigned)app.audio_tx_ret_ok,
                 (unsigned)app.audio_tx_ret_fail,
                 ret,
                 (unsigned)pts,
                 (unsigned)pts_delta,
                 (unsigned)app.audio_tx_last_size,
                 (unsigned)app.audio_tx_min_size,
                 (unsigned)app.audio_tx_max_size);
        log_heap(source);
    }
}

static uint8_t linear16_to_alaw(int16_t sample)
{
    const uint16_t segment_end[8] = { 0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff };
    uint16_t pcm;
    uint8_t mask;
    uint8_t segment = 0;
    uint8_t aval;

    if (sample >= 0) {
        pcm = (uint16_t)sample;
        mask = 0xd5;
    } else {
        pcm = (uint16_t)(-sample - 1);
        mask = 0x55;
    }
    pcm >>= 3;


    while (segment < 8 && pcm > segment_end[segment]) {
        segment++;
    }
    if (segment >= 8) {
        return 0x7f ^ mask;
    }


    aval = segment << 4;
    if (segment < 2) {
        aval |= (pcm >> 1) & 0x0f;
    } else {
        aval |= (pcm >> segment) & 0x0f;
    }
    return aval ^ mask;
}

#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE
static void generate_pcma_tone_frame(uint8_t *frame, size_t frame_size)
{
    static uint32_t phase;
    for (size_t i = 0; i < frame_size; i++) {
        int16_t sample = phase < (CORE_S3_MIC_SAMPLE_RATE / 2U) ? AUDIO_TEST_TONE_AMPLITUDE : -AUDIO_TEST_TONE_AMPLITUDE;
        frame[i] = linear16_to_alaw(sample);
        phase += AUDIO_TEST_TONE_HZ;
        while (phase >= CORE_S3_MIC_SAMPLE_RATE) {
            phase -= CORE_S3_MIC_SAMPLE_RATE;
        }
    }
}

static void audio_test_task(void *arg)
{
    uint8_t frame[CONFIG_STACKCHAN_AUDIO_TEST_FRAME_BYTES];

    uint32_t pts = 0;
    ESP_LOGI(TAG, "audio test source start codec=PCMA sample_rate=8000 channel=1 frame_bytes=%u interval_ms=%u tone_hz=%u",

             (unsigned)sizeof(frame),
             (unsigned)CONFIG_STACKCHAN_AUDIO_TEST_INTERVAL_MS,
             (unsigned)AUDIO_TEST_TONE_HZ);
    log_heap("audio-test-start");

    while (app.audio_task_running) {
        if (app.peer && app.peer_connected) {

            generate_pcma_tone_frame(frame, sizeof(frame));

            esp_peer_audio_frame_t audio = {
                .pts = pts,
                .data = frame,
                .size = sizeof(frame),
            };
            int ret = esp_peer_send_audio(app.peer, &audio);
            log_audio_tx_result("audio-test", sizeof(frame), pts, ret);
            pts += CONFIG_STACKCHAN_AUDIO_TEST_INTERVAL_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_STACKCHAN_AUDIO_TEST_INTERVAL_MS));
    }
    vTaskDelete(NULL);
}
#endif

#if CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
typedef struct {
    uint8_t reg;
    uint8_t value;
} es7210_reg_value_t;

static esp_err_t core_s3_mic_write_es7210_reg(i2c_master_dev_handle_t codec, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = i2c_master_transmit(codec, data, sizeof(data), pdMS_TO_TICKS(100));
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "core-s3 mic es7210 write retry reg=0x%02x value=0x%02x attempt=%d ret=%s",
                 reg,
                 value,
                 attempt,
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ret;
}

static esp_err_t core_s3_mic_configure_es7210(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t codec = NULL;
    i2c_device_config_t codec_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CORE_S3_ES7210_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &codec_config, &codec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "core-s3 mic es7210 add device ret=%s", esp_err_to_name(ret));
        return ret;
    }

    static const es7210_reg_value_t init_sequence[] = {
        { 0x00, 0xff }, // RESET_CTL
        { 0x00, 0x41 }, // RESET_CTL
        { 0x01, 0x1f }, // CLK_ON_OFF: enable clocks during setup
        { 0x06, 0x00 }, // DIGITAL_PDN: power up digital blocks
        { 0x07, 0x20 }, // ADC_OSR
        { 0x08, 0x10 }, // MODE_CFG
        { 0x09, 0x30 }, // TCT0_CHPINI
        { 0x0a, 0x30 }, // TCT1_CHPINI
        { 0x20, 0x0a }, // ADC34_HPF2
        { 0x21, 0x2a }, // ADC34_HPF1
        { 0x22, 0x0a }, // ADC12_HPF2
        { 0x23, 0x2a }, // ADC12_HPF1
        { 0x02, 0xc1 },
        { 0x04, 0x01 },
        { 0x05, 0x00 },
        { 0x11, 0x60 },
        { 0x40, 0x42 }, // ANALOG_SYS
        { 0x41, 0x70 }, // MICBIAS12
        { 0x42, 0x70 }, // MICBIAS34
        { 0x43, 0x1b }, // MIC1_GAIN
        { 0x44, 0x1b }, // MIC2_GAIN
        { 0x45, 0x00 }, // MIC3_GAIN
        { 0x46, 0x00 }, // MIC4_GAIN
        { 0x47, 0x00 }, // MIC1_LP
        { 0x48, 0x00 }, // MIC2_LP
        { 0x49, 0x00 }, // MIC3_LP
        { 0x4a, 0x00 }, // MIC4_LP
        { 0x4b, 0x00 }, // MIC12_PDN: enable mic 1/2
        { 0x4c, 0xff }, // MIC34_PDN: power down unused mic 3/4
        { 0x01, 0x14 }, // CLK_ON_OFF: leave ADC clocks on
    };

    for (size_t i = 0; i < sizeof(init_sequence) / sizeof(init_sequence[0]); i++) {
        ret = core_s3_mic_write_es7210_reg(codec, init_sequence[i].reg, init_sequence[i].value);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "core-s3 mic es7210 write reg=0x%02x value=0x%02x ret=%s",
                     init_sequence[i].reg,
                     init_sequence[i].value,
                     esp_err_to_name(ret));
            break;
        }
        if (init_sequence[i].reg == 0x00) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    esp_err_t del_ret = i2c_master_bus_rm_device(codec);
    if (del_ret != ESP_OK) {
        ESP_LOGW(TAG, "core-s3 mic es7210 remove device ret=%s", esp_err_to_name(del_ret));
    }
    ESP_LOGI(TAG, "core-s3 mic es7210 configure ret=%s registers=%u",
             esp_err_to_name(ret),
             (unsigned)(sizeof(init_sequence) / sizeof(init_sequence[0])));
    return ret;
}

static esp_err_t core_s3_mic_probe_and_configure_es7210(void)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_STACKCHAN_CORE_S3_MIC_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_STACKCHAN_CORE_S3_MIC_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, &bus);
    if (ret == ESP_OK) {
        for (int attempt = 1; attempt <= 3; attempt++) {
            ret = i2c_master_probe(bus, CORE_S3_ES7210_I2C_ADDR, pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "core-s3 mic es7210 probe addr=0x%02x sda=%d scl=%d attempt=%d ret=%s",
                     CORE_S3_ES7210_I2C_ADDR,
                     CONFIG_STACKCHAN_CORE_S3_MIC_I2C_SDA_GPIO,
                     CONFIG_STACKCHAN_CORE_S3_MIC_I2C_SCL_GPIO,
                     attempt,
                     esp_err_to_name(ret));
            if (ret == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (ret == ESP_OK) {
        for (int attempt = 1; attempt <= 3; attempt++) {
            ret = core_s3_mic_configure_es7210(bus);
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "core-s3 mic es7210 configure retry attempt=%d ret=%s",
                     attempt,
                     esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (bus) {
        esp_err_t del_ret = i2c_del_master_bus(bus);
        if (del_ret != ESP_OK) {
            ESP_LOGW(TAG, "core-s3 mic i2c bus cleanup ret=%s", esp_err_to_name(del_ret));
        }
    }
    return ret;
}

static esp_err_t core_s3_mic_i2s_init(i2s_chan_handle_t *out_rx)
{
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_config.dma_desc_num = 4;
    chan_config.dma_frame_num = CONFIG_STACKCHAN_CORE_S3_MIC_FRAME_SAMPLES;

    esp_err_t ret = i2s_new_channel(&chan_config, NULL, out_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "core-s3 mic i2s new channel ret=%s", esp_err_to_name(ret));
        return ret;
    }

    i2s_tdm_config_t tdm_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(CORE_S3_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = CONFIG_STACKCHAN_CORE_S3_MIC_I2S_MCLK_GPIO,
            .bclk = CONFIG_STACKCHAN_CORE_S3_MIC_I2S_BCLK_GPIO,
            .ws = CONFIG_STACKCHAN_CORE_S3_MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_STACKCHAN_CORE_S3_MIC_I2S_DIN_GPIO,
        },
    };

    ret = i2s_channel_init_tdm_mode(*out_rx, &tdm_config);
    if (ret == ESP_OK) {
        ret = i2s_channel_enable(*out_rx);
    }
    ESP_LOGI(TAG, "core-s3 mic i2s init ret=%s sample_rate=%u frame_samples=%u pins mclk=%d bclk=%d ws=%d din=%d",
             esp_err_to_name(ret),
             (unsigned)CORE_S3_MIC_SAMPLE_RATE,
             (unsigned)CONFIG_STACKCHAN_CORE_S3_MIC_FRAME_SAMPLES,
             CONFIG_STACKCHAN_CORE_S3_MIC_I2S_MCLK_GPIO,
             CONFIG_STACKCHAN_CORE_S3_MIC_I2S_BCLK_GPIO,
             CONFIG_STACKCHAN_CORE_S3_MIC_I2S_WS_GPIO,
             CONFIG_STACKCHAN_CORE_S3_MIC_I2S_DIN_GPIO);
    return ret;
}

static void audio_mic_task(void *arg)
{
    i2s_chan_handle_t rx = NULL;
    int16_t pcm[CONFIG_STACKCHAN_CORE_S3_MIC_FRAME_SAMPLES * CORE_S3_MIC_TDM_CHANNELS];
    uint8_t pcma[CONFIG_STACKCHAN_CORE_S3_MIC_FRAME_SAMPLES];
    uint32_t log_ticks = 0;
    uint32_t pts = 0;

    ESP_LOGI(TAG, "core-s3 mic source start codec=PCMA/G711A sample_rate=8000 channel=1 capture_rate=%u frame_samples=%u frame_ms=%u",
             (unsigned)CORE_S3_MIC_SAMPLE_RATE,
             (unsigned)CONFIG_STACKCHAN_CORE_S3_MIC_FRAME_SAMPLES,
             (unsigned)CORE_S3_MIC_FRAME_DURATION_MS);
    ESP_LOGW(TAG, "core-s3 mic uses raw I2S bridge because esp_capture is not in this component set; replace with esp_capture audio_dev_src/sink when that dependency is adopted");
    log_heap("mic-start");
    esp_err_t codec_ret = core_s3_mic_probe_and_configure_es7210();
    if (codec_ret != ESP_OK) {
        ESP_LOGE(TAG, "core-s3 mic source stopped: es7210 probe/config failed ret=%s", esp_err_to_name(codec_ret));
        app.audio_task_running = false;
    }
    if (app.audio_task_running && core_s3_mic_i2s_init(&rx) != ESP_OK) {
        ESP_LOGE(TAG, "core-s3 mic source stopped: i2s init failed");
        app.audio_task_running = false;
    }

    while (app.audio_task_running && rx) {
        size_t bytes_read = 0;
        esp_err_t read_ret = i2s_channel_read(rx, pcm, sizeof(pcm), &bytes_read, pdMS_TO_TICKS(200));
        if (read_ret != ESP_OK) {
            app.mic_read_failures++;
            if (app.mic_read_failures == 1 || app.mic_read_failures % 50 == 0) {
                ESP_LOGW(TAG, "core-s3 mic read failure count=%u ret=%s",
                         (unsigned)app.mic_read_failures,
                         esp_err_to_name(read_ret));
            }
            continue;
        }
        if (bytes_read < sizeof(pcm)) {
            app.mic_short_reads++;
        }

        size_t samples_read = bytes_read / sizeof(pcm[0]);
        size_t frames_read = samples_read / CORE_S3_MIC_TDM_CHANNELS;
        if (frames_read > CONFIG_STACKCHAN_CORE_S3_MIC_FRAME_SAMPLES) {
            frames_read = CONFIG_STACKCHAN_CORE_S3_MIC_FRAME_SAMPLES;
        }
        app.mic_acquire_frames++;

        uint32_t peak = 0;
        uint64_t sum_squares = 0;
        for (size_t i = 0; i < frames_read; i++) {
            int16_t sample = pcm[i * CORE_S3_MIC_TDM_CHANNELS];
            int32_t abs_sample = sample < 0 ? -(int32_t)sample : sample;
            if (abs_sample > peak) {
                peak = abs_sample;
            }
            if (abs_sample >= 32767) {
                app.mic_conversion_clips++;
            }
            sum_squares += (uint64_t)abs_sample * (uint64_t)abs_sample;
            pcma[i] = linear16_to_alaw(sample);
        }
        app.mic_samples += frames_read;

        uint32_t rms = frames_read ? isqrt_u64(sum_squares / frames_read) : 0;
        log_ticks++;
        if (log_ticks == 1 || log_ticks % 50 == 0 || app.mic_short_reads) {
            ESP_LOGI(TAG, "core-s3 mic acquire=%u samples=%u rms=%u peak=%u read_failures=%u short_reads=%u clips=%u bytes_read=%u frame_samples=%u pts=%u",
                     (unsigned)app.mic_acquire_frames,
                     (unsigned)app.mic_samples,
                     (unsigned)rms,
                     (unsigned)peak,
                     (unsigned)app.mic_read_failures,
                     (unsigned)app.mic_short_reads,
                     (unsigned)app.mic_conversion_clips,
                     (unsigned)bytes_read,
                     (unsigned)frames_read,
                     (unsigned)pts);
        }

        if (frames_read > 0 && app.peer && app.peer_connected) {
            esp_peer_audio_frame_t audio = {
                .pts = pts,
                .data = pcma,
                .size = frames_read,
            };
            int ret = esp_peer_send_audio(app.peer, &audio);
            log_audio_tx_result("core-s3-mic", frames_read, pts, ret);
        }
        pts += (uint32_t)((frames_read * 1000U) / CORE_S3_MIC_SAMPLE_RATE);
    }
    if (rx) {
        esp_err_t disable_ret = i2s_channel_disable(rx);
        if (disable_ret != ESP_OK) {
            ESP_LOGW(TAG, "core-s3 mic i2s disable ret=%s", esp_err_to_name(disable_ret));
        }
        esp_err_t del_ret = i2s_del_channel(rx);
        if (del_ret != ESP_OK) {
            ESP_LOGW(TAG, "core-s3 mic i2s delete ret=%s", esp_err_to_name(del_ret));
        }
    }
    vTaskDelete(NULL);
}
#endif

static esp_peer_media_dir_t configured_audio_dir(void)
{
#if CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
    return ESP_PEER_MEDIA_DIR_SEND_RECV;
#elif CONFIG_STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK
    return ESP_PEER_MEDIA_DIR_RECV_ONLY;
#elif CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE
    return ESP_PEER_MEDIA_DIR_SEND_ONLY;
#else
    return ESP_PEER_MEDIA_DIR_NONE;
#endif
}

static const char *configured_media_mode(void)
{
#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE
    return "audio-test-source";
#elif CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
    return "core-s3-mic-duplex";
#elif CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE
    return "core-s3-mic-source";
#elif CONFIG_STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK
    return "core-s3-speaker-sink";
#elif CONFIG_STACKCHAN_MEDIA_VIDEO_PLACEHOLDER
    return "video-placeholder";
#else
    return "none";
#endif
}

static esp_peer_role_t configured_peer_role(void)
{
#if CONFIG_STACKCHAN_PEER_ROLE_ESP_OFFERER
    return ESP_PEER_ROLE_CONTROLLING;
#else
    return ESP_PEER_ROLE_CONTROLLED;
#endif
}

static const char *configured_peer_role_name(void)
{
#if CONFIG_STACKCHAN_PEER_ROLE_ESP_OFFERER
    return "esp-offerer";
#else
    return "browser-offerer";
#endif
}

static void start_local_offer(void);

static void close_peer_for_reoffer(void)
{
    if (!app.peer) {
        app.local_offer_started = false;
        app.peer_connected = false;
        return;
    }

    ESP_LOGI(TAG, "closing stale peer before reoffer");
#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
    app.audio_task_running = false;
#endif
#if CONFIG_STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
    core_s3_speaker_i2s_stop();
#endif
    app.peer_loop_running = false;
    vTaskDelay(pdMS_TO_TICKS(80));
    int ret = esp_peer_close(app.peer);
    ESP_LOGI(TAG, "esp_peer_close ret=%d", ret);
    app.peer = NULL;
    app.peer_connected = false;
    app.local_offer_started = false;
    app.data_stream_id = 0;
    log_heap("after-peer-close");
}

static void handle_reoffer_request(void)
{
#if CONFIG_STACKCHAN_PEER_ROLE_ESP_OFFERER
    ESP_LOGI(TAG, "signaling reoffer-request: recreate peer and publish fresh offer");
    close_peer_for_reoffer();
    start_local_offer();
#else
    ESP_LOGW(TAG, "ignore reoffer-request while configured as browser-offerer answerer");
#endif
}

static esp_err_t ensure_peer_open(void)
{
    if (app.peer) {
        return ESP_OK;
    }

    log_heap("before-peer-open");
    ESP_LOGI(TAG, "peer role=%s media mode=%s audio_dir=%d video_dir=%d",
             configured_peer_role_name(),
             configured_media_mode(),
             configured_audio_dir(),
             ESP_PEER_MEDIA_DIR_NONE);
#if CONFIG_STACKCHAN_MEDIA_VIDEO_PLACEHOLDER
    ESP_LOGW(TAG, "video media mode is a placeholder; no camera/codec source is wired in this slice");
#endif

    peer_default_cfg = (esp_peer_default_cfg_t) {
        .agent_recv_timeout = CONFIG_STACKCHAN_AGENT_RECV_TIMEOUT_MS,
        .data_ch_cfg = {
            .recv_cache_size = 1536,
            .send_cache_size = 1536,
        },
        .rtp_cfg = {
            .audio_recv_jitter = {
                .cache_size = 2048,
            },
            .send_pool_size = 8192,
            .send_queue_num = 16,
        },
    };

    esp_peer_cfg_t cfg = {
        .server_lists = app.ice_url[0] ? app.ice_servers : NULL,
        .server_num = app.ice_url[0] ? 1 : 0,
        .role = configured_peer_role(),
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_info = {
            .codec = ESP_PEER_AUDIO_CODEC_G711A,
            .sample_rate = 8000,
            .channel = 1,
        },
        .audio_dir = configured_audio_dir(),
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = true,
        .extra_cfg = &peer_default_cfg,
        .extra_size = sizeof(peer_default_cfg),
        .ctx = &app,
        .on_state = peer_state_callback,
        .on_msg = peer_msg_callback,
        .on_audio_info = peer_audio_info_callback,
        .on_audio_data = peer_audio_data_callback,
        .on_video_info = peer_video_info_callback,
        .on_video_data = peer_video_data_callback,
        .on_channel_open = peer_channel_open_callback,
        .on_data = peer_data_callback,
        .on_channel_close = peer_channel_close_callback,
    };

    ESP_LOGI(TAG, "peer agent_recv_timeout_ms=%u", (unsigned)CONFIG_STACKCHAN_AGENT_RECV_TIMEOUT_MS);
    ESP_LOGI(TAG, "peer ICE server_num=%u first_url=%s",
             (unsigned)cfg.server_num,
             cfg.server_num ? app.ice_url : "(none)");

    int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &app.peer);
    ESP_LOGI(TAG, "esp_peer_open ret=%d peer=%p", ret, app.peer);
    log_heap("after-peer-open");
    if (ret != 0) {
        return ESP_FAIL;
    }

    app.peer_loop_running = true;
    if (xTaskCreate(peer_loop_task, "peer_loop", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start peer loop task");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_peer_new_connection(app.peer);
    ESP_LOGI(TAG, "esp_peer_new_connection ret=%d", ret);
    if (ret != 0) {
        return ESP_FAIL;
    }
#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
    app.audio_task_running = true;
#endif
#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE
    if (xTaskCreate(audio_test_task, "audio_test", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start audio test task");
        app.audio_task_running = false;
        return ESP_ERR_NO_MEM;
    }
#elif CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE || CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX
    if (xTaskCreate(audio_mic_task, "audio_mic", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start CoreS3 mic task");
        app.audio_task_running = false;
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

static void start_local_offer(void)
{
#if CONFIG_STACKCHAN_PEER_ROLE_ESP_OFFERER
    if (app.local_offer_started) {
        return;
    }
    if (ensure_peer_open() != ESP_OK) {
        return;
    }
    app.local_offer_started = true;
    log_heap("before-esp_peer_new_connection");
    int ret = esp_peer_new_connection(app.peer);
    ESP_LOGI(TAG, "esp_peer_new_connection ret=%d", ret);
    log_heap("after-esp_peer_new_connection");
#endif
}

static void forward_to_peer(const char *type, const char *data, size_t size)
{
    if (!type || !data || size == 0) {
        ESP_LOGW(TAG, "drop empty signaling message");
        return;
    }
    if (ensure_peer_open() != ESP_OK) {
        return;
    }
    if ((strcmp(type, "offer") == 0 && configured_peer_role() == ESP_PEER_ROLE_CONTROLLING) ||
        (strcmp(type, "answer") == 0 && configured_peer_role() == ESP_PEER_ROLE_CONTROLLED)) {
        ESP_LOGW(TAG, "signaling SDP type=%s does not match configured peer role=%s",
                 type,
                 configured_peer_role_name());
    }

    esp_peer_msg_t msg = {
        .type = strcmp(type, "candidate") == 0 ? ESP_PEER_MSG_TYPE_CANDIDATE : ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)data,
        .size = size,
    };
    log_heap("before-esp_peer_send_msg");
    int ret = esp_peer_send_msg(app.peer, &msg);
    ESP_LOGI(TAG, "esp_peer_send_msg type=%s bytes=%u ret=%d", type, (unsigned)size, ret);
    log_heap("after-esp_peer_send_msg");
}

static void handle_signaling_payload(const char *payload, int len)
{
    ESP_LOGI(TAG, "ws recv bytes=%d prefix=%.*s",
             len,
             len < CONFIG_STACKCHAN_LOG_PAYLOAD_PREFIX ? len : CONFIG_STACKCHAN_LOG_PAYLOAD_PREFIX,
             payload);

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "invalid signaling json");
        return;
    }

    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!cJSON_IsObject(message)) {
        message = root;
    }

    const char *type = json_string(message, "type");
    ESP_LOGI(TAG, "signaling message type=%s size=%d", type ? type : "unknown", len);

    if (type && (strcmp(type, "offer") == 0 || strcmp(type, "answer") == 0)) {
        const char *sdp = json_string(message, "sdp");
        forward_to_peer(type, sdp, sdp ? strlen(sdp) + 1 : 0);
    } else if (type && strcmp(type, "candidate") == 0) {
        const char *candidate = json_string(message, "candidate");
        forward_to_peer(type, candidate, candidate ? strlen(candidate) + 1 : 0);
    } else if (type && strcmp(type, "reoffer-request") == 0) {
        handle_reoffer_request();
    }

    cJSON_Delete(root);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket open");
        start_local_offer();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket close");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x1 && data->data_ptr && data->data_len > 0) {
            handle_signaling_payload(data->data_ptr, data->data_len);
        } else {
            ESP_LOGI(TAG, "websocket frame opcode=%d len=%d", data->op_code, data->data_len);
        }
        break;
    default:
        break;
    }
}

static esp_err_t join_room(void)
{
    char url[URL_BUF_SIZE];
    char *response = malloc(HTTP_BUF_SIZE);
    if (!response) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = format_into(url, sizeof(url), "%s/join/%s", app.base_url, app.room);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "join url too long");
        free(response);
        return ret;
    }
    ret = http_json(url, HTTP_METHOD_POST, "{}", response, HTTP_BUF_SIZE);
    if (ret != ESP_OK) {
        free(response);
        return ret;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const char *client_id = json_string(params, "client_id");
    const char *wss_url = json_string(params, "wss_url");
    if (!client_id || !wss_url) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(app.client_id, client_id, sizeof(app.client_id));
    strlcpy(app.wss_url, wss_url, sizeof(app.wss_url));
    app.is_initiator = json_bool_string(params, "is_initiator");
    parse_first_ice_server(params);
    ESP_LOGI(TAG, "join room=%s client_id=%s initiator=%d wss_url=%s",
             app.room, app.client_id, app.is_initiator, app.wss_url);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ping_signaling(void)
{
    char url[URL_BUF_SIZE];
    char response[256];
    ESP_RETURN_ON_ERROR(format_into(url, sizeof(url), "%s/ping", app.base_url), TAG, "ping url too long");
    return http_json(url, HTTP_METHOD_GET, NULL, response, sizeof(response));
}

static esp_err_t start_websocket(void)
{
    ESP_RETURN_ON_ERROR(format_into(app.ws_url, sizeof(app.ws_url), "%s?roomId=%s&clientId=%s", app.wss_url, app.room, app.client_id), TAG, "websocket url too long");
    esp_websocket_client_config_t config = {
        .uri = app.ws_url,
        .network_timeout_ms = 8000,
        .buffer_size = HTTP_BUF_SIZE,
        .task_stack = 12288,
    };
    app.ws = esp_websocket_client_init(&config);
    if (!app.ws) {
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(app.ws, WEBSOCKET_EVENT_ANY, websocket_event_handler, &app);
    esp_err_t ret = esp_websocket_client_start(app.ws);
    ESP_LOGI(TAG, "websocket start url=%s ret=%s", app.ws_url, esp_err_to_name(ret));
    return ret;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count++ < 8) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "wifi retry=%d", wifi_retry_count);
        } else {
            xEventGroupSetBits(wifi_events, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        wifi_retry_count = 0;
        ESP_LOGI(TAG, "wifi connected ip=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t connect_wifi(void)
{
    wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(wifi_events, ESP_ERR_NO_MEM, TAG, "wifi event group allocation failed");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_STACKCHAN_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_STACKCHAN_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_LOGI(TAG, "wifi connecting ssid=%s", CONFIG_STACKCHAN_WIFI_SSID[0] ? CONFIG_STACKCHAN_WIFI_SSID : "(empty)");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

void app_main(void)
{
    strlcpy(app.base_url, CONFIG_STACKCHAN_SIGNAL_BASE_URL, sizeof(app.base_url));
    strlcpy(app.room, CONFIG_STACKCHAN_SIGNAL_ROOM, sizeof(app.room));
    log_heap("boot");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(connect_wifi());
    ESP_ERROR_CHECK(ping_signaling());
    ESP_ERROR_CHECK(join_room());
    ESP_ERROR_CHECK(start_websocket());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
