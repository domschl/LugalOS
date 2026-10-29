# Per-board facts for the QEMU rv64 Sv39 MMU target ("virt" machine),
# consumed by cmake/gen_config.cmake to produce lugalos_config.h (K0,
# ).
#
# No real GPIO pins to describe -- QEMU's UART0 is a 16550 MMIO device, not
# physical silicon, so this board file only carries platform defaults.

set(CONFIG_PALLOC_MAX_PAGES 4096)
set(CONFIG_LISP_NODE_POOL   65536)

# Buddy-allocator arena (kernel/balloc.h), in pages: 16 = 64 KB.
# Kept here where the heap is large and .bss is immaterial;
# RP2350 lowers it to 4 where .bss and the heap share a tighter budget.
set(CONFIG_BALLOC_ARENA_PAGES 16)

# a stand-in bulk page zone, 2 MB of ordinary
# RAM right above the fast zone, so the suite runs the zone code, the
# by-address free and the SRAM fallback without a PSRAM.
set(CONFIG_PALLOC_BULK_PAGES 512)
set(CONFIG_UART0_BASE       0x10000000)

# P6: the NTP server, on the QEMU targets so the suite can exercise it.
#
# These boards have no radio, so the server here can only ever answer
# "unsynchronised" -- which is exactly the property most worth testing. §4's
# whole claim is that admitting ignorance beats advertising a confidence that
# cannot be justified, and a target with nothing to be confident *about* is
# where that claim is checkable without hardware.
set(CONFIG_ENABLE_NTP_SERVER 1)

# Claiming over the network (kernel/setup.c): this target runs the test for it; rv32-nommu does not,
# and runs the test that a build without the flag refuses.
set(CONFIG_NETWORK_CLAIM 1)

# Named by /proc/board and the banner (kernel/boardinfo.h).
set(CONFIG_BOARD_DESC "QEMU virt, rv64 Sv39")
