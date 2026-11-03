# Per-board facts for the QEMU rv32 NOMMU target ("virt" machine), consumed
# by cmake/gen_config.cmake to produce lugalos_config.h (K0,
# ).
#
# No real GPIO pins to describe -- QEMU's UART0 is a 16550 MMIO device, not
# physical silicon, so this board file only carries platform defaults.

set(CONFIG_PALLOC_MAX_PAGES 4096)

# a stand-in bulk page zone, 2 MB of ordinary
# RAM right above the fast zone, so the suite runs the zone code, the
# by-address free and the SRAM fallback without a PSRAM.
set(CONFIG_PALLOC_BULK_PAGES 512)
set(CONFIG_UART0_BASE       0x10000000)

# Named by /proc/board and the banner (kernel/boardinfo.h).
set(CONFIG_BOARD_DESC "QEMU virt, rv32 without MMU")
