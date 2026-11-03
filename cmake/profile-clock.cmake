# Profile: the LED clock appliance -- the Pico-Clock-Green baseboard on a Pico 2 W
# (rp2350-clock): a DCF-77 receiver, Wi-Fi, and an NTP server for the segment.
# What a profile is: cmake/lugalos_layers.cmake.

set(LUGALOS_ENABLE_CC               ON)
set(LUGALOS_ENABLE_ED               ON)
set(LUGALOS_ENABLE_CHESS            OFF)
set(LUGALOS_ENABLE_ZMACHINE         ON)
set(LUGALOS_ENABLE_MOONBASE         ON)
set(LUGALOS_ENABLE_CERES            ON)
set(LUGALOS_ENABLE_RIBBON_ANIMATION ON)

set(CONFIG_BALLOC_ARENA_PAGES 4)

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

# Serve time to the segment (P6). On this persona only: it is the one with a
# radio and a receiver. A board without a time source can still run the server
# honestly -- it would answer LI 3 / stratum 16 forever -- but a port that is
# not listening states that more clearly than one that is listening to say no.
set(CONFIG_ENABLE_NTP_SERVER 1)

# --- P0: the measurement instrument -------------------------------
#
# While CONFIG_DCF77_P0_LOG is 1 the persona is an instrument rather than an
# appliance: it never sets its own clock from the radio (auto-sync is turned
# off at boot), and it asks a reference server what time it is once a minute,
# forever. The reference is site-specific: 192.168.178.23 is this bench's
# GPS-disciplined stratum-1; 192.168.178.1 is the gateway and is NOT a
# reference clock. Results are read from /proc/dcf77log over 9P.
#
# 0 now that P0 and P4 are finished. It broadcasts a sample a minute to the
# segment, which was the instrument those milestones were built on and is
# noise on a network the clock is otherwise only serving time to. Set it to 1
# to re-run either measurement; nothing else depends on it.
set(CONFIG_DCF77_P0_LOG        0)
set(CONFIG_DCF77_P0_NTP        "192.168.178.23")
set(CONFIG_DCF77_P0_PERIOD_S  60)
