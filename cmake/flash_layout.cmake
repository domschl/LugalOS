# The RP2350 flash map, in one place -- I7a, plan/phase21_identity_and_authentication.md §3.3.
#
# These four numbers are the *only* definition of where the three segments
# live. They reach the linker script as --defsym symbols and the C code as
# compile definitions, so a value can never be changed in one place and
# silently disagree in the other. That is not tidiness: with the filesystem no
# longer inside the ELF, the linker cannot ASSERT against it directly, so
# nothing structural would otherwise stop linker/rp2350.ld and the UF2 step
# from drifting apart (§10, "the segment bases drift apart").
#
# Boundaries are 64 KB-aligned deliberately. The bootrom erases whole 4 KB
# sectors to write 256-byte chunks, so a segment ending mid-sector can erase
# into its neighbour; aligning far above the erase granularity makes that
# structurally impossible rather than merely unlikely. Measured on hardware
# 2026-09-01 -- see I7a's boundary result.
#
#   0x10000000  OS image        1.5 MB reserved  (~470 KB used)
#   0x10180000  flash-fs         512 KB
#   0x10200000  unallocated       ~2 MB          -- a writable app FS later
#   0x103FF000  identity sector     4 KB         -- last sector, never in a UF2
#                                                   this build emits

set(LUGALOS_FLASH_BASE      0x10000000)   # XIP window, and the OS image's origin
set(LUGALOS_FLASH_SIZE      0x00400000)   # 4 MB of flash on the Pico 2 / 2 W

set(LUGALOS_FLASHFS_BASE    0x10180000)
set(LUGALOS_FLASHFS_SIZE    0x00080000)   # 512 KB -- must match the FAT32 image

set(LUGALOS_IDENTITY_BASE   0x103FF000)
set(LUGALOS_IDENTITY_SIZE   0x00001000)   # one 4 KB sector (I7b fills it in)
