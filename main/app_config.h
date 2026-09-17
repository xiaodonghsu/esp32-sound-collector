#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_SSID_MAX_LEN        32
#define APP_WIFI_PASS_MAX_LEN   64
#define APP_MQTT_HOST_MAX_LEN   128
#define APP_MQTT_USER_MAX_LEN   64
#define APP_MQTT_PASS_MAX_LEN   64
#define APP_WEB_PASS_MAX_LEN    64

#define APP_MQTT_DEFAULT_HOST     "192.168.4.244"
#define APP_MQTT_DEFAULT_PORT     11883
#define APP_MQTT_DEFAULT_USERNAME "Recorders"
#define APP_MQTT_DEFAULT_PASSWORD "bestlink"

typedef struct {
    char wifi_ssid[APP_SSID_MAX_LEN + 1];
    char wifi_password[APP_WIFI_PASS_MAX_LEN + 1];
    char mqtt_host[APP_MQTT_HOST_MAX_LEN + 1];
    uint16_t mqtt_port;
    char mqtt_username[APP_MQTT_USER_MAX_LEN + 1];
    char mqtt_password[APP_MQTT_PASS_MAX_LEN + 1];
    char web_password[APP_WEB_PASS_MAX_LEN + 1];
} app_config_t;

esp_err_t app_config_init(void);
void app_config_get(app_config_t *out);
esp_err_t app_config_set_wifi(const char *ssid, const char *password);
esp_err_t app_config_set_mqtt(const char *host, uint16_t port,
                              const char *username, const char *password);
esp_err_t app_config_set_web_password(const char *password);
