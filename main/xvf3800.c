#include "xvf3800.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define XVF_I2C_PORT       I2C_NUM_0
#define XVF_I2C_SDA        5
#define XVF_I2C_SCL        6
#define XVF_I2C_ADDRESS    0x2c
#define XVF_RES_GPO        20
#define XVF_RES_APP        48
#define XVF_CMD_VERSION    0
#define XVF_CMD_LED_EFFECT 12
#define XVF_CMD_LED_BRIGHT 13
#define XVF_CMD_LED_SPEED  15
#define XVF_CMD_LED_COLOR  16

static const char *TAG = "xvf3800";
static i2c_master_dev_handle_t s_device;
static SemaphoreHandle_t s_lock;

static esp_err_t read_bytes(uint8_t resid, uint8_t cmd, uint8_t *data, size_t len)
{
    if (!s_device || !data || !len || len > 63) return ESP_ERR_INVALID_ARG;
    uint8_t request[] = {resid, (uint8_t)(cmd | 0x80), (uint8_t)(len + 1)};
    uint8_t response[65];
    esp_err_t err = ESP_FAIL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int attempt = 0; attempt < 5; ++attempt) {
        err = i2c_master_transmit(s_device, request, sizeof(request), 100);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            err = i2c_master_receive(s_device, response, len + 1, 100);
        }
        if (err == ESP_OK && response[0] == 0) {
            memcpy(data, response + 1, len);
            break;
        }
        if (err == ESP_OK) err = response[0] == 64 ? ESP_ERR_TIMEOUT : ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t write_bytes(uint8_t resid, uint8_t cmd, const uint8_t *data, size_t len)
{
    if (!s_device || len > 61 || (len && !data)) return ESP_ERR_INVALID_ARG;
    uint8_t frame[64] = {resid, cmd, (uint8_t)len};
    if (len) memcpy(frame + 3, data, len);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_device, frame, len + 3, 100);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t xvf3800_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    i2c_master_bus_config_t bus_config = {
        .i2c_port = XVF_I2C_PORT,
        .sda_io_num = XVF_I2C_SDA,
        .scl_io_num = XVF_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "create I2C bus");
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XVF_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &s_device), TAG, "add XVF3800");
    uint8_t version[3];
    esp_err_t err = read_bytes(XVF_RES_APP, XVF_CMD_VERSION, version, sizeof(version));
    if (err == ESP_OK) ESP_LOGI(TAG, "XVF3800 firmware %u.%u.%u", version[0], version[1], version[2]);
    else ESP_LOGW(TAG, "XVF3800 not responding yet: %s", esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t xvf3800_get_status(xvf3800_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    uint8_t version[3];
    ESP_RETURN_ON_ERROR(read_bytes(XVF_RES_APP, XVF_CMD_VERSION, version, sizeof(version)), TAG, "read version");
    snprintf(status->version, sizeof(status->version), "%u.%u.%u", version[0], version[1], version[2]);
    ESP_RETURN_ON_ERROR(read_bytes(XVF_RES_GPO, XVF_CMD_LED_EFFECT, &status->led_effect, 1), TAG, "read effect");
    ESP_RETURN_ON_ERROR(read_bytes(XVF_RES_GPO, XVF_CMD_LED_BRIGHT, &status->led_brightness, 1), TAG, "read brightness");
    ESP_RETURN_ON_ERROR(read_bytes(XVF_RES_GPO, XVF_CMD_LED_SPEED, &status->led_speed, 1), TAG, "read speed");
    uint8_t color[4];
    ESP_RETURN_ON_ERROR(read_bytes(XVF_RES_GPO, XVF_CMD_LED_COLOR, color, sizeof(color)), TAG, "read color");
    status->led_color = (uint32_t)color[0] | ((uint32_t)color[1] << 8) |
                        ((uint32_t)color[2] << 16) | ((uint32_t)color[3] << 24);
    return ESP_OK;
}

esp_err_t xvf3800_set_parameter(const char *name, uint32_t value)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    uint8_t cmd;
    size_t len = 1;
    if (!strcasecmp(name, "LED_EFFECT")) cmd = XVF_CMD_LED_EFFECT;
    else if (!strcasecmp(name, "LED_BRIGHTNESS")) cmd = XVF_CMD_LED_BRIGHT;
    else if (!strcasecmp(name, "LED_SPEED")) cmd = XVF_CMD_LED_SPEED;
    else if (!strcasecmp(name, "LED_COLOR")) { cmd = XVF_CMD_LED_COLOR; len = 4; }
    else return ESP_ERR_NOT_SUPPORTED;
    if (len == 1 && value > UINT8_MAX) return ESP_ERR_INVALID_ARG;
    uint8_t data[4] = {value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >> 24) & 0xff};
    return write_bytes(XVF_RES_GPO, cmd, data, len);
}
