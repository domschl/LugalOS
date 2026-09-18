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

# The CPU clock, 34.4 (plan/phase34_esp32p4_pll_bringup.md).
#
# One of **40, 90, 180, 360**, and nothing else: 40 keeps HP_ROOT_CLK on the
# crystal exactly as every milestone before phase 34 ran, and the other three
# are the only CPLL-derived steps ESP-IDF enumerates for silicon below
# revision v3.0 -- which this board, at v1.3, is. The 100/200/400 ladder
# belongs to rev >= 3.0 parts. arch/riscv/common/clk_esp32p4.c refuses
# anything outside the table rather than computing dividers, because a
# combination off it can be silently corrected by hardware without the
# registers reflecting it.
#
# It is a config precisely so that a bad step is one constant away from
# bisecting, and so that 40 stays selectable as a control for every
# measurement this phase makes.
set(CONFIG_CPU_FREQ_MHZ   360)

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
#
# Measured and recorded, but NOT currently routed: the driver is polled, not
# interrupt-driven, because net/netif.h's poll() may not block and netsrv is
# already the pump (phase 28 Z6 argues this at length). The number stays here
# because it is a board fact that took reading Table 13.4-1 to establish, and
# an interrupt-driven receive path would need it on day one.
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

# The PHY's address on the MDIO bus -- MEASURED, Z1, 2026-09-12.
#
# `emac scan` on this physical board read clause-22 registers 2 and 3 at every
# address 0..31. Exactly one answered:
#
#     1: PHYIDR1=0x0243 PHYIDR2=0x0c54  (OUI 00-90-c3, model 5 rev 4)
#
# OUI 00-90-c3 is IC Plus Corp and model 5 is the IP101G family, which is what
# the schematic says is fitted -- so the part is confirmed by its own silicon
# and not only by the silkscreen. The strap nets PHY_AD0/PHY_AD3 evidently
# resolve to 1, which happens to match IDF's example default; that agreement
# is a coincidence worth nothing, and the measurement is what this line
# records.
#
# `emac scan` re-checks this number on every run and says so if the hardware
# disagrees, which makes it a claim rather than a comment.
set(CONFIG_EMAC_PHY_ADDR       1)

# What that PHY must identify as, so a swapped or dead part is caught rather
# than silently tolerated. Checked by emac_phy_scan(); see the Z1 entry in
# plan/phase28_esp32p4_ethernet.md.
set(CONFIG_EMAC_PHY_ID1        0x0243)
set(CONFIG_EMAC_PHY_ID2        0x0C54)

# --- The L2 cache, which is carved out of L2MEM and is therefore not RAM ---
#
# U5, plan/phase32_esp32p4_execute_in_place.md. This number is paid twice and
# in two languages: arch/riscv/common/trap.c asks the ROM for a cache of this
# size, and linker/esp32p4.ld must stop the RAM region where that cache's
# storage begins. Getting them out of step does not fail at link or at boot --
# it gives a heap whose top bytes accept stores and lose them, which is the
# whole of phase 27's E7. So cmake passes this one definition to both, and the
# linker asserts on it.
#
# 128 KB rather than the ROM's 256 KB default, and **measured rather than
# assumed** once .text began executing in place from flash:
#
#     XIP, L2 = 128 KB    boot 282 ms (x3)    heap 372 KB
#     XIP, L2 = 256 KB    boot 282 ms (x3)    heap 244 KB
#
# Doubling it changed nothing measurable and cost 128 KB of heap.
#
# That first result only covered boot, which might not be L2-bound. Enabling
# the chess engine on this board (also phase 32) provided the CPU-bound
# instrument the measurement had been missing, and it agrees -- `(perft 3)`
# node rates across six positions, 128 KB against 256 KB:
#
#     27825 / 27817     32276 / 32299     25408 / 25408
#     32149 / 32172     17723 / 17734     17179 / 17179
#
# Within 0.1% on every position. So the answer holds for a workload that
# sweeps far more code than boot does, and 128 KB stays.
#
# Legal values are 128, 256 and 512 (cache_size_t 9, 10, 11 in
# esp32p4/rom/cache.h). Changing this is the only edit needed: the RAM region
# follows from it.
set(CONFIG_L2_CACHE_KB 128)

# --- The microSD slot: SDMMC slot 0, and the rail that powers it ---------
#
# 35.1, plan/phase35_esp32p4_sdmmc.md. Every number here is either the chip's
# (from the TRM and IDF's generated headers) or this board's (from
# ~/gith/esp/datasheet/ESP32-P4-NANO-schematic.pdf, sheet "MicroSD Card"),
# and each is labelled below with which.

# 0x50083000 = DR_REG_HPPERIPH0_BASE (0x50000000) + 0x83000. TRM Table 9.3-2
# ("SD/MMC Host Controller  0x5008_3000  0x5008_3FFF"), matching IDF's
# reg_base.h DR_REG_SDMMC_BASE.
set(CONFIG_SDMMC_BASE      0x50083000)

# --- The pins.
#
# **These are not a choice and must not be "tidied".** SDMMC slot 0 on the
# ESP32-P4 has no GPIO-matrix route at all -- its six signals are IO_MUX pads
# at function 0 and nowhere else (IDF's sdmmc_pins.h, and
# SDMMC_LL_SLOT_SUPPORT_GPIO_MATRIX(0) is 0). The six numbers below are that
# fixed pad set, and the Waveshare wiring matches it exactly, which is the one
# piece of luck this peripheral offers: there is no routing to get wrong.
#
# Corroborated for *this* board by Waveshare's own
# ~/gith/esp/ESP32-P4-Platform/examples/arduino/examples/SD_Card/SD_Card.ino
# and examples/esp-idf/09_sdmmc, which name the same six.
set(CONFIG_SDMMC_CLK_GPIO  43)
set(CONFIG_SDMMC_CMD_GPIO  44)
set(CONFIG_SDMMC_D0_GPIO   39)
set(CONFIG_SDMMC_D1_GPIO   40)
set(CONFIG_SDMMC_D2_GPIO   41)
set(CONFIG_SDMMC_D3_GPIO   42)

# The card's power switch -- a BOARD fact, not a chip one, and the reason a
# correctly configured controller can still see nothing.
#
# Schematic: the chip's LDO channel 4 (net ESP_LDO_VO4) feeds both VDDPST_5 --
# the IO domain GPIO39-45 live in -- and, through R14 (0R) and Q1 (an AO3401
# P-channel MOSFET), the card's own VDD. Q1's gate is GPIO45, pulled to ground
# by R27 (10K) with the pull-up R23 not fitted, so the switch is CLOSED by
# default and opens only if something drives GPIO45 high.
#
# GPIO45 is also SDMMC slot 0's D4 pad. That matters only for an 8-bit bus,
# which an SD card does not have; on this board the pad is the power switch
# and drivers/sdmmc_esp32p4.c keeps it a plain GPIO driving low.
set(CONFIG_SDMMC_PWR_GPIO  45)

# 4-bit, and the driver proves it rather than assuming it: ACMD6 answers with
# no error whether or not D1-D3 are actually connected, so the driver reads a
# block back at the final clock and falls back to 1-bit if it cannot. Set this
# to 1 to skip the attempt entirely.
set(CONFIG_SDMMC_BUS_WIDTH 4)

# The card clock after identification, in kHz.
#
# 20000, which is PLL_F160M / 8 exactly and is the fastest this driver may use
# without negotiating for it: default speed is capped at 25 MHz by the SD
# physical-layer spec, and going above it means CMD6 (switch to high speed),
# a re-timed sampling phase, and a card that is allowed to refuse. 20 MHz on
# four lines is 10 MB/s of bus, which is already well beyond what the CPU-side
# FIFO path will sustain -- so the next real speed-up is multi-block
# transfers, not a faster clock. See plan/phase35_esp32p4_sdmmc.md §5.
set(CONFIG_SDMMC_FREQ_KHZ  20000)
