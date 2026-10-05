#!/bin/sh
# Build and run tools/minimal_esp32c6.c -- 45.1, plan/phase45_esp32c6.md.
#
# Like tools/build_minimal_esp32p4.sh, a shell script rather than a CMake
# target: the C6 has no CMake preset until 45.4.
#
# Usage:
#   tools/build_minimal_esp32c6.sh        build only
#   tools/build_minimal_esp32c6.sh run    build, load into RAM, watch the console
#
# `run` never writes flash: esptool load-ram delivers the image into HP SRAM and
# jumps to it. A reset restores whatever is in flash.

set -eu

cd "$(dirname "$0")/.."
OUT=build/esp32c6-minimal

find_tool() {
    for n in "$@"; do
        if command -v "$n" >/dev/null 2>&1; then command -v "$n"; return 0; fi
    done
    echo "no RISC-V cross tool found (tried: $*)" >&2; exit 1
}
CC=$(find_tool riscv64-elf-gcc riscv-none-elf-gcc riscv32-elf-gcc \
               riscv64-unknown-elf-gcc riscv32-unknown-elf-gcc riscv64-linux-gnu-gcc)
ARCH="-march=rv32imac_zicsr_zifencei -mabi=ilp32"

mkdir -p "$OUT"

$CC $ARCH -mcmodel=medany -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -Os -ggdb \
    -T tools/minimal_esp32c6.ld \
    -Wl,--gc-sections -Wl,-Map,"$OUT/minimal_esp32c6.map" \
    tools/minimal_esp32c6_entry.S tools/minimal_esp32c6.c \
    -o "$OUT/minimal_esp32c6.elf"

echo "built $OUT/minimal_esp32c6.elf"
"${CC%gcc}size" "$OUT/minimal_esp32c6.elf" 2>/dev/null || true

[ "${1:-}" = "run" ] || exit 0
exec tools/c6run.py "$OUT/minimal_esp32c6.elf" --listen-secs 8
