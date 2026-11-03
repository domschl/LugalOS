#!/usr/bin/env python3
"""Prepends the RP2350-E10 workaround block to a UF2.

    uf2_e10.py IMAGE.uf2 SECTOR_PHYS

Erratum RP2350-E10 (datasheet, Errata): the A2 bootrom reads the partition
table before it has set up the flash, so a drag-and-drop download into a
partition fails without a word. The workaround is one block at the start of
the UF2 with the absolute family id, block 0 of 2: the bootrom writes it,
which sets up the flash, and the family change that follows starts the real
transfer. The block erases the 4 KB sector it lands in, so SECTOR_PHYS must be
one nothing uses (cmake/flash_layout.cmake LUGALOS_E10_SECTOR_PHYS). On A3 and
later bootroms the block is written too, harmlessly, into that same sector.
"""

import struct
import sys
from pathlib import Path

UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x00002000
FAMILY_ABSOLUTE = 0xE48BFF57
FLASH_BASE = 0x10000000


def e10_block(sector_phys: int) -> bytes:
    head = struct.pack("<8I", UF2_MAGIC0, UF2_MAGIC1, FLAG_FAMILY, FLASH_BASE + sector_phys,
                       256, 0, 2, FAMILY_ABSOLUTE)
    return head + b"\xff" * 256 + b"\0" * 220 + struct.pack("<I", UF2_MAGIC_END)


def main() -> int:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, sector = Path(sys.argv[1]), int(sys.argv[2], 0)
    if sector % 4096:
        sys.exit("SECTOR_PHYS %#x is not on a 4 KB boundary" % sector)
    data = path.read_bytes()
    if struct.unpack_from("<I", data, 28)[0] == FAMILY_ABSOLUTE:
        return 0                                  # already there
    path.write_bytes(e10_block(sector) + data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
