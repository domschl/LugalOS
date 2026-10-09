# Per-board facts for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B, consumed by
# cmake/gen_config.cmake. 47.0, plan/phase47_esp32p4_lcd7b_ribbon.md.
#
# Same LUGALOS_TARGET, driver sources and linker script as the NANO
# (cmake/board-esp32p4-nano.cmake). Read that file for why each chip-level
# number is what it is; this one says only where the boards differ, and where
# a number was read.
#
# Sources, named on every line below:
#   [sch]  ~/Source/gith/esp/datasheet/ESP32-P4-WIFI6-Touch-LCD-7B.pdf, page 1,
#          read at 450 dpi block by block, 2026-10-09
#   [ws]   Waveshare's examples, ~/Source/gith/esp/ESP32-P4-WIFI6-Touch-LCD-7B/
#          examples/{arduino,esp-idf}
#   [meas] measured on this unit

# **Silicon revision v3.2** [meas: esptool flash-id, ROM banner
# "ESP-ROM:esp32p4-eco7-20260109"]. Not the NANO's v1.3, and the difference is
# not cosmetic: on v3.x the L2 cache sits at the *bottom* of L2MEM and the
# ROM's data at the top, the reverse of v1.3; the ROM is a different build;
# the CPU clock ladder is 100/200/400. 47.1 audits every place the kernel
# depends on this. The NANO-built kernel nevertheless **boots here at 40 MHz**
# [meas, 2026-10-09]: its RAM half (0x4ff40000..0x4ff9e000) happens to miss
# both the v3.x cache at the bottom and the ROM's data at the top.
set(CONFIG_ESP32P4_REV 302)

set(CONFIG_PALLOC_MAX_PAGES 128)
set(CONFIG_BALLOC_ARENA_PAGES 4)

# --- UART0: the console --------------------------------------------------
#
# [sch "USB to UART"] U6 CH343P: its TXD drives GPIO38 and its RXD hears
# GPIO37 -- UART0's IO_MUX pads, as on the NANO. The PH2.0 "UART" header H8
# is the same two pins, so it is UART0 too, not a second UART.
#
# Reset: U7 (EMH4T2R) -- RTS to ESP_EN, DTR to GPIO35, C33 1 uF on ESP_EN. One
# cable carries console and reset [meas: tools/p4run.py --reset-test, both
# sequences work]. Every *open* of the port resets the chip unless the tty
# was left at B0 [meas], which is what p4run.py's park() handles.
set(CONFIG_UART0_BASE     0x500CA000)
set(CONFIG_UART0_TX_GPIO  37)
set(CONFIG_UART0_RX_GPIO  38)
set(CONFIG_UART0_SCLK_HZ  40000000)
set(CONFIG_UART0_BAUD     115200)
set(CONFIG_XTAL_HZ        40000000)   # [meas: esptool]

# The CPU clock. **40**, the crystal, until 47.2 brings up the PLL on v3.x:
# clk_esp32p4.c's ladder (90/180/360) is v1.3's, and v3.x has 100/200/400.
set(CONFIG_CPU_FREQ_MHZ   40)

# The L2 cache size. The NANO's measured 128 KB; whether the same figure is
# right on v3.x (where the cache is at the bottom of L2MEM) is 47.1's.
set(CONFIG_L2_CACHE_KB 128)

# --- microSD: SDMMC slot 0 ------------------------------------------------
#
# [sch "MicroSD Card", ws 08_SD_Card] The same six IO_MUX pads as the NANO --
# they are the only ones slot 0 has.
set(CONFIG_SDMMC_BASE      0x50083000)
set(CONFIG_SDMMC_CLK_GPIO  43)
set(CONFIG_SDMMC_CMD_GPIO  44)
set(CONFIG_SDMMC_D0_GPIO   39)
set(CONFIG_SDMMC_D1_GPIO   40)
set(CONFIG_SDMMC_D2_GPIO   41)
set(CONFIG_SDMMC_D3_GPIO   42)
# Card power [sch]: Q1, an AO3401 P-channel MOSFET from ESP_3V3 to the card's
# VDD, gate on GPIO45, pulled to ground by R29 10K (R23 "NC/10K" not
# fitted). So the card is ON by default and GPIO45 high switches it OFF --
# the NANO's polarity. One difference: here the switch is fed from ESP_3V3,
# while the data pull-ups R5-R10 (10K) go to ESP_LDO_VO4, so LDO channel 4
# must be up for the GPIO39-45 bank as on the NANO. No card-detect line
# reaches a GPIO (the socket's CD pin only has R10 to VO4).
set(CONFIG_SDMMC_PWR_GPIO  45)
set(CONFIG_SDMMC_BUS_WIDTH 4)
set(CONFIG_SDMMC_FREQ_KHZ  20000)

# --- Not declared yet: facts read, each waiting for the milestone that drives it.
#
# I2C [sch "7inch Display", ws]: SDA GPIO7, SCL GPIO8 (ESP_I2C_SDA/SCL), shared
#   by the GT911 (0x5D or 0x14), ES8311 (0x18), ES7210 (0x40) and the PH2.0 I2C
#   header. 47.2.
#
# Panel [sch, ws] (47.4): EK79007, 1024x600, MIPI-DSI 2 lanes at 1 Gbps on the
#   dedicated DSI pads (DSI_CLK/D0/D1 to connector P2). DSI PHY supply: on-chip
#   LDO channel 3 at 2500 mV [ws 07_color_panel].
#   GPIO33 -> RESET_LCD (R42 0R), pulled DOWN by R45 10K (R41 not fitted): the
#     panel is held in reset until we drive GPIO33 high.
#   GPIO32 -> BL_CTRL (R48 0R) -> R87 10K / R85 68K into the feedback node of
#     U12 (AP3032 backlight boost, EN tied on through R76). A higher level means
#     a DIMMER backlight [ws: "active low", LEDC 5 kHz 10-bit inverted].
#   Panel 1.8 V (U10 RT9193-18) and AVDD/VGH/VGL (U9 AP3012) have no GPIO
#     enable: they are on whenever the board is.
#
# Touch [sch J3] (47.12): GT911 on the I2C bus above. GPIO23 -> RESET_TP
#   (R54 0R). INT_TP reaches only test point TP1 -- no GPIO -- so touch is
#   polled, as Waveshare's own examples do.
#
# USB-A [sch "POWER", J1] (47.9): the P4's USB 2.0 OTG HS on its dedicated PHY
#   pads (USBD_N/USBD_P). VBUS_OUT comes from U2, a DIO7003 current-limited
#   load switch fed from Core_5V, whose EN is pulled up to its input by R2 10K
#   (R11 not fitted): the port's 5 V is ALWAYS ON, with no GPIO.
#
# USB1.1 Type-C [sch H2] (47.13): the P4's USB 1.1 FS pads (USB1P1_N/P), i.e.
#   USB-Serial/JTAG or OTG FS.
#
# ESP32-C6-MINI-1 [sch "ESP32-C6"] (phase 47's step 2, not this phase):
#   SDIO on P4 GPIO14-19 (C6 IO18-23, 51K pull-ups to ESP_3V3); C6_CHIP_PU
#   from P4 GPIO54 (R33 0R); C6_IO2 from P4 GPIO6 (R31 0R); the C6's own UART0
#   and IO9 (its BOOT strap) on header H4 "C6-UART".
#
# Audio [ws] (not this phase): I2S MCLK 13, BCLK 12, LRCK 10, DOUT 9, DIN 11;
#   speaker PA enable GPIO53. RS485 UART1 TX 27 / RX 26. CAN TWAI TX 22 / RX 21.
#
# No user LED [sch]: the red LED is Core_5V power, the green one the ETA6098
#   charger's status. So, as on the RP2350-LCD-7, no CONFIG_LED_*.
#
# No Ethernet: no CONFIG_EMAC_*, and drivers/emac_esp32p4.c builds its stubs.
