#!/usr/bin/env python3
"""
create_sd_image.py - LugalOS FAT32 Disk Image Generator & Pre-populator
Copyright (c) 2026 LugalOS Developers
License: MIT License
"""

import os
import string
import struct
import sys

SECTOR_SIZE: int = 512
NUM_SECTORS: int = 2048  # 1024 KB (1 MB) image size
RESERVED_SECTORS: int = 32
FAT_SIZE_SECTORS: int = 16
DATA_START_SECTOR: int = RESERVED_SECTORS + 2 * FAT_SIZE_SECTORS  # Sector 64
# The SD card image the QEMU suite mounts: 2 MB. The staged content is about 0.7 MB and
# the suite writes to the card (a screenshot of the canvas, the editor's saves), so it
# needs room; at 1 MB it was 95% full and those tests failed on a full disk. The
# /flash0 image (create_flash_fs.py) stays NUM_SECTORS: on hardware it must fill its
# flash region exactly.
SD_IMAGE_SECTORS: int = 4096


_83_CHARS: frozenset[str] = frozenset(string.ascii_letters + string.digits + "!#$%&'()-@^_`{}~")
_LFN_OFFSETS: tuple[int, ...] = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
_FORBIDDEN: frozenset[str] = frozenset('"*/:<>?\\|')


def fits_83(name: str) -> tuple[bytes, int] | None:
    """`name` as an 8.3 entry name and its NT case flags, when it is one
    exactly (each part in one case), as fs/fat32.c stores it; else None."""
    if not name or len(name) > 12 or name[0] == "." or name.count(".") > 1:
        return None
    base, dot, ext = name.partition(".")
    if not 1 <= len(base) <= 8 or len(ext) > 3 or (dot and not ext):
        return None
    if any(c not in _83_CHARS for c in base + ext):
        return None
    flags = 0
    for part, flag in ((base, 0x08), (ext, 0x10)):
        lower = any(c.islower() for c in part)
        if lower and any(c.isupper() for c in part):
            return None
        if lower:
            flags |= flag
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii"), flags


def short_alias(name: str, taken: set[bytes]) -> bytes:
    """A "~N" alias not in `taken`: upper case, spaces and leading dots
    dropped, the extension after the last dot, '_' for what 8.3 cannot hold."""
    s = name.lstrip(". ")
    base_part, _, ext_part = s.rpartition(".") if "." in s else (s, "", "")

    def conv(t: str) -> str:
        return "".join(c.upper() if c in _83_CHARS else "_" for c in t if c not in " .")

    base = conv(base_part)[:8] or "_"
    ext = conv(ext_part)[:3]
    n = 1
    while True:
        tail = f"~{n}"
        raw = ((base[: 8 - len(tail)] + tail).ljust(8) + ext.ljust(3)).encode("ascii")
        if raw not in taken:
            return raw
        n += 1


def lfn_checksum(short: bytes) -> int:
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def lfn_slots(name: str, short: bytes) -> list[bytes]:
    """The long-name slots for `name`, in disk order (last part first)."""
    data = name.encode("utf-16-le")
    units = [data[i : i + 2] for i in range(0, len(data), 2)]
    if len(units) > 255:
        raise ValueError(f"'{name}' is longer than 255 UTF-16 units")
    n = (len(units) + 12) // 13
    if len(units) % 13:
        units.append(b"\x00\x00")
    units += [b"\xff\xff"] * (n * 13 - len(units))
    csum = lfn_checksum(short)
    out: list[bytes] = []
    for k in range(n, 0, -1):
        e = bytearray(32)
        e[0] = k | (0x40 if k == n else 0)
        e[11] = 0x0F
        e[13] = csum
        for i, off in enumerate(_LFN_OFFSETS):
            e[off : off + 2] = units[(k - 1) * 13 + i]
        out.append(bytes(e))
    return out


def dir_entry(short: bytes, attr: int, nt: int, cluster: int, size: int) -> bytes:
    e = bytearray(32)
    e[0:11] = short
    e[11] = attr
    e[12] = nt
    struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def entry_set(name: str, attr: int, cluster: int, size: int, taken: set[bytes]) -> bytes:
    """`name`'s slots: an 8.3 entry alone when the name is one, else a
    long-name set and a unique alias. Adds the 8.3 name to `taken`."""
    if any(c in _FORBIDDEN or ord(c) < 0x20 for c in name) or name in (".", "..") or name[-1] in ". ":
        raise ValueError(f"'{name}' cannot be a FAT file name")
    fit = fits_83(name)
    if fit and fit[0] not in taken:
        taken.add(fit[0])
        return dir_entry(fit[0], attr, fit[1], cluster, size)
    short = short_alias(name, taken)
    taken.add(short)
    return b"".join(lfn_slots(name, short)) + dir_entry(short, attr, 0, cluster, size)


def build_fat32_image(
    output_path: str,
    src_dir: str | None = None,
    num_sectors: int = NUM_SECTORS,
    exclude: frozenset[str] = frozenset(),
) -> None:
    # The FAT has one 4-byte entry per cluster (one cluster a sector).
    fat_size: int = FAT_SIZE_SECTORS if num_sectors == NUM_SECTORS else -(-num_sectors // 128)
    data_start: int = RESERVED_SECTORS + 2 * fat_size
    img: bytearray = bytearray(num_sectors * SECTOR_SIZE)

    # Boot Sector (BPB)
    img[0:3] = b"\xeb\x58\x90"  # JMP
    img[3:11] = b"LUGALOS "  # OEM Name
    struct.pack_into("<H", img, 11, SECTOR_SIZE)  # Bytes per sector
    img[13] = 1  # Sectors per cluster
    struct.pack_into("<H", img, 14, RESERVED_SECTORS)  # Reserved sectors
    img[16] = 2  # Number of FATs
    struct.pack_into("<H", img, 17, 0)  # Root entries (0 for FAT32)
    struct.pack_into("<H", img, 19, 0)  # Total sectors 16 (0 for FAT32)
    img[21] = 0xF8  # Media descriptor
    struct.pack_into("<H", img, 22, 0)  # FAT size 16 (0 for FAT32)
    struct.pack_into("<H", img, 24, 63)  # Sectors per track
    struct.pack_into("<H", img, 26, 255)  # Number of heads
    struct.pack_into("<I", img, 28, 0)  # Hidden sectors
    struct.pack_into("<I", img, 32, num_sectors)  # Total sectors 32
    struct.pack_into("<I", img, 36, fat_size)  # FAT size 32 (16 sectors per FAT)
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
    struct.pack_into("<I", img, 512 + 488, num_sectors - data_start)  # Free clusters
    struct.pack_into("<I", img, 512 + 492, 2)  # Next free cluster
    img[512 + 510 : 512 + 512] = b"\x55\xaa"  # Trail Sig

    # FAT 1 & FAT 2 Base Offsets
    fat1_base: int = RESERVED_SECTORS * SECTOR_SIZE
    fat2_base: int = (RESERVED_SECTORS + fat_size) * SECTOR_SIZE

    def set_fat_entry(clus: int, val: int) -> None:
        struct.pack_into("<I", img, fat1_base + clus * 4, val)
        struct.pack_into("<I", img, fat2_base + clus * 4, val)

    # Reserve Cluster 0, 1, and 2 (Root Dir)
    set_fat_entry(0, 0x0FFFFFF8)  # Media type
    set_fat_entry(1, 0x0FFFFFFF)  # Clean shutdown
    set_fat_entry(2, 0x0FFFFFFF)  # Root cluster EOF

    current_free_cluster: int = 3
    capacity: int = num_sectors - data_start

    def alloc(count: int) -> list[int]:
        """`count` clusters in a row, chained, the last one ending the chain."""
        nonlocal current_free_cluster
        first = current_free_cluster
        current_free_cluster += count
        if current_free_cluster - 2 > capacity:
            raise ValueError(f"the content needs more than the {num_sectors}-sector image's "
                             f"{capacity} sectors of data")
        clusters = list(range(first, first + count))
        for i, c in enumerate(clusters):
            set_fat_entry(c, clusters[i + 1] if i + 1 < len(clusters) else 0x0FFFFFFF)
        return clusters

    def write_clusters(clusters: list[int], data: bytes) -> None:
        for i, c in enumerate(clusters):
            chunk = data[i * SECTOR_SIZE : (i + 1) * SECTOR_SIZE]
            off = (data_start + c - 2) * SECTOR_SIZE
            img[off : off + len(chunk)] = chunk

    def populate_dir(dir_path: str, first: int, parent: int) -> None:
        """Writes the directory at cluster `first` (already allocated, one
        cluster) and everything below it."""
        taken: set[bytes] = set()
        slots = bytearray()
        if first != 2:
            slots += dir_entry(b".          ", 0x10, 0, first, 0)
            slots += dir_entry(b"..         ", 0x10, 0, parent if parent != 2 else 0, 0)
        subdirs: list[tuple[str, int]] = []
        for name in sorted(os.listdir(dir_path)):
            # TrueType fonts (~2.9 MB) are staged in sd_root/fonts/ for hardware
            # p9sync, but exceed the 1 MB flashfs and 2 MB QEMU SD image budgets.
            if (first == 2 and name == "fonts") or name in exclude:
                continue
            full_path = os.path.join(dir_path, name)
            if os.path.isdir(full_path):
                sub = alloc(1)[0]
                subdirs.append((full_path, sub))
                slots += entry_set(name, 0x10, sub, 0, taken)
                print(f"[SD Image] Added Directory '{name}' (Cluster {sub})")
            elif os.path.isfile(full_path):
                with open(full_path, "rb") as f:
                    content = f.read()
                # An empty file has no cluster (first cluster 0), as the FAT
                # spec and fsck.fat have it.
                clusters = alloc(-(-len(content) // SECTOR_SIZE)) if content else []
                write_clusters(clusters, content)
                start = clusters[0] if clusters else 0
                slots += entry_set(name, 0x20, start, len(content), taken)
                print(f"[SD Image] Added File '{name}' ({len(content)} bytes, Cluster {start})")
        # The directory's own clusters: its first, then as many more as its slots need.
        need = max(1, -(-len(slots) // SECTOR_SIZE))
        chain = [first] + (alloc(need - 1) if need > 1 else [])
        if need > 1:
            set_fat_entry(first, chain[1])
        write_clusters(chain, bytes(slots))
        for full_path, sub in subdirs:
            populate_dir(full_path, sub, first)

    if src_dir and os.path.isdir(src_dir):
        populate_dir(src_dir, 2, 2)

    if current_free_cluster - 2 > num_sectors - data_start:
        raise ValueError(f"the content needs {current_free_cluster - 2} sectors of data and the "
                         f"{num_sectors}-sector image holds {num_sectors - data_start}")
    # Update FSInfo free cluster count & next free cluster pointer
    struct.pack_into("<I", img, 512 + 488, (num_sectors - data_start) - (current_free_cluster - 2))
    struct.pack_into("<I", img, 512 + 492, current_free_cluster)
    # The backup boot sector and FSInfo the boot sector points at (6 and 7).
    img[6 * SECTOR_SIZE : 8 * SECTOR_SIZE] = img[0 : 2 * SECTOR_SIZE]

    with open(output_path, "wb") as f:
        f.write(img)

    print(f"[SD Image] Successfully generated FAT32 disk image '{output_path}' ({len(img)} bytes)")


def newer_than_image(sd_root: str, out_file: str) -> bool:
    """Is anything in the staging directory -- or this builder -- newer than
    the built image?

    The builder counts: when the image's geometry changed (512 KB -> 1 MB),
    an image whose staged files had not changed was kept in the old layout,
    and nothing said so."""
    try:
        img_mtime = os.path.getmtime(out_file)
    except OSError:
        return True
    if os.path.getmtime(os.path.abspath(__file__)) > img_mtime:
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

    build_fat32_image(out_file, sd_root, SD_IMAGE_SECTORS)
