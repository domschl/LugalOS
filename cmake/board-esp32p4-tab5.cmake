# Per-board facts for the M5Stack Tab5 (C145), consumed by
# cmake/gen_config.cmake. Phase 56, plan/phase56_m5stack_tab5.md.
#
# Sources:
#   [sch]  ~/Source/gith/esp/datasheet/Tab5_Sch.pdf (20 pages)
#   [m5]   ~/Source/gith/esp/M5Stack/{M5GFX,M5Unified}
#   [meas] measured on this physical Tab5 unit (MAC e8:f6:0a:e2:fc:97)

set(CONFIG_BOARD_TAB5 1)

# Silicon revision v1.3 [meas: esptool flash-id, ROM banner
# "ESP-ROM:esp32p4-eco2-20240710", Chip is ESP32-P4 (revision v1.3)].
# Selects linker/esp32p4_memory_rev1.ld and the v1.3 360 MHz CPU clock ladder.
set(CONFIG_ESP32P4_REV 103)

set(CONFIG_PALLOC_MAX_PAGES 128)
set(CONFIG_BALLOC_ARENA_PAGES 4)

# --- Console: Native USB-Serial/JTAG (J8 USB-C) + UART0 (GPIO37/38) -------
#
# [sch sheet 6 "USB_UART" & sheet 13 "ESP32_P4_SOC"] The USB-UART bridge U32
# (CH9102F) is not populated on the Tab5; the USB-C receptacle J8 routes
# USB_DP/USB_DN directly to GPIO25/GPIO24 (the ESP32-P4's Full-Speed
# USB-Serial/JTAG peripheral at 0x500D2000, VID:PID 303a:1001).
# UART0 on GPIO37/38 is kept active in parallel so both wires carry console TX
# and accept console RX.
set(CONFIG_UART0_BASE     0x500CA000)
set(CONFIG_UART0_TX_GPIO  37)
set(CONFIG_UART0_RX_GPIO  38)
set(CONFIG_UART0_SCLK_HZ  40000000)
set(CONFIG_UART0_BAUD     921600)
set(CONFIG_USBJTAG_BASE   0x500D2000)
set(CONFIG_XTAL_HZ        40000000)   # [meas: esptool, 40 MHz]

# CPU clock: 360 MHz, the top of v1.3's ladder (40/90/180/360).
set(CONFIG_CPU_FREQ_MHZ   360)

# L2 cache size in L2MEM: 128 KB.
set(CONFIG_L2_CACHE_KB    128)

# --- PSRAM: 32 MB in-package Hex DDR PSRAM --------------------------------
#
# [meas: 32 MB AP Hex DDR PSRAM @ 200 MHz].
set(CONFIG_PSRAM_BYTES       33554432)
set(CONFIG_PALLOC_BULK_PAGES 8192)
set(CONFIG_PSRAM_SPEED_MHZ   200)
set(CONFIG_RAMDISK_MAX_KB    4096)
set(CONFIG_LISP_NODE_POOL    65536)
set(CONFIG_LISP_STRING_POOL  3072)

# --- Internal System I2C (I2C0 routed via GPIO matrix to GPIO31/32) -------
#
# [sch sheet 13, m5 M5Unified.cpp] SYS_I2C_SDA = GPIO31, SYS_I2C_SCL = GPIO32.
# Hosts PI4IOE1 (0x43), PI4IOE2 (0x44), ST7123 touch (0x55) / GT911 (0x14/0x5D),
# ES8388 (0x10), RX8130CE RTC (0x32), SC2356 (0x36), ES7210 (0x40), INA226 (0x41),
# and BMI270 IMU (0x68) [meas: factory I2C scan].
set(CONFIG_I2C_RTC_SDA_GPIO 31)
set(CONFIG_I2C_RTC_SCL_GPIO 32)

# --- microSD: SDMMC slot 0 (GPIO39-44) ------------------------------------
#
# [sch sheet 15 "SD_Card"] Slot 0 dedicated IO_MUX pads (CLK=43, CMD=44,
# D0..D3=39..42). Card VDD is wired directly to SOC_3.3V (no GPIO45 MOSFET
# switch; GPIO45 is CAM_MCLK), so CONFIG_SDMMC_PWR_GPIO is omitted.
set(CONFIG_SDMMC_BASE      0x50083000)
set(CONFIG_SDMMC_CLK_GPIO  43)
set(CONFIG_SDMMC_CMD_GPIO  44)
set(CONFIG_SDMMC_D0_GPIO   39)
set(CONFIG_SDMMC_D1_GPIO   40)
set(CONFIG_SDMMC_D2_GPIO   41)
set(CONFIG_SDMMC_D3_GPIO   42)
set(CONFIG_SDMMC_BUS_WIDTH 4)
set(CONFIG_SDMMC_FREQ_KHZ  20000)

# --- Display: 5" 720x1280 MIPI-DSI (ST7123 / ST7121 / ILI9881C) -----------
#
# [sch sheet 16 "Display", m5 M5GFX.cpp] 2-lane MIPI-DSI on dedicated DSI PHY
# pads, powered by on-chip LDO channel 3 at 2500 mV.
#   LCD_RST is driven by PI4IOE1 (0x43) P4 (bit 4), not a direct P4 GPIO.
#   LCD_BL_CTRL is GPIO22 -> U8 ME2212 boost converter EN (active-high PWM,
#   with 5.1k pull-down R24 to GND).
set(CONFIG_DSI_LCD_BL_GPIO        22)
set(CONFIG_DSI_LCD_BL_ACTIVE_HIGH 1)

# --- Touch: ST7123 (0x55) / GT911 (0x14/0x5D) on SYS_I2C ------------------
#
# [sch sheet 16, m5 Touch_ST7123.hpp] TP_RST on PI4IOE1 (0x43) P5 (bit 5);
# TP_INT on GPIO23. This physical unit carries the ST7123 at 0x55 [meas].
set(CONFIG_TOUCH_GT911    1)
set(CONFIG_TOUCH_ST7123   1)
set(CONFIG_TOUCH_INT_GPIO 23)

# --- USB Host: USB Type-A port (J10) on DWC2 OTG 2.0 High-Speed PHY -------
#
# [sch sheet 7 "USB_HOST"] Dedicated High-Speed OTG PHY pads (USB2_OTG_DP/DN).
# 5V VBUS (SYS_USB5V) is switched by U27 (MT9700) controlled by PI4IOE2 (0x44)
# P3 (USB5V_EN, bit 3 = 1), enabled at boot by drivers/i2c_bus.c.
set(CONFIG_USBHOST_DWC2 1)
