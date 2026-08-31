# Per-board facts for RP2350 (Pico 2), populated for the **network gateway**
# persona (N3, plan/phase18_networking_and_auth.md): a bare Pico 2 carrying an
# Ethernet module on SPI0 and an SD card on SPI1, and nothing else.
#
# **No Ethernet part is fitted right now.** The W5500 this persona was built
# around was cancelled (phase 18's STATUS section: bad parts, and TCP in
# silicon was never the bare-metal answer) and its driver and pin block were
# removed by phase 19's R0. What is left is a headless Pico 2 with an SD card,
# a console, a USB 9P link and a UART1 downlink -- all of which still work.
# Phase 19's R4 fits an ENC28J60 on the same SPI0 pins reserved below.
#
# Same RP2350 silicon, arch and linker script as cmake/board-rp2350.cmake --
# a third board file, not a third LUGALOS_TARGET, selected via
# -DLUGALOS_BOARD_FILE=cmake/board-rp2350-gateway.cmake (the rp2350-gateway
# preset). Built with ST7735, TM1638, CHESS, PICO_CLOCK_GREEN and DCF77 all
# OFF: there is no display, no keypad, no matrix and no radio on this board.
#
# What it is for: terminating 9P over TCP from the LAN, and re-exporting the
# namespace of a board reached over UART. See the plan's §3 and §5.

set(CONFIG_PALLOC_MAX_PAGES 128)

# Same 4 pages (16 KB) as the other two RP2350 personas, and for the same
# reason (§1.1, plan/phase15_memory_reclamation.md): the arena comes out of
# the heap on first use and the buddy tree is permanent .bss, both on the same
# 512 KB budget. Nothing on this persona is a heavy balloc caller.
set(CONFIG_BALLOC_ARENA_PAGES 4)

# UART0: the console, GP0/GP1, unchanged from every other RP2350 persona.
set(CONFIG_UART0_BASE     0x40070000)
set(CONFIG_UART0_TX_GPIO  0)
set(CONFIG_UART0_RX_GPIO  1)

# Heartbeat LED.
#
# **NOT GP16.** cmake/board-rp2350.cmake blinks the heartbeat on GP16 as
# LED_EXT, and on this board GP16 is the Ethernet module's MISO line -- an
# output driven once a second into the module's data-out pin, which is both a
# bus fight and an excellent way to spend an afternoon wondering why SPI reads
# garbage (user, 2026-08-24, before a single byte had been flashed). The part
# that made this true is gone; the pin reservation below is not, so neither is
# this warning.
#
# Pointed at GP25 instead, the Pico 2's own onboard LED: it is free on a bare
# board, it is visible, and "is this headless box alive" is a real question on
# a persona whose only other outputs are a network jack and a serial port.
# CONFIG_LED_EXT_GPIO is the pin the heartbeat drives (drivers/uart_rp2350.c
# uses it unconditionally); CONFIG_LED_ONBOARD_GPIO is deliberately not set,
# since setting both to 25 would just OR the same bit into the mask twice.
set(CONFIG_LED_EXT_GPIO   25)

# SPI1: the SD card, on the chess persona's own already-proven GP10-13.
# Phase 18 §3 argues why a gateway has a card at all: it turns a router into a
# file server, it is where the auth keys and the network config live, and it
# gives the hardware tests something real to read and write.
set(CONFIG_SPI1_BASE      0x40088000)
set(CONFIG_SPI1_SCK_GPIO  10)
set(CONFIG_SPI1_MOSI_GPIO 11)
set(CONFIG_SPI1_MISO_GPIO 12)
set(CONFIG_SPI1_CS_GPIO   13)

# SPI0: **the ENC28J60 (R4, plan/phase19_ip_stack_and_ethernet.md).**
#
# GP16-19 is the standard SPI0 grouping on RP2350 (RX=16, CSn=17, SCK=18,
# TX=19 are all valid function-1 assignments for that controller); RSTn and
# INTn are plain SIO GPIOs. The map is disjoint from SPI1 (SD, GP10-13) and
# UART1 (downlink, GP8/9).
set(CONFIG_SPI0_BASE      0x40080000)
set(CONFIG_ETH_SCK_GPIO   18)
set(CONFIG_ETH_MOSI_GPIO  19)
set(CONFIG_ETH_MISO_GPIO  16)
set(CONFIG_ETH_CS_GPIO    17)
set(CONFIG_ETH_RST_GPIO   20)
set(CONFIG_ETH_INT_GPIO   21)
#
# **The part in hand (2026-08-31): a HanRun V823 HR911105A ENC28J60 module**
# -- the common ten-pin SPI breakout, magnetics-integrated RJ45. Its header is
# two rows of five, pin order as printed on the board:
#
#   row A:  CLOUT   WOL   SI    CS    VCC
#   row B:  INT     SO    SCK   RESET GND
#
# Mapped onto the pins above:
#
#   module SI    (MOSI, into the chip)  -> GP19 (SPI0 TX/MOSI)
#   module SO    (MISO, out of the chip)-> GP16 (SPI0 RX/MISO)
#   module SCK                          -> GP18 (SPI0 SCK)
#   module CS                           -> GP17 (SPI0 CSn)
#   module RESET (active low)           -> GP20 (RSTn)
#   module INT   (active low, open-drain, needs the RP2350's internal
#                 pull-up enabled -- the module has none on board)
#                                        -> GP21 (INTn)
#   module VCC                          -> 3V3 -- **not 5V**. This module has
#                 no onboard regulator; the ENC28J60 die is 3.3V-only and 5V
#                 on VCC kills it. Confirm with a meter before first power-on,
#                 not after.
#   module GND                          -> GND, common with the RP2350
#   module WOL   (wake-on-LAN output)   -> not connected. No WOL support is
#                 planned; leaving it floating is correct.
#   module CLOUT (CLKOUT, a buffered
#                 divider of the 25 MHz crystal)
#                                        -> not connected. Nothing downstream
#                 needs a clock source from this chip.
#
# Decoupling before power-on (phase 19 §5's lesson, paid for once already):
# 220 uF bulk plus 100 nF ceramic across the module's own VCC/GND pins, short
# SPI leads, and a ground return run alongside them rather than a shared
# return elsewhere on the board.

# UART1: the downlink to a chess or clock board (N5, plan §4), SLIP-framed 9P
# over three wires.
#
# GP8/GP9 rather than the more obvious GP4/GP5, which are also valid UART1
# pins: GP4/GP5 are I2C0's, and drivers/i2c_rtc.c is built for *every* RP2350
# persona -- it probes those pins at boot whether or not a clock chip is
# fitted. Two drivers configuring the same pads for different functions is a
# conflict that only shows up as one of them mysteriously not working.
set(CONFIG_UART1_BASE     0x40078000)
set(CONFIG_UART1_TX_GPIO  8)
set(CONFIG_UART1_RX_GPIO  9)

# DS1307/DS3231 RTC pins. No RTC is fitted on this board; these are here
# because drivers/i2c_rtc.c is unconditional on RP2350 and needs somewhere to
# probe. It finds nothing, says so once at boot, and the kernel clock runs
# from its own monotonic counter -- which for a gateway is the honest state of
# affairs until something on the network tells it the time.
set(CONFIG_I2C_RTC_BASE     0x40090000) # I2C0
set(CONFIG_I2C_RTC_SDA_GPIO 4)
set(CONFIG_I2C_RTC_SCL_GPIO 5)
