# Board: StarFive VisionFive 2 v1.3B (JH7110 quad U74-MC RV64GC + S7 monitor).
# Loaded by stock U-Boot 2021.10 / OpenSBI v1.0 at 0x40200000 in S-mode.
# What a board file is: cmake/lugalos_layers.cmake.

set(CONFIG_BOARD_DESC        "StarFive VisionFive 2 v1.3B (JH7110)")

# Fast page zone: 8192 pages = 32 MB in main DRAM above _kernel_end.
# Bulk page zone: 16384 pages = 64 MB right above the fast zone.
set(CONFIG_PALLOC_MAX_PAGES  8192)
set(CONFIG_PALLOC_BULK_PAGES 16384)

# DesignWare DW-APB-UART0 at 0x10000000 (32-bit registers on 4-byte stride,
# PLIC source 32). Clocks and pinmux are configured by SPL/U-Boot (24 MHz).
set(CONFIG_UART0_BASE        0x10000000)
set(CONFIG_UART_REG_SHIFT    2)
set(CONFIG_UART0_IRQ         32)

# JH7110 CLINT / rdtime increments at 4 MHz (24 MHz OSC / 6).
set(CONFIG_TIMEBASE_HZ       4000000)
set(CONFIG_PLIC_BASE         0x0C000000)
set(CONFIG_CLINT_BASE        0x02000000)

# Synopsys DesignWare Mobile Storage Host Controller (sdio1@16020000),
# wired to the microSD slot (4-bit, 50 MHz High Speed, 32-word FIFO).
set(LUGALOS_ENABLE_SDMMC     ON)
set(CONFIG_SDMMC_BASE        0x16020000)
set(CONFIG_SDMMC_BUS_WIDTH   4)
set(CONFIG_SDMMC_FREQ_KHZ    50000)
