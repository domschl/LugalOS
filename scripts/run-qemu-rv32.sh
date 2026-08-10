#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build/rv32"

mkdir -p "$BUILD_DIR"
echo "==> Building LugalOS for RV32 (NOMMU)..."
cmake -B "$BUILD_DIR" -S "$ROOT_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-rv32-nommu.cmake"
ninja -C "$BUILD_DIR"

if command -v qemu-system-riscv32 >/dev/null 2>&1; then
    echo "==> Launching QEMU RV32 (Press Ctrl+A then X to exit)..."
    qemu-system-riscv32 -M virt -nographic -bios none \
        -d guest_errors,unimp \
        -drive file="$BUILD_DIR/lugalos_sd.img",if=none,format=raw,id=hd0 \
        -device virtio-blk-device,drive=hd0 \
        -kernel "$BUILD_DIR/lugalos.elf"


else
    echo "[!] qemu-system-riscv32 binary not found. Install 'qemu-system-riscv' package to execute."
    echo "[+] Binary built successfully at: $BUILD_DIR/lugalos.elf"
fi
