#!/usr/bin/env python3
"""Extract R_RISCV_32 / R_RISCV_64 relocation offsets from an unstripped ELF
linked with -Wl,-q (--emit-relocs) and append a LREL PT_NOTE table to the
stripped ELF so the kernel loader can relocate position-independent images on
both NOMMU and MMU targets without dynamic linker sections.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

SHT_RELA = 4
PT_NOTE = 4
R_RISCV_32 = 1
R_RISCV_64 = 2
MAGIC = b"LREL"


def extract_abs_relocs(elf_bytes: bytes) -> list[int]:
    if elf_bytes[:4] != b"\x7fELF":
        raise ValueError("Not an ELF file")
    elf_class = elf_bytes[4]
    if elf_class == 1:
        eh = struct.unpack_from("<16sHHIIIIIHHHHHH", elf_bytes, 0)
        e_shoff, e_shentsize, e_shnum = eh[6], eh[11], eh[12]
        sh_fmt = "<IIIIIIIIII"
        rela_fmt = "<IIi"
        want_type = R_RISCV_32
        r_type_mask = 0xFF
    elif elf_class == 2:
        eh = struct.unpack_from("<16sHHIQQQIHHHHHH", elf_bytes, 0)
        e_shoff, e_shentsize, e_shnum = eh[6], eh[11], eh[12]
        sh_fmt = "<IIQQQQIIQQ"
        rela_fmt = "<QQq"
        want_type = R_RISCV_64
        r_type_mask = 0xFFFFFFFF
    else:
        raise ValueError(f"Unsupported ELF class {elf_class}")

    # Collect ALLOC section ranges (SHF_ALLOC = 0x2)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        fields = struct.unpack_from(sh_fmt, elf_bytes, off)
        sections.append(fields)

    offsets: list[int] = []
    rela_ent = struct.calcsize(rela_fmt)
    for sh in sections:
        sh_type = sh[1]
        if sh_type != SHT_RELA:
            continue
        sh_offset = sh[4]
        sh_size = sh[5]
        sh_info = sh[7]  # target section index
        if sh_info >= len(sections):
            continue
        target_flags = sections[sh_info][2]
        if (target_flags & 0x2) == 0:  # skip non-ALLOC (.debug_*)
            continue
        for pos in range(sh_offset, sh_offset + sh_size, rela_ent):
            r_offset, r_info, _ = struct.unpack_from(rela_fmt, elf_bytes, pos)
            if (r_info & r_type_mask) == want_type:
                offsets.append(r_offset)
    offsets.sort()
    return offsets


def append_lrel_note(stripped_path: Path, offsets: list[int]) -> None:
    data = bytearray(stripped_path.read_bytes())
    elf_class = data[4]
    note_off = len(data)
    payload = bytearray(MAGIC + struct.pack("<I", len(offsets)))
    for off in offsets:
        payload.extend(struct.pack("<I", off))
    note_size = len(payload)

    if elf_class == 1:
        e_phoff, = struct.unpack_from("<I", data, 28)
        e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
        new_phdr_off = e_phoff + e_phnum * e_phentsize
        if new_phdr_off + e_phentsize > 0x1000:
            raise ValueError("No room in program header page for LREL PT_NOTE")
        struct.pack_into("<IIIIIIII", data, new_phdr_off, PT_NOTE, note_off, 0, 0, note_size, note_size, 4, 4)
        struct.pack_into("<H", data, 44, e_phnum + 1)
    elif elf_class == 2:
        e_phoff, = struct.unpack_from("<Q", data, 32)
        e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
        new_phdr_off = e_phoff + e_phnum * e_phentsize
        if new_phdr_off + e_phentsize > 0x1000:
            raise ValueError("No room in program header page for LREL PT_NOTE")
        struct.pack_into("<IIQQQQQQ", data, new_phdr_off, PT_NOTE, 4, note_off, 0, 0, note_size, note_size, 8)
        struct.pack_into("<H", data, 56, e_phnum + 1)
    else:
        raise ValueError(f"Unsupported ELF class {elf_class}")

    data.extend(payload)
    stripped_path.write_bytes(data)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <unstripped.elf> <stripped>", file=sys.stderr)
        return 2
    offsets = extract_abs_relocs(Path(sys.argv[1]).read_bytes())
    append_lrel_note(Path(sys.argv[2]), offsets)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
