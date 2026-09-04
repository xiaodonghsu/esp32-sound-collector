#include "app_config.h"
#include "audio_stream.h"
#include "device_identity.h"
#include "mqtt_control.h"
#include "web_config.h"
#include "wifi_manager.h"
#include "xvf3800.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "recorder";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(device_identity_init());
    ESP_LOGI(TAG, "device id: %s", device_identity_id());

    ESP_ERROR_CHECK(wifi_manager_init());
    err = wifi_manager_apply_saved();
    if (err == ESP_ERR_NOT_FOUND) ESP_LOGW(TAG, "Wi-Fi is not configured; use http://192.168.100.1");
    else if (err != ESP_OK) ESP_LOGW(TAG, "saved Wi-Fi connection start failed: %s", esp_err_to_name(err));

    ESP_ERROR_CHECK(web_config_start());
    ESP_ERROR_CHECK(xvf3800_init());
    ESP_ERROR_CHECK(audio_stream_init());
    ESP_ERROR_CHECK(mqtt_control_init());
    ESP_LOGI(TAG, "Bestlink recorder services started");
}
