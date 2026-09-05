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
# Console: the CH343P bridge, wired to UART0 on GPIO37/38, at 115200 8N1 --
# the ROM's own pins and baud, which is why this program does not configure the
# UART at all. /dev/ttyUSB0 on Linux, /dev/cu.usbserial-* on macOS;
# tools/p4run.py finds it.

set -eu

cd "$(dirname "$0")/.."
OUT=build/esp32p4-minimal

# The toolchain already installed for every other target in this tree. The P4
# is RV32IMAFC (revision <v3 has no Zb; E0 §5), and we compile the IMAC subset
# -- no float, no bit-manipulation, nothing the kernel would not also use.
#
# The search order matches cmake/toolchain-rp2350.cmake's, plus the names
# Linux distributions actually ship, so this builds on either host.
find_tool() {
    for n in "$@"; do
        if command -v "$n" >/dev/null 2>&1; then command -v "$n"; return 0; fi
    done
    echo "no RISC-V cross tool found (tried: $*)" >&2; exit 1
}
CC=$(find_tool riscv64-elf-gcc riscv-none-elf-gcc riscv32-elf-gcc \
               riscv64-unknown-elf-gcc riscv32-unknown-elf-gcc riscv64-linux-gnu-gcc)
OBJCOPY=$(find_tool riscv64-elf-objcopy riscv-none-elf-objcopy riscv32-elf-objcopy \
                    riscv64-unknown-elf-objcopy riscv64-linux-gnu-objcopy)
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
"${CC%gcc}size" "$OUT/minimal_esp32p4.elf" 2>/dev/null || true

[ "${1:-}" = "run" ] || exit 0

# elf2image turns the ELF into the segment-and-header format the ROM loader
# speaks; load-ram then delivers those segments and jumps to the entry point.
# --no-stub: the stub flasher is a program that also wants to live in RAM, and
# there is no reason to have two.
esp() { uv tool run --from esptool "$@"; }

# tools/p4run.py owns the loading: it autodetects the port on both Linux and
# macOS, tries an automatic reset before asking for buttons, and listens to the
# console afterwards. Pass LUGALOS_P4_PORT to override the port.
exec tools/p4run.py "$OUT/minimal_esp32p4.elf" --listen-secs 10
