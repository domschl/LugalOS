#!/usr/bin/env python3
"""
create_sd_image.py - LugalOS FAT32 Disk Image Generator & Pre-populator
Copyright (c) 2026 LugalOS Developers
License: MIT License
"""

import os
import struct
import sys

SECTOR_SIZE: int = 512
NUM_SECTORS: int = 1024  # 512 KB image size


def filename_to_83(name: str) -> bytes:
    parts = name.split(".")
    base = parts[0][:8].upper().ljust(8)
    ext = parts[1][:3].upper().ljust(3) if len(parts) > 1 else "   "
    return (base + ext).encode("ascii")


def build_fat32_image(output_path: str, src_dir: str | None = None) -> None:
    img: bytearray = bytearray(NUM_SECTORS * SECTOR_SIZE)

    # Boot Sector (BPB)
    img[0:3] = b"\xeb\x58\x90"  # JMP
    img[3:11] = b"LUGALOS "  # OEM Name
    struct.pack_into("<H", img, 11, SECTOR_SIZE)  # Bytes per sector
    img[13] = 1  # Sectors per cluster
    struct.pack_into("<H", img, 14, 32)  # Reserved sectors
    img[16] = 2  # Number of FATs
    struct.pack_into("<H", img, 17, 0)  # Root entries (0 for FAT32)
    struct.pack_into("<H", img, 19, 0)  # Total sectors 16 (0 for FAT32)
    img[21] = 0xF8  # Media descriptor
    struct.pack_into("<H", img, 22, 0)  # FAT size 16 (0 for FAT32)
    struct.pack_into("<H", img, 24, 63)  # Sectors per track
    struct.pack_into("<H", img, 26, 255)  # Number of heads
    struct.pack_into("<I", img, 28, 0)  # Hidden sectors
    struct.pack_into("<I", img, 32, NUM_SECTORS)  # Total sectors 32
    struct.pack_into("<I", img, 36, 8)  # FAT size 32 (8 sectors per FAT)
    struct.pack_into("<H", img, 40, 0)  # Ext flags
    struct.pack_into("<H", img, 42, 0)  # FS version
    struct.pack_into("<I", img, 44, 2)  # Root cluster (Cluster 2)
    struct.pack_into("<H", img, 48, 1)  # FS info sector
    struct.pack_into("<H", img, 50, 6)  # Backup boot sector
    img[66] = 0x29  # Boot signature
    struct.pack_into("<I", img, 67, 0x12345678)  # Volume ID
    img[71:82] = b"LUGALOS_FAT"  # Volume Label
    img[82:90] = b"FAT32   "  # System Identifier
    img[510:512] = b"\x55\xaa"  # Boot Signature

    # FSInfo Sector (Sector 1)
    struct.pack_into("<I", img, 512, 0x41615252)  # Lead Sig
    struct.pack_into("<I", img, 512 + 484, 0x61417272)  # Struct Sig
    struct.pack_into("<I", img, 512 + 488, NUM_SECTORS - 48)  # Free clusters
    struct.pack_into("<I", img, 512 + 492, 2)  # Next free cluster
    img[512 + 510 : 512 + 512] = b"\x55\xaa"  # Trail Sig

    # FAT 1 & FAT 2 Base Offsets (Sectors 32 & 40)
    fat1_base: int = 32 * SECTOR_SIZE
    fat2_base: int = 40 * SECTOR_SIZE

    def set_fat_entry(clus: int, val: int) -> None:
        struct.pack_into("<I", img, fat1_base + clus * 4, val)
        struct.pack_into("<I", img, fat2_base + clus * 4, val)

    # Reserve Cluster 0, 1, and 2 (Root Dir)
    set_fat_entry(0, 0x0FFFFFF8)  # Media type
    set_fat_entry(1, 0x0FFFFFFF)  # Clean shutdown
    set_fat_entry(2, 0x0FFFFFFF)  # Root cluster EOF

    current_free_cluster: int = 3

    def populate_dir(dir_path: str, parent_cluster: int) -> None:
        nonlocal current_free_cluster
        if not os.path.isdir(dir_path):
            return

        entries: list[str] = sorted(os.listdir(dir_path))
        dir_lba: int = 48 + (parent_cluster - 2)
        dir_offset_base: int = dir_lba * SECTOR_SIZE

        # Determine starting entry index (if parent_cluster != 2, index starts after '.' and '..')
        entry_idx: int = 0
        if parent_cluster != 2:
            # Check existing entries
            for i in range(16):
                if img[dir_offset_base + i * 32] == 0:
                    entry_idx = i
                    break

        for name in entries:
            full_path: str = os.path.join(dir_path, name)
            if entry_idx >= 16:
                print(f"[SD Image] Warning: Directory sector full in '{dir_path}'")
                break

            entry_offset: int = dir_offset_base + entry_idx * 32
            name83: bytes = filename_to_83(name)

            if os.path.isdir(full_path):
                sub_cluster: int = current_free_cluster
                current_free_cluster += 1
                set_fat_entry(sub_cluster, 0x0FFFFFFF)

                # Write directory entry in parent
                img[entry_offset : entry_offset + 11] = name83
                img[entry_offset + 11] = 0x10  # ATTR_DIRECTORY
                struct.pack_into("<H", img, entry_offset + 20, (sub_cluster >> 16) & 0xFFFF)
                struct.pack_into("<H", img, entry_offset + 26, sub_cluster & 0xFFFF)
                struct.pack_into("<I", img, entry_offset + 28, 0)

                # Initialize sub_cluster with '.' and '..'
                sub_lba: int = 48 + (sub_cluster - 2)
                sub_base: int = sub_lba * SECTOR_SIZE

                # Entry 0: '.'
                img[sub_base : sub_base + 11] = b".          "
                img[sub_base + 11] = 0x10
                struct.pack_into("<H", img, sub_base + 20, (sub_cluster >> 16) & 0xFFFF)
                struct.pack_into("<H", img, sub_base + 26, sub_cluster & 0xFFFF)

                # Entry 1: '..'
                up_clus: int = parent_cluster if parent_cluster != 2 else 0
                img[sub_base + 32 : sub_base + 43] = b"..         "
                img[sub_base + 43] = 0x10
                struct.pack_into("<H", img, sub_base + 52, (up_clus >> 16) & 0xFFFF)
                struct.pack_into("<H", img, sub_base + 58, up_clus & 0xFFFF)

                entry_idx += 1
                print(f"[SD Image] Added Directory '{name}' (Cluster {sub_cluster})")

                # Recurse into subdirectory
                populate_dir(full_path, sub_cluster)

            elif os.path.isfile(full_path):
                with open(full_path, "rb") as f:
                    content: bytes = f.read()

                file_size: int = len(content)
                clusters_needed: int = (file_size + SECTOR_SIZE - 1) // SECTOR_SIZE
                if clusters_needed == 0:
                    clusters_needed = 1

                start_cluster: int = current_free_cluster

                # Write file content to clusters & update FAT chain
                for c in range(clusters_needed):
                    cur_cluster: int = start_cluster + c
                    data_lba: int = 48 + (cur_cluster - 2)
                    chunk_start: int = c * SECTOR_SIZE
                    chunk_end: int = min(file_size, chunk_start + SECTOR_SIZE)
                    chunk_data: bytes = content[chunk_start:chunk_end]

                    img[data_lba * SECTOR_SIZE : data_lba * SECTOR_SIZE + len(chunk_data)] = chunk_data

                    next_val: int = (cur_cluster + 1) if (c + 1 < clusters_needed) else 0x0FFFFFFF
                    set_fat_entry(cur_cluster, next_val)

                # Write Directory Entry
                img[entry_offset : entry_offset + 11] = name83
                img[entry_offset + 11] = 0x20  # ATTR_ARCHIVE
                struct.pack_into("<H", img, entry_offset + 20, (start_cluster >> 16) & 0xFFFF)
                struct.pack_into("<H", img, entry_offset + 26, start_cluster & 0xFFFF)
                struct.pack_into("<I", img, entry_offset + 28, file_size)

                entry_idx += 1
                current_free_cluster += clusters_needed
                print(f"[SD Image] Added File '{name}' (8.3: {name83.decode('ascii')}, {file_size} bytes, Cluster {start_cluster})")

    if src_dir and os.path.isdir(src_dir):
        populate_dir(src_dir, 2)

    # Update FSInfo free cluster count & next free cluster pointer
    struct.pack_into("<I", img, 512 + 488, (NUM_SECTORS - 48) - (current_free_cluster - 2))
    struct.pack_into("<I", img, 512 + 492, current_free_cluster)

    with open(output_path, "wb") as f:
        f.write(img)

    print(f"[SD Image] Successfully generated FAT32 disk image '{output_path}' ({len(img)} bytes)")


def newer_than_image(sd_root: str, out_file: str) -> bool:
    """Is anything in the staging directory newer than the built image?"""
    try:
        img_mtime = os.path.getmtime(out_file)
    except OSError:
        return True
    for root, _dirs, files in os.walk(sd_root):
        for f in files:
            try:
                if os.path.getmtime(os.path.join(root, f)) > img_mtime:
                    return True
            except OSError:
                return True
    return False


if __name__ == "__main__":
    # --if-stale: keep an existing image unless the staged files have changed.
    #
    # This used to be --if-missing, which kept the image unconditionally. The
    # intent was not to clobber a disk that tests had written to, and that part
    # is worth keeping -- but it also meant editing init.lisp left the image
    # holding the previous copy with nothing to say so. It surfaced twice as
    # three unrelated *9P transport* tests failing on content that should have
    # matched, because they compare what the image serves against what the
    # source now says. Comparing mtimes keeps the intent and removes the trap.
    if_stale: bool = "--if-stale" in sys.argv or "--if-missing" in sys.argv
    args: list[str] = [a for a in sys.argv[1:]
                       if a not in ("--if-stale", "--if-missing")]

    out_file: str = args[0] if len(args) > 0 else "lugalos_sd.img"
    sd_root: str = args[1] if len(args) > 1 else os.path.join(os.path.dirname(__file__), "sd_root")

    if if_stale and os.path.exists(out_file) and not newer_than_image(sd_root, out_file):
        print(f"[SD Image] Preserving existing disk image '{out_file}' (up to date).")
        sys.exit(0)

    build_fat32_image(out_file, sd_root)
