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

# --- Ethernet: the EMAC and the IP101GRI on RMII ------------------------
#
# Z0, plan/phase28_esp32p4_ethernet.md. Every number below is read from the
# TRM, the NANO schematic or ESP-IDF's generated headers under ~/gith/esp/ --
# the §3.2 rule, which on this chip has already caught two clock-gate bits
# that "obvious" reasoning got wrong.

# 0x50098000 = DR_REG_HPPERIPH0_BASE (0x50000000) + 0x98000. IDF's
# components/soc/esp32p4/register/hw_ver1/soc/reg_base.h, DR_REG_EMAC_BASE.
# The block is a Synopsys DesignWare core in three layers (EMAC_CORE,
# EMAC_MTL, EMAC_DMA); TRM chapter 55.
set(CONFIG_EMAC_BASE       0x50098000)

# Interrupt-matrix source number for ETH_MAC_INTR, TRM Table 13.4-1
# (COREx_ETH_MAC_INT_MAP_REG). The EMAC raises four sources -- 89 GMII_PHY,
# 90 LPI, 91 PMT, 92 ETH_MAC -- and only the last is the one carrying frame
# and DMA events, so it is the only one routed. The CLIC line it is routed
# *to* is not here: that is a kernel-side allocation and lives in
# arch/riscv/include/arch/esp32p4_intr.h, where a collision is a visible edit.
set(CONFIG_EMAC_INTR_SRC   92)

# --- The pins.
#
# READ THIS BEFORE CHANGING ANY NUMBER BELOW.
#
# The RMII data signals are IO_MUX pads, not GPIO-matrix routes, and each
# signal has its own short list of legal pads
# (IDF components/esp_hal_emac/esp32p4/emac_periph.c). Those lists look like
# three tidy groups -- a low one (28-36), a middle one (40-48) and a high one
# (49-54) -- and this board deliberately uses a MIXED selection: receive and
# TXD from the low group, TX_EN and the reference clock from the high group.
#
# Worse, three pads this board uses for something else are themselves
# alternates in those same lists: GPIO31 is MDC here but is also
# EMAC_PHY_RXER_PAD, GPIO51 is the PHY reset but is also EMAC_PHY_RXDV_PAD,
# and GPIO52 is MDIO but is also EMAC_PHY_RXD0_PAD.
#
# So "tidying" this table into one contiguous group produces a driver that
# configures without complaint and receives nothing. The numbers are a fact
# about the board, not a choice.
#
# Provenance: IDF's ETH_ESP32_EMAC_DEFAULT_CONFIG() for CONFIG_IDF_TARGET_ESP32P4
# (components/esp_eth/include/esp_eth_mac_esp.h), corroborated for *this*
# board by Waveshare's own
# ~/gith/esp/ESP32-P4-Platform/examples/esp-idf/15_eth2ap/sdkconfig.defaults,
# whose MDC/MDIO/reset overrides are the three numbers phase 27 §1 recorded.

# Station-management (MDIO/clause-22), routed through the GPIO matrix.
set(CONFIG_EMAC_MDC_GPIO       31)
set(CONFIG_EMAC_MDIO_GPIO      52)
# The PHY's reset, a plain GPIO output. Active low.
set(CONFIG_EMAC_PHY_RST_GPIO   51)

# The RMII reference clock is an INPUT on this board. The IP101GRI has its
# own 25 MHz crystal (schematic nets X1/X2) and drives 50 MHz out of M_CLKO
# into the P4 -- which is why TRM §55.5.1's "reference clock sourced from the
# external crystal" branch applies, and why the P4 must NOT enable
# PAD_EMAC_REF_CLK or PLL_F50M onto a pad the PHY is already driving.
# IO_MUX FUNC_GPIO50_EMAC_RMII_CLK_PAD.
set(CONFIG_EMAC_RMII_CLK_GPIO  50)

# RMII data. IO_MUX pads, see the warning above.
set(CONFIG_EMAC_TX_EN_GPIO     49)
set(CONFIG_EMAC_TXD0_GPIO      34)
set(CONFIG_EMAC_TXD1_GPIO      35)
set(CONFIG_EMAC_CRS_DV_GPIO    28)
set(CONFIG_EMAC_RXD0_GPIO      29)
set(CONFIG_EMAC_RXD1_GPIO      30)

# The PHY's address on the MDIO bus -- NOT YET MEASURED.
#
# 0xFF is out of clause-22's 5-bit address range (0..31) and means "scan":
# the driver reads registers 2 and 3 at every address and reports which one
# answers. The schematic has PHY_AD0/PHY_AD3 strap nets and IDF's examples
# default to 1, but a strap resistor is not a number this project is willing
# to infer -- phase 28's Z1 replaces this line with the address the hardware
# actually reports, and with the identifier it reported alongside it.
set(CONFIG_EMAC_PHY_ADDR       0xFF)
