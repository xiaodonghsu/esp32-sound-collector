#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "app_config.h"
#include "device_identity.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static bool s_connected;
static int s_retry_count;
static wifi_manager_state_cb_t s_state_cb;
static void *s_state_ctx;

static void notify_state(bool connected)
{
    if (s_connected == connected) return;
    s_connected = connected;
    if (s_state_cb) s_state_cb(connected, s_state_ctx);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        app_config_t config;
        app_config_get(&config);
        if (config.wifi_ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        notify_state(false);
        if (++s_retry_count <= 10) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupClearBits(s_events, WIFI_FAILED_BIT);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        notify_state(true);
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "station connected, ip=" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_init(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_sta_netif && ap_netif, ESP_ERR_NO_MEM, TAG, "netif create");

    esp_netif_ip_info_t ap_ip = {0};
    IP4_ADDR(&ap_ip.ip, 192, 168, 100, 1);
    IP4_ADDR(&ap_ip.gw, 192, 168, 100, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap_netif), TAG, "stop dhcp server");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ap_ip), TAG, "set AP ip");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), TAG, "start dhcp server");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG, "ip handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "APSTA mode");

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, device_identity_ap_ssid(), sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(device_identity_ap_ssid());
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_LOGI(TAG, "configuration AP %s at 192.168.100.1", device_identity_ap_ssid());
    return ESP_OK;
}

static esp_err_t apply_station(const char *ssid, const char *password)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_retry_count = 0;
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "station config");
    return esp_wifi_connect();
}

esp_err_t wifi_manager_apply_saved(void)
{
    app_config_t config;
    app_config_get(&config);
    if (!config.wifi_ssid[0]) return ESP_ERR_NOT_FOUND;
    return apply_station(config.wifi_ssid, config.wifi_password);
}

esp_err_t wifi_manager_test_and_save(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!ssid || !ssid[0] || !password) return ESP_ERR_INVALID_ARG;
    app_config_t old;
    app_config_get(&old);
    ESP_RETURN_ON_ERROR(apply_station(ssid, password), TAG, "test connection");
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "connection validation failed for %s", ssid);
        if (old.wifi_ssid[0]) apply_station(old.wifi_ssid, old.wifi_password);
        return ESP_ERR_TIMEOUT;
    }
    return app_config_set_wifi(ssid, password);
}

esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count)
{
    if (!records || !count) return ESP_ERR_INVALID_ARG;
    *records = NULL;
    *count = 0;
    wifi_scan_config_t scan = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan, true), TAG, "scan");
    uint16_t found = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&found), TAG, "scan count");
    if (!found) return ESP_OK;
    wifi_ap_record_t *list = calloc(found, sizeof(*list));
    ESP_RETURN_ON_FALSE(list, ESP_ERR_NO_MEM, TAG, "scan records");
    esp_err_t err = esp_wifi_scan_get_ap_records(&found, list);
    if (err != ESP_OK) {
        free(list);
        return err;
    }
    *records = list;
    *count = found;
    return ESP_OK;
}

bool wifi_manager_is_connected(void) { return s_connected; }

int wifi_manager_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
}

void wifi_manager_ip(char *out, size_t size)
{
    if (!out || !size) return;
    esp_netif_ip_info_t info;
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK) {
        snprintf(out, size, IPSTR, IP2STR(&info.ip));
    } else strlcpy(out, "0.0.0.0", size);
}

void wifi_manager_ssid(char *out, size_t size)
{
    if (!out || !size) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) strlcpy(out, (char *)ap.ssid, size);
    else out[0] = '\0';
}

void wifi_manager_set_state_callback(wifi_manager_state_cb_t callback, void *ctx)
{
    s_state_cb = callback;
    s_state_ctx = ctx;
}
