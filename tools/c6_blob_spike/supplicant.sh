#!/bin/sh
# 45.2, plan/phase45_esp32c6.md: how big is the open-source half of WPA2-PSK?
#
# The Wi-Fi blob does not do the WPA handshake: that is IDF's wpa_supplicant
# component (open source), which the blob calls back into. This compiles -- does
# not link or run -- the files a *station* with WPA2-PSK needs, with the
# supplicant's own internal crypto (CONFIG_CRYPTO_INTERNAL, no mbedtls), and
# sums their size. Not SAE/WPA3, not EAP/enterprise, not WPS, not SoftAP.
#
# IDF's headers reach for FreeRTOS and an sdkconfig.h; both are stubbed in
# build/esp32c6-blob-spike/stub/ -- enough to *compile*, which is all this
# measures. Whether the result links is 45.7's question.

set -eu
cd "$(dirname "$0")/../.."
IDF=${1:-${IDF_ROOT:-$HOME/Source/gith/esp/esp-idf}}
[ -d "$IDF/components" ] || IDF=$HOME/gith/esp/esp-idf
C=$IDF/components
OUT=build/esp32c6-blob-spike
STUB=$OUT/stub
mkdir -p "$STUB/freertos" "$OUT/sup"
CC=$(command -v riscv64-elf-gcc || command -v riscv64-unknown-elf-gcc)

cat > "$STUB/freertos/FreeRTOS.h" <<'EOF'
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
EOF
for h in task semphr queue event_groups portmacro timers; do
    echo '#include "freertos/FreeRTOS.h"' > "$STUB/freertos/$h.h"
done
cat > "$STUB/sdkconfig.h" <<'EOF'
#define CONFIG_LOG_DEFAULT_LEVEL 3
#define CONFIG_LOG_MAXIMUM_LEVEL 3
#define CONFIG_LOG_VERSION 1
#define CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT 1
#define CONFIG_LOG_TIMESTAMP_SOURCE_RTOS 1
#define CONFIG_IDF_TARGET_ESP32C6 1
#define CONFIG_SOC_WIFI_HE_SUPPORT 1
EOF

# Every component's public include dir, except the ones that would shadow our
# stubs or pull in another target.
INC="-I$STUB"
for d in "$C"/*/include "$C"/*/port/include "$C"/*/include/esp32c6 "$C"/*/esp32c6/include \
         "$C"/*/*/include "$C"/soc/esp32c6/register "$C"/esp_hw_support/port/esp32c6 "$C"/esp_system/port/include; do
    case "$d" in */linux/*|*/freertos/*|*/newlib/*|*esp32c2*|*esp32s*|*esp32c3*|*esp32h*|*esp32p4*|*esp32c5*|*esp32c61*|*/esp32/*) continue;; esac
    [ -d "$d" ] && INC="$INC -I$d"
done
W=$C/wpa_supplicant
INC="$INC -I$W/src -I$W/src/utils -I$W/esp_supplicant/src -I$W/esp_supplicant/include/esp_private \
 -I$C/esp_wifi/include/local -I$C/esp_wifi/include/esp_private \
 -I$C/esp_wifi/wifi_apps/roaming_app/include -I$C/esp_wifi/wifi_apps/include"
DEFS="-D__ets__ -DESP_SUPPLICANT -DIEEE8021X_EAPOL -DESPRESSIF_USE -DCONFIG_IEEE80211W \
 -DCONFIG_SHA256 -DCONFIG_NO_RADIUS -DCONFIG_CRYPTO_INTERNAL -DESP_PLATFORM"

FILES="src/rsn_supp/wpa.c src/rsn_supp/wpa_ie.c src/rsn_supp/pmksa_cache.c
 src/common/wpa_common.c src/common/ieee802_11_common.c
 src/utils/common.c src/utils/wpabuf.c src/utils/wpa_debug.c
 src/crypto/sha1-prf.c src/crypto/sha256-prf.c src/crypto/sha1-pbkdf2.c src/crypto/sha1.c
 src/crypto/sha1-internal.c src/crypto/sha256.c src/crypto/sha256-internal.c
 src/crypto/md5.c src/crypto/md5-internal.c src/crypto/aes-wrap.c src/crypto/aes-unwrap.c
 src/crypto/aes-internal.c src/crypto/aes-internal-enc.c src/crypto/aes-internal-dec.c
 src/crypto/aes-omac1.c src/crypto/ccmp.c src/crypto/aes-ccm.c src/crypto/crypto_ops.c
 esp_supplicant/src/esp_wpa_main.c esp_supplicant/src/esp_wpas_glue.c esp_supplicant/src/esp_common.c"

rm -f "$OUT"/sup/*.o
fail=0
for f in $FILES; do
    n=$(basename "$f" .c)
    # shellcheck disable=SC2086
    if ! $CC -march=rv32imac_zicsr_zifencei -mabi=ilp32 -Os -ffunction-sections -fdata-sections \
         $DEFS $INC -c "$W/$f" -o "$OUT/sup/$n.o" 2> "$OUT/sup/$n.err"; then
        echo "FAIL $f: $(grep -m1 error "$OUT/sup/$n.err" | cut -c1-150)"; fail=1
    fi
done
"${CC%gcc}size" -t "$OUT"/sup/*.o | sort -k4 -n -r | head -12
[ $fail = 0 ] || exit 1
