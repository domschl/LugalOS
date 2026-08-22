# Per-board facts for the QEMU rv64 Sv39 MMU target ("virt" machine),
# consumed by cmake/gen_config.cmake to produce lugalos_config.h (K0,
# plan/phase7_kernel_config.md).
#
# No real GPIO pins to describe -- QEMU's UART0 is a 16550 MMIO device, not
# physical silicon, so this board file only carries platform defaults.

set(CONFIG_PALLOC_MAX_PAGES 4096)

# Buddy-allocator arena (kernel/balloc.h), in pages: 16 = 64 KB, M1's
# original figure. Kept here where the heap is 128 MB and the tree's 8190
# bytes of .bss are immaterial; RP2350 lowers it to 4 because there .bss and
# the heap are the same 512 KB budget (§1.1,
# plan/phase15_memory_reclamation.md).
set(CONFIG_BALLOC_ARENA_PAGES 16)
set(CONFIG_UART0_BASE       0x10000000)
