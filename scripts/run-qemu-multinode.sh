#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build/rv32"

mkdir -p "$BUILD_DIR"
echo "==> Building LugalOS for RV32 Multi-Node Interconnect..."
cmake -B "$BUILD_DIR" -S "$ROOT_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-rv32-nommu.cmake"
ninja -C "$BUILD_DIR"

if command -v qemu-system-riscv32 >/dev/null 2>&1; then
    echo "==> Launching LugalOS Multi-Node Topology over TCP 4444 Interconnect..."
    echo "  Node 2 (Server): TCP 4444 Listener"
    echo "  Node 1 (Client): TCP 4444 Connector"

    # Start Node 2 in background
    qemu-system-riscv32 -M virt -nographic -bios none \
        -d guest_errors,unimp \
        -drive file="$ROOT_DIR/build/lugalos_sd.img",if=none,format=raw,id=hd0 \
        -device virtio-blk-device,drive=hd0 \
        -serial tcp:127.0.0.1:4444,server,nowait \
        -kernel "$BUILD_DIR/lugalos.elf" &
    NODE2_PID=$!

    sleep 1

    # Start Node 1 in foreground
    qemu-system-riscv32 -M virt -nographic -bios none \
        -d guest_errors,unimp \
        -drive file="$ROOT_DIR/build/lugalos_sd.img",if=none,format=raw,id=hd0 \
        -device virtio-blk-device,drive=hd0 \
        -serial tcp:127.0.0.1:4444 \
        -kernel "$BUILD_DIR/lugalos.elf" || true

    kill $NODE2_PID >/dev/null 2>&1 || true
else
    echo "[!] qemu-system-riscv32 binary not found."
fi
