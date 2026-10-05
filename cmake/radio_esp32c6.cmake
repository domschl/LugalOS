# The Wi-Fi radio of the ESP32-C6 (45.6, plan/phase45_esp32c6.md): Espressif's
# precompiled blob, the open code around it, and what it takes to compile and
# link them into the kernel image.
#
# Optional by construction. It needs an ESP-IDF tree for two things only -- its
# *headers* (the OS-table structs and wifi_init_config_t, whose layout the blob
# checks by md5 at run time) and its *archives* (libpp, libnet80211, libcore,
# libphy, libcoexist, plus the two that hold a stray symbol each). Nothing of
# IDF's code is compiled except two data tables. With no IDF the kernel builds
# without a radio, exactly as before 45.6, and `radio` says so.
#
# IDF is found at $IDF_ROOT, ~/Source/gith/esp/esp-idf or ~/gith/esp/esp-idf
# (AGENTS.md section 1: both roots are valid).

set(LUGALOS_RADIO_C6 OFF)
set(_idf_candidates "$ENV{IDF_ROOT}" "$ENV{HOME}/Source/gith/esp/esp-idf" "$ENV{HOME}/gith/esp/esp-idf")
foreach(_c IN LISTS _idf_candidates)
    if(_c AND EXISTS "${_c}/components/esp_wifi/lib/esp32c6/libpp.a")
        set(LUGALOS_IDF_ROOT "${_c}")
        set(LUGALOS_RADIO_C6 ON)
        break()
    endif()
endforeach()

if(LUGALOS_RADIO_C6)
    set(_C "${LUGALOS_IDF_ROOT}/components")
    set(_STUB "${CMAKE_CURRENT_BINARY_DIR}/radio_stub")
    file(MAKE_DIRECTORY "${_STUB}/freertos")

    # IDF's headers reach for FreeRTOS and an sdkconfig.h. Neither is wanted --
    # the radio's OS is the table in esp32c6_osi_table.c -- so both are stubbed to
    # exactly the types the headers need to *compile*. The CONFIG_ESP_WIFI_*
    # values are the ones Espressif's own default for this chip gives (read off
    # the Waveshare ESP32-C6-Zero kit's softap sdkconfig): the blob is built for
    # them, so a different value is a different, untested radio. Sizing them
    # down is a measured decision for later, made here and nowhere else.
    file(WRITE "${_STUB}/freertos/FreeRTOS.h" [=[
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void *SemaphoreHandle_t; typedef void *QueueHandle_t; typedef void *TaskHandle_t;
typedef void *EventGroupHandle_t; typedef void *TimerHandle_t; typedef uint32_t TickType_t;
typedef int BaseType_t; typedef unsigned UBaseType_t; typedef uint32_t EventBits_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(x) (x)
typedef struct { int dummy; } portMUX_TYPE;
]=])
    foreach(_h task semphr queue event_groups portmacro timers)
        file(WRITE "${_STUB}/freertos/${_h}.h" "#include \"freertos/FreeRTOS.h\"\n")
    endforeach()
    file(MAKE_DIRECTORY "${_STUB}/sys")
    file(WRITE "${_STUB}/sys/lock.h" "#pragma once\n#include_next <sys/lock.h>\n#ifndef _LOCK_T_DEFINED_BY_RADIO\n#define _LOCK_T_DEFINED_BY_RADIO\ntypedef int _lock_t;\n#endif\n")
    file(WRITE "${_STUB}/sdkconfig.h" [=[
#pragma once
#define CONFIG_IDF_TARGET_ESP32C6 1
#define CONFIG_SOC_WIFI_HE_SUPPORT 1
#define CONFIG_LOG_DEFAULT_LEVEL 3
#define CONFIG_LOG_MAXIMUM_LEVEL 3
#define CONFIG_LOG_VERSION 1
#define CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT 1
#define CONFIG_LOG_TIMESTAMP_SOURCE_RTOS 1
#define CONFIG_SW_COEXIST_ENABLE 1
#define CONFIG_ESP_COEX_ENABLED 1
#define CONFIG_ESP_PHY_ENABLED 1
#define CONFIG_ESP_WIFI_ENABLED 1
#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM 10
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 32
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER 1
#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE 1
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM 32
#define CONFIG_ESP_WIFI_STATIC_RX_MGMT_BUFFER 1
#define CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF 0
#define CONFIG_ESP_WIFI_RX_MGMT_BUF_NUM_DEF 5
#define CONFIG_ESP_WIFI_AMPDU_TX_ENABLED 1
#define CONFIG_ESP_WIFI_TX_BA_WIN 6
#define CONFIG_ESP_WIFI_AMPDU_RX_ENABLED 1
#define CONFIG_ESP_WIFI_RX_BA_WIN 6
#define CONFIG_ESP_WIFI_SOFTAP_BEACON_MAX_LEN 752
#define CONFIG_ESP_WIFI_MGMT_SBUF_NUM 32
#define CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE 1
#define CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM 7
#define CONFIG_ESP_WIFI_TX_HETB_QUEUE_NUM 3
#define CONFIG_ESP_WIFI_SOFTAP_SUPPORT 1
#define CONFIG_ESP_WIFI_FTM_ENABLE 1
#define CONFIG_ESP_PHY_MAX_WIFI_TX_POWER 20
#define CONFIG_ESP_PHY_MAX_TX_POWER 20
]=])

    set(_ARCH "${_C}/esp_wifi/lib/esp32c6")
    set(_ORIG_ARCHIVES
        "${_ARCH}/libnet80211.a" "${_ARCH}/libpp.a" "${_ARCH}/libcore.a"
        "${_ARCH}/libmesh.a" "${_ARCH}/libespnow.a"
        "${_C}/esp_phy/lib/esp32c6/libphy.a" "${_C}/esp_coex/lib/esp32c6/libcoexist.a")

    # The blob calls memcpy, strlen... by name, and in this image those names are
    # the kernel's. tools/c6_radio_libs.py renames the blob's libc/ROM imports in
    # *copies* of the archives (radio_<name>) and generates the script that gives
    # the ROM ones their ROM addresses -- see its header for why neither the
    # kernel's libc nor Espressif's ROM scripts can be linked as they are. It runs
    # at configure time: the inputs are a fixed IDF tree.
    set(_RLIBS "${CMAKE_CURRENT_BINARY_DIR}/radio_libs")
    execute_process(
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/c6_radio_libs.py ${_RLIBS}
                ${_C}/esp_rom/esp32c6/ld ${_ORIG_ARCHIVES}
        RESULT_VARIABLE _rl_rc OUTPUT_VARIABLE _rl_out ERROR_VARIABLE _rl_err)
    if(NOT _rl_rc EQUAL 0)
        message(FATAL_ERROR "tools/c6_radio_libs.py failed: ${_rl_err}")
    endif()
    string(STRIP "${_rl_out}" _rl_out)
    message(STATUS "${_rl_out}")
    set(LUGALOS_RADIO_ARCHIVES "")
    foreach(_a IN LISTS _ORIG_ARCHIVES)
        get_filename_component(_n "${_a}" NAME)
        list(APPEND LUGALOS_RADIO_ARCHIVES "${_RLIBS}/${_n}")
    endforeach()
    set(LUGALOS_RADIO_ROM_LD "-Wl,-T,${_RLIBS}/radio_rom.ld")

    # Everything that includes an IDF header is compiled with these, as system
    # headers (their warnings are Espressif's, not ours).
    set(LUGALOS_RADIO_INC
        "${_STUB}" "${CMAKE_CURRENT_SOURCE_DIR}/drivers/radio" "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include"
        "${_C}/esp_wifi/include" "${_C}/esp_coex/include" "${_C}/esp_common/include"
        "${_C}/esp_hw_support/include" "${_C}/esp_event/include" "${_C}/esp_timer/include"
        "${_C}/log/include" "${_C}/esp_rom/include" "${_C}/soc/esp32c6/include" "${_C}/soc/include"
        "${_C}/soc/esp32c6/register" "${_C}/esp_system/include" "${_C}/hal/include" "${_C}/heap/include"
        "${_C}/newlib/platform_include" "${_C}/esp_netif/include" "${_C}/esp_phy/include"
        "${_C}/riscv/include" "${_C}/esp_hal_gpio/include" "${_C}/esp_hal_gpio/esp32c6/include"
        "${_C}/esp_hw_support/port/esp32c6/include" "${_C}/esp_wifi/include/esp_private"
        "${_C}/esp_wifi/include/local" "${_C}/esp_wifi/regulatory" "${_C}/hal/esp32c6/include" "${_C}/hal/platform_port/include" "${_C}/esp_phy/esp32c6/include" "${_C}/esp_hal_pmu/esp32c6/include" "${_C}/esp_hal_pmu/include")
    set(LUGALOS_RADIO_DEFS CONFIG_IDF_TARGET_ESP32C6=1 CONFIG_SOC_WIFI_HE_SUPPORT=1 ESP_PLATFORM=1)
    set(LUGALOS_RADIO_IDF_SOURCES
        "${_C}/esp_wifi/regulatory/esp_wifi_regulatory.c"
        "${_C}/esp_wifi/src/ftm_load_calibration.c"
        "${_C}/esp_phy/esp32c6/phy_init_data.c")
    message(STATUS "C6 radio: ESP-IDF at ${LUGALOS_IDF_ROOT}")
else()
    message(STATUS "C6 radio: no ESP-IDF tree found (set IDF_ROOT) -- building without the Wi-Fi blob")
endif()
