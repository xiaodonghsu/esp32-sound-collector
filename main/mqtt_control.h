#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t mqtt_control_init(void);
esp_err_t mqtt_control_reload(void);
bool mqtt_control_is_connected(void);
