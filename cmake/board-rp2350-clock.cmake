# Per-board facts for RP2350 (Pico 2 / Pico 2 W), populated for the
# Waveshare Pico-Clock-Green baseboard persona (L1,
# plan/phase11_pico_clock_green.md). Same RP2350 silicon/arch/linker script
# as cmake/board-rp2350.cmake -- this is a second board file, not a second
# LUGALOS_TARGET, selected via -DLUGALOS_BOARD_FILE=cmake/board-rp2350-clock.cmake
# (see CMakeLists.txt) because the baseboard's soldered GPIO wiring collides
# with that file's SPI1 (SD card) and LED_EXT pin choices (phase11 L0's
# pin-conflict table).
#
# Built with LUGALOS_ENABLE_SPISD=OFF, LUGALOS_ENABLE_ST7735=OFF,
# LUGALOS_ENABLE_TM1638=OFF, LUGALOS_ENABLE_CHESS=OFF (see
# CMakePresets.json's rp2350-clock preset) -- none of that hardware is
# wired on this baseboard.

set(CONFIG_PALLOC_MAX_PAGES 128)

# Buddy-allocator arena (kernel/balloc.h), in pages. 4 = 16 KB, against 16
# (64 KB) on the QEMU targets.
#
# This board is the reason the constant is per-board at all (§1.1,
# plan/phase15_memory_reclamation.md). It is paid twice: the arena itself
# comes out of the heap on first use, and the buddy tree is permanent .bss
# whose size scales with this number -- 2046 bytes at 4 pages, 8190 at 16 --
# and on RP2350 .bss and the heap are the same 512 KB budget. 4 pages still
# leaves better than 2x headroom for the largest thing that actually calls
# this allocator (`ballocdemo`'s churn holds 7264 bytes of blocks at peak).
set(CONFIG_BALLOC_ARENA_PAGES 4)

# UART0: PL011, the board's console/9P wire -- unaffected by this baseboard,
# same GP0/GP1 as cmake/board-rp2350.cmake.
set(CONFIG_UART0_BASE     0x40070000)
set(CONFIG_UART0_TX_GPIO  0)
set(CONFIG_UART0_RX_GPIO  1)

# Heartbeat LED. GP16 (LED_EXT on cmake/board-rp2350.cmake) is claimed by the
# clock display's A0 row-address line (phase11 L0), so drivers/uart_rp2350.c's
# unconditional heartbeat blink is remapped to GP9 instead -- free on this
# baseboard's pinout (~/gith/pico-clock-green/define.h).
#
# Deliberately NO CONFIG_LED_ONBOARD_GPIO (phase17 C1). On a Pico 2 **W** the
# user LED is on the wireless module (WL_GPIO0) and GP25 is the CYW43439's
# chip select, so "onboard LED = GP25" was simply false here. It was harmless
# -- the pin is only driven at init and the radio is unpowered -- but it is a
# chip-select line being driven a few centimetres from a longwave receiver,
# which is exactly the kind of thing that should not be found later while
# chasing DCF-77 noise. uart_rp2350.c and /proc/config both treat the key as
# optional now, so its absence is the whole change.
set(CONFIG_LED_EXT_GPIO     9)

# Baseboard buttons and buzzer (phase17 C1). Active LOW with the internal
# pull-up enabled, and the buzzer active HIGH -- both taken from the vendor
# firmware rather than guessed: Pico-Clock-Green.c:196 tests
# `gpio_get(SET_FUNCTION) == 0` for "pressed" after gpio_pull_up() on all
# three (lines 137-139), and gpio_put(BUZZ,1) is "sound on" (line 413).
set(CONFIG_CLOCK_BTN_SET_GPIO    2)
set(CONFIG_CLOCK_BTN_UP_GPIO    17)
set(CONFIG_CLOCK_BTN_DOWN_GPIO  15)
set(CONFIG_CLOCK_BUZZER_GPIO    14)

# Display-path temperature correction, in whole degrees C (phase17 C2). The
# DS3231 reports its own die temperature, inside a closed case next to a
# self-heating LED matrix and an RP2350, so it reads high; -2 is the starting
# figure from the user's own comparison against a reference thermometer.
# Applied only where the number is shown -- i2c_rtc_read_temperature_c() still
# returns the raw reading, because a sensor driver that quietly hands back a
# fudged value is a trap for every other caller. Becomes a menu item in C3.
set(CONFIG_CLOCK_TEMP_OFFSET_C  -2)

# 1 Hz colon blink on the idle screen. Off, deliberately: the whole point of
# the C2 idle screen is a display that does not change on its own. The key
# exists for whoever disagrees.
set(CONFIG_CLOCK_COLON_BLINK     0)

# Boot-progress beacon: one click on the buzzer per CLOCK_BOOT_MARK() and a
# latching LED count on the top row. OFF -- an appliance should not click when
# it is plugged in.
#
# Kept in the tree rather than deleted, because it earned it: this board has
# no console, no LED and no SD card, and on 2026-08-23 the beacon was the only
# instrument that could say anything at all about a boot that stopped before
# the display existed. It localised "the clock will not start on a USB power
# adapter" down to a single function call, one bisect at a time. Set to 1 and
# scatter CLOCK_BOOT_MARK(n) over the suspect region; the count you hear is
# the number of marks passed, and the LEDs latch so a hang leaves its last
# mark lit.
set(CONFIG_CLOCK_BOOT_BEACON     0)

# The nightly automatic DCF-77 sync (phase17 D4), in LOCAL time. Off the hour
# deliberately: the longwave band is quieter there, and every other radio
# clock in the house is listening on the hour. 03:17 also covers the European
# DST changeover at 03:00 CEST / 02:00 CET.
set(CONFIG_DCF77_AUTO_HOUR       3)
set(CONFIG_DCF77_AUTO_MIN       17)
set(CONFIG_DCF77_AUTO_ENABLED    1)

# GP3 is the DS3231's square-wave output on this baseboard. It gets no config
# key, only this comment: nothing reads it, and phase 11 L1's rule is that a
# board fact earns a key when something consumes it. It becomes one the day a
# 1 Hz display tick or a hardware second-reference for the DCF-77 decoder
# actually wants it.

# Deliberately no CONFIG_SPI1_*: GP10-13 are the clock display's CLK/SDI/
# LE/OE (SM16106 shift registers) on this baseboard, not the SD-card bus --
# see LUGALOS_ENABLE_SPISD=OFF above and drivers/spisd_rp2350.c's stub
# branch. Deliberately no CONFIG_SPI0_*/CONFIG_ST7735_*/CONFIG_TM1638_*
# either: this persona has neither the chess TFT nor the TM1638 keypad.

# L2 (plan/phase11_pico_clock_green.md): SM16106 x2 (column shift
# registers) + SM5166P (row address decoder) driving the 8x24 LED matrix,
# plus the LDR for auto-brightness. Pin numbers straight from
# ~/gith/Pico-Clock-Green/define.h (OE/SDI/CLK/LE/A0-A2/ADC_Light) -- the
# baseboard's soldered wiring, not a LugalOS choice.
set(CONFIG_CLOCK_OE_GPIO         13)
set(CONFIG_CLOCK_SDI_GPIO        11)
set(CONFIG_CLOCK_CLK_GPIO        10)
set(CONFIG_CLOCK_LE_GPIO         12)
set(CONFIG_CLOCK_A0_GPIO         16)
set(CONFIG_CLOCK_A1_GPIO         18)
set(CONFIG_CLOCK_A2_GPIO         22)
set(CONFIG_CLOCK_ADC_LIGHT_GPIO  26)

# L3 (plan/phase11_pico_clock_green.md): DS3231 RTC on GP6/GP7 -- the
# vendor's own wiring (~/gith/Pico-Clock-Green/define.h: SDA=6, SCL=7,
# i2c1). RP2350's GPIO-to-controller mapping alternates every 4 pins, so
# GP6/GP7 land on the I2C1 peripheral instance, not the I2C0 instance
# cmake/board-rp2350.cmake's GP4/GP5 use -- a different base address, not
# just different pins (drivers/i2c_rtc.c derives which RESETS bit to use
# from this base address).
set(CONFIG_I2C_RTC_BASE     0x40098000) # I2C1
set(CONFIG_I2C_RTC_SDA_GPIO 6)
set(CONFIG_I2C_RTC_SCL_GPIO 7)

# D2 (plan/phase17_clock_ui_and_dcf77.md): DCF-77 longwave receiver module,
# soldered to the two free header pins farthest from the display's GP10-13
# (the LED matrix's switching lines are the loudest interferer this board
# has -- phase17 section 3). GP27 = header pin 32, GP28 = header pin 34;
# the module's VDD/GND go to 3V3(OUT)/GND (pins 36/38).
#
# OUT is the demodulated second pulse (carrier attenuated for 100ms = bit 0,
# 200ms = bit 1, nothing at all in second 59 = the minute mark). PON is the
# receiver's enable input: low = receiver powered, on nearly every module.
# "Nearly" is why CONFIG_DCF77_PON_ACTIVE_LOW exists as a board fact rather
# than a hardcoded constant, and why drivers/dcf77_rp2350.c's probe tries the
# other polarity before reporting failure -- neither polarity is guaranteed
# by anything we can read, only by what the hardware does.
set(CONFIG_DCF77_OUT_GPIO        27)

# Which level on OUT is the carrier attenuation -- i.e. the pulse. A board
# fact, not something to infer at runtime (user, 2026-08-22): the module
# carries a polarity jumper, currently on "neg.", so the pulses go LOW. The
# decoder used to work this out from duty cycle, which needs a good signal to
# be right and is therefore least reliable exactly when a clock is starting
# up or an antenna is being moved -- guessing wrong there locks it onto the
# gaps instead of the pulses. Measured duty is still reported, but only to
# warn that this setting and the jumper disagree.
set(CONFIG_DCF77_OUT_ACTIVE_LOW   1)

# PON is NOT wired on this board (user, 2026-08-23). The active receiver that
# ended up here does not have an enable input at all -- the reference designs
# for it tie the corresponding pin to ground and leave it there -- so there is
# nothing to drive, nothing to wait for, and no reason to reserve a GPIO. The
# pin numbers stay in the file because the driver still supports a module that
# does have PON, and because GP28 being *free* is a fact worth recording; the
# PRESENT flag is what decides whether any of it is touched.
set(CONFIG_DCF77_PON_PRESENT      0)
set(CONFIG_DCF77_PON_GPIO        28)
set(CONFIG_DCF77_PON_ACTIVE_LOW   1)

# How long after power-up before the module's AGC has settled enough for its
# output to mean anything. Datasheets for this class of receiver quote
# anywhere from ~1s to ~30s; 5s is a starting guess to be replaced with a
# measured number (phase17 D5), not a datasheet figure. With PON absent the
# receiver has been running since the board was powered, so the driver waits
# only for whatever part of this has not already elapsed since boot.
set(CONFIG_DCF77_WARMUP_MS     5000)

# --- CYW43439 wireless -----------------------------------------------------
#
# The board under the Pico-Clock-Green baseboard is a Pico 2 W, so the radio
# has been sitting there unused. Enabling it is what lets this persona stop
# being a clock that has to be told the time: an NTP client (phase 19's R6),
# or the inverse -- a DCF77-disciplined clock *serving* time to the segment.
#
# Same four pins as cmake/board-rp2350-wifi.cmake, and they cost this
# baseboard nothing: GP23/24/25/29 are internal to the Pico 2 W module and
# never reach the 40-pin header, so no baseboard can be using them. Checked
# against this file's own assignments before enabling -- the clock takes
# GP0,1,2,6,7,9,10,11,12,13,14,15,16,17,18,22,26,27,28, and none of those is
# one of these four.
#
#   GP23  WL_ON    wireless module power enable (WL_REG_ON), active high
#   GP24  WL_D     gSPI data, bidirectional, one wire
#   GP25  WL_CS    gSPI chip select (NOT the onboard LED on a W)
#   GP29  WL_CLK   gSPI clock
set(CONFIG_WL_ON_GPIO   23)
set(CONFIG_WL_DATA_GPIO 24)
set(CONFIG_WL_CS_GPIO   25)
set(CONFIG_WL_CLK_GPIO  29)
