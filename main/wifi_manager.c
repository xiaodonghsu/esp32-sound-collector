#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "app_config.h"
#include "device_identity.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_BARRIER_BIT   BIT1
#define WIFI_RETRY_MIN_MS  1000U
#define WIFI_RETRY_MAX_MS  60000U
#define WIFI_CONNECT_TIMEOUT_US (30LL * 1000000)

ESP_EVENT_DEFINE_BASE(WIFI_MANAGER_EVENT);

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static bool s_connected;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_config_lock;
static bool s_enabled;
static bool s_attempting;
static bool s_scanning;
static bool s_ap_enabled;
static uint8_t s_disconnect_reason;
static char s_target_ssid[APP_SSID_MAX_LEN + 1];
static uint32_t s_retry_ms = WIFI_RETRY_MIN_MS;
static int64_t s_deadline_us;
static wifi_manager_state_cb_t s_state_cb;
static void *s_state_ctx;

static void notify_state(bool connected)
{
    if (connected) xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    else xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
    if (s_connected == connected) return;
    s_connected = connected;
    if (s_state_cb) s_state_cb(connected, s_state_ctx);
}

/* Called with s_lock held. Jitter is below the cap, including at 60 seconds. */
static void schedule_retry(void)
{
    if (!s_enabled) return;
    uint32_t delay_ms = s_retry_ms - s_retry_ms / 5 + esp_random() % (s_retry_ms / 5 + 1);
    s_attempting = false;
    s_deadline_us = esp_timer_get_time() + (int64_t)delay_ms * 1000;
    ESP_LOGW(TAG, "station offline; retry in %lu ms", (unsigned long)delay_ms);
    s_retry_ms = s_retry_ms >= WIFI_RETRY_MAX_MS / 2 ? WIFI_RETRY_MAX_MS : s_retry_ms * 2;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_MANAGER_EVENT) {
        xEventGroupSetBits(s_events, WIFI_BARRIER_BIT);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_enabled) s_disconnect_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        bool was_active = s_connected || s_attempting;
        notify_state(false);
        if (was_active) schedule_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && s_enabled) {
        wifi_ap_record_t ap;
        /* Ignore delayed IP events from an association that has been replaced. */
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
            !strncmp((const char *)ap.ssid, s_target_ssid, sizeof(ap.ssid))) {
            s_attempting = false;
            s_retry_ms = WIFI_RETRY_MIN_MS;
            notify_state(true);
            const ip_event_got_ip_t *event = data;
            ESP_LOGI(TAG, "station connected, ip=" IPSTR, IP2STR(&event->ip_info.ip));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP && s_enabled) {
        notify_state(false);
        s_attempting = true;
        s_deadline_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);
}

static void reconnect_task(void *arg)
{
    (void)arg;
    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_enabled && !s_connected && !s_scanning && esp_timer_get_time() >= s_deadline_us) {
            if (s_attempting) {
                /* Also recover an association that never obtains a DHCP lease. */
                esp_wifi_disconnect();
                schedule_retry();
            } else {
                s_attempting = true;
                s_deadline_us = esp_timer_get_time() + WIFI_CONNECT_TIMEOUT_US;
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "connect start failed: %s", esp_err_to_name(err));
                    schedule_retry();
                }
            }
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t wifi_manager_init(void)
{
    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    s_config_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_events && s_lock && s_config_lock, ESP_ERR_NO_MEM, TAG, "Wi-Fi synchronization");

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
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "ip handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_MANAGER_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "barrier handler");
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
    s_ap_enabled = true;
#if CONFIG_SOC_WIFI_SUPPORT_5G
    ESP_RETURN_ON_ERROR(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO), TAG, "dual-band mode");
    /* Select the initial SoftAP band before starting STA connection attempts.
     * AUTO retains scanning/connecting on both bands; STA can later move AP. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(36, WIFI_SECOND_CHAN_NONE), TAG, "initial 5 GHz AP channel");
#endif
    ESP_RETURN_ON_FALSE(xTaskCreate(reconnect_task, "wifi_reconnect", 4096, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "reconnect task");
    #if CONFIG_SOC_WIFI_SUPPORT_5G
    ESP_LOGI(TAG, "configuration AP %s at 192.168.100.1, initial 5 GHz channel 36", device_identity_ap_ssid());
#else
    ESP_LOGI(TAG, "configuration AP %s at 192.168.100.1, initial 2.4 GHz channel 1", device_identity_ap_ssid());
#endif
    return ESP_OK;
}

void wifi_manager_set_ap_enabled(bool enabled)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Keep the configuration path available if STA drops before the switch. */
    enabled = enabled || !s_connected;
    if (enabled != s_ap_enabled && !s_scanning) {
        esp_err_t err = esp_wifi_set_mode(enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA);
        if (err == ESP_OK) {
            s_ap_enabled = enabled;
            ESP_LOGI(TAG, "configuration AP %s", enabled ? "enabled" : "disabled");
        } else {
            ESP_LOGW(TAG, "configuration AP mode switch failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t apply_station(const char *ssid, const char *password)
{
    /* s_config_lock serializes complete validation/rollback transactions. Disable
     * retries and drain old connection events before enabling the new station. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_enabled = false;
    s_attempting = false;
    notify_state(false);
    xSemaphoreGive(s_lock);
    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, WIFI_BARRIER_BIT);
    ESP_RETURN_ON_ERROR(esp_event_post(WIFI_MANAGER_EVENT, 0, NULL, 0, portMAX_DELAY), TAG, "station barrier");
    xEventGroupWaitBits(s_events, WIFI_BARRIER_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    if (!ssid[0]) return ESP_OK;
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
#if CONFIG_SOC_WIFI_SUPPORT_5G
    /* Cover the full usable RSSI range: prefer a matching 5 GHz AP even
     * when its signal is weaker, while retaining 2.4 GHz as a fallback. */
    sta.sta.threshold.rssi_5g_adjustment = 128;
#endif
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "station config");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_target_ssid, ssid, sizeof(s_target_ssid));
    s_retry_ms = WIFI_RETRY_MIN_MS;
    s_disconnect_reason = 0;
    s_deadline_us = 0;
    s_enabled = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t wifi_manager_apply_saved(void)
{
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    app_config_t config;
    app_config_get(&config);
    esp_err_t err = config.wifi_ssid[0] ? apply_station(config.wifi_ssid, config.wifi_password) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(s_config_lock);
    return err;
}

esp_err_t wifi_manager_test_and_save(const char *ssid, const char *password, uint32_t timeout_ms)
{
    return wifi_manager_test_and_save_detailed(ssid, password, timeout_ms, NULL, 0);
}

esp_err_t wifi_manager_test_and_save_detailed(const char *ssid, const char *password,
                                             uint32_t timeout_ms, char *message, size_t size)
{
    if (message && size) message[0] = '\0';
    if (!ssid || !ssid[0] || !password || strlen(ssid) > APP_SSID_MAX_LEN ||
        strlen(password) > APP_WIFI_PASS_MAX_LEN) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_config_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    app_config_t old;
    app_config_get(&old);
    esp_err_t err = apply_station(ssid, password);
    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        err = (bits & WIFI_CONNECTED_BIT) ? app_config_set_wifi(ssid, password) : ESP_ERR_TIMEOUT;
    }
    if (err != ESP_OK) {
        if (message && size) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            uint8_t reason = s_disconnect_reason;
            xSemaphoreGive(s_lock);
            const char *detail = "连接或获取 IP 超时，请检查信号及路由器 DHCP";
            switch (reason) {
            case WIFI_REASON_NO_AP_FOUND: detail = "未找到目标网络，请检查 SSID、信号和频段"; break;
            case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
                detail = "目标网络安全设置不兼容，请检查路由器认证方式"; break;
            case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
                detail = "目标网络信号过弱，请靠近路由器后重试"; break;
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT: detail = "认证或握手失败，请检查密码及路由器安全设置"; break;
            case WIFI_REASON_ASSOC_FAIL: detail = "关联失败，请检查路由器接入限制和信号"; break;
            default: break;
            }
            if (err == ESP_ERR_TIMEOUT)
                snprintf(message, size, "%s（Wi-Fi 原因码 %u）", detail, (unsigned)reason);
            else snprintf(message, size, "配置应用或保存失败：%s", esp_err_to_name(err));
        }
        ESP_LOGW(TAG, "Wi-Fi validation/save failed; restoring saved configuration");
        esp_err_t restore_err = apply_station(old.wifi_ssid, old.wifi_password);
        if (restore_err != ESP_OK) ESP_LOGE(TAG, "restore failed: %s", esp_err_to_name(restore_err));
    }
    xSemaphoreGive(s_config_lock);
    return err;
}

unsigned wifi_manager_frequency_mhz(uint8_t channel)
{
    if (channel == 14) return 2484;
    if (channel >= 1 && channel <= 13) return 2407 + 5 * channel;
    if (channel >= 32 && channel <= 177) return 5000 + 5 * channel;
    return 0;
}

static int compare_scan_records(const void *left, const void *right)
{
    const wifi_ap_record_t *a = left;
    const wifi_ap_record_t *b = right;
    /* ESP32-C5 supports 2.4 GHz (channels 1-14) and 5 GHz. */
    bool a_5g = a->primary > 14;
    bool b_5g = b->primary > 14;
    if (a_5g != b_5g) return a_5g ? -1 : 1;
    return (int)b->rssi - (int)a->rssi;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count)
{
    if (!records || !count) return ESP_ERR_INVALID_ARG;
    *records = NULL;
    *count = 0;
    if (xSemaphoreTake(s_config_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_attempting) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_config_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_scanning = true;
    xSemaphoreGive(s_lock);
    wifi_scan_config_t scan = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    bool completed = err == ESP_OK;
    uint16_t found = 0;
    wifi_ap_record_t *list = NULL;
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_num(&found);
    if (err == ESP_OK && found) {
        list = calloc(found, sizeof(*list));
        if (!list) err = ESP_ERR_NO_MEM;
        else err = esp_wifi_scan_get_ap_records(&found, list);
    }
    if (completed) esp_wifi_clear_ap_list();
    if (err == ESP_OK) {
        if (found) qsort(list, found, sizeof(*list), compare_scan_records);
        *records = list;
        *count = found;
    } else free(list);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scanning = false;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_config_lock);
    return err;
}

bool wifi_manager_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT);
}

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
