# The ESP32-P4-NANO flash map -- E6, plan/phase27_esp32p4_bringup.md.
#
# Same job, and the same reason, as cmake/flash_layout.cmake does for RP2350:
# these numbers are the *only* definition of where the segment lives, and they
# reach both the C code (as compile definitions) and the flashing tool (as
# generated arguments), so a value cannot be changed in one place and silently
# disagree in the other.
#
# ## Where the numbers come from, and why they cannot collide
#
# This board shipped with a Waveshare factory demo, and that image is not
# obtainable again once overwritten. Rather than trust a guess about where it
# ends, the partition table was read out of the verified backup at
# ~/gith/esp/p4nano-factory-flash/ and parsed:
#
#     nvs         data 0x02  0x00009000  0x00006000
#     phy_init    data 0x01  0x0000f000  0x00001000
#     factory     app  0x00  0x00010000  0x00900000
#     storage     data 0x82  0x00910000  0x00400000
#
# The highest byte any factory partition claims is 0x00D10000 (13.06 MB), and
# every one of the 3,080,192 bytes above that reads 0xFF -- erased, unused,
# and confirmed so from the backup image rather than assumed.
#
# So the segment starts at 0x00E00000, leaving ~0.9 MB of untouched margin
# below it. **E6 therefore cannot damage the factory image**, and the board
# still boots the factory demo after every flash write this project makes.
# The backup stays a safety net rather than a load-bearing part of the plan.
#
# Boundaries are 64 KB-aligned for the reason flash_layout.cmake states: the
# erase granularity is a 4 KB sector, and a segment ending mid-sector can
# erase into its neighbour. Aligning far above that makes it structurally
# impossible rather than merely unlikely.
#
#   0x00000000  factory bootloader, tables, app, storage   13.06 MB  NEVER TOUCHED
#   0x00D10000  unallocated margin                           ~0.9 MB
#   0x00E00000  flash-fs                                      512 KB
#   0x00E80000  unallocated                                   1.5 MB
#
# There is deliberately no OS-image segment here. This kernel is loaded into
# L2MEM by `esptool load-ram` and does not boot from flash at all, so unlike
# RP2350 there is nothing to reserve for it. If that ever changes it goes
# below 0x00D10000 only by first retiring the factory image on purpose.

set(LUGALOS_P4_FLASH_SIZE     0x01000000)   # 16 MB, GigaDevice c8 4018

set(LUGALOS_P4_FLASHFS_BASE   0x00E00000)
set(LUGALOS_P4_FLASHFS_SIZE   0x00080000)   # 512 KB -- must match the FAT32 image

# The floor every write in drivers/flash_esp32p4.c is checked against. Not
# simply FLASHFS_BASE: naming it separately is what lets the guard say "below
# the region this project owns" rather than "outside this one segment", so a
# second segment added later does not quietly widen what is writable.
set(LUGALOS_P4_FLASH_WRITABLE_FLOOR 0x00E00000)
