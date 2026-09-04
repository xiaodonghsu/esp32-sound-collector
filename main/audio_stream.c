#include "audio_stream.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

#define AUDIO_RATE             16000
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_INPUT_RATE       CONFIG_XVF_I2S_INPUT_RATE_HZ
#define AUDIO_I2S_BCLK         8
#define AUDIO_I2S_WS           7
#define AUDIO_I2S_DOUT         44
#define AUDIO_I2S_DIN          43
#define AUDIO_EXIT_CAPTURE     BIT0
#define AUDIO_EXIT_NETWORK     BIT1
#define AUDIO_WS_CONNECTED     BIT2

static const char *TAG = "audio_stream";
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static RingbufHandle_t s_ring;
static i2s_chan_handle_t s_rx;
static esp_websocket_client_handle_t s_ws;
static volatile bool s_running;
static uint32_t s_segment_ms;
static char s_url[512];
static audio_stream_started_cb_t s_callback;
static void *s_callback_ctx;
static bool s_callback_called;
static int64_t s_started_at_us;
static uint32_t s_duration;
static uint32_t s_segments;

static uint32_t elapsed_seconds(void)
{
    int64_t elapsed_us = esp_timer_get_time() - s_started_at_us;
    if (elapsed_us <= 0) return 0;
    uint64_t seconds = (uint64_t)elapsed_us / 1000000;
    return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

static void mark_stream_stopped(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        s_duration = elapsed_seconds();
        s_running = false;
    }
    xSemaphoreGive(s_lock);
}

static void notify_started(bool success, const char *detail)
{
    audio_stream_started_cb_t cb = NULL;
    void *ctx = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_callback_called && s_callback) {
        s_callback_called = true;
        cb = s_callback;
        ctx = s_callback_ctx;
    }
    xSemaphoreGive(s_lock);
    if (cb) cb(success, detail, ctx);
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WebSocket connected");
        xEventGroupSetBits(s_events, AUDIO_WS_CONNECTED);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "WebSocket disconnected; client will reconnect");
        xEventGroupClearBits(s_events, AUDIO_WS_CONNECTED);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "WebSocket transport error");
    }
}

static esp_err_t i2s_start(void)
{
    if (AUDIO_INPUT_RATE != 16000 && AUDIO_INPUT_RATE != 48000) {
        ESP_LOGE(TAG, "XVF input rate must be 16000 or 48000 Hz");
        return ESP_ERR_INVALID_ARG;
    }
    /* Match Seeed's record/playback example: the ESP32 supplies BCLK and WS. */
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 8;
    channel.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, NULL, &s_rx), TAG, "I2S channel");
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_INPUT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_BCLK,
            .ws = AUDIO_I2S_WS,
            .dout = AUDIO_I2S_DOUT,
            .din = AUDIO_I2S_DIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(s_rx, &config);
    if (err == ESP_OK) err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    return err;
}

static void capture_task(void *arg)
{
    (void)arg;
    const size_t segment_bytes = AUDIO_RATE * AUDIO_BYTES_PER_SAMPLE * s_segment_ms / 1000;
    int16_t *segment = malloc(segment_bytes);
    int32_t *input = malloc(256 * 2 * sizeof(int32_t));
    if (!segment || !input) {
        notify_started(false, "audio buffer allocation failed");
        mark_stream_stopped();
        goto done;
    }
    size_t used = 0;
    const unsigned downsample = AUDIO_INPUT_RATE / AUDIO_RATE;
    unsigned downsample_count = 0;
    int32_t downsample_sum = 0;
    while (s_running) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx, input, 256 * 2 * sizeof(int32_t), &bytes_read, pdMS_TO_TICKS(200));
        if (err == ESP_ERR_TIMEOUT) continue;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(err));
            notify_started(false, "I2S read failed");
            mark_stream_stopped();
            break;
        }
        size_t frames = bytes_read / (2 * sizeof(int32_t));
        for (size_t i = 0; i < frames; ++i) {
            downsample_sum += (int16_t)(input[i * 2] >> 16);
            if (++downsample_count < downsample) continue;
            segment[used / 2] = (int16_t)(downsample_sum / (int32_t)downsample);
            downsample_sum = 0;
            downsample_count = 0;
            used += 2;
            if (used == segment_bytes) {
                if (xRingbufferSend(s_ring, segment, segment_bytes, 0) != pdTRUE) {
                    size_t dropped_size = 0;
                    uint8_t *dropped = xRingbufferReceive(s_ring, &dropped_size, 0);
                    if (dropped) vRingbufferReturnItem(s_ring, dropped);
                    if (xRingbufferSend(s_ring, segment, segment_bytes, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "audio ring full; dropping newest %ums segment", (unsigned)s_segment_ms);
                    }
                }
                used = 0;
            }
        }
    }
done:
    free(segment);
    free(input);
    xEventGroupSetBits(s_events, AUDIO_EXIT_CAPTURE);
    vTaskDelete(NULL);
}

static void network_task(void *arg)
{
    (void)arg;
    esp_websocket_client_config_t config = {
        .uri = s_url,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 5000,
        .ping_interval_sec = 10,
    };
    s_ws = esp_websocket_client_init(&config);
    if (!s_ws) {
        notify_started(false, "WebSocket client initialization failed");
        mark_stream_stopped();
        goto done;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, websocket_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        notify_started(false, "WebSocket client start failed");
        mark_stream_stopped();
        goto destroy;
    }

    TickType_t start_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (s_running) {
        if (!(xEventGroupGetBits(s_events) & AUDIO_WS_CONNECTED)) {
            if (!s_callback_called && (int32_t)(xTaskGetTickCount() - start_deadline) >= 0) {
                notify_started(false, "WebSocket connection timeout");
                mark_stream_stopped();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t size = 0;
        uint8_t *item = xRingbufferReceive(s_ring, &size, pdMS_TO_TICKS(200));
        if (!item) {
            if (!s_callback_called && (int32_t)(xTaskGetTickCount() - start_deadline) >= 0) {
                ESP_LOGE(TAG, "no I2S audio received before start timeout");
                notify_started(false, "I2S audio timeout");
                mark_stream_stopped();
            }
            continue;
        }
        int sent = esp_websocket_client_send_bin(s_ws, (const char *)item, size, pdMS_TO_TICKS(5000));
        vRingbufferReturnItem(s_ring, item);
        if (sent == (int)size) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_segments < UINT32_MAX) s_segments++;
            xSemaphoreGive(s_lock);
            notify_started(true, "first audio segment sent");
        } else ESP_LOGW(TAG, "audio segment send failed (%d/%u)", sent, (unsigned)size);
    }
    esp_websocket_client_stop(s_ws);
destroy:
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
done:
    xEventGroupSetBits(s_events, AUDIO_EXIT_NETWORK);
    vTaskDelete(NULL);
}

esp_err_t audio_stream_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_lock || !s_events) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t audio_stream_start(const char *url, uint32_t segment_ms,
                             audio_stream_started_cb_t callback, void *ctx)
{
    if (!url || (strncmp(url, "ws://", 5) && strncmp(url, "wss://", 6)) ||
        strlen(url) >= sizeof(s_url) || segment_ms < 20 || segment_ms > 2000) return ESP_ERR_INVALID_ARG;
    if (!s_running && s_ring) audio_stream_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_url, url, sizeof(s_url));
    s_segment_ms = segment_ms;
    s_callback = callback;
    s_callback_ctx = ctx;
    s_callback_called = false;
    s_started_at_us = esp_timer_get_time();
    s_duration = 0;
    s_segments = 0;
    xEventGroupClearBits(s_events, AUDIO_EXIT_CAPTURE | AUDIO_EXIT_NETWORK | AUDIO_WS_CONNECTED);
    size_t segment_bytes = AUDIO_RATE * AUDIO_BYTES_PER_SAMPLE * segment_ms / 1000;
    s_ring = xRingbufferCreate(segment_bytes * 10 + 256, RINGBUF_TYPE_NOSPLIT);
    if (!s_ring) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = i2s_start();
    if (err != ESP_OK) {
        vRingbufferDelete(s_ring); s_ring = NULL;
        xSemaphoreGive(s_lock);
        return err;
    }
    s_running = true;
    BaseType_t network_ok = xTaskCreatePinnedToCore(network_task, "audio_ws", 6144, NULL, 6, NULL, 1);
    if (network_ok != pdPASS) {
        s_running = false;
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx); s_rx = NULL;
        vRingbufferDelete(s_ring); s_ring = NULL;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    BaseType_t capture_ok = xTaskCreatePinnedToCore(capture_task, "audio_capture", 4096, NULL, 8, NULL, 0);
    if (capture_ok != pdPASS) {
        s_running = false;
        xEventGroupSetBits(s_events, AUDIO_EXIT_CAPTURE);
        xSemaphoreGive(s_lock);
        audio_stream_stop();
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "stream starting: I2S master, %uHz input -> %uHz PCM, %ums segments, %u bytes",
             (unsigned)AUDIO_INPUT_RATE, (unsigned)AUDIO_RATE,
             (unsigned)segment_ms, (unsigned)segment_bytes);
    return ESP_OK;
}

esp_err_t audio_stream_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running && !s_ring) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) s_duration = elapsed_seconds();
    s_running = false;
    bool needs_callback = !s_callback_called;
    xSemaphoreGive(s_lock);
    EventBits_t bits = xEventGroupWaitBits(s_events, AUDIO_EXIT_CAPTURE | AUDIO_EXIT_NETWORK,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(7000));
    if ((bits & (AUDIO_EXIT_CAPTURE | AUDIO_EXIT_NETWORK)) != (AUDIO_EXIT_CAPTURE | AUDIO_EXIT_NETWORK)) {
        ESP_LOGE(TAG, "stream tasks did not stop cleanly");
        return ESP_ERR_TIMEOUT;
    }
    if (needs_callback) notify_started(false, "recording stopped before first segment");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_ring) { vRingbufferDelete(s_ring); s_ring = NULL; }
    s_callback = NULL;
    s_callback_ctx = NULL;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "stream stopped");
    return ESP_OK;
}

bool audio_stream_is_recording(void)
{
    audio_stream_status_t status;
    audio_stream_get_status(&status);
    return status.recording;
}

void audio_stream_get_status(audio_stream_status_t *status)
{
    if (!status) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    status->recording = s_running;
    status->duration = s_running ? elapsed_seconds() : s_duration;
    status->segments = s_segments;
    xSemaphoreGive(s_lock);
}
