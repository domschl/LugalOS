# The RP2350 flash map, in one place -- I7a, .
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
#   storage    what                                       runtime address
#   0x000000   partition table (one sector)               -
#   0x001000   slot A, image up to 0x17C000 bytes          0x10000000 when booted
#   0x17D000   slot A's update header (one sector)        -
#   0x180000   flash-fs  1 MB                             0x10400000
#   0x280000   slot B  (same size as A)                   0x10000000 when booted
#   0x3FD000   boot records (two sectors)                 -
#   0x3FF000   identity sector                            0x10800000
#
# ## Two addresses for everything outside the slots
#
# The bootrom boots a partition by pointing the QMI's address translation at
# it: the partition appears at 0x10000000 and *nothing else of the flash is
# mapped* there (an access past its size is a bus fault; datasheet 5.1.19).
# So the filesystem and the identity sector, which are read through pointers,
# get windows of their own that the kernel sets up in ATRANS1 and ATRANS2
# before it touches either (arch/riscv/rp2350/xip_map.c). The `_PHYS` names are
# flash storage offsets -- what the flash write routines and the UF2 files
# use; the `_BASE` names are where the running kernel reads them.
#
# The slots are an A/B pair for the bootrom (tools/rp2350_ptable.py makes the
# table); the update header sits in the slot's last sector, because the first
# bytes of a slot are the image the bootrom must find.

set(LUGALOS_FLASH_BASE      0x10000000)   # XIP window, and the OS image's origin
set(LUGALOS_FLASH_SIZE      0x00400000)   # 4 MB of flash on the Pico 2 / 2 W

set(LUGALOS_PTABLE_PHYS     0x00000000)
set(LUGALOS_SLOT_A_PHYS     0x00001000)
set(LUGALOS_SLOT_B_PHYS     0x00280000)
set(LUGALOS_SLOT_SIZE       0x0017C000)   # the image, up to this size
set(LUGALOS_SLOT_HDR_OFF    0x0017C000)   # the update header, in the slot's last sector
set(LUGALOS_SLOT_SPAN       0x0017D000)   # the slot with its header sector
set(LUGALOS_BOOTREC_PHYS    0x003FD000)

set(LUGALOS_FLASHFS_PHYS    0x00180000)
set(LUGALOS_FLASHFS_BASE    0x10400000)
set(LUGALOS_FLASHFS_SIZE    0x00100000)   # 1 MB -- must match the FAT32 image

set(LUGALOS_IDENTITY_PHYS   0x003FF000)
set(LUGALOS_IDENTITY_BASE   0x10800000)
set(LUGALOS_IDENTITY_SIZE   0x00001000)   # one 4 KB sector
