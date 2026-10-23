# Generates lugalos_config.h from a per-board cmake/board-*.cmake file (K0,
# ).
#
# Run as a build-time step (not just at configure time), same shape as
# gen_build_id.cmake, so an edited board file is picked up without a
# re-configure. The file is only rewritten when its contents change, so an
# unchanged config does not force a rebuild.
#
# The *schema* -- which CONFIG_* keys exist, and which are mandatory -- lives
# here, not in the board files. That keeps each board file a flat, diffable
# list of facts ("is this number right") rather than something that also has
# to get the shape right.
#
# Inputs: BOARD (path to the board-*.cmake file), OUT (header path to write),
# ENABLE_CC / ENABLE_ED / ENABLE_ST7735 / ENABLE_TM1638 / ENABLE_CHESS /
# ENABLE_SPISD / ENABLE_SDMMC / ENABLE_PICO_CLOCK_GREEN / ENABLE_DCF77 (ON/OFF feature
# flags, F0 , H3 ,
# L1/L2 , D2 
# dcf77.md -- build persona choices, not board facts, so they arrive as
# generator inputs rather than living in the board file). The last six are
# meaningful only on RP2350 (no display/keypad/chess/spisd/clockdisplay/
# dcf77 code path exists for any other target), but are still emitted
# everywhere for the same reason ENABLE_CC/
# ENABLE_ED are: the CMake option surface is uniform across targets, and a
# QEMU build carrying an inert CONFIG_ENABLE_ST7735 is harmless.

if(NOT DEFINED BOARD OR NOT EXISTS "${BOARD}")
    message(FATAL_ERROR "gen_config.cmake: board file not found: '${BOARD}'")
endif()
if(NOT DEFINED ENABLE_MOONBASE)
    set(ENABLE_MOONBASE OFF)
endif()
foreach(_flag ENABLE_CC ENABLE_ED ENABLE_ST7735 ENABLE_TM1638 ENABLE_CHESS ENABLE_ZMACHINE ENABLE_SPISD ENABLE_SDMMC ENABLE_PICO_CLOCK_GREEN ENABLE_DCF77 ENABLE_GPS ENABLE_SMP ENABLE_RIBBON_ANIMATION ENABLE_MOONBASE)
    if(NOT DEFINED ${_flag})
        message(FATAL_ERROR "gen_config.cmake: ${_flag} is required")
    endif()
endforeach()

include("${BOARD}")

# Mandatory on every target that has a board file at all.
foreach(_key CONFIG_PALLOC_MAX_PAGES CONFIG_BALLOC_ARENA_PAGES CONFIG_UART0_BASE)
    if(NOT DEFINED ${_key})
        message(FATAL_ERROR "gen_config.cmake: ${BOARD} is missing required key ${_key}")
    endif()
endforeach()

foreach(_flag ENABLE_CC ENABLE_ED ENABLE_ST7735 ENABLE_TM1638 ENABLE_CHESS ENABLE_ZMACHINE ENABLE_SPISD ENABLE_SDMMC ENABLE_PICO_CLOCK_GREEN ENABLE_DCF77 ENABLE_GPS ENABLE_SMP ENABLE_RIBBON_ANIMATION ENABLE_MOONBASE)
    if(${_flag})
        set(CONFIG_${_flag} 1)
    else()
        set(CONFIG_${_flag} 0)
    endif()
endforeach()

# Optional -- only a board with real GPIO pins to map (RP2350 today) sets
# these. Emitted if present, skipped otherwise: a QEMU target's virtio
# devices have no pins to describe.
set(_optional_keys
    CONFIG_UART0_TX_GPIO CONFIG_UART0_RX_GPIO
    # the console UART's source clock
    # and baud rate. Optional like the pin maps -- a board whose UART is a
    # 16550 at a fixed emulated rate has neither to state -- and separate
    # from CONFIG_XTAL_HZ below, which is the crystal rather than the clock
    # this kernel chooses to drive the UART from.
    CONFIG_UART0_SCLK_HZ CONFIG_UART0_BAUD
    CONFIG_XTAL_HZ CONFIG_CPU_FREQ_MHZ
    # the ESP32-P4's silicon
    # revision as major*100+minor (103 = v1.3, 302 = v3.2). A board fact
    # chosen at build time, because v3.x moved the L2 cache, the ROM's data
    # and its entry points; 47.1 makes the code act on it. Absent: v1.3.
    CONFIG_ESP32P4_REV
    # the C6's console peripheral and its clock.
    CONFIG_USBJTAG_BASE CONFIG_SYSTIMER_BASE
    CONFIG_SPI1_BASE CONFIG_SPI1_SCK_GPIO CONFIG_SPI1_MOSI_GPIO
    CONFIG_SPI1_MISO_GPIO CONFIG_SPI1_CS_GPIO
    CONFIG_LED_ONBOARD_GPIO CONFIG_LED_EXT_GPIO
    CONFIG_SPI0_BASE CONFIG_ST7735_SCK_GPIO CONFIG_ST7735_MOSI_GPIO
    CONFIG_ST7735_CS_GPIO CONFIG_ST7735_DC_GPIO CONFIG_ST7735_RST_GPIO
    CONFIG_TM1638_STB_GPIO CONFIG_TM1638_CLK_GPIO CONFIG_TM1638_DIO_GPIO
    CONFIG_CLOCK_OE_GPIO CONFIG_CLOCK_SDI_GPIO CONFIG_CLOCK_CLK_GPIO CONFIG_CLOCK_LE_GPIO
    CONFIG_CLOCK_A0_GPIO CONFIG_CLOCK_A1_GPIO CONFIG_CLOCK_A2_GPIO CONFIG_CLOCK_ADC_LIGHT_GPIO
    CONFIG_CLOCK_BTN_SET_GPIO CONFIG_CLOCK_BTN_UP_GPIO CONFIG_CLOCK_BTN_DOWN_GPIO
    CONFIG_CLOCK_BUZZER_GPIO
    CONFIG_CLOCK_TEMP_OFFSET_C CONFIG_CLOCK_COLON_BLINK CONFIG_CLOCK_BOOT_BEACON
    CONFIG_I2C_RTC_BASE CONFIG_I2C_RTC_SDA_GPIO CONFIG_I2C_RTC_SCL_GPIO
    # a second I2C controller (ESP32-P4 I2C1) and its pins, and the RTC chip
    # when it is a board part rather than a module found by probing 0x68
    CONFIG_I2C1_SDA_GPIO CONFIG_I2C1_SCL_GPIO CONFIG_RTC_RX8130
    # a battery the board carries, and the monitor on it (drivers/battery.h,
    # drivers/ina226.h)
    CONFIG_BATT_CELLS CONFIG_BATT_EMPTY_MV CONFIG_BATT_FULL_MV CONFIG_BATT_RINT_MOHM
    CONFIG_BATT_CAPACITY_MAH CONFIG_INA226_SHUNT_UOHM CONFIG_INA226_INVERT
    # the display rotation that reads upright with gravity along each IMU
    # axis (kernel/orient.c)
    CONFIG_IMU_ROT_XPOS CONFIG_IMU_ROT_XNEG CONFIG_IMU_ROT_YPOS CONFIG_IMU_ROT_YNEG
    # N5, : the gateway persona's UART1
    # downlink. Optional like everything else here -- a board with no second
    # UART simply does not set them. (The W5500's own pins lived here too,
    # until R0 removed the part.)
    CONFIG_UART1_BASE CONFIG_UART1_TX_GPIO CONFIG_UART1_RX_GPIO
    # MH-Z19B NDIR CO2 sensor via UART
    CONFIG_MHZ19B_UART_BASE CONFIG_MHZ19B_TX_GPIO CONFIG_MHZ19B_RX_GPIO
    # the P4's EMAC and the IP101GRI
    # it drives over RMII. Optional like every pin map here -- the three
    # boards in this tree that have no MAC simply do not set them, and
    # drivers/emac_esp32p4.c compiles only for the one that does. The pin
    # numbers are not interchangeable and not tidy-able; the board file says
    # why at length.
    CONFIG_EMAC_BASE CONFIG_EMAC_INTR_SRC
    CONFIG_EMAC_MDC_GPIO CONFIG_EMAC_MDIO_GPIO CONFIG_EMAC_PHY_RST_GPIO
    CONFIG_EMAC_RMII_CLK_GPIO
    CONFIG_EMAC_TX_EN_GPIO CONFIG_EMAC_TXD0_GPIO CONFIG_EMAC_TXD1_GPIO
    CONFIG_EMAC_CRS_DV_GPIO CONFIG_EMAC_RXD0_GPIO CONFIG_EMAC_RXD1_GPIO
    CONFIG_EMAC_PHY_ADDR CONFIG_EMAC_PHY_ID1 CONFIG_EMAC_PHY_ID2
    # the L2 cache size in KB. Read by arch/riscv/common/trap.c, and
    # passed separately to the linker so the two cannot drift.
    CONFIG_L2_CACHE_KB
    # the P4's SD/MMC host and the
    # microSD socket the NANO hangs off it. Optional like every pin map here.
    # CONFIG_SDMMC_PWR_GPIO is not one of the bus's signals -- it is this
    # board's power switch, and the board file says what it switches.
    CONFIG_SDMMC_BASE
    CONFIG_SDMMC_CLK_GPIO CONFIG_SDMMC_CMD_GPIO
    CONFIG_SDMMC_D0_GPIO CONFIG_SDMMC_D1_GPIO
    CONFIG_SDMMC_D2_GPIO CONFIG_SDMMC_D3_GPIO
    CONFIG_SDMMC_PWR_GPIO CONFIG_SDMMC_BUS_WIDTH CONFIG_SDMMC_FREQ_KHZ
    # the same controller's slot 1,
    # wired to the board's ESP32-C6 over SDIO, and the C6's reset (CHIP_PU).
    CONFIG_SDIO_CLK_GPIO CONFIG_SDIO_CMD_GPIO
    CONFIG_SDIO_D0_GPIO CONFIG_SDIO_D1_GPIO CONFIG_SDIO_D2_GPIO CONFIG_SDIO_D3_GPIO
    CONFIG_SDIO_FREQ_KHZ CONFIG_C6_CHIP_PU_GPIO
    # the C6's SDIO slave toward its P4, on a module that has one.
    CONFIG_SDIO_SLAVE
    # R4, : the ENC28J60 on SPI0.
    CONFIG_ETH_SCK_GPIO CONFIG_ETH_MOSI_GPIO CONFIG_ETH_MISO_GPIO
    CONFIG_ETH_CS_GPIO CONFIG_ETH_RST_GPIO CONFIG_ETH_INT_GPIO
    CONFIG_DCF77_OUT_GPIO CONFIG_DCF77_PON_GPIO CONFIG_DCF77_PON_ACTIVE_LOW
    CONFIG_DCF77_OUT_ACTIVE_LOW
    CONFIG_DCF77_WARMUP_MS
    CONFIG_GPS_UART_BASE
    CONFIG_GPS_RX_GPIO
    CONFIG_GPS_TX_GPIO
    CONFIG_GPS_PPS_GPIO
    CONFIG_GPS_BAUD
    CONFIG_GPS_PPS_ACTIVE_LOW
    CONFIG_GPS_UBX_TIMEPULSE
    CONFIG_GPS_UBX_PERSIST
    CONFIG_DCF77_DELAY_US
    CONFIG_ENABLE_NTP_SERVER
    CONFIG_DCF77_P0_LOG
    CONFIG_DCF77_P0_PERIOD_S
    CONFIG_DCF77_P0_PORT
    CONFIG_DCF77_PON_PRESENT
    CONFIG_DCF77_AUTO_HOUR CONFIG_DCF77_AUTO_MIN CONFIG_DCF77_AUTO_ENABLED
    # R5, : the CYW43439 wireless
    # module's gSPI bus, bit-banged over PIO0. GP23/24/25/29 on every
    # Pico 2 W, whichever persona carries one.
    CONFIG_WL_ON_GPIO CONFIG_WL_DATA_GPIO CONFIG_WL_CS_GPIO CONFIG_WL_CLK_GPIO
    # the RP2350-LCD-7's PIO-driven
    # USB host port (J7). Optional like every pin map here; its presence is what
    # builds drivers/boardprobe_rp2350.c's content.
    CONFIG_PIOUSB_DP_GPIO CONFIG_PIOUSB_DM_GPIO
    # the RP2350's clk_sys in Hz. Optional; arch/rp2350_clocks.h
    # defaults it to the 150 MHz every persona ran at before it existed.
    CONFIG_CLK_SYS_HZ
    # the RP2350-LCD-7's RGB panel. Guard-by-pin-map: their presence
    # builds drivers/lcd7_rp2350.c's content.
    CONFIG_LCD_DE_GPIO CONFIG_LCD_PCLK_GPIO CONFIG_LCD_DATA0_GPIO
    CONFIG_LCD_RST_GPIO CONFIG_LCD_BL_GPIO CONFIG_LCD_EN_GPIO
    # LLP on UART0 at this rate, toward a radio beside the board (the
    # terminal's header H7 and a C6-Zero). Its presence builds kernel/llp_node.c.
    CONFIG_LLP_UART_BAUD
    # a board with a wired and a radio interface (the NANO: eth0 and its C6's
    # wlan0): the stack serves the wired one while it has a carrier (net/stack.c).
    CONFIG_NET_PREFER_WIRED
    # the panel scanned out turned by 180 degrees (1), for a board
    # whose glass is mounted the other way up from its inscriptions.
    CONFIG_LCD_ROTATE_180
    # Lisp's node pool, when a
    # board wants other than user/lisp/lisp.c's per-target default.
    CONFIG_LISP_NODE_POOL
    # and its string pool (slots, both tiers),
    # likewise when a board wants other than the per-target default.
    CONFIG_LISP_STRING_POOL
    # a QSPI PSRAM on the QMI's second chip
    # select, and its size. Guard-by-pin-map: their presence builds
    # drivers/psram_rp2350.c's content and makes the persona require it (S1).
    CONFIG_PSRAM_CS_GPIO CONFIG_PSRAM_BYTES
    # the bulk page zone's bitmap capacity, in pages. On a board with
    # PSRAM it covers the chip; on QEMU it sizes a stand-in zone carved from
    # the RAM above the fast zone, so the suite runs the zone code. Absent:
    # no bulk zone, and the bulk calls are the fast ones.
    CONFIG_PALLOC_BULK_PAGES
    # the ESP32-P4 PSRAM's bus
    # speed, 20 (untuned) or 200 (DQS timing tuning at boot). Absent: 20.
    CONFIG_PSRAM_SPEED_MHZ
    # the LCD-7B's / Tab5's MIPI-DSI
    # panel -- its reset and backlight GPIOs. Absent: no display, and
    # drivers/dsi_esp32p4.c is an empty unit.
    CONFIG_BOARD_TAB5
    CONFIG_DSI_LCD_RST_GPIO CONFIG_DSI_LCD_BL_GPIO CONFIG_DSI_LCD_BL_ACTIVE_HIGH
    # the ESP32-P4's OTG HS controller as a USB host (the LCD-7B's / Tab5's
    # Type-A port), and the keyboard task on it. Absent: no USB host, and
    # drivers/usbhost_esp32p4.c is an empty unit.
    CONFIG_USBHOST_DWC2
    # a GT911 or ST7123 touch controller on the board's I2C bus, polled
    # by drivers/touch_gt911.c (tap to focus, swipe to move the ribbon).
    CONFIG_TOUCH_GT911 CONFIG_TOUCH_ST7123 CONFIG_TOUCH_INT_GPIO
    # How its touch layer sits on the panel: its X / Y run against the
    # picture's. Absent: with it.
    CONFIG_TOUCH_MIRROR_X CONFIG_TOUCH_MIRROR_Y
    # Typography: HiDPI display scaling profile (e.g. 16x32 cells for Tab5).
    # Absent: Standard density (8x16 cells).
    CONFIG_FONT_HIDPI CONFIG_FONT_CELL_W CONFIG_FONT_CELL_H
    # the largest /ram0 a mount may ask for, in KB, when a board wants
    # other than drivers/ramdisk.c's per-target default.
    CONFIG_RAMDISK_MAX_KB
    # the chess transposition table, in KB, when a board wants other
    # than the 32 KB default (sign-off S4).
    CONFIG_CHESS_TT_KB
    # the chess engine's hot files execute from SRAM, their tables in
    # SRAM too (the lugalos_text/rodata/ramcode.ld fragments below).
    CONFIG_CHESS_HOT_RAM
)

# Emitted as a quoted C string rather than a bare token, which is the only
# reason it needs its own list. A default is supplied here rather than in each
# board file because a timezone is not a board fact -- the same hardware works
# anywhere -- but every build needs one, and the kernel clock runs on UTC
# (kernel/timezone.h) so this only ever affects what a human is shown.
# P0. Off everywhere unless a
# board file turns it on: it is an instrument, not a feature, and it costs an
# NTP query a minute for as long as it runs.
if(NOT DEFINED CONFIG_ENABLE_NTP_SERVER)
    set(CONFIG_ENABLE_NTP_SERVER 0)
endif()
if(NOT DEFINED CONFIG_DCF77_DELAY_US)
    set(CONFIG_DCF77_DELAY_US 0)
endif()
if(NOT DEFINED CONFIG_GPS_UBX_PERSIST)
    set(CONFIG_GPS_UBX_PERSIST 0)
endif()
if(NOT DEFINED CONFIG_GPS_UBX_TIMEPULSE)
    set(CONFIG_GPS_UBX_TIMEPULSE 0)
endif()
if(NOT DEFINED CONFIG_GPS_PPS_ACTIVE_LOW)
    set(CONFIG_GPS_PPS_ACTIVE_LOW 0)
endif()
if(NOT DEFINED CONFIG_DCF77_P0_LOG)
    set(CONFIG_DCF77_P0_LOG 0)
endif()
if(NOT DEFINED CONFIG_DCF77_P0_NTP)
    set(CONFIG_DCF77_P0_NTP "0.0.0.0")
endif()
if(NOT DEFINED CONFIG_DCF77_P0_PERIOD_S)
    set(CONFIG_DCF77_P0_PERIOD_S 60)
endif()
if(NOT DEFINED CONFIG_DCF77_P0_PORT)
    set(CONFIG_DCF77_P0_PORT 5959)
endif()

if(NOT DEFINED CONFIG_TIMEZONE)
    set(CONFIG_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3")   # Europe/Berlin
endif()
# --- Node identity ---
#
# Every node needs a name and a MAC that are its own. A board file may pin
# either; when it does not, both are *derived* here.
#
# **Derived, not randomised**, and the distinction is the whole design. A
# build-time random number gives uniqueness and takes reproducibility with it:
# two builds of identical sources would differ, `lugalos_build_id` would stop
# meaning anything, and a board's identity would change every time it was
# reflashed -- so every ARP cache, every host-side config and every later DHCP
# reservation keyed to it would go stale for no reason. Hashing the build
# context instead gives the same uniqueness with none of that: stable across
# rebuilds, distinct between machines, distinct between personas on one
# machine, and reproducible for whoever builds it.
#
# The board file path carries both the persona and the source tree; the
# hostname separates two developers building the same tree. Eight hex digits
# is 32 bits, which for the handful of nodes this project will ever put on one
# segment is collision probability in the millionths.
#
# Real silicon does better and should: once board_unique_id() can read the
# RP2350's flash id (R4, where it can be verified on hardware), that takes
# precedence over this seed at runtime. This is the floor, not the ceiling.
if(NOT DEFINED CONFIG_NODE_SEED)
    cmake_host_system_information(RESULT _node_host QUERY HOSTNAME)
    string(SHA256 _node_hash "${BOARD}|${_node_host}")
    string(SUBSTRING "${_node_hash}" 0 8 CONFIG_NODE_SEED)
endif()
if(NOT DEFINED CONFIG_NODE_PERSONA)
    get_filename_component(_persona "${BOARD}" NAME_WE)
    string(REGEX REPLACE "^board-" "" _persona "${_persona}")
    set(CONFIG_NODE_PERSONA "${_persona}")
endif()

set(_string_keys CONFIG_TIMEZONE CONFIG_NODE_SEED CONFIG_NODE_PERSONA CONFIG_DCF77_P0_NTP)
# Optional, and only emitted when a board file pins them: a node whose name or
# MAC is a board fact (a soldered appliance, a MAC from a label) says so here
# and the derivation stays out of the way.
foreach(_key CONFIG_NODE_NAME CONFIG_NODE_MAC)
    if(DEFINED ${_key})
        list(APPEND _string_keys ${_key})
    endif()
endforeach()

set(_content "/* Generated by cmake/gen_config.cmake from ${BOARD} -- do not edit. */\n")
string(APPEND _content "#ifndef LUGALOS_CONFIG_H\n#define LUGALOS_CONFIG_H\n\n")
string(APPEND _content "#define CONFIG_PALLOC_MAX_PAGES ${CONFIG_PALLOC_MAX_PAGES}\n")
string(APPEND _content "#define CONFIG_BALLOC_ARENA_PAGES ${CONFIG_BALLOC_ARENA_PAGES}\n")
string(APPEND _content "#define CONFIG_UART0_BASE ${CONFIG_UART0_BASE}\n")
foreach(_flag ENABLE_CC ENABLE_ED ENABLE_ST7735 ENABLE_TM1638 ENABLE_CHESS ENABLE_ZMACHINE ENABLE_SPISD ENABLE_SDMMC ENABLE_PICO_CLOCK_GREEN ENABLE_DCF77 ENABLE_GPS ENABLE_SMP ENABLE_RIBBON_ANIMATION ENABLE_MOONBASE)
    string(APPEND _content "#define CONFIG_${_flag} ${CONFIG_${_flag}}\n")
endforeach()
foreach(_key ${_optional_keys})
    if(DEFINED ${_key})
        string(APPEND _content "#define ${_key} ${${_key}}\n")
    endif()
endforeach()
foreach(_key ${_string_keys})
    string(APPEND _content "#define ${_key} \"${${_key}}\"\n")
endforeach()

# The parts soldered to an I2C bus, as a list of ADDR:name items in ascending
# address order (drivers/i2c_bus.c searches the table with bsearch). Emitted as
# a C initializer: {0x10u, "es8388"}, {0x14u, "gt911"}, ...
foreach(_bus 0 1)
    set(_key CONFIG_I2C${_bus}_PARTS)
    if(NOT DEFINED ${_key})
        continue()
    endif()
    set(_init "")
    set(_prev -1)
    foreach(_item ${${_key}})
        if(NOT _item MATCHES "^0x([0-9a-fA-F][0-9a-fA-F]?):([a-z0-9_]+)$")
            message(FATAL_ERROR "gen_config.cmake: ${_key} item '${_item}' is not ADDR:name (e.g. 0x10:es8388)")
        endif()
        set(_name "${CMAKE_MATCH_2}")
        math(EXPR _addr "0x${CMAKE_MATCH_1}")
        if(_addr LESS_EQUAL _prev OR _addr GREATER 127)
            message(FATAL_ERROR "gen_config.cmake: ${_key} must list 7-bit addresses in ascending order ('${_item}')")
        endif()
        set(_prev ${_addr})
        string(APPEND _init "{${_addr}u, \"${_name}\"}, ")
    endforeach()
    string(APPEND _content "#define ${_key} ${_init}\n")
endforeach()
string(APPEND _content "\n#endif /* LUGALOS_CONFIG_H */\n")

# lugalos_bulk.ld, INCLUDEd by
# linker/rp2350.ld just before .bss. GNU ld cannot pick a memory region
# conditionally, so the choice is made here, where the board's keys are known.
# With PSRAM, `.bss.bulk` (BULK_BSS in kernel/palloc.h) gets its own NOLOAD
# section at the start of the PSRAM window, and the bulk page zone begins
# after it. Without, the file defines nothing and .bss's own `.bss.*` takes
# the same objects into SRAM, zeroed by the reset handler like any .bss.
get_filename_component(_out_dir "${OUT}" DIRECTORY)
if(DEFINED CONFIG_PSRAM_BYTES)
    set(_bulk_ld "/* Generated by cmake/gen_config.cmake from ${BOARD} -- do not edit. */
/* This board has a PSRAM: BULK_BSS objects live there (38.4). Zeroed by
 * psram_init(), not by the reset handler, which runs before the chip is up. */
.bulk_bss (NOLOAD) : ALIGN(4096) {
    _bulk_bss_start = .;
    *(.bss.bulk .bss.bulk.*)
    . = ALIGN(4096);
    _bulk_bss_end = .;
} > PSRAM
ASSERT(_bulk_bss_end - ORIGIN(PSRAM) <= ${CONFIG_PSRAM_BYTES} / 2,
       \"BULK_BSS has outgrown half the PSRAM -- the bulk page zone needs the rest\")
")
else()
    set(_bulk_ld "/* Generated by cmake/gen_config.cmake from ${BOARD} -- do not edit. */
/* No PSRAM on this board: BULK_BSS objects stay in .bss (SRAM). */
_bulk_bss_start = 0;
_bulk_bss_end = 0;
")
endif()
set(_bulk_old "")
if(EXISTS "${_out_dir}/lugalos_bulk.ld")
    file(READ "${_out_dir}/lugalos_bulk.ld" _bulk_old)
endif()
if(NOT _bulk_old STREQUAL _bulk_ld)
    file(WRITE "${_out_dir}/lugalos_bulk.ld" "${_bulk_ld}")
endif()

# the chess engine's hot path in SRAM. ld gives
# an input section to the first rule in script order that matches it, and
# .text's `*(.text .text.*)` comes long before .data -- so moving whole files
# means the .text and .rodata rules must exclude them, and the RAM-code part
# of .data must name them. Three fragments, INCLUDEd at those three places by
# linker/rp2350.ld; on every other board they are the plain rules and an
# empty list. Only the files' `.rodata` moves: their strings
# (`.rodata.str*`) are not on the hot path and stay in flash.
function(_lugalos_write_if_changed path content)
    set(_prev "")
    if(EXISTS "${path}")
        file(READ "${path}" _prev)
    endif()
    if(NOT _prev STREQUAL content)
        file(WRITE "${path}" "${content}")
    endif()
endfunction()
set(_hdr "/* Generated by cmake/gen_config.cmake from ${BOARD} -- do not edit. */\n")
if(DEFINED CONFIG_CHESS_HOT_RAM AND CONFIG_CHESS_HOT_RAM)
    set(_hot_list search movegen position evaluation bitboard tt)
    list(TRANSFORM _hot_list PREPEND "*chess/src/")
    list(TRANSFORM _hot_list APPEND ".c.obj")
    list(JOIN _hot_list " " _hot)
    set(_text_ld "${_hdr}/* the chess hot path is in .data's RAM code instead. */\nEXCLUDE_FILE(${_hot}) *(.text .text.*)\n")
    set(_rodata_ld "${_hdr}EXCLUDE_FILE(${_hot}) *(.rodata)\n*(.rodata.*)\n")
    set(_ramcode_ld "${_hdr}/* the chess engine's hot files and their tables, from SRAM. */\n__chess_ram_start = .;\n")
    foreach(_f ${_hot_list})
        string(APPEND _ramcode_ld "${_f}(.text .text.* .rodata)\n")
    endforeach()
    string(APPEND _ramcode_ld "__chess_ram_end = .;\n")
else()
    set(_text_ld "${_hdr}*(.text .text.*)\n")
    set(_rodata_ld "${_hdr}*(.rodata .rodata.*)\n")
    set(_ramcode_ld "${_hdr}__chess_ram_start = .;\n__chess_ram_end = .;\n")
endif()
_lugalos_write_if_changed("${_out_dir}/lugalos_text.ld" "${_text_ld}")
_lugalos_write_if_changed("${_out_dir}/lugalos_rodata.ld" "${_rodata_ld}")
_lugalos_write_if_changed("${_out_dir}/lugalos_ramcode.ld" "${_ramcode_ld}")

set(_old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _content)
    file(WRITE "${OUT}" "${_content}")
endif()
