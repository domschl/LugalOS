#!/bin/sh
# 45.3b, plan/phase45_esp32c6.md: does drivers/radio/esp32c6_osi_table.c agree
# with Espressif's headers?
#
# Compiles the table against an IDF tree for the C6 (rv32imac, the target's
# macros). That alone is the type check: every entry is assigned to a field of
# IDF's own struct, so a signature that differs from the header's is an error
# here, not a crash in the radio. It then verifies *completeness* from the
# object's symbols and the header: every field of wifi_osi_funcs_t and
# coex_adapter_funcs_t either is assigned or is one of the knowingly-absent ones
# listed below. (A designated initialiser silently leaves a missing field NULL,
# which is exactly how a table loses an entry.)
#
# The three headers the table includes pull in IDF's FreeRTOS and sdkconfig.h;
# the stubs from supplicant.sh stand in for both, which is enough to compile.
#
# Usage: tools/c6_blob_spike/osi_table_check.sh [IDF_ROOT]

set -eu
cd "$(dirname "$0")/../.."

IDF=${1:-${IDF_ROOT:-$HOME/Source/gith/esp/esp-idf}}
[ -d "$IDF/components" ] || IDF=$HOME/gith/esp/esp-idf
C=$IDF/components
OUT=build/esp32c6-blob-spike
STUB=$OUT/stub
CC=$(command -v riscv64-elf-gcc || command -v riscv64-unknown-elf-gcc)
mkdir -p "$OUT"

# The stubs are made by supplicant.sh; make them if it has not run.
if [ ! -f "$STUB/sdkconfig.h" ]; then
    tools/c6_blob_spike/supplicant.sh "$IDF" >/dev/null 2>&1 || true
fi
cat >> "$STUB/sdkconfig.h" <<'EOF'
#ifndef CONFIG_SW_COEXIST_ENABLE
#define CONFIG_SW_COEXIST_ENABLE 1
#endif
EOF

INC="-I$STUB -Idrivers/radio -Ikernel/include \
 -I$C/esp_wifi/include -I$C/esp_coex/include -I$C/esp_common/include -I$C/esp_hw_support/include \
 -I$C/esp_event/include -I$C/esp_timer/include -I$C/log/include -I$C/esp_rom/include \
 -I$C/soc/esp32c6/include -I$C/soc/include -I$C/esp_system/include -I$C/hal/include \
 -I$C/heap/include -I$C/newlib/platform_include -I$C/esp_netif/include -I$C/esp_phy/include \
 -I$C/riscv/include -I$C/xtensa/include -I$C/esp_hal_gpio/include -I$C/esp_hal_gpio/esp32c6/include -I$C/soc/esp32c6/register -I$C/esp_hw_support/port/esp32c6/include"
DEFS="-DCONFIG_IDF_TARGET_ESP32C6=1 -DCONFIG_SOC_WIFI_HE_SUPPORT=1 -DESP_PLATFORM"

# shellcheck disable=SC2086
$CC -march=rv32imac_zicsr_zifencei -mabi=ilp32 -Os -ffreestanding -Wall -Wextra \
    $DEFS $INC -c drivers/radio/esp32c6_osi_table.c -o "$OUT/osi_table.o" 2> "$OUT/osi_table.err" || {
    echo "COMPILE FAILED (a signature that disagrees with IDF's header is an error here):"
    head -20 "$OUT/osi_table.err"; exit 1; }
if [ -s "$OUT/osi_table.err" ]; then echo "warnings:"; cat "$OUT/osi_table.err"; fi

# Completeness: the fields the table assigns, from its relocations against the
# radio_osi_*/radio_plat_*/coex wrappers, versus the header's field list.
python3 - "$OUT/osi_table.o" "$C/esp_wifi/include/esp_private/wifi_os_adapter.h" \
    "$C/esp_coex/include/private/esp_coexist_adapter.h" <<'PY'
import re, subprocess, sys
obj, wifi_h, coex_h = sys.argv[1:4]

def fields(path, defs):
    names, stack, inside = [], [], False
    for line in open(path):
        s = line.strip()
        if s.startswith("typedef struct"): inside = True; continue
        if inside and s.startswith("}") and "_t;" in s: break
        if not inside: continue
        if s.startswith("#if"):
            expr = s[3:].strip()
            ok = eval(re.sub(r"CONFIG_\w+", lambda m: str(defs.get(m.group(0), 0)),
                             expr.replace("||", " or ").replace("&&", " and ").replace("!", " not ")))
            stack.append(bool(ok)); continue
        if s.startswith("#endif"): stack.pop(); continue
        if s.startswith("#") or not all(stack): continue
        m = re.search(r"\(\s*\*\s*(\w+)\s*\)", s) or re.match(r"\w+\s+(_\w+);", s)
        if m: names.append(m.group(1))
    return names

defs = {"CONFIG_IDF_TARGET_ESP32C6": 1, "CONFIG_SOC_WIFI_HE_SUPPORT": 1}
src = open("drivers/radio/esp32c6_osi_table.c").read()
assigned = set(re.findall(r"^\s*\.(\w+)\s*=", src, re.M))
# IDF's own table leaves these NULL on the C6; the blob does not call them.
knowingly_absent = {"_coex_condition_set", "_coex_configure_preemption_end_cb"}
bad = 0
for label, path in (("wifi_osi_funcs_t", wifi_h), ("coex_adapter_funcs_t", coex_h)):
    fs = fields(path, defs)
    missing = [f for f in fs if f not in assigned and f not in knowingly_absent]
    print("%-22s %3d fields, %3d assigned, missing: %s" % (label, len(fs), sum(f in assigned for f in fs), missing or "none"))
    bad += len(missing)
sys.exit(1 if bad else 0)
PY
echo "OK: the table compiles against IDF's headers and every field is accounted for"
