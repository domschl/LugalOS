# Per-board facts for the Waveshare RP2350-LCD-7 (non-touch), populated for
# the **terminal / stand-alone workstation** persona
# (plan/phase36_rp2350_lcd7_terminal.md).
#
# An RP2350**B** (QFN-80, GPIO0-47) on one board with a 7" 800x480 RGB panel
# (ST7262), 16 MB flash, 2 MB PSRAM, a microSD socket, RS485, CAN and two
# USB-C ports: the native one (J3: flashing, /dev/ttyACM0) and a PIO-driven
# host port (J7, GP42/43) for the keyboard. Every pin below was read off the
# schematic's pin table (datasheet/RP2350-Touch-LCD-7.pdf, 2026-09-30), not
# taken from Waveshare's demo headers -- see §1.5 of the plan for where the
# two disagree.
#
# Same RP2350 silicon family, arch and linker script as every other RP2350
# persona -- a board file, not a new LUGALOS_TARGET, selected via the
# rp2350-terminal preset.

set(CONFIG_PALLOC_MAX_PAGES 128)
set(CONFIG_BALLOC_ARENA_PAGES 4)

# clk_sys at 144 MHz, not the 150 every other persona runs (36.1, plan §3.3).
# PIO-USB wants a multiple of 12 MHz -- at 150 the full-speed TX divider is
# 3.125, and a fractional PIO divider dithers each bit edge by a whole cycle
# (6.7 ns of an 83 ns bit) -- and 144 also gives the panel an integer 24 MHz
# pixel clock at 6 cycles per pixel. PLL_SYS: 12 MHz x 120 = 1440 MHz VCO,
# /5 /2. arch/rp2350_clocks.h derives the rest, and every UART/SPI/I2C/PWM
# divider follows it.
set(CONFIG_CLK_SYS_HZ 144000000)

# UART0 on GP16/GP17, header H7 (pin 1 3V3, 2 GND, 3 RXD0, 4 TXD0).
#
# **Not GP0/GP1, which every other RP2350 board file uses, and this is not a
# preference.** GP0 on this board is the PSRAM's /CS (QSPI_SS2, R21 10K
# pull-up), and the PSRAM shares QSPI_SD0-3 and QSPI_SCLK with the flash that
# .text executes from in place. A UART TX idles high, but every start bit
# would pull /CS low and select the PSRAM in the middle of an XIP fetch --
# two chips driving the same data lines, and a board fetching garbage
# instructions whenever the console prints. Nothing on this persona touches
# GP0 until PSRAM support configures it as XIP_CS1 on purpose.
set(CONFIG_UART0_BASE     0x40070000)
set(CONFIG_UART0_TX_GPIO  16)
set(CONFIG_UART0_RX_GPIO  17)

# Deliberately no CONFIG_LED_*: this board has no user LED on any GPIO
# (Led1/Led2 on the schematic are power and charge indicators). With neither
# key set, drivers/uart_rp2350.c starts no heartbeat task and claims no pin.

# SPI1 for the microSD socket, in SPI mode (plan §4.9). The socket is wired
# for 4-bit SDIO, and those pins happen to be exactly SPI1 by the RP2350
# function table: GP10 = SDIO_SCK = SPI1 SCK, GP11 = SDIO_CMD = SPI1 TX (the
# card's DI in SPI mode), GP12 = SDIO_D0 = SPI1 RX (DO). SDIO_D3 is the card's
# chip select in SPI mode and sits on GP15, driven as a plain GPIO, which is
# how drivers/spisd_rp2350.c drives CS anyway. SDIO_D1/D2 (GP13/14) are left
# as inputs.
set(CONFIG_SPI1_BASE      0x40088000)
set(CONFIG_SPI1_SCK_GPIO  10)
set(CONFIG_SPI1_MOSI_GPIO 11)
set(CONFIG_SPI1_MISO_GPIO 12)
set(CONFIG_SPI1_CS_GPIO   15)

# I2C1 on GP6/GP7: the touch controller's bus on the touch version (not
# fitted here), brought out through an NDC7002N level shifter to header H6
# (VCC, GND, SDA, SCL). Nothing is attached today; the RTC/EEPROM/BME280
# probes find nothing and say so once. An RP2350 build needs *some* I2C bus
# declared (drivers/i2c_bus.c), and this is the only one the board brings
# out. I2C1, not I2C0: GP6/7 are I2C1 in the function table.
set(CONFIG_I2C_RTC_BASE     0x40098000) # I2C1
set(CONFIG_I2C_RTC_SDA_GPIO 6)
set(CONFIG_I2C_RTC_SCL_GPIO 7)

# The PIO-USB host port, USB-C J7: D+ on GP42, D- on GP43, each through 27 R
# (schematic "Type C" block). J7 *supplies* VBUS from the board's 5 V through
# Q4, which is what makes it the host port; the keyboard lives here. Declared
# now for boardprobe (36.0), driven by PIO from 36.7.
set(CONFIG_PIOUSB_DP_GPIO 42)
set(CONFIG_PIOUSB_DM_GPIO 43)

# Not declared yet, each with a pin already reserved by the board:
#   UART1 GP8/9  -- hard-wired to the SP3485 RS485 transceiver (plan §7)
#   SPI0 GP2-5   -- the XL2515 CAN controller, INT GP1, RST GP46
#   GP20-45      -- the LCD (DE/VSYNC/HSYNC/PCLK, RGB565 on GP24-39, RST 41,
#                   backlight 44, EN 45), 36.3 onward
#   GP40         -- battery ADC
