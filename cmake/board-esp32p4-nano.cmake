# Per-board facts for the Waveshare ESP32-P4-NANO, consumed by
# cmake/gen_config.cmake to produce lugalos_config.h (K0,
# plan/phase7_kernel_config.md). E2, plan/phase27_esp32p4_bringup.md.
#
# Flat list of numbers, reviewable as "is this number right" rather than
# "does this expand correctly" -- the schema lives in gen_config.cmake.
#
# Every address here was read out of the ESP32-P4 TRM and cross-checked
# against ESP-IDF's own generated headers, never inferred from another
# Espressif part (section 3.2 of the phase plan, and the 0x88888888 bug
# phase 24 paid for).

# 512 KB of heap-and-image, so at most 128 pages of 4 KB even before the
# kernel's own footprint comes out of it. See linker/esp32p4.ld for why the
# figure is 512 and not the chip's 768.
set(CONFIG_PALLOC_MAX_PAGES 128)

# Buddy-allocator arena (kernel/balloc.h), in pages: 4 = 16 KB, the RP2350
# figure rather than QEMU's 16.
#
# The reason is the RP2350 board file's reason, unchanged: this constant is
# paid twice -- the arena itself comes out of the heap, and the buddy tree is
# permanent .bss whose size scales with it (2046 bytes at 4 pages, 8190 at
# 16) -- and on this board, as on that one, .bss and the heap are the same
# half-megabyte. QEMU's 16 is free against 128 MB; here it is not.
set(CONFIG_BALLOC_ARENA_PAGES 4)

# --- UART0: the console, and the only wire this board has in E2 ----------
#
# 0x500CA000 = DR_REG_HPPERIPH1_BASE (0x500C0000) + 0xA000. TRM Table 9.3-2
# ("UART0  0x500C_A000  0x500C_AFFF"), matching IDF's
# components/soc/esp32p4/register/hw_ver1/soc/reg_base.h.
set(CONFIG_UART0_BASE     0x500CA000)

# GPIO37/38 are UART0's IO_MUX pads -- function 0 on each, not a GPIO-matrix
# route (IDF's soc/uart_pins.h: U0TXD_GPIO_NUM 37 / U0RXD_GPIO_NUM 38, both
# with MUX_FUNC 0). They are the ROM console's own pins, which is why E1
# could print without configuring anything.
set(CONFIG_UART0_TX_GPIO  37)
set(CONFIG_UART0_RX_GPIO  38)

# The UART's source clock, and the baud rate driven from it.
#
# XTAL_CLK, deliberately -- not the 80 MHz PLL, and not whatever the ROM
# happened to leave selected. The crystal on this board is 40 MHz (esptool
# reports it, and E1 confirmed it on this physical unit), and it does not
# move when the CPU clock does. Selecting it means the console's baud rate
# is independent of the PLL and of the CPU frequency, so E2 does not have to
# bring up the clock tree to get a shell -- and nothing later that *does*
# touch the clock tree can silently take the console away.
#
# 115200 is the ROM's own rate, so the same host command that watches the
# boot ROM's banner watches ours.
set(CONFIG_UART0_SCLK_HZ  40000000)
set(CONFIG_UART0_BAUD     115200)

# The crystal itself. Separate from CONFIG_UART0_SCLK_HZ even though the two
# are the same number today, because they are different facts: one is a
# component on this board, the other is a clock-source choice this kernel
# makes and could change. kernel/time.c derives the system timer's count
# rate from this one (TRM 16.4: CNT_CLK is XTAL_CLK scaled by 2.5).
set(CONFIG_XTAL_HZ        40000000)
