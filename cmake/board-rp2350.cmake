# Per-board facts for RP2350 (Pico 2), consumed by cmake/gen_config.cmake to
# produce lugalos_config.h (K0, plan/phase7_kernel_config.md).
#
# Flat list of numbers, reviewable the same way kernel/board.c's device
# tables are: "is this number right", not "does this expand correctly". The
# schema (which keys are required, which are optional) lives in
# gen_config.cmake, not here.
#
# These were previously two independent hand-typed copies -- kernel/board.c's
# board_uart_base() and drivers/uart_rp2350.c's own UART0_BASE #define -- with
# nothing keeping them in sync. This file is now the one place both read from.

set(CONFIG_PALLOC_MAX_PAGES 128)

# Buddy-allocator arena (kernel/balloc.h), in pages. 4 = 16 KB, against 16
# (64 KB) on the QEMU targets.
#
# This board is the reason the constant is per-board at all (§1.1,
# plan/phase15_memory_reclamation.md). It is paid twice: the arena itself
# comes out of the heap on first use, and the buddy tree is permanent .bss
# whose size scales with this number -- 2046 bytes at 4 pages, 8190 at 16 --
# and on RP2350 .bss and the heap are the same 512 KB budget. 4 pages still
# leaves better than 2x headroom for the largest thing that actually calls
# this allocator (`ballocdemo`'s churn holds 7264 bytes of blocks at peak).
set(CONFIG_BALLOC_ARENA_PAGES 4)

# UART0: PL011, the board's console/9P wire (kernel/device.h's DEV_WIRE_UART0).
set(CONFIG_UART0_BASE     0x40070000)
set(CONFIG_UART0_TX_GPIO  0)
set(CONFIG_UART0_RX_GPIO  1)

# SPI1: the SD card bus. Not a dev_wire_t entry -- nothing else ever
# contends for it, so it was never a claimable/arbitrated wire (see
# plan/phase7_kernel_config.md's namespace note).
set(CONFIG_SPI1_BASE      0x40088000)
set(CONFIG_SPI1_SCK_GPIO  10)
set(CONFIG_SPI1_MOSI_GPIO 11)
set(CONFIG_SPI1_MISO_GPIO 12)
set(CONFIG_SPI1_CS_GPIO   13)

# Onboard + external status LEDs.
set(CONFIG_LED_ONBOARD_GPIO 25)
set(CONFIG_LED_EXT_GPIO     16)

# SPI0: the ST7735 TFT canvas bus (phase9 H1, plan/phase9_chess_computer.md).
# A second, independent PL022 controller from SPI1 above -- no contention, so
# same as SPI1 this is not a dev_wire_t entry.
set(CONFIG_SPI0_BASE        0x40080000)
set(CONFIG_ST7735_SCK_GPIO  18)
set(CONFIG_ST7735_MOSI_GPIO 19)
set(CONFIG_ST7735_CS_GPIO   17)
set(CONFIG_ST7735_DC_GPIO   20)
set(CONFIG_ST7735_RST_GPIO  21)

# TM1638: 8-digit 7-segment display + 4x4 keypad (phase9 H2). A bit-banged
# 3-wire protocol, not a real SPI/I2C peripheral -- these pins were chosen
# specifically to avoid the UART0/SPI1 claims above (see
# drivers/tm1638_rp2350.c).
set(CONFIG_TM1638_STB_GPIO 6)
set(CONFIG_TM1638_CLK_GPIO 7)
set(CONFIG_TM1638_DIO_GPIO 8)

# DS1307/DS3231 RTC (drivers/i2c_rtc.c). GP4/GP5 land on the I2C0
# peripheral instance on RP2350 (L3, plan/phase11_pico_clock_green.md --
# the GPIO-to-controller mapping alternates every 4 pins, it isn't a
# software choice). The Pico-Clock-Green baseboard's own board file uses
# GP6/GP7/I2C1 instead, since that's what its RTC is physically wired to.
set(CONFIG_I2C_RTC_BASE     0x40090000) # I2C0
set(CONFIG_I2C_RTC_SDA_GPIO 4)
set(CONFIG_I2C_RTC_SCL_GPIO 5)
