# The ESP32-C6-MINI-1's flash map -- 49.5, plan/phase49_c6_network_nodes.md.
# Selected by the esp32c6-mini preset (LUGALOS_C6_MODULE=MINI) in place of
# cmake/flash_layout_esp32c6.cmake, whose map assumes the C6-Zero's 8 MB.
#
#   0x000000  second-stage image: the RAM half of the kernel        128 KB
#   0x020000  OS image, slot A: .text + .rodata, executed in place  1.5 MB
#   0x1A0000  OS image, slot B: reserved for updates over 9P (49.8)  1.5 MB
#   0x320000  spare                                                 832 KB
#   0x3F0000  identity block: one 4 KB record sector at its start    64 KB
#
# 1.5 MB per slot: the OS image is 1.0 MB with the Wi-Fi blob (2026-10-12).
# Both slots now, so that 49.8's A/B update is not a re-layout of boards that
# are already in the field. No /flash0: the C6 does not mount one.
set(LUGALOS_C6_FLASH_SIZE        0x400000)
set(LUGALOS_C6_STAGE2_BASE       0x000000)
set(LUGALOS_C6_STAGE2_SIZE       0x020000)
set(LUGALOS_C6_OSIMAGE_BASE      0x020000)
set(LUGALOS_C6_OSIMAGE_SIZE      0x180000)
set(LUGALOS_C6_OSIMAGE_B_BASE    0x1A0000)
set(LUGALOS_C6_OSIMAGE_B_SIZE    0x180000)
set(LUGALOS_C6_IDENTITY_BASE     0x3F0000)
set(LUGALOS_C6_IDENTITY_SIZE     0x001000)
