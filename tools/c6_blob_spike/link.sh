#!/bin/sh
# 45.2, plan/phase45_esp32c6.md: link Espressif's ESP32-C6 Wi-Fi/PHY/coexistence
# archives with --gc-sections and *no* OS and *no* IDF code, and report what
# the blob costs and what it asks the outside world for.
#
# Output goes to build/esp32c6-blob-spike/. Nothing here is run on a chip; it
# is a static measurement. Usage: tools/c6_blob_spike/link.sh [IDF_ROOT]
#
# Roots are the Wi-Fi entry points a station that scans, joins and passes
# frames needs. Anything not reachable from them (mesh, ESP-NOW, SoftAP, FTM
# responder, ...) is discarded by --gc-sections and does not count.

set -eu
cd "$(dirname "$0")/../.."

IDF=${1:-${IDF_ROOT:-$HOME/Source/gith/esp/esp-idf}}
[ -d "$IDF/components/esp_wifi/lib/esp32c6" ] || IDF=$HOME/gith/esp/esp-idf
C=$IDF/components
ROM=$C/esp_rom/esp32c6/ld
OUT=build/esp32c6-blob-spike
mkdir -p "$OUT"

CC=$(command -v riscv64-elf-gcc || command -v riscv64-unknown-elf-gcc)

ROOTS=""
for r in esp_wifi_init_internal esp_wifi_deinit_internal esp_wifi_start esp_wifi_stop \
         esp_wifi_set_mode esp_wifi_set_config esp_wifi_get_config \
         esp_wifi_scan_start esp_wifi_scan_get_ap_records esp_wifi_scan_get_ap_num \
         esp_wifi_connect_internal esp_wifi_disconnect_internal \
         esp_wifi_internal_reg_rxcb esp_wifi_internal_tx esp_wifi_internal_free_rx_buffer \
         esp_wifi_get_macaddr_internal esp_wifi_set_country_code esp_wifi_set_ps \
         wifi_osi_funcs_register wifi_osi_ready \
         register_chipv7_phy phy_get_romfunc_addr \
         coex_pre_init coex_init coex_deinit coex_enable coex_disable coex_status_get \
         coex_wifi_request coex_wifi_release coex_wifi_channel_set coex_event_duration_get \
         coex_pti_get coex_schm_status_bit_clear coex_schm_status_bit_set \
         coex_schm_interval_set coex_schm_interval_get coex_schm_curr_period_get \
         coex_schm_curr_phase_get coex_schm_process_restart coex_schm_register_callback \
         coex_register_start_cb coex_schm_flexible_period_set coex_schm_flexible_period_get \
         coex_schm_get_phase_by_idx coex_rom_osi_funcs_init coex_rom_data_init; do
    ROOTS="$ROOTS -Wl,-u,$r"
done

# The chip's ROM contains pp, net80211, phy and coexist code and *data*; these
# scripts are how the blob's references to it resolve. The data symbols
# (g_osi_funcs_p and friends) are the interesting part: see census.py.
LDS=""
for f in rom rom.pp rom.net80211 rom.phy rom.coexist rom.libc rom.newlib rom.libgcc rom.api; do
    LDS="$LDS -Wl,-T,$ROM/esp32c6.$f.ld"
done

# --warn-unresolved-symbols: we *want* the list of what is missing.
# shellcheck disable=SC2086
$CC -march=rv32imac_zicsr_zifencei -mabi=ilp32 -nostdlib \
    -T tools/c6_blob_spike/spike.ld $LDS \
    -Wl,--gc-sections -Wl,--warn-unresolved-symbols -Wl,--no-warn-rwx-segments \
    -Wl,-Map,"$OUT/blob.map" $ROOTS \
    -Wl,--start-group \
      "$C/esp_wifi/lib/esp32c6/libnet80211.a" "$C/esp_wifi/lib/esp32c6/libpp.a" \
      "$C/esp_wifi/lib/esp32c6/libcore.a" "$C/esp_phy/lib/esp32c6/libphy.a" \
      "$C/esp_coex/lib/esp32c6/libcoexist.a" \
    -Wl,--end-group -o "$OUT/blob.elf" > "$OUT/link.log" 2>&1 || true

grep -o "undefined reference to .*" "$OUT/link.log" | sed "s/undefined reference to .//; s/'\$//" \
    | sort -u > "$OUT/unresolved.txt"
"${CC%gcc}size" "$OUT/blob.elf"
echo "unresolved symbols: $(wc -l < "$OUT/unresolved.txt")  (list: $OUT/unresolved.txt)"
echo "IDF: $IDF"
