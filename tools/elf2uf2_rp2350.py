#!/usr/bin/env python3
"""
elf2uf2_rp2350.py — Convert a raw binary (from objcopy -O binary) to RP2350-RISCV UF2.

Usage:
    python3 elf2uf2_rp2350.py <input.bin> <output.uf2> [--base BASE_ADDR] [--elf ELF_FILE]

Steps:
  1. Read the raw binary.
  2. If --elf is given, read the ELF symbol table to find __picobin_sec_block_flash
     and patch the secondary PICOBIN block's next_block_rel field.
  3. Pad the binary to a multiple of 256 bytes.
  4. Split into 256-byte UF2 payload blocks at FLASH addresses.
  5. Append the RP2350-E10 'absolute block' at 0x10FFFF00.

UF2 block layout (512 bytes):
  Offset  Size  Description
   0      4     Magic 0    = 0x0A324655 ('UF2\\n')
   4      4     Magic 1    = 0x9E5D5157
   8      4     Flags      = 0x00002000 (family ID present)
  12      4     Target address
  16      4     Payload size = 256
  20      4     Block number (0-based)
  24      4     Total blocks
  28      4     Family ID  = 0xE48BFF57 (RP2350-RISCV)
  32    256     Payload
 288    220     Zeros (padding)
 508      4     End magic  = 0x0AB16F30
"""

import sys
import struct
import argparse
import subprocess
import os

# UF2 constants
UF2_MAGIC_0    = 0x0A324655
UF2_MAGIC_1    = 0x9E5D5157
UF2_MAGIC_END  = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000

# RP2350-RISCV family ID
FAMILY_ID_RP2350_RISCV = 0xE48BFF57

# Flash base address for RP2350
FLASH_BASE     = 0x10000000

# RP2350-E10 absolute block target address
ABS_BLOCK_ADDR = 0x10FFFF00

# Payload bytes per UF2 block
PAYLOAD_SIZE   = 256

# PICOBIN primary block is always at Flash offset 0x14
PRIMARY_BLOCK_FLASH_ADDR = 0x10000014

# Secondary block's next_block_rel is at byte offset +0x0c within secondary block
# (after MAGIC=+0x00, IGNORED=+0x04, ITEM_LAST=+0x08, next_block_rel=+0x0c, SENTINEL=+0x10)
SEC_NEXT_BLOCK_REL_OFFSET = 0x0c


def make_uf2_block(target_addr: int, payload: bytes, block_num: int, total_blocks: int) -> bytes:
    """Build one 512-byte UF2 block."""
    assert len(payload) == PAYLOAD_SIZE
    header = struct.pack('<8I',
        UF2_MAGIC_0,
        UF2_MAGIC_1,
        UF2_FLAG_FAMILY_ID,
        target_addr,
        PAYLOAD_SIZE,
        block_num,
        total_blocks,
        FAMILY_ID_RP2350_RISCV,
    )
    padding = bytes(512 - 32 - PAYLOAD_SIZE - 4)
    end = struct.pack('<I', UF2_MAGIC_END)
    return header + payload + padding + end


def make_abs_block_payload() -> bytes:
    """
    RP2350-E10 absolute block payload (256 bytes).
    Format: 4-byte family ID + 252 zero bytes.
    """
    return struct.pack('<I', FAMILY_ID_RP2350_RISCV) + bytes(PAYLOAD_SIZE - 4)


def read_elf_symbol(elf_path: str, symbol_name: str) -> int | None:
    """Read a symbol value from an ELF file using nm."""
    try:
        out = subprocess.check_output(
            ['riscv64-elf-nm', '--print-size', elf_path],
            stderr=subprocess.DEVNULL
        ).decode()
        for line in out.splitlines():
            parts = line.split()
            # nm output: [addr] [size] [type] [name]  or  [addr] [type] [name]
            if parts[-1] == symbol_name:
                return int(parts[0], 16)
    except Exception:
        pass
    return None


def patch_secondary_picobin(data: bytearray, sec_addr: int, base_addr: int) -> None:
    """
    Patch the secondary PICOBIN block's next_block_rel field to point back
    to the primary block at PRIMARY_BLOCK_FLASH_ADDR.

    next_block_rel = PRIMARY_BLOCK_FLASH_ADDR - sec_addr  (signed 32-bit)
    """
    next_block_rel = (PRIMARY_BLOCK_FLASH_ADDR - sec_addr) & 0xFFFFFFFF
    sec_bin_offset = sec_addr - base_addr
    patch_offset = sec_bin_offset + SEC_NEXT_BLOCK_REL_OFFSET

    # Verify secondary block magic
    magic = struct.unpack_from('<I', data, sec_bin_offset)[0]
    if magic != 0xFFFFDED3:
        print(f"WARNING: Secondary PICOBIN magic not found at 0x{sec_addr:08x} "
              f"(got 0x{magic:08x}), skipping patch", file=sys.stderr)
        return

    struct.pack_into('<I', data, patch_offset, next_block_rel)
    print(f"Patched secondary PICOBIN next_block_rel at 0x{sec_addr + SEC_NEXT_BLOCK_REL_OFFSET:08x}: "
          f"0x{next_block_rel:08x} (→ primary at 0x{PRIMARY_BLOCK_FLASH_ADDR:08x})")


def verify_picobin_ring(data: bytearray, base_addr: int) -> bool:
    """Verify the PICOBIN doubly-linked block ring is valid."""
    # Primary block at +0x14
    pri_offset = PRIMARY_BLOCK_FLASH_ADDR - base_addr
    pri_words = struct.unpack_from('<8I', data, pri_offset)
    if pri_words[0] != 0xFFFFDED3:
        print(f"ERROR: Primary PICOBIN magic missing at 0x{PRIMARY_BLOCK_FLASH_ADDR:08x}")
        return False

    next_rel = pri_words[6]  # word 6 = +0x18 from block start = +0x2c from Flash 0
    sec_addr = (PRIMARY_BLOCK_FLASH_ADDR + next_rel) & 0xFFFFFFFF
    sec_offset = sec_addr - base_addr

    if sec_offset < 0 or sec_offset + 16 > len(data):
        print(f"ERROR: Secondary block address 0x{sec_addr:08x} out of range")
        return False

    sec_magic = struct.unpack_from('<I', data, sec_offset)[0]
    if sec_magic != 0xFFFFDED3:
        print(f"ERROR: Secondary PICOBIN magic missing at 0x{sec_addr:08x}")
        return False

    # next_block_rel is at SEC_NEXT_BLOCK_REL_OFFSET within the secondary block
    sec_next_rel = struct.unpack_from('<I', data, sec_offset + SEC_NEXT_BLOCK_REL_OFFSET)[0]
    back_addr = (sec_addr + sec_next_rel) & 0xFFFFFFFF
    if back_addr != PRIMARY_BLOCK_FLASH_ADDR:
        print(f"ERROR: Ring broken — secondary next points to 0x{back_addr:08x}, "
              f"expected 0x{PRIMARY_BLOCK_FLASH_ADDR:08x}")
        return False

    entry_pc = pri_words[4]   # word 4 = _start
    entry_sp = pri_words[5]   # word 5 = _stack_top
    img_type = pri_words[1]   # word 1 = IMAGE_TYPE
    print(f"PICOBIN ring OK:")
    print(f"  Primary  @ 0x{PRIMARY_BLOCK_FLASH_ADDR:08x}: next_rel=0x{next_rel:08x} → 0x{sec_addr:08x}")
    print(f"  Secondary@ 0x{sec_addr:08x}: next_rel=0x{back_rel:08x} → 0x{back_addr:08x}")
    print(f"  Entry PC=0x{entry_pc:08x}  Entry SP=0x{entry_sp:08x}  IMG_TYPE=0x{img_type:08x}")
    return True


def convert(bin_path: str, uf2_path: str, base_addr: int = FLASH_BASE,
            elf_path: str | None = None) -> None:
    with open(bin_path, 'rb') as f:
        data = bytearray(f.read())

    # Patch the secondary PICOBIN next_block_rel using ELF symbol table
    sec_addr = None
    if elf_path and os.path.exists(elf_path):
        sec_addr = read_elf_symbol(elf_path, '__picobin_sec_block_flash')
        if sec_addr is not None:
            patch_secondary_picobin(data, sec_addr, base_addr)
        else:
            print(f"WARNING: __picobin_sec_block_flash not found in {elf_path}", file=sys.stderr)

    # Verify ring
    if not verify_picobin_ring(data, base_addr):
        sys.exit(1)

    # Pad to multiple of PAYLOAD_SIZE
    if len(data) % PAYLOAD_SIZE:
        pad = PAYLOAD_SIZE - (len(data) % PAYLOAD_SIZE)
        data += bytes(pad)

    num_flash_blocks = len(data) // PAYLOAD_SIZE
    total_blocks = num_flash_blocks + 1  # +1 for absolute block

    blocks = []
    for i in range(num_flash_blocks):
        addr = base_addr + i * PAYLOAD_SIZE
        payload = bytes(data[i * PAYLOAD_SIZE:(i + 1) * PAYLOAD_SIZE])
        blocks.append(make_uf2_block(addr, payload, i, total_blocks))

    # Absolute block at 0x10FFFF00
    abs_payload = make_abs_block_payload()
    blocks.append(make_uf2_block(ABS_BLOCK_ADDR, abs_payload, num_flash_blocks, total_blocks))

    with open(uf2_path, 'wb') as f:
        for b in blocks:
            f.write(b)

    print(f"RP2350-E10: Adding absolute block to UF2 targeting 0x{ABS_BLOCK_ADDR:08x}")
    print(f"Written {total_blocks} blocks ({len(data)} bytes image) to {uf2_path}")


def main():
    parser = argparse.ArgumentParser(description='Convert raw binary to RP2350-RISCV UF2')
    parser.add_argument('input',  help='Input raw binary file (.bin)')
    parser.add_argument('output', help='Output UF2 file')
    parser.add_argument('--base', type=lambda x: int(x, 0),
                        default=FLASH_BASE,
                        help=f'Flash base address (default: 0x{FLASH_BASE:08x})')
    parser.add_argument('--elf',  default=None,
                        help='ELF file to read __picobin_sec_block_flash symbol from')
    args = parser.parse_args()
    convert(args.input, args.output, args.base, args.elf)


if __name__ == '__main__':
    main()
