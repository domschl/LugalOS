# Per-board facts for RP2350 (Pico 2), populated for the **network gateway**
# persona (N3, plan/phase18_networking_and_auth.md): a bare Pico 2 carrying a
# W5500 Ethernet module on SPI0 and an SD card on SPI1, and nothing else.
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
# LED_EXT, and on this board GP16 is the W5500's MISO line -- an output driven
# once a second into the module's data-out pin, which is both a bus fight and
# an excellent way to spend an afternoon wondering why SPI reads garbage
# (user, 2026-08-24, before a single byte had been flashed).
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

# SPI0: the W5500 (a USR-ES1 module -- WIZnet W5500 behind a HanRun HR961160C
# magjack). Pin numbers confirmed against the module's own silkscreen; see the
# plan's §2 for the full header map and the 200 mA supply note.
#
# GP16-19 is the standard SPI0 grouping on RP2350 (RX=16, CSn=17, SCK=18,
# TX=19 are all valid function-1 assignments for that controller). RSTn and
# INTn are plain SIO GPIOs.
#
# Declared here even though the driver arrives in N4: a pin map is a board
# fact, and writing it down now is what stopped the GP16 heartbeat collision
# above from reaching hardware.
set(CONFIG_SPI0_BASE       0x40080000)
set(CONFIG_W5500_SCK_GPIO  18)
set(CONFIG_W5500_MOSI_GPIO 19)   # module 1-3 MOSI
set(CONFIG_W5500_MISO_GPIO 16)   # module 2-6 MISO
set(CONFIG_W5500_CS_GPIO   17)   # module 1-5 SCSn
set(CONFIG_W5500_RST_GPIO  20)   # module 2-5 RSTn, active low
set(CONFIG_W5500_INT_GPIO  21)   # module 1-6 INTn, active low, polled in N4

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
