#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
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
#define AUDIO_TEST_SAMPLE_RATE_HZ 8000
#define AUDIO_TEST_TONE_HZ 440
#define AUDIO_TEST_TONE_AMPLITUDE 12000

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
    bool is_initiator;
    esp_websocket_client_handle_t ws;
    esp_peer_handle_t peer;
    bool peer_loop_running;
    bool peer_connected;
    bool local_offer_started;
    bool audio_task_running;
    uint32_t audio_tx_frames;
    uint32_t audio_tx_bytes;
    uint32_t audio_tx_drops;
    uint32_t audio_rx_frames;
    uint32_t audio_rx_bytes;
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

static int peer_audio_data_callback(esp_peer_audio_frame_t *frame, void *ctx)
{
    app.audio_rx_frames++;
    app.audio_rx_bytes += frame->size;
    if (app.audio_rx_frames == 1 || app.audio_rx_frames % 50 == 0) {
        ESP_LOGI(TAG, "audio rx frames=%u bytes=%u pts=%u last_size=%d",
                 (unsigned)app.audio_rx_frames,
                 (unsigned)app.audio_rx_bytes,
                 (unsigned)frame->pts,
                 frame->size);
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

static uint8_t linear16_to_alaw(int16_t sample)
{
    static const int16_t segment_end[] = {0x001f, 0x003f, 0x007f, 0x00ff, 0x01ff, 0x03ff, 0x07ff, 0x0fff};
    int16_t pcm = sample >> 3;
    uint8_t mask;

    if (pcm >= 0) {
        mask = 0xd5;
    } else {
        mask = 0x55;
        pcm = -pcm - 1;
    }

    int segment = 0;
    while (segment < 8 && pcm > segment_end[segment]) {
        segment++;
    }
    if (segment >= 8) {
        return 0x7f ^ mask;
    }

    uint8_t encoded = (uint8_t)(segment << 4);
    if (segment < 2) {
        encoded |= (pcm >> 1) & 0x0f;
    } else {
        encoded |= (pcm >> segment) & 0x0f;
    }
    return encoded ^ mask;
}

static void generate_pcma_tone_frame(uint8_t *frame, size_t frame_size)
{
    static uint32_t phase;
    for (size_t i = 0; i < frame_size; i++) {
        int16_t sample = phase < (AUDIO_TEST_SAMPLE_RATE_HZ / 2) ? AUDIO_TEST_TONE_AMPLITUDE : -AUDIO_TEST_TONE_AMPLITUDE;
        frame[i] = linear16_to_alaw(sample);
        phase += AUDIO_TEST_TONE_HZ;
        if (phase >= AUDIO_TEST_SAMPLE_RATE_HZ) {
            phase -= AUDIO_TEST_SAMPLE_RATE_HZ;
        }
    }
}

static void audio_test_task(void *arg)
{
#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE
    uint8_t frame[CONFIG_STACKCHAN_AUDIO_TEST_FRAME_BYTES];
    ESP_LOGI(TAG, "audio test source start codec=PCMA sample_rate=%u channel=1 tone_hz=%u frame_bytes=%u interval_ms=%u",
             (unsigned)AUDIO_TEST_SAMPLE_RATE_HZ,
             (unsigned)AUDIO_TEST_TONE_HZ,
             (unsigned)sizeof(frame),
             (unsigned)CONFIG_STACKCHAN_AUDIO_TEST_INTERVAL_MS);
    log_heap("audio-test-start");

    while (app.audio_task_running) {
        if (app.peer && app.peer_connected) {
            generate_pcma_tone_frame(frame, sizeof(frame));
            esp_peer_audio_frame_t audio = {
                .pts = (uint32_t)(esp_timer_get_time() / 1000),
                .data = frame,
                .size = sizeof(frame),
            };
            int ret = esp_peer_send_audio(app.peer, &audio);
            if (ret == 0) {
                app.audio_tx_frames++;
                app.audio_tx_bytes += sizeof(frame);
            } else {
                app.audio_tx_drops++;
            }
            if (app.audio_tx_frames == 1 || app.audio_tx_frames % 50 == 0 || ret != 0) {
                ESP_LOGI(TAG, "audio tx frames=%u bytes=%u drops=%u last_ret=%d",
                         (unsigned)app.audio_tx_frames,
                         (unsigned)app.audio_tx_bytes,
                         (unsigned)app.audio_tx_drops,
                         ret);
                log_heap("audio-test-running");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_STACKCHAN_AUDIO_TEST_INTERVAL_MS));
    }
#endif
    vTaskDelete(NULL);
}

static esp_peer_media_dir_t configured_audio_dir(void)
{
#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE
    return ESP_PEER_MEDIA_DIR_SEND_ONLY;
#else
    return ESP_PEER_MEDIA_DIR_NONE;
#endif
}

static const char *configured_media_mode(void)
{
#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE
    return "audio-test-source";
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
        .agent_recv_timeout = 100,
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
#if CONFIG_STACKCHAN_MEDIA_AUDIO_TEST_SOURCE
    app.audio_task_running = true;
    if (xTaskCreate(audio_test_task, "audio_test", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start audio test task");
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
