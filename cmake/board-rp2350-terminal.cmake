# Per-board facts for the Waveshare RP2350-LCD-7 (non-touch), populated for
# the **terminal / stand-alone workstation** persona
# (plan/phase36_rp2350_lcd7_terminal.md).
#
# An RP2350**B** (QFN-80, GPIO0-47) on one board with a 7" 800x480 RGB panel
# (ST7262), 16 MB flash, 8 MB PSRAM, a microSD socket, RS485, CAN and two
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

# Lisp's node pool. 37.3a raised it to 2048 (from the RP2350's 1024) for the
# showcase's cellular automaton, at about 20 KB -- five heap pages -- of SRAM.
set(CONFIG_LISP_NODE_POOL 65536)

# 38.5 (plan/phase38_psram.md, sign-off S2): the pools are BULK_BSS, so on
# this board they live in PSRAM and the 2048 above became 65 536 -- 1 MB of
# nodes, 256 KB of GC work stack. Measured ~1.7x slower than SRAM per node
# touched (plan/phase38_preliminaries.md §3), accepted for "basically
# unlimited" nodes. The node mark bitmap stays in SRAM: 8 KB at this size.
# The string pool at eight times the RP2350 default of 384 slots (both
# tiers: 2560 x 32 B and 512 x 128 B, 144 KB).
set(CONFIG_LISP_STRING_POOL 3072)

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

# The PSRAM, on GP0 as the QMI's second chip select (38.2,
# plan/phase38_psram.md). **8 MB**: an APS6404-class part (KGD 0x5D, EID
# 0x53), measured by aliasing in the phase 38 preliminaries -- this file said
# 2 MB until then, which is what the board's listing says too. Checked at
# every boot; a size smaller than this is refused. Setting these makes the
# persona *require* PSRAM: without it the board halts with the reason (S1).
set(CONFIG_PSRAM_CS_GPIO 0)
set(CONFIG_PSRAM_BYTES   8388608)
# The bulk page zone's capacity (38.4): the whole chip's worth of pages; the
# zone itself starts after BULK_BSS, so it uses fewer. A 256-byte bitmap.
set(CONFIG_PALLOC_BULK_PAGES 2048)
# /ram0's cap (38.6): its storage comes from the bulk zone, so the cap is a
# share of PSRAM, not of the heap. init.lisp mounts 2 MB at boot (sign-off
# S3); `(mount-ramdisk 4096)` can ask for up to this.
set(CONFIG_RAMDISK_MAX_KB 4096)

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

# The 7" RGB panel (ST7262), 36.3. From the schematic's pin table:
#   GP20 DE, GP21 VSYNC, GP22 HSYNC, GP23 PCLK -- consecutive, which the PIO
#   timing program relies on (drivers/lcd7_rp2350.c asserts it);
#   GP24..GP39 RGB565 data, in native order: B3-B7 on 24-28, G2-G7 on 29-34,
#   R3-R7 on 35-39, so an RGB565 word's bit n drives GP24+n;
#   GP41 LCD_RST, GP44 LCD_BL (PWM10 A), GP45 LCD_EN.
# LCD_EN enables the AP3032 backlight boost converter. LCD_BL feeds a voltage
# into that converter's feedback node through R41/R42, so a HIGHER level means
# a DIMMER backlight (the demo drives it inverted for this reason), and R41/R42
# are marked "10K/NC" / "68K/NC" -- on a board without them, dimming does
# nothing and LCD_EN is the only control.
set(CONFIG_LCD_DE_GPIO    20)
set(CONFIG_LCD_PCLK_GPIO  23)
set(CONFIG_LCD_DATA0_GPIO 24)
set(CONFIG_LCD_RST_GPIO   41)
set(CONFIG_LCD_BL_GPIO    44)
set(CONFIG_LCD_EN_GPIO    45)

# Not declared yet, each with a pin already reserved by the board:
#   UART1 GP8/9  -- hard-wired to the SP3485 RS485 transceiver (plan §7)
#   SPI0 GP2-5   -- the XL2515 CAN controller, INT GP1, RST GP46
#   GP40         -- battery ADC
