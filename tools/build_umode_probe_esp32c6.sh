#!/bin/sh
# Build and run tools/umode_probe_esp32c6.c -- 45.3, plan/phase45_esp32c6.md.
# Usage: tools/build_umode_probe_esp32c6.sh [run]. Never writes flash.
set -eu
cd "$(dirname "$0")/.."
OUT=build/esp32c6-umode-probe
find_tool() { for n in "$@"; do command -v "$n" >/dev/null 2>&1 && { command -v "$n"; return 0; }; done
              echo "no RISC-V cross tool found (tried: $*)" >&2; exit 1; }
CC=$(find_tool riscv64-elf-gcc riscv-none-elf-gcc riscv32-elf-gcc riscv64-unknown-elf-gcc riscv64-linux-gnu-gcc)
mkdir -p "$OUT"
# -fno-jump-tables: the U-mode functions live in one granted page; a jump table
# in ordinary .rodata would be outside it (drivers/README.md).
$CC -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -ffreestanding -nostdlib -fno-builtin \
    -fno-jump-tables -Wall -Wextra -Os -ggdb -T tools/umode_probe_esp32c6.ld \
    -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map,"$OUT/probe.map" \
    tools/umode_probe_esp32c6_entry.S tools/umode_probe_esp32c6.c -o "$OUT/probe.elf"
echo "built $OUT/probe.elf"; "${CC%gcc}size" "$OUT/probe.elf"
[ "${1:-}" = "run" ] || exit 0
exec tools/c6run.py "$OUT/probe.elf" --listen-secs 9 --expect "[C6_UPROBE]"
