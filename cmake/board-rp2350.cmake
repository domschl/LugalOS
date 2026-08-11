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
