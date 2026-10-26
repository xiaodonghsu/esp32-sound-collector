#pragma once

#include "sdkconfig.h"

/* XIAO header positions stay the same; ESP-IDF expects chip GPIO numbers. */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define BOARD_PIN_D4   5
#define BOARD_PIN_D5   6
#define BOARD_PIN_D6   43
#define BOARD_PIN_D7   44
#define BOARD_PIN_D8   7
#define BOARD_PIN_D9   8
#define BOARD_PIN_D10  9
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
#define BOARD_PIN_D4   23
#define BOARD_PIN_D5   24
#define BOARD_PIN_D6   11
#define BOARD_PIN_D7   12
#define BOARD_PIN_D8   8
#define BOARD_PIN_D9   9
#define BOARD_PIN_D10  10
#else
#error "Unsupported XIAO target: select esp32s3 or esp32c5"
#endif

/* XVF3800 control and audio wiring, from the ESP32's perspective. */
#define BOARD_XVF_I2C_SDA    BOARD_PIN_D4
#define BOARD_XVF_I2C_SCL    BOARD_PIN_D5
#define BOARD_AUDIO_I2S_BCLK BOARD_PIN_D9
#define BOARD_AUDIO_I2S_WS   BOARD_PIN_D8
#define BOARD_AUDIO_I2S_DOUT BOARD_PIN_D7
#define BOARD_AUDIO_I2S_DIN  BOARD_PIN_D6
