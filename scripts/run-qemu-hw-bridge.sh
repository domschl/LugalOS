#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build/rv32"

SERIAL_DEV="${1:-/dev/ttyUSB0}"

if [ ! -e "$SERIAL_DEV" ]; then
    echo "[!] Target host serial device '$SERIAL_DEV' does not exist."
    echo "    Available serial ports:"
    ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "    No /dev/ttyUSB* or /dev/ttyACM* devices found."
    echo ""
    echo "Usage: $0 [/dev/ttyUSB0 | /dev/ttyACM1]"
    exit 1
fi

mkdir -p "$BUILD_DIR"
echo "==> Building LugalOS for RV32 QEMU-to-Hardware Bridge..."
cmake -B "$BUILD_DIR" -S "$ROOT_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-rv32-nommu.cmake"
ninja -C "$BUILD_DIR"

echo "==> Launching LugalOS QEMU Workstation Node attached to Physical Hardware ($SERIAL_DEV)..."
echo "  QEMU Serial Channel -> $SERIAL_DEV"
echo "----------------------------------------------------------------------"

qemu-system-riscv32 -M virt -nographic -bios none \
    -d guest_errors,unimp \
    -drive file="$BUILD_DIR/lugalos_sd.img",if=none,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -chardev serial,id=tty_hw,path="$SERIAL_DEV" \
    -serial chardev:tty_hw \
    -kernel "$BUILD_DIR/lugalos.elf"
