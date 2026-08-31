# Per-board facts for RP2350W ("Pico 2 W"), populated for the **wireless
# netif** persona (R5, plan/phase19_ip_stack_and_ethernet.md): a bare Pico 2 W
# carrying an SD card on SPI1 and nothing else -- the wireless counterpart to
# cmake/board-rp2350-gateway.cmake's wired one.
#
# A *different* physical Pico 2 W than the one under the Pico-Clock-Green
# baseboard (cmake/board-rp2350-clock.cmake, phase 11): that board's CYW43439
# pins are already fully documented (phase17 §1's pin table, confirmed against
# the vendor schematic) and reused here rather than rediscovered --
# GP23/24/25/29 are internal to the Pico 2 W module on *every* board carrying
# one, not something this specific board's wiring could vary.
#
# Same RP2350 silicon, arch and linker script as cmake/board-rp2350.cmake --
# a third/fourth board file, not a third/fourth LUGALOS_TARGET, selected via
# -DLUGALOS_BOARD_FILE=cmake/board-rp2350-wifi.cmake (the rp2350-wifi
# preset). Built with ST7735, TM1638, CHESS, PICO_CLOCK_GREEN and DCF77 all
# OFF, same as the gateway persona: no display, no keypad, no matrix, no
# longwave receiver on this board.
#
# What it is for: a second, independent netif_t under the same IP stack
# net/tcp.c and net/ip.c already provide, so R5's job is a driver
# (drivers/cyw43_rp2350.c, not yet written) rather than a second stack.

set(CONFIG_PALLOC_MAX_PAGES 128)

# Same 4 pages (16 KB) as every other RP2350 persona (§1.1,
# plan/phase15_memory_reclamation.md) -- this board has nothing unusually
# heavy on the buddy allocator either.
set(CONFIG_BALLOC_ARENA_PAGES 4)

# UART0: the console, GP0/GP1, unchanged from every other RP2350 persona.
set(CONFIG_UART0_BASE     0x40070000)
set(CONFIG_UART0_TX_GPIO  0)
set(CONFIG_UART0_RX_GPIO  1)

# Heartbeat LED. Deliberately NOT CONFIG_LED_ONBOARD_GPIO -- phase17 C1's
# finding for the clock persona applies identically here: on a Pico 2 **W**
# the user LED is on the wireless module (WL_GPIO0), reachable only through
# the CYW43 driver's own ioctl once it exists, not a plain GPIO. GP25 (the
# "onboard LED" pin on a bare RP2350) is the CYW43439's own chip select on
# this board and must not be driven by anything else -- see the pin table
# below. GP9 taken from the clock persona's own precedent (free on every
# RP2350W board this project has met so far).
set(CONFIG_LED_EXT_GPIO   9)

# SPI1: the SD card, on the chess/gateway personas' own already-proven
# GP10-13. Nothing on the Pico 2 W's wireless module uses these pins.
set(CONFIG_SPI1_BASE      0x40088000)
set(CONFIG_SPI1_SCK_GPIO  10)
set(CONFIG_SPI1_MOSI_GPIO 11)
set(CONFIG_SPI1_MISO_GPIO 12)
set(CONFIG_SPI1_CS_GPIO   13)

# CYW43439 wireless, R5. GP23/24/25/29, confirmed against phase17 §1's pin
# table (itself checked against the Pico 2 W schematic) and the Pico SDK's
# own pico_w.h/cyw43_pins.h:
#
#   GP23  WL_ON    wireless module power enable (WL_REG_ON), active high
#   GP24  WL_D     gSPI data, bidirectional, one wire
#   GP25  WL_CS    gSPI chip select (NOT the onboard LED on a W -- see above)
#   GP29  WL_CLK   gSPI clock
#
# None of these are on the 40-pin header; they are internal to the Pico 2 W
# module on every board that carries one. drivers/cyw43_rp2350.c bit-bangs
# this link through RP2350's PIO0 block rather than the hardware SPI
# peripheral -- gSPI's turnaround timing (one bidirectional data line,
# direction flips mid-transfer) does not fit a standard SPI controller.
set(CONFIG_WL_ON_GPIO   23)
set(CONFIG_WL_DATA_GPIO 24)
set(CONFIG_WL_CS_GPIO   25)
set(CONFIG_WL_CLK_GPIO  29)

# DS1307/DS3231 RTC pins. No RTC is fitted on this board; these are here
# because drivers/i2c_rtc.c is unconditional on RP2350 and needs somewhere
# to probe -- same reasoning as cmake/board-rp2350-gateway.cmake's own
# identical block. I2C0 on GP4/GP5, free on this persona.
set(CONFIG_I2C_RTC_BASE     0x40090000) # I2C0
set(CONFIG_I2C_RTC_SDA_GPIO 4)
set(CONFIG_I2C_RTC_SCL_GPIO 5)
