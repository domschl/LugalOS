#!/usr/bin/env python3
"""
RP2350 UF2 Packaging Tool for LugalOS (Hazard3 RISC-V Target)
Converts raw binary/ELF outputs into standard RP2350 UF2 firmware files with PicoBIN header.
"""

import sys
import struct
from pathlib import Path

UF2_MAGIC_START_0 = 0x0A324655
UF2_MAGIC_START_1 = 0x9E5D5157
UF2_MAGIC_END     = 0x0AB16F30
UF2_FLAG_FAMILYID = 0x00002000

# RP2350 RISC-V Family ID (Hazard3 Core: 0xE48DA562)
RP2350_RISCV_FAMILY_ID = 0xE48DA562
FLASH_BASE_ADDR        = 0x10000000

def crc32_mpeg2(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= (byte << 24)
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc

def create_picobin_header() -> bytearray:
    header = bytearray(256)
    
    # 0x00: PicoBIN Block Marker Magic (0xFFFFDED3)
    struct.pack_into('<I', header, 0x00, 0xFFFFDED3)
    
    # 0x04: IMAGE_TYPE Item (ID 0x42): Executable=1, Arch=RISC-V (1) => 0x10210042
    struct.pack_into('<I', header, 0x04, 0x10210042)
    
    # 0x08: VECTOR_TABLE Item (ID 0x43): Offset 0x100 => (0x100 << 8) | 0x43 = 0x00000143
    struct.pack_into('<I', header, 0x08, 0x00000143)

    # 0x0C: ENTRY_POINT Item (ID 0x04): Offset 0x100 => (0x100 << 8) | 0x44 = 0x00000144
    struct.pack_into('<I', header, 0x0C, 0x00000144)

    # 0x10: LAST_ITEM (ID 0xAB) => 0x000000AB
    struct.pack_into('<I', header, 0x10, 0x000000AB)

    # 0xFC: Compute CRC32 over bytes 0..251 using MPEG-2 polynomial (0x04C11DB7)
    crc = crc32_mpeg2(header[:252])
    struct.pack_into('<I', header, 252, crc)
    
    return header



def convert_bin_to_uf2(input_bin_path: str, output_uf2_path: str, base_addr: int = FLASH_BASE_ADDR) -> None:
    input_file = Path(input_bin_path)
    if not input_file.exists():
        print(f"[Error] Input binary '{input_bin_path}' not found.")
        sys.exit(1)

    raw_data = bytearray(input_file.read_bytes())

    # Ensure binary is at least 256 bytes long
    if len(raw_data) < 256:
        raw_data.extend(b'\x00' * (256 - len(raw_data)))

    # Overwrite first 256 bytes with valid PicoBIN Image Header for RP2350 RISC-V
    picobin_hdr = create_picobin_header()
    raw_data[:256] = picobin_hdr

    block_size = 256
    num_blocks = (len(raw_data) + block_size - 1) // block_size

    uf2_output = bytearray()

    for block_no in range(num_blocks):
        offset = block_no * block_size
        chunk = raw_data[offset:offset + block_size]
        if len(chunk) < block_size:
            chunk.extend(b'\x00' * (block_size - len(chunk)))

        target_addr = base_addr + offset

        # 32-byte UF2 block header
        header = struct.pack(
            '<IIIIIIII',
            UF2_MAGIC_START_0,
            UF2_MAGIC_START_1,
            UF2_FLAG_FAMILYID,
            target_addr,
            block_size,
            block_no,
            num_blocks,
            RP2350_RISCV_FAMILY_ID
        )

        # 476 bytes payload area (256 bytes data + 220 padding bytes)
        payload = bytearray(chunk)
        payload.extend(b'\x00' * (476 - len(chunk)))

        # 4-byte UF2 footer magic
        footer = struct.pack('<I', UF2_MAGIC_END)

        uf2_output.extend(header + payload + footer)

    Path(output_uf2_path).write_bytes(uf2_output)
    print(f"[UF2 Converter] Successfully generated valid RP2350 RISC-V PicoBIN firmware: '{output_uf2_path}' ({len(uf2_output)} bytes, {num_blocks} blocks)")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python3 elf2uf2_rp2350.py <input.bin> <output.uf2>")
        sys.exit(1)

    convert_bin_to_uf2(sys.argv[1], sys.argv[2])
