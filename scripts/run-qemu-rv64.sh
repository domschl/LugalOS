#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build/rv64"

mkdir -p "$BUILD_DIR"
echo "==> Building LugalOS for RV64 (MMU)..."
cmake -B "$BUILD_DIR" -S "$ROOT_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-rv64-mmu.cmake"
ninja -C "$BUILD_DIR"

if command -v qemu-system-riscv64 >/dev/null 2>&1; then
    echo "==> Launching QEMU RV64 (Press Ctrl+A then X to exit)..."
    qemu-system-riscv64 -M virt -nographic -bios none -kernel "$BUILD_DIR/lugalos.elf"
else
    echo "[!] qemu-system-riscv64 binary not found. Install 'qemu-system-riscv' package to execute."
    echo "[+] Binary built successfully at: $BUILD_DIR/lugalos.elf"
fi
