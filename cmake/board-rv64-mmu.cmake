# Per-board facts for the QEMU rv64 Sv39 MMU target ("virt" machine),
# consumed by cmake/gen_config.cmake to produce lugalos_config.h (K0,
# plan/phase7_kernel_config.md).
#
# No real GPIO pins to describe -- QEMU's UART0 is a 16550 MMIO device, not
# physical silicon, so this board file only carries platform defaults.

set(CONFIG_PALLOC_MAX_PAGES 4096)
set(CONFIG_UART0_BASE       0x10000000)
