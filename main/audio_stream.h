#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*audio_stream_started_cb_t)(bool success, const char *detail, void *ctx);

esp_err_t audio_stream_init(void);
esp_err_t audio_stream_start(const char *url, uint32_t segment_ms,
                             audio_stream_started_cb_t callback, void *ctx);
esp_err_t audio_stream_stop(void);
bool audio_stream_is_recording(void);
