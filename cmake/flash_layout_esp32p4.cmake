# The ESP32-P4-NANO flash map -- U0, plan/phase32_esp32p4_execute_in_place.md.
#
# Same job, and the same reason, as cmake/flash_layout.cmake does for RP2350:
# these numbers are the *only* definition of where each segment lives, and
# they reach both the C code (as compile definitions) and the flashing tool
# (as generated arguments), so a value cannot be changed in one place and
# silently disagree in the other.
#
# ## This map replaces E6's, and the difference is ownership
#
# E6 laid its single segment out *around* the Waveshare factory demo, which
# the board shipped with and which is not obtainable again once overwritten.
# That produced a correct map with an unreconstructable shape: a filesystem at
# 0x00E00000 for no reason a later reader could derive, above 13 MB labelled
# NEVER TOUCHED.
#
# Phase 32 retires the factory image on purpose (the decision E6's version of
# this file explicitly required be a decision, not a side effect) because U4
# needs offset 0x2000, which is inside it. **LugalOS owns all 16 MB.** The
# backup at ~/gith/esp/p4nano-factory-flash/ stops being a constraint on this
# layout and becomes an archive -- 16,777,216 bytes, sha256 verified
# 2026-09-12, restorable with `esptool write-flash 0 p4nano-factory-16mb.bin`
# if anyone ever wants the demo back.
#
# What that buys is coherence rather than space: 256 KB would have fitted the
# old margin several times over. The map below can be read top to bottom and
# every boundary has a reason.
#
# ## The map
#
#   0x00000000  reserved, below the ROM's bootloader offset          8 KB
#   0x00002000  second-stage bootloader (U4)                        56 KB
#   0x00010000  OS image: .text + .rodata, executed in place         1 MB
#   0x00110000  flash filesystem /flash0                           512 KB
#   0x00190000  spare, contiguous                                 14.4 MB
#
# ### Why 0x2000 for the bootloader
#
# Not a choice. The P4's boot ROM reads its second-stage image from 0x2000
# and nowhere else: IDF's components/esp_rom/esp32p4/esp_rom_caps.h says
# `ESP_ROM_BOOTLOADER_OFFSET_FLASH (0x2000)`, commented "determined by the ROM
# bootloader", and components/bootloader/Kconfig.projbuild adds "It's not
# configurable in ESP-IDF". The factory image agrees: its partition table sat
# at 0x8000 with nvs at 0x9000, leaving exactly 0x2000..0x8000 for a
# bootloader.
#
# 56 KB of room for it is generous by a wide margin -- IDF's own second-stage
# bootloader is around 26 KB and ours does far less -- and the generosity
# costs nothing, because the next boundary has to be 64 KB-aligned anyway.
#
# ### Why 0x00010000 for the OS image, and why 1 MB
#
# 64 KB-aligned because the flash MMU maps in 64 KB pages: Cache_FLASH_MMU_Set
# refuses a physical address that is not aligned to the page size (error 2,
# "vaddr or paddr is not aligned"). So this boundary is a hardware
# requirement, not the house style -- though it satisfies that too.
#
# 1 MB against a present need of 256 KB (.text 186,200 + .rodata 68,904 as of
# U0). Four times the current size, because this is the segment that grows
# with the kernel and moving it later means moving everything above it.
#
# ### Why the filesystem moved
#
# It was at 0x00E00000 only because the factory image ended below it. With the
# factory image gone that address means nothing, and a number that means
# nothing is a number the next reader has to research. 512 KB is unchanged --
# the FAT32 image's size, which must match.
#
# ### Why the spare is at the top and contiguous
#
# Growth is cheap at the end of a map and expensive in the middle. Every
# segment above can grow into the spare by moving one boundary; a segment with
# spare on both sides of it cannot grow at all without moving its neighbours.
#
# Boundaries are 64 KB-aligned throughout, for E6's reason as well as the
# MMU's: the erase granularity is a 4 KB sector, and a segment ending
# mid-sector can erase into its neighbour. Aligning far above that makes it
# structurally impossible rather than merely unlikely.

set(LUGALOS_P4_FLASH_SIZE     0x01000000)   # 16 MB, GigaDevice c8 4018

# U4. The ROM's fixed offset; see above.
set(LUGALOS_P4_BOOTLOADER_BASE 0x00002000)
set(LUGALOS_P4_BOOTLOADER_SIZE 0x0000E000)  # 56 KB

# U1/U2. .text and .rodata, mapped into the flash XIP window and executed in
# place. Must be 64 KB-aligned: the MMU maps 64 KB pages.
set(LUGALOS_P4_OSIMAGE_BASE   0x00010000)
set(LUGALOS_P4_OSIMAGE_SIZE   0x00100000)   # 1 MB

set(LUGALOS_P4_FLASHFS_BASE   0x00110000)
set(LUGALOS_P4_FLASHFS_SIZE   0x00080000)   # 512 KB -- must match the FAT32 image

# The floor every write in drivers/flash_esp32p4.c is checked against.
#
# Named separately from FLASHFS_BASE even though they are the same number
# today, because they are different facts: one is where the filesystem starts,
# the other is the lowest address this kernel will write at run time. Keeping
# them apart is what lets the guard mean "below this lies code that must not
# be modified by a running system" -- the bootloader and the OS image -- so a
# second writable segment added later widens the filesystem without widening
# what a stray write can reach.
set(LUGALOS_P4_FLASH_WRITABLE_FLOOR 0x00110000)
