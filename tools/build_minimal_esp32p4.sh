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
# **On Linux, entering download mode is automatic.** The board's U6 (an
# EMH4T2R pair) wires the bridge's RTS to ESP_EN and DTR to GPIO35, and
# tools/p4run.py drives those lines itself: no buttons, and the ROM confirms
# the mode it chose ("boot:0x307 (DOWNLOAD(USB/UART0/SPI))"). Verified
# 2026-09-05. On a host whose driver does not carry the modem-control lines
# -- macOS with the built-in CH34x driver does not -- it falls back to asking:
#
#   hold BOOT, press and release RESET, release BOOT
#
# Press RESET afterwards, or run `tools/p4run.py --run`, to return the board
# to whatever is in flash.
#
# Do NOT reach for the native USB-Serial-JTAG socket. Resetting the chip
# through it may leave UART0 emitting bytes that decode at no baud rate we
# could find; the evidence is mixed and the question is unresolved. See the
# E1 notes in plan/phase27_esp32p4_bringup.md.
#
# Console: UART0 on GPIO37/38 at 115200 8N1 -- the ROM's own pins and baud,
# which is why this program does not configure the UART at all. Which cable
# that is depends on the host, so tools/p4run.py identifies ports by USB
# VID:PID rather than by name; run `tools/p4run.py --ports` to see what it
# found. Override with LUGALOS_P4_PORT and LUGALOS_P4_RESET_PORT.

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

# tools/p4run.py owns the loading: it identifies the ports, resets the board
# into download mode itself (falling back to asking for the buttons), and
# listens to the console afterwards. Where the reset lines and the console are
# different cables it watches UART0 *through* the load, so the banner and the
# CSR dump -- printed once, in the instant the ROM jumps to us -- survive.
exec tools/p4run.py "$OUT/minimal_esp32p4.elf" --listen-secs 10
