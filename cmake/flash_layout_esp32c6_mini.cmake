# The ESP32-C6-MINI-1's flash map.
# Selected by the esp32c6-mini preset (LUGALOS_C6_MODULE=MINI) in place of
# cmake/flash_layout_esp32c6.cmake, whose map assumes the C6-Zero's 8 MB.
#
#   0x000000  the loader (arch/riscv/esp32c6/loader.c), what the ROM boots   64 KB
#   0x010000  boot records: two 4 KB sectors, one valid at a time             8 KB
#   0x012000  reserved                                                       56 KB
#   0x020000  slot A: an update image (header, RAM half, XIP half)          1.5 MB
#   0x1A0000  slot B: the same                                              1.5 MB
#   0x320000  spare                                                         832 KB
#   0x3F0000  identity block: one 4 KB record sector at its start            64 KB
#
# Each slot holds one update image as kernel/include/kernel/ota_image.h lays it
# out: the 256-byte header in the slot's first sector, the RAM half (the ROM's
# image format) at +4 KB, the XIP half at +128 KB -- 64 KB-aligned, because the
# flash MMU maps 64 KB pages. The loader is installed once, through the header
# pins; slots are rewritten over 9P. No /flash0: the C6 does not mount one.
#
# 1.5 MB per slot: the XIP half is 1.0 MB with the Wi-Fi blob (2026-10-12).
set(LUGALOS_C6_FLASH_SIZE        0x400000)
set(LUGALOS_C6_LOADER_BASE       0x000000)
set(LUGALOS_C6_LOADER_SIZE       0x010000)
set(LUGALOS_C6_BOOTREC_BASE      0x010000)
set(LUGALOS_C6_SLOT_A_BASE       0x020000)
set(LUGALOS_C6_SLOT_B_BASE       0x1A0000)
set(LUGALOS_C6_SLOT_SIZE         0x180000)
set(LUGALOS_C6_IDENTITY_BASE     0x3F0000)
set(LUGALOS_C6_IDENTITY_SIZE     0x001000)
