#!/usr/bin/env python3
"""
RP2350 UF2 Packaging Tool for LugalOS (Hazard3 RISC-V Target)
Reads an ELF file, extracts _start / _stack_top symbols, embeds a valid
RP2350 PICOBIN IMAGE_DEF block into the first 256 bytes (boot2 region),
then generates a standards-compliant UF2 firmware file.

RP2350 BootROM expects:
  - UF2 Family ID for code blocks:     0xE48BFF5A  (RP2350 RISC-V)
  - UF2 Family ID for IMAGE_DEF block: 0xE48BFF57  (RP2350 RISC-V image def)
  - A PICOBIN block at some address in Flash containing:
      MAGIC        0xFFFFDED3
      IMAGE_TYPE   0x11010142  (RISC-V, non-secure, executable)
      ENTRY_POINT  0x00000344  (3-word item)
        word1: initial PC  (_start address in Flash)
        word2: initial SP  (_stack_top address in SRAM)
      LAST         0x000004FF  (id=0xFF)
      block_size   total words in this block
      next_block   0xAB123579  (loop-back sentinel)
"""

import sys
import struct
import subprocess
from pathlib import Path

# --- UF2 constants ---
UF2_MAGIC_START_0  = 0x0A324655
UF2_MAGIC_START_1  = 0x9E5D5157
UF2_MAGIC_END      = 0x0AB16F30
UF2_FLAG_FAMILYID  = 0x00002000
UF2_FLAG_NOT_MAIN  = 0x00002000
UF2_FLAG_MD5       = 0x00008000

# RP2350 RISC-V Family IDs (from official pico-sdk / picotool source)
RP2350_RISCV_FAMILY_CODE = 0xE48BFF5A   # code blocks
RP2350_RISCV_FAMILY_DEF  = 0xE48BFF57   # IMAGE_DEF block

FLASH_BASE_ADDR = 0x10000000
IMAGEDEF_FLASH_ADDR = 0x10FFFF00        # special address for the IMAGE_DEF UF2 block

# --- PICOBIN item constants ---
PICOBIN_MAGIC              = 0xFFFFDED3
PICOBIN_ITEM_IMAGE_TYPE    = 0x42
PICOBIN_ITEM_ENTRY_POINT   = 0x44
PICOBIN_ITEM_LAST          = 0xFF
PICOBIN_IMAGE_TYPE_WORD    = 0x11010142  # RISC-V, chip=RP2350, non-secure executable
PICOBIN_BLOCK_LOOP_NEXT    = 0xAB123579  # chain loops back to start

def read_elf_symbols(elf_path: str) -> dict[str, int]:
    """Extract symbol addresses from ELF using riscv32-elf-nm."""
    result = subprocess.run(
        ['riscv32-elf-nm', elf_path],
        capture_output=True, text=True, check=True
    )
    symbols: dict[str, int] = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            try:
                symbols[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return symbols

def make_picobin_block(entry_pc: int, entry_sp: int) -> bytes:
    """
    Build a minimal valid PICOBIN IMAGE_DEF block.
    Layout (7 words = 28 bytes):
      [0] MAGIC
      [1] IMAGE_TYPE item (1-word, id=0x42)
      [2] ENTRY_POINT item header (3-word item, id=0x44, word_count=3)
      [3] initial PC
      [4] initial SP
      [5] LAST item (id=0xFF, word_count=4? — matches reference)
      [6] block_total_words (= 7)
      [7] next_block = loop-back sentinel
    Total: 8 words = 32 bytes. Padded to 256 bytes with 0xEF.
    """
    words = [
        PICOBIN_MAGIC,
        PICOBIN_IMAGE_TYPE_WORD,
        # ENTRY_POINT: id=0x44, word_count=3 (header + PC + SP)
        (0x0000 << 16) | (0x03 << 8) | PICOBIN_ITEM_ENTRY_POINT,
        entry_pc,
        entry_sp,
        # LAST item: id=0xFF, word_count=4 (matches pico-sdk output)
        (0x0000 << 16) | (0x04 << 8) | PICOBIN_ITEM_LAST,
    ]
    total_words = len(words) + 2  # +2 for the two trailing block fields
    words.append(total_words)
    words.append(PICOBIN_BLOCK_LOOP_NEXT)

    raw = struct.pack(f'<{len(words)}I', *words)
    # Pad to 256 bytes with 0xEF (pico-sdk style)
    raw = raw + bytes([0xEF] * (256 - len(raw)))
    return raw

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
    if len(raw_data) < 256:
        raw_data.extend(b'\x00' * (256 - len(raw_data)))

    # Embed PICOBIN IMAGE_DEF into the first 256 bytes (.boot2 region)
    picobin = make_picobin_block(entry_pc, entry_sp)
    raw_data[:256] = picobin
    print(f"[UF2] Embedded PICOBIN IMAGE_DEF at Flash 0x{FLASH_BASE_ADDR:08X}")

    block_size = 256
    num_code_blocks = (len(raw_data) + block_size - 1) // block_size

    uf2_blocks: list[bytes] = []

    # Block 0: special IMAGE_DEF UF2 block at 0x10FFFF00 (256 bytes of 0xEF padding).
    # CRITICAL: each Family ID uses its OWN independent block counter.
    # IMAGE_DEF family counter: block_no=0, num_blocks=1 (just one metadata block).
    # Code family counter:       block_no=0..N-1, num_blocks=N.
    imagedef_payload = bytes([0xEF] * 256)
    uf2_blocks.append(make_uf2_block(
        imagedef_payload,
        IMAGEDEF_FLASH_ADDR,
        block_no=0,
        num_blocks=1,                    # independent IMAGE_DEF family counter
        family_id=RP2350_RISCV_FAMILY_DEF,
        flags=UF2_FLAG_FAMILYID | UF2_FLAG_MD5,
    ))

    # Code blocks: block_no restarts from 0 for the code family (0xE48BFF5A).
    # BootROM triggers reboot when it sees the last block (block_no == num_blocks-1).
    for i in range(num_code_blocks):
        offset = i * block_size
        chunk = bytes(raw_data[offset:offset + block_size])
        if len(chunk) < block_size:
            chunk = chunk + bytes(block_size - len(chunk))
        uf2_blocks.append(make_uf2_block(
            chunk,
            FLASH_BASE_ADDR + offset,
            block_no=i,                  # 0-based for code family, independent of IMAGE_DEF
            num_blocks=num_code_blocks,
            family_id=RP2350_RISCV_FAMILY_CODE,
            flags=UF2_FLAG_FAMILYID,
        ))

    uf2_output = b''.join(uf2_blocks)
    Path(output_uf2_path).write_bytes(uf2_output)
    print(
        f"[UF2] Generated '{output_uf2_path}' "
        f"({len(uf2_output)} bytes, {len(uf2_blocks)} blocks)"
    )

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: python3 elf2uf2_rp2350.py <input.elf> <input.bin> <output.uf2>")
        sys.exit(1)

    convert_elf_to_uf2(sys.argv[1], sys.argv[2], sys.argv[3])
