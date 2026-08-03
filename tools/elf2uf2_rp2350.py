#!/usr/bin/env python3
"""
tools/elf2uf2_rp2350.py - LugalOS UF2 Packager for RP2350 (Raspberry Pi Pico 2 RISC-V)

Implements official RP2350 UF2 block ordering and PICOBIN image definition:
1. Code blocks (family 0xE48BFF5A) placed FIRST starting at block_no=0.
2. Metadata IMAGE_DEF block (family 0xE48BFF57) placed LAST.
3. Embedded PICOBIN Primary Metadata Block at Flash 0x10000014.
4. Embedded PICOBIN Secondary Metadata Block at end of binary.
"""

import struct
import sys
import subprocess
from pathlib import Path

UF2_MAGIC_START_0 = 0x0A324655
UF2_MAGIC_START_1 = 0x9E5D5157
UF2_MAGIC_END     = 0x0AB16558

UF2_FLAG_FAMILYID = 0x00002000
UF2_FLAG_MD5      = 0x00008000

FLASH_BASE_ADDR       = 0x10000000
IMAGEDEF_FLASH_ADDR   = 0x10FFFF00

RP2350_RISCV_FAMILY_CODE = 0xE48BFF5A
RP2350_RISCV_FAMILY_DEF  = 0xE48BFF57

PICOBIN_MAGIC             = 0xFFFFDED3
PICOBIN_IMAGE_TYPE_WORD   = 0x11010142  # RISC-V, non-secure
PICOBIN_ITEM_ENTRY_POINT  = 0x44
PICOBIN_ITEM_LAST         = 0xFF
PICOBIN_BLOCK_LOOP_NEXT   = 0xAB123579


def read_elf_symbols(elf_path: str) -> dict[str, int]:
    symbols = {}
    try:
        res = subprocess.run(
            ["riscv32-elf-nm", elf_path],
            capture_output=True,
            text=True,
            check=True,
        )
        for line in res.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                try:
                    symbols[parts[2]] = int(parts[0], 16)
                except ValueError:
                    pass
    except Exception as e:
        print(f"[Warning] Failed to run riscv32-elf-nm: {e}")

    if not symbols:
        try:
            res = subprocess.run(
                ["riscv64-elf-nm", elf_path],
                capture_output=True,
                text=True,
                check=True,
            )
            for line in res.stdout.splitlines():
                parts = line.split()
                if len(parts) == 3:
                    try:
                        symbols[parts[2]] = int(parts[0], 16)
                    except ValueError:
                        pass
        except Exception:
            pass
    return symbols


def make_uf2_block(
    data: bytes,
    target_addr: int,
    block_no: int,
    num_blocks: int,
    family_id: int,
    flags: int = UF2_FLAG_FAMILYID,
) -> bytes:
    assert len(data) == 256
    header = struct.pack(
        '<IIIIIIII',
        UF2_MAGIC_START_0,
        UF2_MAGIC_START_1,
        flags,
        target_addr,
        256,               # payload_size
        block_no,
        num_blocks,
        family_id,
    )
    payload = data + bytes(476 - 256)   # pad to 476 bytes
    footer = struct.pack('<I', UF2_MAGIC_END)
    return header + payload + footer


def convert_elf_to_uf2(elf_path: str, bin_path: str, output_uf2_path: str) -> None:
    print(f"[UF2] Reading ELF symbols from '{elf_path}'...")
    syms = read_elf_symbols(elf_path)

    entry_pc = syms.get('_start')
    entry_sp = syms.get('_stack_top')

    if entry_pc is None or entry_sp is None:
        print(f"[Error] Required symbols '_start' and/or '_stack_top' not found in ELF.")
        print(f"  Found: {list(syms.keys())[:20]}")
        sys.exit(1)

    print(f"[UF2]   _start    = 0x{entry_pc:08X}")
    print(f"[UF2]   _stack_top= 0x{entry_sp:08X}")

    raw_data = bytearray(Path(bin_path).read_bytes())

    # Ensure binary size is at least 512 bytes and aligned to 16 bytes
    if len(raw_data) < 512:
        raw_data.extend(b'\x00' * (512 - len(raw_data)))
    if len(raw_data) % 16 != 0:
        raw_data.extend(b'\x00' * (16 - (len(raw_data) % 16)))

    sec_block_offset = len(raw_data)
    raw_data.extend(b'\x00' * 16)
    sec_block_flash_addr = FLASH_BASE_ADDR + sec_block_offset

    # Primary Metadata Block at 0x10000014 (points to sec_block_flash_addr)
    prefix = [
        0x7188EBF2,
        0x10014454,
        0x10014474,
        0x100000A8,
        0xE71AA390,
    ]
    primary_picobin = [
        PICOBIN_MAGIC,                                              # 0x10000014
        PICOBIN_IMAGE_TYPE_WORD,                                     # 0x10000018
        (0x0000 << 16) | (0x03 << 8) | PICOBIN_ITEM_ENTRY_POINT,    # 0x1000001C
        entry_pc,                                                   # 0x10000020
        entry_sp,                                                   # 0x10000024
        (0x0000 << 16) | (0x04 << 8) | PICOBIN_ITEM_LAST,           # 0x10000028
        sec_block_flash_addr & 0x00FFFFFF,                         # 0x1000002C -> points to sec block!
        PICOBIN_BLOCK_LOOP_NEXT,                                    # 0x10000030
    ]

    primary_bytes = struct.pack(f'<{len(prefix + primary_picobin)}I', *(prefix + primary_picobin))
    raw_data[:len(primary_bytes)] = primary_bytes

    # Secondary Metadata Block at sec_block_offset (points back to 0x10000014)
    sec_picobin = [
        PICOBIN_MAGIC,                                              # +0x00
        (0x0000 << 16) | (0x04 << 8) | PICOBIN_ITEM_LAST,           # +0x04
        0x00000014,                                                 # +0x08 -> points back to 0x10000014!
        PICOBIN_BLOCK_LOOP_NEXT,                                    # +0x0C
    ]
    sec_bytes = struct.pack(f'<{len(sec_picobin)}I', *sec_picobin)
    raw_data[sec_block_offset : sec_block_offset + len(sec_bytes)] = sec_bytes

    # Pad binary size to 256-byte UF2 block boundary
    block_size = 256
    if len(raw_data) % block_size != 0:
        raw_data.extend(b'\x00' * (block_size - (len(raw_data) % block_size)))

    num_code_blocks = len(raw_data) // block_size

    uf2_blocks: list[bytes] = []

    # 1. CODE BLOCKS FIRST: Family 0xE48BFF5A, block_no 0..N-1
    for i in range(num_code_blocks):
        offset = i * block_size
        chunk = bytes(raw_data[offset : offset + block_size])
        uf2_blocks.append(make_uf2_block(
            chunk,
            FLASH_BASE_ADDR + offset,
            block_no=i,
            num_blocks=num_code_blocks,
            family_id=RP2350_RISCV_FAMILY_CODE,
            flags=UF2_FLAG_FAMILYID,
        ))

    # 2. IMAGE_DEF METADATA BLOCK LAST: Family 0xE48BFF57 at 0x10FFFF00
    imagedef_payload = bytes([0xEF] * 256)
    uf2_blocks.append(make_uf2_block(
        imagedef_payload,
        IMAGEDEF_FLASH_ADDR,
        block_no=0,
        num_blocks=2,                    # matches pico-sdk num_blocks=2
        family_id=RP2350_RISCV_FAMILY_DEF,
        flags=UF2_FLAG_FAMILYID | UF2_FLAG_MD5,
    ))

    uf2_output = b''.join(uf2_blocks)
    Path(output_uf2_path).write_bytes(uf2_output)
    print(f"[UF2] Generated '{output_uf2_path}' ({len(uf2_output)} bytes, {len(uf2_blocks)} blocks)")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python3 elf2uf2_rp2350.py <input.elf> <input.bin> <output.uf2>")
        sys.exit(1)
    convert_elf_to_uf2(sys.argv[1], sys.argv[2], sys.argv[3])
