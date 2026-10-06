# The ESP32-C6-Zero flash map -- 45.4.2, plan/phase45_esp32c6.md.
#
# The only definition of where each segment lives, reaching the C code (compile
# definitions) and the flashing tool (the generated manifest) so the two cannot
# disagree -- the same discipline as cmake/flash_layout_esp32p4.cmake. 8 MB part
# (esptool reported 8 MB on this board in 45.1), and LugalOS owns all of it: the
# Waveshare demo that shipped on it is expendable (decision 2026-10-05).
#
#   0x000000  second-stage image: the RAM half of the kernel        128 KB
#   0x020000  OS image: .text + .rodata, executed in place            2 MB
#   0x220000  /flash0 filesystem (45.4.3+)                            1 MB
#   0x320000  spare, contiguous                                      4.9 MB
#
# 0x0 is not a choice: the C6's boot ROM reads its second-stage image from flash
# offset 0 (IDF components/esp_rom/esp32c6/include/esp_rom_caps.h,
# ESP_ROM_BOOTLOADER_OFFSET, and the factory image's own header there). The OS
# image has to be 64 KB-aligned because the flash MMU maps 64 KB pages (the
# 45.4.2 probe wrote its pattern at 0x20000 for the same reason), and 2 MB is
# five times the present text + rodata and room for the Wi-Fi blob's 374 KB.
set(LUGALOS_C6_FLASH_SIZE        0x800000)
set(LUGALOS_C6_STAGE2_BASE       0x000000)
set(LUGALOS_C6_STAGE2_SIZE       0x020000)
set(LUGALOS_C6_OSIMAGE_BASE      0x020000)
set(LUGALOS_C6_OSIMAGE_SIZE      0x200000)
set(LUGALOS_C6_FLASHFS_BASE      0x220000)
set(LUGALOS_C6_FLASHFS_SIZE      0x100000)
