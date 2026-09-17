#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t firmware_update_init(void);
esp_err_t firmware_update_confirm_boot(void);
/* Serialize MQTT commands with OTA; successful OTA holds this until reboot. */
bool firmware_update_control_lock(void);
void firmware_update_control_unlock(void);
/* Caller must authenticate the request. */
esp_err_t firmware_update_info(httpd_req_t *req);
esp_err_t firmware_update_upload(httpd_req_t *req);
