# Per-board facts for the ESP32-C6-MINI-1 module beside the P4 on the Waveshare
# ESP32-P4-NANO (and the same module on the ESP32-P4-WIFI6-Touch-LCD-7B). 49.5,
# .
#
# The same chip as the C6-Zero (ESP32-C6FH4 here, rev v0.2, read with esptool
# 2026-10-12), so every chip fact below is cmake/board-esp32c6-zero.cmake's;
# what differs is the module and what it is wired to:
#
# * 4 MB of in-package flash, not 8: its own map,
#   cmake/flash_layout_esp32c6_mini.cmake (the preset selects it).
# * The console is the same USB-Serial/JTAG, reached on the NANO through
#   header P2 pins 21 (D-) and 23 (D+) -- tools/c6run.py and c6flash.py work
#   unchanged (§5.3 of the plan).
# * IO18..IO23 are the SDIO slave's pins, wired to the P4: LLP's channel
#   (drivers/sdio_slave_esp32c6.c).
# * No LED: GPIO8 is only a strapping pin with a pull-up here, so `led` drives
#   nothing. No sensors: GPIO0/1, the C6-Zero's I2C pins, are not connected.

set(CONFIG_PALLOC_MAX_PAGES 124)
set(CONFIG_BALLOC_ARENA_PAGES 4)

# UART0: a second console beside the USB one (drivers/usbjtag_esp32c6.c). On
# the NANO header P2 pins 20/22 (C6_U0RXD/U0TXD); on the LCD-7B header H4
# "C6-UART" (SH1.0: 1 IO9, 2 GND, 3 U0RXD, 4 U0TXD), its only console, since
# the 7B does not route the C6's USB pair anywhere.
set(CONFIG_UART0_BASE     0x60000000)
set(CONFIG_UART0_TX_GPIO  16)
set(CONFIG_UART0_RX_GPIO  17)
set(CONFIG_UART0_SCLK_HZ  40000000)
set(CONFIG_UART0_BAUD     115200)

set(CONFIG_XTAL_HZ        40000000)
set(CONFIG_USBJTAG_BASE   0x6000F000)
set(CONFIG_SYSTIMER_BASE  0x6000A000)
set(CONFIG_CPU_FREQ_MHZ   160)

# The SDIO slave toward the P4 (IO_MUX function 0 of GPIO18..23, fixed): LLP's
# channel on this module, instead of the USB port the C6-Zero uses.
set(CONFIG_SDIO_SLAVE 1)

# Lisp-light: a C6 runs configuration and small sensor scripts, not the CAS.
# 1024 nodes and 384 string slots cost 46 KB of static RAM; a sensor node used
# 11 nodes after boot. Half the nodes and half the strings return ~23 KB to a
# heap that the radio, TCP and the identity store's 8 KB scratch compete for.
set(CONFIG_LISP_NODE_POOL   512)
set(CONFIG_LISP_STRING_POOL 192)
