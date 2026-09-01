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

def generate_flash_fs_bin(output_bin_path: str, src_dir: str) -> None:
    """The same FAT32 image, as a raw binary rather than a C array.

    I7a, plan/phase21_identity_and_authentication.md §3.3: on RP2350 the
    filesystem is no longer compiled into the image. It is flashed to its own
    region as a separate UF2, so the OS image stops carrying 512 KB it cannot
    write and does not need to rewrite on every build. The QEMU targets keep
    the C-array form -- they have no flash map to place a segment in, and
    embedding is exactly right for a ROMdisk in a hosted image.
    """
    build_fat32_image(output_bin_path, src_dir)
    size = os.path.getsize(output_bin_path)
    print(f"[FlashFS] Wrote {size} bytes of FAT32 image to '{output_bin_path}'")


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", nargs="?", default="build/flash_fs.c",
                    help="output path (.c for the embedded array, .bin for a raw image)")
    ap.add_argument("src", nargs="?", default=str(tools_dir / "sd_root"),
                    help="directory tree to place into the image")
    ap.add_argument("--format", choices=("c", "bin"), default=None,
                    help="output format; inferred from the output extension when omitted")
    args = ap.parse_args()

    fmt = args.format or ("bin" if args.output.endswith(".bin") else "c")
    outdir = os.path.dirname(args.output)
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    if fmt == "bin":
        generate_flash_fs_bin(args.output, args.src)
    else:
        generate_flash_fs_c(args.output, args.src)
