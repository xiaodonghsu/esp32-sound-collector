#include "firmware_update.h"

#include <stdlib.h>
#include <string.h>
#include "audio_stream.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "firmware_update";
static SemaphoreHandle_t s_control_lock;

esp_err_t firmware_update_init(void)
{
    s_control_lock = xSemaphoreCreateMutex();
    return s_control_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

bool firmware_update_control_lock(void)
{
    return s_control_lock && xSemaphoreTake(s_control_lock, 0) == pdTRUE;
}

void firmware_update_control_unlock(void)
{
    xSemaphoreGive(s_control_lock);
}

esp_err_t firmware_update_confirm_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Boot self-check complete: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t firmware_update_info(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    bool available = running && next && next->address != running->address &&
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    char body[192];
    snprintf(body, sizeof(body), "{\"available\":%s,\"max_bytes\":%lu,\"running\":\"%s\"}",
             available ? "true" : "false", (unsigned long)(available ? next->size : 0),
             running ? running->label : "unknown");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t upload_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, body);
    /* Do not interpret any unread binary body as a subsequent HTTP request. */
    return ESP_FAIL;
}

esp_err_t firmware_update_upload(httpd_req_t *req)
{
    char type[40], marker[8];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/octet-stream") ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Upload", marker, sizeof(marker)) != ESP_OK ||
        strcmp(marker, "1")) {
        return upload_error(req, "400 Bad Request", "请从固件升级页面上传应用 BIN 文件");
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!running || !next || running->address == next->address ||
        !esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL)) {
        return upload_error(req, "409 Conflict", "设备尚未配置双 OTA 分区，请先通过 USB 烧录");
    }
    const size_t prefix_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (req->content_len < prefix_size || req->content_len > next->size) {
        return upload_error(req, "413 Content Too Large", "固件为空、过小或超过 OTA 分区容量");
    }
    if (!firmware_update_control_lock()) {
        return upload_error(req, "409 Conflict", "设备正在执行控制命令，请稍后重试");
    }

    esp_ota_handle_t handle = 0;
    bool begun = false;
    uint8_t *buffer = malloc(4096);
    const char *status = "400 Bad Request";
    const char *message = "固件接收失败，请重新上传";
    esp_err_t err = ESP_OK;
    if (!buffer) { status = "503 Service Unavailable"; message = "内存不足"; goto fail; }

    /* Read the complete header even when TCP delivers it in tiny fragments. */
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 120LL * 1000000;
    while (received < prefix_size) {
        int n = httpd_req_recv(req, (char *)buffer + received, prefix_size - received);
        if (n <= 0 || esp_timer_get_time() > deadline) goto fail;
        received += n;
    }
    esp_image_header_t header;
    esp_app_desc_t description;
    memcpy(&header, buffer, sizeof(header));
    memcpy(&description, buffer + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(description));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
        header.hash_appended != 1 || description.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(description.project_name, esp_app_get_description()->project_name, sizeof(description.project_name))) {
        message = "固件不匹配：请选择本项目的 ESP32-S3 应用 BIN，不能使用合并烧录包";
        goto fail;
    }

    err = audio_stream_stop();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        status = "503 Service Unavailable"; message = "录音任务未能停止，请稍后重试"; goto fail;
    }
    err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) { message = "无法开始升级，请检查启动验证状态和分区"; goto fail; }
    begun = true;
    err = esp_ota_write(handle, buffer, received);
    if (err != ESP_OK) { message = "固件写入失败"; goto fail; }
    while (received < req->content_len) {
        size_t remaining = req->content_len - received;
        int n = httpd_req_recv(req, (char *)buffer, remaining < 4096 ? remaining : 4096);
        if (n <= 0 || esp_timer_get_time() > deadline) goto fail;
        err = esp_ota_write(handle, buffer, n);
        if (err != ESP_OK) { message = "固件写入失败"; goto fail; }
        received += n;
    }
    err = esp_ota_end(handle);
    begun = false; /* esp_ota_end always frees its handle, including on failure. */
    if (err != ESP_OK) { message = "固件完整性校验失败，原固件保持不变"; goto fail; }
    esp_image_metadata_t metadata = {0};
    const esp_partition_pos_t position = {.offset = next->address, .size = next->size};
    err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (err != ESP_OK || metadata.image_len != received) {
        message = "固件长度不匹配，请上传未合并、未填充的应用 BIN"; goto fail;
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) { message = "设置启动分区失败"; goto fail; }
    free(buffer);
    ESP_LOGI(TAG, "Validated %u bytes in %s; restarting", (unsigned)received, next->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;

fail:
    ESP_LOGW(TAG, "Upload aborted: %s (%s)", message, esp_err_to_name(err));
    if (begun) esp_ota_abort(handle);
    free(buffer);
    firmware_update_control_unlock();
    return upload_error(req, status, message);
}
