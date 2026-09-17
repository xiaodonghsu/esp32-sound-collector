#include "app_config.h"
#include "audio_stream.h"
#include "firmware_update.h"
#include "device_identity.h"
#include "mqtt_control.h"
#include "web_config.h"
#include "wifi_manager.h"
#include "xvf3800.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "nvs_flash.h"

static const char *TAG = "recorder";

void app_main(void)
{
#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        size_t size = esp_psram_get_size();
        ESP_LOGI(TAG, "PSRAM detected: %zu bytes (%zu KiB)", size, size / 1024);
        ESP_LOGI(TAG, "PSRAM heap: total=%zu, free=%zu, largest=%zu bytes",
                 heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else {
        ESP_LOGW(TAG, "PSRAM initialization failed; capacity unavailable, using internal RAM");
    }
#else
    ESP_LOGW(TAG, "PSRAM disabled in firmware; physical capacity unknown");
#endif
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

    ESP_ERROR_CHECK(xvf3800_init());
    ESP_ERROR_CHECK(audio_stream_init());
    ESP_ERROR_CHECK(firmware_update_init());
    ESP_ERROR_CHECK(web_config_start());
    ESP_ERROR_CHECK(mqtt_control_init());
    ESP_ERROR_CHECK(firmware_update_confirm_boot());
    ESP_LOGI(TAG, "Bestlink recorder services started");
}
