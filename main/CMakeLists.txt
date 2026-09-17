idf_component_register(
    SRCS
        "app_main.c"
        "app_config.c"
        "device_identity.c"
        "wifi_manager.c"
        "web_config.c"
        "xvf3800.c"
        "audio_stream.c"
        "mqtt_control.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash esp_wifi esp_event esp_netif esp_http_server spi_flash esp_psram esp_app_format
        esp_driver_i2c esp_driver_i2s esp_ringbuf esp_timer mqtt json esp_websocket_client
)
