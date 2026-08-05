#!/usr/bin/env python3
"""
create_flash_fs.py - Embedded Flash FAT32 Filesystem Generator for LugalOS
Converts pre-populated FAT32 disk image into a compiled C array (flash_fs.c).
"""

import os
import sys
from pathlib import Path

# Add tools directory to path to import create_sd_image
tools_dir = Path(__file__).resolve().parent
sys.path.insert(0, str(tools_dir))

from create_sd_image import build_fat32_image

def generate_flash_fs_c(output_c_path: str, src_dir: str) -> None:
    temp_img_path = output_c_path + ".tmp.bin"
    build_fat32_image(temp_img_path, src_dir)

    with open(temp_img_path, "rb") as f:
        data = f.read()

    os.remove(temp_img_path)

    print(f"[FlashFS] Embedding {len(data)} bytes FAT32 image into '{output_c_path}'...")
    with open(output_c_path, "w") as f:
        f.write("/* Auto-generated Embedded Flash FAT32 Filesystem for LugalOS */\n")
        f.write('#include <stdint.h>\n\n')
        f.write('const uint8_t g_flash_fs_start[%d] __attribute__((aligned(4))) = {\n' % len(data))
        
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
            f.write(f"    {hex_str},\n")

        f.write("};\n")
        f.write(f"const uint32_t g_flash_fs_size = {len(data)};\n")

if __name__ == "__main__":
    out_c = sys.argv[1] if len(sys.argv) > 1 else "build/flash_fs.c"
    src_d = sys.argv[2] if len(sys.argv) > 2 else str(tools_dir / "sd_root")
    os.makedirs(os.path.dirname(out_c), exist_ok=True)
    generate_flash_fs_c(out_c, src_d)
