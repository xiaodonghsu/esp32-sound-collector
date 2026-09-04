#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types_generic.h"

typedef void (*wifi_manager_state_cb_t)(bool connected, void *ctx);

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_apply_saved(void);
esp_err_t wifi_manager_test_and_save(const char *ssid, const char *password, uint32_t timeout_ms);
esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count);
bool wifi_manager_is_connected(void);
int wifi_manager_rssi(void);
void wifi_manager_ip(char *out, size_t size);
void wifi_manager_ssid(char *out, size_t size);
void wifi_manager_set_state_callback(wifi_manager_state_cb_t callback, void *ctx);
