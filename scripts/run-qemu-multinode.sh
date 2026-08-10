#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR_1="$ROOT_DIR/build/rv32"
BUILD_DIR_2="$ROOT_DIR/build/rv64"
PORT=15590

# A4 (plan/phase5_distributed_design.md): this used to cross-connect the two
# nodes' interactive CONSOLES over TCP -- a demo of raw byte interleaving,
# not a real data channel. Now that a real 9P link exists (A3's
# link_virtio_console, drivers/virtio_console.c), this bridges THAT instead:
# QEMU's own socket chardev backend wires the two nodes' virtio-console
# ports directly together (`server=on` on Node 2, a plain connecting socket
# chardev on Node 1) -- no relay code needed. Node 1 (RV32 NOMMU) is your
# interactive session, landing on this terminal as before. Node 2 (RV64
# Sv39 MMU) runs headless with its console logged to a file; this script
# feeds it one setup command via a FIFO so it has a real marker file for
# Node 1 to fetch live over the wire with `(p9-remote-cat ...)`.

mkdir -p "$BUILD_DIR_1" "$BUILD_DIR_2"
echo "==> Building LugalOS for RV32 (Node 1) and RV64 (Node 2)..."
cmake -B "$BUILD_DIR_1" -S "$ROOT_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-rv32-nommu.cmake"
ninja -C "$BUILD_DIR_1"
cmake -B "$BUILD_DIR_2" -S "$ROOT_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-rv64-mmu.cmake"
ninja -C "$BUILD_DIR_2"

# Separate disk image copies so the two nodes don't fight over the same
# backing file.
cp "$BUILD_DIR_1/lugalos_sd.img" "$ROOT_DIR/build/lugalos_sd_node1.img"
cp "$BUILD_DIR_2/lugalos_sd.img" "$ROOT_DIR/build/lugalos_sd_node2.img"

if ! command -v qemu-system-riscv64 >/dev/null 2>&1 || ! command -v qemu-system-riscv32 >/dev/null 2>&1; then
    echo "[!] qemu-system-riscv32/riscv64 binaries not found. Install the 'qemu-system-riscv' package."
    exit 1
fi

NODE2_FIFO="$(mktemp -u)"
mkfifo "$NODE2_FIFO"

echo "==> Launching Node 2 (RV64 Sv39 MMU, headless) -- 9P link listening on TCP $PORT..."
echo "    Console log: /tmp/lugalos_node2.log"

qemu-system-riscv64 -M virt -nographic -bios none \
    -d guest_errors,unimp \
    -drive file="$ROOT_DIR/build/lugalos_sd_node2.img",if=none,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -device virtio-serial-device -device virtconsole,chardev=p9c \
    -chardev socket,id=p9c,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -kernel "$BUILD_DIR_2/lugalos.elf" < "$NODE2_FIFO" > /tmp/lugalos_node2.log 2>&1 &
NODE2_PID=$!

# Keep the FIFO open for writing for the life of the script (a FIFO closes
# once every writer closes it, which would end Node 2's console session).
exec 3>"$NODE2_FIFO"
trap 'exec 3>&-; kill $NODE2_PID >/dev/null 2>&1 || true; rm -f "$NODE2_FIFO"' EXIT

sleep 2.0
printf 'lisp\n(write-file "/ram0/multinode_marker.txt" "HELLO_FROM_NODE2_RV64")\nexit\n' >&3

echo "----------------------------------------------------------------------"
echo "==> Launching Node 1 (RV32 NOMMU, interactive) -- dialing Node 2's 9P link..."
echo "    Try:  lisp"
echo "          (p9-remote-cat \"/ram0/multinode_marker.txt\")"
echo "    Expected: \"HELLO_FROM_NODE2_RV64\" -- fetched live from Node 2 over the wire."
echo "----------------------------------------------------------------------"

qemu-system-riscv32 -M virt -nographic -bios none \
    -d guest_errors,unimp \
    -drive file="$ROOT_DIR/build/lugalos_sd_node1.img",if=none,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -device virtio-serial-device -device virtconsole,chardev=p9c \
    -chardev socket,id=p9c,host=127.0.0.1,port=$PORT \
    -kernel "$BUILD_DIR_1/lugalos.elf" || true
