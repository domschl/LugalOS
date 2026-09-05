#!/bin/sh
# Build and run tools/minimal_esp32p4.c -- E1,
# plan/phase27_esp32p4_bringup.md.
#
# A shell script rather than a CMake target, because there is no ESP32-P4
# CMake target yet: cmake/toolchain-esp32p4.cmake is E2's work, and E1 is
# specified as the standalone thing that runs *before* the build system knows
# this board exists. It gets folded into CMake when E2 gives it somewhere to
# be folded into.
#
# Usage:
#   tools/build_minimal_esp32p4.sh            build only
#   tools/build_minimal_esp32p4.sh run        build, then load into RAM and run
#
# **This never writes flash.** `esptool load-ram` delivers the image over the
# download protocol into L2MEM and jumps to it; the factory image, or whatever
# else is in flash, is untouched. A power cycle restores the board exactly.
#
# Entering download mode is manual:
#
#   hold BOOT, press and release RESET, release BOOT
#
# then run this with "run". Press RESET afterwards to return the board to
# whatever is in flash.
#
# **The board does have an auto-reset circuit and it still does not help.**
# The schematic shows U6, an EMH4T2R transistor pair wiring the CH343P's RTS
# to ESP_EN and DTR to GPIO35 -- the standard ESP arrangement, which normally
# lets esptool reset the board into download mode with no buttons at all.
# Driving those lines from macOS does nothing: four polarity/timing
# combinations were tried against the ROM banner on 2026-09-05 and not one
# produced a reset, which points at the built-in CH34x driver not carrying
# modem-control lines rather than at the board. Worth retrying under Linux;
# until then, the buttons are the mechanism.
#
# Do NOT reach for the other USB socket to avoid the buttons. Resetting over
# the native USB-Serial-JTAG port does work, but it leaves UART0 in a state
# where our own output comes back corrupted -- see the E1 notes in
# plan/phase27_esp32p4_bringup.md. It cost most of an afternoon.
#
# Console: /dev/cu.usbserial-0001 (the CH343P, wired to UART0 on GPIO37/38)
# at 115200 8N1 -- the ROM's own pins and baud, which is why this program does
# not configure the UART at all.

set -eu

cd "$(dirname "$0")/.."
OUT=build/esp32p4-minimal
PORT="${LUGALOS_P4_PORT:-/dev/cu.usbserial-0001}"

# The toolchain already installed for every other target in this tree. The P4
# is RV32IMAFC (revision <v3 has no Zb; E0 §5), and we compile the IMAC subset
# -- no float, no bit-manipulation, nothing the kernel would not also use.
CC=$(command -v riscv64-elf-gcc || command -v riscv-none-elf-gcc || command -v riscv32-elf-gcc)
OBJCOPY=$(command -v riscv64-elf-objcopy || command -v riscv-none-elf-objcopy)
ARCH="-march=rv32imac_zicsr_zifencei -mabi=ilp32"

mkdir -p "$OUT"

$CC $ARCH -mcmodel=medany -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -Os -ggdb \
    -T tools/minimal_esp32p4.ld \
    -Wl,--gc-sections -Wl,-Map,"$OUT/minimal_esp32p4.map" \
    tools/minimal_esp32p4_entry.S tools/minimal_esp32p4.c \
    -o "$OUT/minimal_esp32p4.elf"

$OBJCOPY -O binary "$OUT/minimal_esp32p4.elf" "$OUT/minimal_esp32p4.bin"
echo "built $OUT/minimal_esp32p4.elf"
riscv64-elf-size "$OUT/minimal_esp32p4.elf" 2>/dev/null || true

[ "${1:-}" = "run" ] || exit 0

# elf2image turns the ELF into the segment-and-header format the ROM loader
# speaks; load-ram then delivers those segments and jumps to the entry point.
# --no-stub: the stub flasher is a program that also wants to live in RAM, and
# there is no reason to have two.
esp() { uv tool run --from esptool "$@"; }

esp esptool --chip esp32p4 elf2image \
    -o "$OUT/minimal_esp32p4.img" "$OUT/minimal_esp32p4.elf"

echo
echo ">>> Put the board in download mode now:"
echo ">>>   hold BOOT, press and release RESET, release BOOT"
echo
esp esptool --chip esp32p4 --port "$PORT" --before no-reset --after no-reset \
    --no-stub load-ram "$OUT/minimal_esp32p4.img"
