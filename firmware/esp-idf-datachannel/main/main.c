#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

static const char *TAG = "stackchan_dc";
static EventGroupHandle_t wifi_events;
static int wifi_retry_count;

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
} app_ctx_t;

static app_ctx_t app;

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
    const char *type = msg->type == ESP_PEER_MSG_TYPE_SDP ? "answer" : "candidate";
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
    return 0;
}

static int peer_data_callback(esp_peer_data_frame_t *frame, void *ctx)
{
    ESP_LOGI(TAG, "datachannel message label=%s type=%d bytes=%u",
             frame->label ? frame->label : "(none)",
             frame->type,
             (unsigned)frame->size);

    int prefix = frame->size < CONFIG_STACKCHAN_LOG_PAYLOAD_PREFIX ? frame->size : CONFIG_STACKCHAN_LOG_PAYLOAD_PREFIX;
    ESP_LOGI(TAG, "datachannel prefix=%.*s", prefix, (const char *)frame->data);

    if (frame->type == ESP_PEER_DATA_CHANNEL_STRING && bytes_contain(frame->data, frame->size, "\"ping\"")) {
        const char pong[] = "{\"type\":\"pong\",\"from\":\"cores3\"}";
        esp_peer_data_frame_t response = {
            .type = ESP_PEER_DATA_CHANNEL_STRING,
            .data = (uint8_t *)pong,
            .size = strlen(pong),
            .label = frame->label,
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

static esp_err_t ensure_peer_open(void)
{
    if (app.peer) {
        return ESP_OK;
    }

    log_heap("before-peer-open");
    esp_peer_cfg_t cfg = {
        .role = ESP_PEER_ROLE_CONTROLLED,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_dir = ESP_PEER_MEDIA_DIR_NONE,
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = true,
        .ctx = &app,
        .on_state = peer_state_callback,
        .on_msg = peer_msg_callback,
        .on_data = peer_data_callback,
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
    return ESP_OK;
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

    esp_peer_msg_t msg = {
        .type = strcmp(type, "candidate") == 0 ? ESP_PEER_MSG_TYPE_ICE : ESP_PEER_MSG_TYPE_SDP,
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
    char response[HTTP_BUF_SIZE];
    snprintf(url, sizeof(url), "%s/join/%s", app.base_url, app.room);
    esp_err_t ret = http_json(url, HTTP_METHOD_POST, "{}", response, sizeof(response));
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *root = cJSON_Parse(response);
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
    snprintf(url, sizeof(url), "%s/ping", app.base_url);
    return http_json(url, HTTP_METHOD_GET, NULL, response, sizeof(response));
}

static esp_err_t start_websocket(void)
{
    snprintf(app.ws_url, sizeof(app.ws_url), "%s?roomId=%s&clientId=%s", app.wss_url, app.room, app.client_id);
    esp_websocket_client_config_t config = {
        .uri = app.ws_url,
        .network_timeout_ms = 8000,
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
