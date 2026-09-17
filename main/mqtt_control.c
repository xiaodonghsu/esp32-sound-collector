#include "mqtt_control.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "app_config.h"
#include "audio_stream.h"
#include "device_identity.h"
#include "wifi_manager.h"
#include "xvf3800.h"

#define MQTT_COMMAND_MAX 2048
#define MQTT_MID_MAX     128
#define MQTT_CONNECTED_BIT BIT0
#define MQTT_RETRY_BIT BIT1

static const char *TAG = "mqtt_control";
static esp_mqtt_client_handle_t s_client;
static bool s_client_started;
static EventGroupHandle_t s_connection_events;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_commands;
static char s_topic_in[64];
static char s_topic_out[64];

typedef struct { char *payload; } command_item_t;
typedef struct { char mid[MQTT_MID_MAX]; } start_pending_t;

typedef struct {
    uint32_t segment_ms;
    uint32_t sample_rate_khz;
    uint32_t bit_rate;
    uint32_t channels;
} audio_url_params_t;

static void publish_json(cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    /* Queue without blocking the audio task on network I/O. The bounded SDK
     * outbox is retained while reconnection is paused for unavailable Wi-Fi. */
    if (client && esp_mqtt_client_enqueue(client, s_topic_out, text, 0, 1, 0, true) < 0) {
        ESP_LOGW(TAG, "response could not be queued (outbox full or allocation failed)");
    }
    xSemaphoreGive(s_lock);
    free(text);
}

static void publish_result(const char *mid, bool success, const char *message)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mid", mid ? mid : "");
    cJSON_AddStringToObject(j, "result", success ? "success" : "failed");
    if (message && message[0]) cJSON_AddStringToObject(j, "message", message);
    publish_json(j);
}

static void stream_started(bool success, const char *detail, void *ctx)
{
    start_pending_t *pending = ctx;
    publish_result(pending ? pending->mid : "", success, success ? NULL : detail);
    free(pending);
}

static const char *get_string(cJSON *j, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(j, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool parse_uint_param(const char *value, size_t length, uint32_t *result)
{
    if (!value || !length || length >= 16) return false;
    char text[16];
    memcpy(text, value, length);
    text[length] = '\0';
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || parsed > UINT32_MAX) return false;
    *result = (uint32_t)parsed;
    return true;
}

static bool query_name_equals(const char *name, size_t length, const char *expected)
{
    return strlen(expected) == length && !strncmp(name, expected, length);
}

static const char *parse_audio_url_params(const char *url, audio_url_params_t *params)
{
    *params = (audio_url_params_t) {
        .segment_ms = 200,
        .sample_rate_khz = 16,
        .bit_rate = 16,
        .channels = 1,
    };

    const char *query = strchr(url, '?');
    if (!query) return NULL;
    for (query++; *query && *query != '#';) {
        const char *entry_end = strpbrk(query, "&#");
        if (!entry_end) entry_end = query + strlen(query);
        const char *equals = memchr(query, '=', entry_end - query);
        if (equals) {
            uint32_t *target = NULL;
            size_t name_length = equals - query;
            if (query_name_equals(query, name_length, "segment")) target = &params->segment_ms;
            else if (query_name_equals(query, name_length, "samplerate")) target = &params->sample_rate_khz;
            else if (query_name_equals(query, name_length, "bitrate")) target = &params->bit_rate;
            else if (query_name_equals(query, name_length, "channel")) target = &params->channels;
            if (target && !parse_uint_param(equals + 1, entry_end - equals - 1, target)) {
                return "invalid audio parameter in url";
            }
        }
        query = *entry_end == '&' ? entry_end + 1 : entry_end;
    }

    if (params->segment_ms < 20 || params->segment_ms > 2000) return "segment must be 20-2000 ms";
    if (params->sample_rate_khz != 16) return "only samplerate=16 is supported";
    if (params->bit_rate != 16) return "only bitrate=16 is supported";
    if (params->channels != 1) return "only channel=1 is supported";
    return NULL;
}

static void handle_start(cJSON *root, const char *mid)
{
    const char *url = get_string(root, "url");
    if (!url) { publish_result(mid, false, "url is required"); return; }
    audio_url_params_t params;
    const char *validation_error = parse_audio_url_params(url, &params);
    if (validation_error) { publish_result(mid, false, validation_error); return; }
    start_pending_t *pending = calloc(1, sizeof(*pending));
    if (!pending) { publish_result(mid, false, "out of memory"); return; }
    strlcpy(pending->mid, mid, sizeof(pending->mid));
    esp_err_t err = audio_stream_start(url, params.segment_ms, stream_started, pending);
    if (err != ESP_OK) {
        free(pending);
        publish_result(mid, false, esp_err_to_name(err));
    }
}

static void handle_status(const char *mid)
{
    xvf3800_status_t xvf;
    audio_stream_status_t stream;
    xvf3800_get_status(&xvf);
    audio_stream_get_status(&stream);
    char ssid[APP_SSID_MAX_LEN + 1], ip[16];
    wifi_manager_ssid(ssid, sizeof(ssid));
    wifi_manager_ip(ip, sizeof(ip));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mid", mid);
    cJSON_AddStringToObject(j, "result", "success");
    cJSON_AddStringToObject(j, "wifi-ssid", ssid);
    cJSON_AddNumberToObject(j, "wifi-rssi", wifi_manager_rssi());
    cJSON_AddStringToObject(j, "wifi-mac", device_identity_id());
    cJSON_AddStringToObject(j, "wifi-ipaddr", ip);
    cJSON_AddStringToObject(j, "recorder-type", "XVF3800");
    cJSON_AddStringToObject(j, "recorder-version", xvf.version_valid ? xvf.version : "");
    cJSON_AddBoolToObject(j, "recording", stream.recording);
    cJSON_AddNumberToObject(j, "duration", stream.duration);
    cJSON_AddNumberToObject(j, "segments", stream.segments);
    if (xvf.led_effect_valid) cJSON_AddNumberToObject(j, "led-effict", xvf.led_effect);
    else cJSON_AddStringToObject(j, "led-effict", "");
    if (xvf.led_brightness_valid) cJSON_AddNumberToObject(j, "led-brightness", xvf.led_brightness);
    else cJSON_AddStringToObject(j, "led-brightness", "");
    if (xvf.led_speed_valid) cJSON_AddNumberToObject(j, "led-speed", xvf.led_speed);
    else cJSON_AddStringToObject(j, "led-speed", "");
    if (xvf.led_color_valid) cJSON_AddNumberToObject(j, "led-color", xvf.led_color);
    else cJSON_AddStringToObject(j, "led-color", "");
    publish_json(j);
}

static void handle_set(cJSON *root, const char *mid)
{
    cJSON *parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
    if (!cJSON_IsArray(parameters)) { publish_result(mid, false, "parameters must be an array"); return; }
    app_config_t cfg;
    app_config_get(&cfg);
    char new_ssid[APP_SSID_MAX_LEN + 1], new_password[APP_WIFI_PASS_MAX_LEN + 1];
    strlcpy(new_ssid, cfg.wifi_ssid, sizeof(new_ssid));
    strlcpy(new_password, cfg.wifi_password, sizeof(new_password));
    bool wifi_changed = false;
    cJSON *entry;
    cJSON_ArrayForEach(entry, parameters) {
        const char *name = get_string(entry, "para");
        cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "value");
        if (!name || !value) { publish_result(mid, false, "invalid parameter entry"); return; }
        if (!strcasecmp(name, "wifi-ssid") && cJSON_IsString(value)) {
            if (strlen(value->valuestring) > APP_SSID_MAX_LEN) { publish_result(mid, false, "wifi-ssid is too long"); return; }
            strlcpy(new_ssid, value->valuestring, sizeof(new_ssid)); wifi_changed = true;
        } else if (!strcasecmp(name, "wifi-password") && cJSON_IsString(value)) {
            if (strlen(value->valuestring) > APP_WIFI_PASS_MAX_LEN) { publish_result(mid, false, "wifi-password is too long"); return; }
            strlcpy(new_password, value->valuestring, sizeof(new_password)); wifi_changed = true;
        } else if (cJSON_IsNumber(value)) {
            if (value->valuedouble < 0 || value->valuedouble > UINT32_MAX) {
                publish_result(mid, false, "numeric parameter is out of range"); return;
            }
            esp_err_t err = xvf3800_set_parameter(name, (uint32_t)value->valuedouble);
            if (err != ESP_OK) { publish_result(mid, false, esp_err_to_name(err)); return; }
        } else { publish_result(mid, false, "unsupported parameter or value type"); return; }
    }
    if (wifi_changed) {
        esp_err_t err = wifi_manager_test_and_save(new_ssid, new_password, 20000);
        if (err != ESP_OK) { publish_result(mid, false, "Wi-Fi validation failed"); return; }
    }
    publish_result(mid, true, NULL);
}

static void handle_command(char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) { publish_result("", false, "invalid JSON"); return; }
    const char *mid = get_string(root, "mid");
    const char *cmd = get_string(root, "cmd");
    if (!mid || !mid[0] || strlen(mid) >= MQTT_MID_MAX || !cmd) {
        publish_result(mid, false, "mid and cmd are required");
        cJSON_Delete(root);
        return;
    }
    if (!strcmp(cmd, "start")) handle_start(root, mid);
    else if (!strcmp(cmd, "stop")) {
        esp_err_t err = audio_stream_stop();
        publish_result(mid, err == ESP_OK, err == ESP_OK ? NULL : esp_err_to_name(err));
    } else if (!strcmp(cmd, "status")) handle_status(mid);
    else if (!strcmp(cmd, "set")) handle_set(root, mid);
    else publish_result(mid, false, "unknown cmd");
    cJSON_Delete(root);
}

static void command_task(void *arg)
{
    (void)arg;
    command_item_t item;
    while (true) {
        if (xQueueReceive(s_commands, &item, portMAX_DELAY) == pdTRUE) {
            handle_command(item.payload);
            free(item.payload);
        }
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupClearBits(s_connection_events, MQTT_RETRY_BIT);
        xEventGroupSetBits(s_connection_events, MQTT_CONNECTED_BIT);
        esp_mqtt_client_subscribe(event->client, s_topic_in, 1);
        ESP_LOGI(TAG, "connected; subscribed to %s", s_topic_in);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        xEventGroupClearBits(s_connection_events, MQTT_CONNECTED_BIT);
        xEventGroupSetBits(s_connection_events, MQTT_RETRY_BIT);
        ESP_LOGW(TAG, "disconnected; reconnect when Wi-Fi is ready");
    } else if (id == MQTT_EVENT_ERROR && event->error_handle) {
        ESP_LOGW(TAG, "MQTT error type=%d, connect return=%d, socket errno=%d",
                 event->error_handle->error_type, event->error_handle->connect_return_code,
                 event->error_handle->esp_transport_sock_errno);
    } else if (id == MQTT_EVENT_DATA) {
        if (event->current_data_offset != 0 || event->data_len != event->total_data_len ||
            event->data_len <= 0 || event->data_len > MQTT_COMMAND_MAX) {
            ESP_LOGW(TAG, "dropping fragmented or oversized command (%d bytes)", event->total_data_len);
            return;
        }
        command_item_t item = {.payload = calloc(1, event->data_len + 1)};
        if (!item.payload) return;
        memcpy(item.payload, event->data, event->data_len);
        if (xQueueSend(s_commands, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "command queue full");
            free(item.payload);
        }
    }
}

static esp_err_t create_client_locked(void)
{
    app_config_t cfg;
    app_config_get(&cfg);
    char uri[192];
    if (strstr(cfg.mqtt_host, "://")) snprintf(uri, sizeof(uri), "%s:%u", cfg.mqtt_host, cfg.mqtt_port);
    else snprintf(uri, sizeof(uri), "mqtt://%s:%u", cfg.mqtt_host, cfg.mqtt_port);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = device_identity_id(),
        .credentials.username = cfg.mqtt_username,
        .credentials.authentication.password = cfg.mqtt_password,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 3000,
        .network.disable_auto_reconnect = true,
        .outbox.limit = 32768,
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_ERR_NO_MEM, TAG, "mqtt init");
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    ESP_LOGI(TAG, "MQTT %s, client=%s", uri, device_identity_id());
    return ESP_OK;
}

/* Reconnect only with an IP. Do not stop the SDK task on Wi-Fi loss: stopping
 * it clears the outbox. Keep lifecycle calls out of Wi-Fi and MQTT callbacks. */
static void connection_task(void *arg)
{
    (void)arg;
    int64_t retry_at_us = 0;
    bool was_online = false;
    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool online = wifi_manager_is_connected();
        int64_t now = esp_timer_get_time();
        if (online && !was_online) retry_at_us = 0;
        if (s_client && online && !s_client_started && now >= retry_at_us) {
            esp_err_t err = esp_mqtt_client_start(s_client);
            if (err == ESP_OK) {
                s_client_started = true;
                ESP_LOGI(TAG, "Wi-Fi ready; MQTT started");
            } else ESP_LOGW(TAG, "MQTT start failed: %s", esp_err_to_name(err));
            retry_at_us = now + 3000000;
        } else if (s_client && s_client_started) {
            EventBits_t bits = xEventGroupGetBits(s_connection_events);
            if (!online && (was_online || (bits & MQTT_CONNECTED_BIT))) {
                esp_mqtt_client_disconnect(s_client);
            } else if (online && (bits & MQTT_RETRY_BIT) && now >= retry_at_us) {
                xEventGroupClearBits(s_connection_events, MQTT_RETRY_BIT);
                if (esp_mqtt_client_reconnect(s_client) != ESP_OK) {
                    xEventGroupSetBits(s_connection_events, MQTT_RETRY_BIT);
                }
                retry_at_us = now + 3000000;
            }
        }
        was_online = online;
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t mqtt_control_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_connection_events = xEventGroupCreate();
    s_commands = xQueueCreate(8, sizeof(command_item_t));
    if (!s_lock || !s_commands || !s_connection_events) return ESP_ERR_NO_MEM;
    snprintf(s_topic_in, sizeof(s_topic_in), "to/recorder/%s", device_identity_id());
    snprintf(s_topic_out, sizeof(s_topic_out), "from/recorder/%s", device_identity_id());
    if (xTaskCreate(command_task, "mqtt_commands", 6144, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = create_client_locked();
    xSemaphoreGive(s_lock);
    if (err == ESP_OK && xTaskCreate(connection_task, "mqtt_connection", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return err;
}

esp_err_t mqtt_control_reload(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_client) {
        if (s_client_started) {
            esp_err_t err = esp_mqtt_client_stop(s_client);
            if (err != ESP_OK) {
                xSemaphoreGive(s_lock);
                return err;
            }
            s_client_started = false;
        }
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    xEventGroupClearBits(s_connection_events, MQTT_CONNECTED_BIT | MQTT_RETRY_BIT);
    esp_err_t err = create_client_locked();
    xSemaphoreGive(s_lock);
    return err;
}
