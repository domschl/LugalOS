#!/usr/bin/env bash
# K210 silicon truth probe (65.1): build (and optionally ISP-load+run).
#
#   tools/build_minimal_k210.sh          build only
#   tools/build_minimal_k210.sh run      build, SRAM-load via kflash,
#                                        capture the report (host tool)
#
# Nothing here writes the board's flash: kflash -s downloads to SRAM
# and boots. The ELF entry must be exactly 0x80000000 (the BROM jump
# target); the linker script guarantees it and kflash warns otherwise.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=build/k210
mkdir -p "$OUT"

CC=${RISCV_GCC:-$(command -v riscv64-elf-gcc || command -v riscv64-unknown-elf-gcc || true)}
LD=${CC%-gcc}-ld
OBJCOPY=${CC%-gcc}-objcopy
[ -n "$CC" ] || { echo "no riscv64 cross compiler found" >&2; exit 1; }

# rv64imafdc so the assembler accepts the explicit F/D/atomics
# experiments; lp64 (integer ABI) so C code itself cannot emit float,
# matching the kernel policy (plan/phase65 §D2).
CFLAGS="-std=gnu11 -nostdlib -ffreestanding -O1 -g -Wall -Wextra \
        -march=rv64imafdc_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
        -fno-pic -fno-stack-protector -fno-common"

set -x
$CC $CFLAGS -c tools/minimal_k210.c          -o $OUT/minimal_k210.o
$CC $CFLAGS -c tools/minimal_k210_entry.S    -o $OUT/minimal_k210_entry.o
$LD -T tools/minimal_k210.ld -Map $OUT/minimal_k210.map \
    -o $OUT/minimal_k210.elf $OUT/minimal_k210_entry.o $OUT/minimal_k210.o
set +x

${CC%-gcc}-readelf -h $OUT/minimal_k210.elf | grep -E "Entry|Class|Machine"
${CC%-gcc}-size -A $OUT/minimal_k210.elf | tail -3
echo "built $OUT/minimal_k210.elf"

if [ "${1:-}" = run ]; then
    exec uv run --with pyserial python3 tools/k210_probe_host.py
fi
