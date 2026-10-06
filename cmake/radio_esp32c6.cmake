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
extern void radio_osi_task_delay(uint32_t);
#define vTaskDelay(t) radio_osi_task_delay(t)
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
#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM 8
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 16
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER 1
#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE 1
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM 16
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
        "${_C}/esp_wifi/include/local" "${_C}/esp_wifi/regulatory" "${_C}/hal/esp32c6/include" "${_C}/hal/platform_port/include" "${_C}/esp_phy/esp32c6/include" "${_C}/esp_hal_clock/esp32c6/include" "${_C}/esp_hal_clock/include" "${_C}/esp_hal_pmu/esp32c6/include" "${_C}/esp_hal_pmu/include" "${_C}/esp_hal_regi2c/esp32c6/include" "${_C}/esp_hal_regi2c/include" "${_C}/esp_rom/esp32c6" "${_C}/esp_rom/esp32c6/include" "${_C}/soc/esp32c6/include/modem" "${_C}/esp_hw_support/port/esp32c6/private_include" "${_C}/esp_hw_support/port/esp32c6" "/home/dsc/Source/gith/esp/esp-idf/components/esp_hw_support/port/include" "${_C}/esp_hw_support/include/esp_private" "${_C}/esp_rom/esp32c6/include/esp32c6")
    set(LUGALOS_RADIO_DEFS CONFIG_IDF_TARGET_ESP32C6=1 CONFIG_SOC_WIFI_HE_SUPPORT=1 ESP_PLATFORM=1)
    set(LUGALOS_RADIO_IDF_SOURCES
        "${_C}/esp_wifi/regulatory/esp_wifi_regulatory.c"
        "${_C}/esp_wifi/src/ftm_load_calibration.c"
        "${_C}/esp_phy/esp32c6/phy_init_data.c"
        "${_C}/esp_hal_regi2c/esp32c6/regi2c_impl.c"
        "${_C}/hal/esp32c6/efuse_hal.c" "${_C}/hal/efuse_hal.c" "${_C}/esp_hw_support/port/esp32c6/pmu_param.c")
    set(LUGALOS_RADIO_PMU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/drivers/radio/radio_pmu_init.c")
    # The supplicant: IDF's wpa_supplicant (station, WPA2-PSK, its own crypto -- no mbedtls, no
    # SAE/EAP/WPS/SoftAP), compiled into the radio domain. tools/c6_blob_spike/supplicant.sh
    # measured it at ~46 KB of text; 45.7 links it.
    set(_W "${_C}/wpa_supplicant")
    set(LUGALOS_RADIO_SUP_SOURCES "")
    foreach(_f src/rsn_supp/wpa.c src/rsn_supp/wpa_ie.c src/rsn_supp/pmksa_cache.c
               src/common/wpa_common.c src/common/ieee802_11_common.c
               src/utils/common.c src/utils/wpabuf.c src/utils/wpa_debug.c
               src/crypto/sha1-prf.c src/crypto/sha256-prf.c src/crypto/sha1-pbkdf2.c src/crypto/sha1.c
               src/crypto/sha1-internal.c src/crypto/sha256.c src/crypto/sha256-internal.c
               src/crypto/md5.c src/crypto/md5-internal.c src/crypto/rc4.c src/crypto/aes-wrap.c
               src/crypto/aes-unwrap.c src/crypto/aes-internal.c src/crypto/aes-internal-enc.c
               src/crypto/aes-internal-dec.c src/crypto/aes-omac1.c src/crypto/ccmp.c
               src/crypto/aes-ccm.c src/crypto/aes-cbc.c src/crypto/crypto_ops.c
               esp_supplicant/src/esp_wpa_main.c esp_supplicant/src/esp_wpas_glue.c
               esp_supplicant/src/esp_common.c port/eloop.c)
        list(APPEND LUGALOS_RADIO_SUP_SOURCES "${_W}/${_f}")
    endforeach()
    set(LUGALOS_RADIO_SUP_INC
        "${_W}/src" "${_W}/src/utils" "${_W}/esp_supplicant/src" "${_W}/esp_supplicant/include"
        "${_W}/esp_supplicant/include/esp_private" "${_W}/port/include" "${_W}/include" "${_W}/include/esp_supplicant"
        "${_C}/esp_wifi/include/local" "${_C}/esp_wifi/wifi_apps/include" "${_C}/esp_wifi/wifi_apps/roaming_app/include" "${_C}/esp_rom/esp32c6/include/esp32c6")
    # ...and the supplicant reaches for most of IDF's public headers: the spike's rule, every
    # component's include dirs for this chip, none for another target or for the OS layers.
    file(GLOB _sup_globs "${_C}/*/include" "${_C}/*/port/include" "${_C}/*/include/esp32c6"
                         "${_C}/*/esp32c6/include" "${_C}/*/*/include" "${_C}/esp_hw_support/port/esp32c6"
                         "${_C}/esp_system/port/include" "${_C}/soc/esp32c6/register")
    foreach(_d IN LISTS _sup_globs)
        if(_d MATCHES "/linux/|/freertos/|/newlib/|esp32c2|esp32s|esp32c3|esp32h|esp32p4|esp32c5|esp32c61|/esp32/")
            continue()
        endif()
        list(APPEND LUGALOS_RADIO_SUP_INC "${_d}")
    endforeach()
    set(LUGALOS_RADIO_SUP_DEFS ESP_SUPPLICANT IEEE8021X_EAPOL ESPRESSIF_USE CONFIG_IEEE80211W CONFIG_SHA256
        CONFIG_NO_RADIUS CONFIG_CRYPTO_INTERNAL __ets__
        # The kernel has its own SHA-256 under the same names (kernel/sha256.c), in kernel
        # text; the supplicant's copies are the radio domain's and take another name.
        sha256_init=wpa_sha256_init sha256_update=wpa_sha256_update sha256_final=wpa_sha256_final
        hmac_sha256=wpa_hmac_sha256 sha256=wpa_sha256)
    message(STATUS "C6 radio: ESP-IDF at ${LUGALOS_IDF_ROOT}")
else()
    message(STATUS "C6 radio: no ESP-IDF tree found (set IDF_ROOT) -- building without the Wi-Fi blob")
endif()
