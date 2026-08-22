# Per-board facts for RP2350 (Pico 2 / Pico 2 W), populated for the
# Waveshare Pico-Clock-Green baseboard persona (L1,
# plan/phase11_pico_clock_green.md). Same RP2350 silicon/arch/linker script
# as cmake/board-rp2350.cmake -- this is a second board file, not a second
# LUGALOS_TARGET, selected via -DLUGALOS_BOARD_FILE=cmake/board-rp2350-clock.cmake
# (see CMakeLists.txt) because the baseboard's soldered GPIO wiring collides
# with that file's SPI1 (SD card) and LED_EXT pin choices (phase11 L0's
# pin-conflict table).
#
# Built with LUGALOS_ENABLE_SPISD=OFF, LUGALOS_ENABLE_ST7735=OFF,
# LUGALOS_ENABLE_TM1638=OFF, LUGALOS_ENABLE_CHESS=OFF (see
# CMakePresets.json's rp2350-clock preset) -- none of that hardware is
# wired on this baseboard.
#
# The buttons/buzzer (SET_FUNCTION/UP/DOWN/SQW/BUZZ) aren't declared here
# yet -- deliberately out of scope for L2/L3; see
# plan/phase11_pico_clock_green.md.

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

# UART0: PL011, the board's console/9P wire -- unaffected by this baseboard,
# same GP0/GP1 as cmake/board-rp2350.cmake.
set(CONFIG_UART0_BASE     0x40070000)
set(CONFIG_UART0_TX_GPIO  0)
set(CONFIG_UART0_RX_GPIO  1)

# Onboard LED only. GP16 (LED_EXT on cmake/board-rp2350.cmake) is claimed by
# the clock display's A0 row-address line (phase11 L0), so drivers/uart_
# rp2350.c's unconditional heartbeat blink is remapped to GP9 instead --
# free on this baseboard's pinout (~/gith/Pico-Clock-Green/define.h).
set(CONFIG_LED_ONBOARD_GPIO 25)
set(CONFIG_LED_EXT_GPIO     9)

# Deliberately no CONFIG_SPI1_*: GP10-13 are the clock display's CLK/SDI/
# LE/OE (SM16106 shift registers) on this baseboard, not the SD-card bus --
# see LUGALOS_ENABLE_SPISD=OFF above and drivers/spisd_rp2350.c's stub
# branch. Deliberately no CONFIG_SPI0_*/CONFIG_ST7735_*/CONFIG_TM1638_*
# either: this persona has neither the chess TFT nor the TM1638 keypad.

# L2 (plan/phase11_pico_clock_green.md): SM16106 x2 (column shift
# registers) + SM5166P (row address decoder) driving the 8x24 LED matrix,
# plus the LDR for auto-brightness. Pin numbers straight from
# ~/gith/Pico-Clock-Green/define.h (OE/SDI/CLK/LE/A0-A2/ADC_Light) -- the
# baseboard's soldered wiring, not a LugalOS choice.
set(CONFIG_CLOCK_OE_GPIO         13)
set(CONFIG_CLOCK_SDI_GPIO        11)
set(CONFIG_CLOCK_CLK_GPIO        10)
set(CONFIG_CLOCK_LE_GPIO         12)
set(CONFIG_CLOCK_A0_GPIO         16)
set(CONFIG_CLOCK_A1_GPIO         18)
set(CONFIG_CLOCK_A2_GPIO         22)
set(CONFIG_CLOCK_ADC_LIGHT_GPIO  26)

# L3 (plan/phase11_pico_clock_green.md): DS3231 RTC on GP6/GP7 -- the
# vendor's own wiring (~/gith/Pico-Clock-Green/define.h: SDA=6, SCL=7,
# i2c1). RP2350's GPIO-to-controller mapping alternates every 4 pins, so
# GP6/GP7 land on the I2C1 peripheral instance, not the I2C0 instance
# cmake/board-rp2350.cmake's GP4/GP5 use -- a different base address, not
# just different pins (drivers/i2c_rtc.c derives which RESETS bit to use
# from this base address).
set(CONFIG_I2C_RTC_BASE     0x40098000) # I2C1
set(CONFIG_I2C_RTC_SDA_GPIO 6)
set(CONFIG_I2C_RTC_SCL_GPIO 7)
