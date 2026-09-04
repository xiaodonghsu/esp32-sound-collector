#include "app_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_NAMESPACE "recorder"

static app_config_t s_config;
static SemaphoreHandle_t s_lock;

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!src) src = "";
    strlcpy(dst, src, dst_size);
}

static void read_string(nvs_handle_t nvs, const char *key, char *dst, size_t size)
{
    size_t required = size;
    if (nvs_get_str(nvs, key, dst, &required) != ESP_OK) dst[0] = '\0';
}

static esp_err_t write_string(nvs_handle_t nvs, const char *key, const char *value)
{
    return nvs_set_str(nvs, key, value ? value : "");
}

esp_err_t app_config_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    memset(&s_config, 0, sizeof(s_config));
    strlcpy(s_config.mqtt_host, "192.168.4.244", sizeof(s_config.mqtt_host));
    s_config.mqtt_port = 11883;
    strlcpy(s_config.mqtt_username, "Recorders", sizeof(s_config.mqtt_username));
    strlcpy(s_config.mqtt_password, "bestlink", sizeof(s_config.mqtt_password));
    strlcpy(s_config.web_password, "bestlink", sizeof(s_config.web_password));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    read_string(nvs, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    read_string(nvs, "wifi_pass", s_config.wifi_password, sizeof(s_config.wifi_password));
    read_string(nvs, "mqtt_host", s_config.mqtt_host, sizeof(s_config.mqtt_host));
    read_string(nvs, "mqtt_user", s_config.mqtt_username, sizeof(s_config.mqtt_username));
    read_string(nvs, "mqtt_pass", s_config.mqtt_password, sizeof(s_config.mqtt_password));
    read_string(nvs, "web_pass", s_config.web_password, sizeof(s_config.web_password));
    uint16_t port;
    if (nvs_get_u16(nvs, "mqtt_port", &port) == ESP_OK) s_config.mqtt_port = port;
    nvs_close(nvs);

    if (!s_config.mqtt_host[0]) strlcpy(s_config.mqtt_host, "192.168.4.244", sizeof(s_config.mqtt_host));
    if (!s_config.mqtt_username[0]) strlcpy(s_config.mqtt_username, "Recorders", sizeof(s_config.mqtt_username));
    if (!s_config.mqtt_password[0]) strlcpy(s_config.mqtt_password, "bestlink", sizeof(s_config.mqtt_password));
    if (!s_config.web_password[0]) strlcpy(s_config.web_password, "bestlink", sizeof(s_config.web_password));
    return ESP_OK;
}

void app_config_get(app_config_t *out)
{
    if (!out || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_lock);
}

esp_err_t app_config_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > APP_SSID_MAX_LEN ||
        !password || strlen(password) > APP_WIFI_PASS_MAX_LEN) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs), "config", "open nvs");
    esp_err_t err = write_string(nvs, "wifi_ssid", ssid);
    if (err == ESP_OK) err = write_string(nvs, "wifi_pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        copy_string(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), ssid);
        copy_string(s_config.wifi_password, sizeof(s_config.wifi_password), password);
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t app_config_set_mqtt(const char *host, uint16_t port,
                              const char *username, const char *password)
{
    if (!host || !host[0] || strlen(host) > APP_MQTT_HOST_MAX_LEN || port == 0 ||
        !username || strlen(username) > APP_MQTT_USER_MAX_LEN ||
        !password || strlen(password) > APP_MQTT_PASS_MAX_LEN) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs), "config", "open nvs");
    esp_err_t err = write_string(nvs, "mqtt_host", host);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "mqtt_port", port);
    if (err == ESP_OK) err = write_string(nvs, "mqtt_user", username);
    if (err == ESP_OK) err = write_string(nvs, "mqtt_pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), host);
        s_config.mqtt_port = port;
        copy_string(s_config.mqtt_username, sizeof(s_config.mqtt_username), username);
        copy_string(s_config.mqtt_password, sizeof(s_config.mqtt_password), password);
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t app_config_set_web_password(const char *password)
{
    if (!password || strlen(password) < 6 || strlen(password) > APP_WEB_PASS_MAX_LEN) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs), "config", "open nvs");
    esp_err_t err = write_string(nvs, "web_pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        copy_string(s_config.web_password, sizeof(s_config.web_password), password);
        xSemaphoreGive(s_lock);
    }
    return err;
}
