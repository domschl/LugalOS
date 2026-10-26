# The Waveshare RP2350-LCD-7's flash map: 16 MB, and 4 MB slots.
#
# Everything the common map (flash_layout.cmake) puts in the first 4 MB stays
# where it is -- the partition table, /flash0, the boot records and the
# identity sector -- so tools/provision.py's identity address, the first-time
# install and flashfs.uf2 are the same as on a Pico 2. Only the slots move up
# into the part of the chip the Pico does not have:
#
#   storage    what                                       runtime address
#   0x000000   partition table (one sector)               -
#   0x180000   flash-fs  1 MB                             0x10400000
#   0x3FD000   boot records (two sectors)                 -
#   0x3FF000   identity sector                            0x10800000
#   0x400000   slot A, image up to 0x3FF000 bytes          0x10000000 when booted
#   0x7FF000   slot A's update header (one sector)        -
#   0x800000   slot B (the same)                          0x10000000 when booted
#   0xC00000   unallocated, 4 MB
#
# A slot is exactly 4 MB, the largest one QMI address-translation window
# (ATRANS0) maps, so the booted partition leaves ATRANS1 and ATRANS2 for the
# /flash0 and identity windows (arch/riscv/rp2350/bootrom.c) as on the Pico.
# A bigger slot would take ATRANS1 and need a different arrangement. The
# terminal's image is about 0.8 MB, so this is headroom, not need: the
# Pico-sized slots would have worked, and the low 4 MB below 0x400000 that the
# common map gave them is simply not used for a slot here.

include(${CMAKE_CURRENT_LIST_DIR}/flash_layout.cmake)

set(LUGALOS_FLASH_SIZE      0x01000000)   # 16 MB

set(LUGALOS_SLOT_A_PHYS     0x00400000)
set(LUGALOS_SLOT_B_PHYS     0x00800000)
set(LUGALOS_SLOT_SIZE       0x003FF000)
set(LUGALOS_SLOT_HDR_OFF    0x003FF000)
set(LUGALOS_SLOT_SPAN       0x00400000)
