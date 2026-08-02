#!/usr/bin/env python3
"""
create_sd_image.py - LugalOS FAT32 Disk Image Generator & Pre-populator
Copyright (c) 2026 LugalOS Developers
License: MIT License
"""

import sys
import os
import struct

SECTOR_SIZE = 512
NUM_SECTORS = 1024  # 512 KB image size

def build_fat32_image(output_path: str) -> None:
    img = bytearray(NUM_SECTORS * SECTOR_SIZE)

    # Boot Sector (BPB)
    img[0:3] = b'\xeb\x58\x90'  # JMP
    img[3:11] = b'LUGALOS '    # OEM Name
    struct.pack_into('<H', img, 11, SECTOR_SIZE)  # Bytes per sector
    img[13] = 1                # Sectors per cluster
    struct.pack_into('<H', img, 14, 32)           # Reserved sectors
    img[16] = 2                # Number of FATs
    struct.pack_into('<H', img, 17, 0)            # Root entries (0 for FAT32)
    struct.pack_into('<H', img, 19, 0)            # Total sectors 16 (0 for FAT32)
    img[21] = 0xF8             # Media descriptor
    struct.pack_into('<H', img, 22, 0)            # FAT size 16 (0 for FAT32)
    struct.pack_into('<H', img, 24, 63)           # Sectors per track
    struct.pack_into('<H', img, 26, 255)          # Number of heads
    struct.pack_into('<I', img, 28, 0)            # Hidden sectors
    struct.pack_into('<I', img, 32, NUM_SECTORS)  # Total sectors 32
    struct.pack_into('<I', img, 36, 8)            # FAT size 32 (8 sectors per FAT)
    struct.pack_into('<H', img, 40, 0)            # Ext flags
    struct.pack_into('<H', img, 42, 0)            # FS version
    struct.pack_into('<I', img, 44, 2)            # Root cluster (Cluster 2)
    struct.pack_into('<H', img, 48, 1)            # FS info sector
    struct.pack_into('<H', img, 50, 6)            # Backup boot sector
    img[66] = 0x29                                # Boot signature
    struct.pack_into('<I', img, 67, 0x12345678)   # Volume ID
    img[71:82] = b'LUGALOS_FAT'                    # Volume Label
    img[82:90] = b'FAT32   '                       # System Identifier
    img[510:512] = b'\x55\xaa'                    # Boot Signature

    # FSInfo Sector (Sector 1)
    struct.pack_into('<I', img, 512, 0x41615252)  # Lead Sig
    struct.pack_into('<I', img, 512 + 484, 0x61417272) # Struct Sig
    struct.pack_into('<I', img, 512 + 488, NUM_SECTORS - 48) # Free clusters
    struct.pack_into('<I', img, 512 + 492, 2)     # Next free cluster
    img[512 + 510:512 + 512] = b'\x55\xaa'        # Trail Sig

    # FAT 1 & FAT 2 (Sectors 32..47)
    for fat_base in [32 * SECTOR_SIZE, 40 * SECTOR_SIZE]:
        struct.pack_into('<I', img, fat_base + 0, 0x0FFFFFF8) # Cluster 0 (Media)
        struct.pack_into('<I', img, fat_base + 4, 0x0FFFFFFF) # Cluster 1 (EOF)
        struct.pack_into('<I', img, fat_base + 8, 0x0FFFFFFF) # Cluster 2 (Root EOF)

    with open(output_path, 'wb') as f:
        f.write(img)

    print(f"[SD Image] Successfully generated FAT32 disk image '{output_path}' ({len(img)} bytes)")

if __name__ == '__main__':
    out_file = sys.argv[1] if len(sys.argv) > 1 else 'lugalos_sd.img'
    build_fat32_image(out_file)
