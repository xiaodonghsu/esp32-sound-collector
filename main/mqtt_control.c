#include "mqtt_control.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "app_config.h"
#include "audio_stream.h"
#include "device_identity.h"
#include "wifi_manager.h"
#include "xvf3800.h"

#define MQTT_COMMAND_MAX 2048
#define MQTT_MID_MAX     128

static const char *TAG = "mqtt_control";
static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_commands;
static char s_topic_in[64];
static char s_topic_out[64];

typedef struct { char *payload; } command_item_t;
typedef struct { char mid[MQTT_MID_MAX]; } start_pending_t;

static void publish_json(cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    if (client) esp_mqtt_client_publish(client, s_topic_out, text, 0, 1, 0);
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

static void handle_start(cJSON *root, const char *mid)
{
    const char *url = get_string(root, "url");
    cJSON *segment_item = cJSON_GetObjectItemCaseSensitive(root, "segment");
    int segment = cJSON_IsNumber(segment_item) ? segment_item->valueint : 200;
    if (!url) { publish_result(mid, false, "url is required"); return; }
    start_pending_t *pending = calloc(1, sizeof(*pending));
    if (!pending) { publish_result(mid, false, "out of memory"); return; }
    strlcpy(pending->mid, mid, sizeof(pending->mid));
    esp_err_t err = audio_stream_start(url, segment, stream_started, pending);
    if (err != ESP_OK) {
        free(pending);
        publish_result(mid, false, esp_err_to_name(err));
    }
}

static void handle_status(const char *mid)
{
    xvf3800_status_t xvf;
    esp_err_t xvf_err = xvf3800_get_status(&xvf);
    char ssid[APP_SSID_MAX_LEN + 1], ip[16];
    wifi_manager_ssid(ssid, sizeof(ssid));
    wifi_manager_ip(ip, sizeof(ip));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mid", mid);
    cJSON_AddStringToObject(j, "result", xvf_err == ESP_OK ? "success" : "failed");
    cJSON_AddStringToObject(j, "wifi-ssid", ssid);
    cJSON_AddNumberToObject(j, "wifi-rssi", wifi_manager_rssi());
    cJSON_AddStringToObject(j, "wifi-mac", device_identity_id());
    cJSON_AddStringToObject(j, "wifi-ipaddr", ip);
    cJSON_AddStringToObject(j, "recorder-type", "XVF3800");
    cJSON_AddStringToObject(j, "recorder-version", xvf_err == ESP_OK ? xvf.version : "unknown");
    if (xvf_err == ESP_OK) {
        cJSON_AddNumberToObject(j, "led-effict", xvf.led_effect);
        cJSON_AddNumberToObject(j, "led-brightness", xvf.led_brightness);
        cJSON_AddNumberToObject(j, "led-speed", xvf.led_speed);
        cJSON_AddNumberToObject(j, "led-color", xvf.led_color);
    } else cJSON_AddStringToObject(j, "message", esp_err_to_name(xvf_err));
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
        esp_mqtt_client_subscribe(event->client, s_topic_in, 1);
        ESP_LOGI(TAG, "connected; subscribed to %s", s_topic_in);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected; automatic reconnect enabled");
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
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_ERR_NO_MEM, TAG, "mqtt init");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL), TAG, "mqtt events");
    ESP_LOGI(TAG, "MQTT %s, client=%s", uri, device_identity_id());
    return esp_mqtt_client_start(s_client);
}

esp_err_t mqtt_control_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(8, sizeof(command_item_t));
    if (!s_lock || !s_commands) return ESP_ERR_NO_MEM;
    snprintf(s_topic_in, sizeof(s_topic_in), "to/recorder/%s", device_identity_id());
    snprintf(s_topic_out, sizeof(s_topic_out), "from/recorder/%s", device_identity_id());
    if (xTaskCreate(command_task, "mqtt_commands", 6144, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = create_client_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t mqtt_control_reload(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    esp_err_t err = create_client_locked();
    xSemaphoreGive(s_lock);
    return err;
}
