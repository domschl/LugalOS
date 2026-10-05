# Per-board facts for the Waveshare ESP32-C6-Zero (and the C6 itself),
# consumed by cmake/gen_config.cmake to produce lugalos_config.h. 45.4,
# plan/phase45_esp32c6.md.
#
# Every address was read from IDF's generated register headers
# (components/soc/esp32c6/register/soc/*.h, 2026-10-05) and, where the kernel
# depends on it, confirmed on this silicon in 45.1/45.3a.

# SRAM is 512 KB at 0x40800000; the top 16 KB (0x4087c000..) is the ROM's own
# working data and must never be handed out (plan §4.5: the ROM's radio code
# keeps its state there, e.g. g_osi_funcs_p at 0x4087ff6c). That leaves
# 496 KB = 124 pages for image, stack and heap together; the kernel image is
# RAM-resident until 45.4.3 moves it to flash.
set(CONFIG_PALLOC_MAX_PAGES 124)

# The buddy arena (kernel/balloc.h), in pages: the RP2350/P4 figure.
set(CONFIG_BALLOC_ARENA_PAGES 4)

# UART0, the chip's own: TRM / reg_base.h DR_REG_UART0_BASE. It is *not* the
# console on the C6-Zero -- the USB-C port is wired to the USB-Serial/JTAG
# peripheral below -- but it is a mandatory key and a real peripheral, and the
# board's header pads carry it.
set(CONFIG_UART0_BASE     0x60000000)
set(CONFIG_UART0_TX_GPIO  16)
set(CONFIG_UART0_RX_GPIO  17)
set(CONFIG_UART0_SCLK_HZ  40000000)
set(CONFIG_UART0_BAUD     115200)

# The crystal. 40 MHz, as on every C6 board here (esptool reports it); the
# system timer's count rate is XTAL/2.5 = 16 MHz, measured in 45.1.
set(CONFIG_XTAL_HZ        40000000)

# The console: the USB-Serial/JTAG peripheral (reg_base.h
# DR_REG_USB_SERIAL_JTAG_BASE). One cable for console, loading and reset.
set(CONFIG_USBJTAG_BASE   0x6000F000)

# The system timer (reg_base.h DR_REG_SYSTIMER_BASE), the kernel's clock.
set(CONFIG_SYSTIMER_BASE  0x6000A000)

# The CPU clock as the ROM leaves it, measured in 45.1 (160 MHz, no PLL work
# needed).
set(CONFIG_CPU_FREQ_MHZ   160)
