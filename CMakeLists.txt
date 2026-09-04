cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/esp-protocols/components/esp_websocket_client"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_4mic_array_collector)
