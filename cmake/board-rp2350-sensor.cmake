# Per-board facts for RP2350W ("Pico 2 W"), populated for the **environment
# sensor** persona (Q7, plan/phase26_mqtt_and_environment_sensors.md): a bare
# Pico 2 W with a BME280 on I2C0 and nothing else at all.
#
# Minimal hardware, and that is the point of it being its own persona rather
# than a note on cmake/board-rp2350-wifi.cmake: no SD card, no display, no
# keypad, no RTC, no Ethernet. A board on a USB charger with four wires to a
# sensor.
#
# **Why a persona exists at all**, given the sensor itself needs no board-file
# change (it sits on the I2C0 pins every RP2350 persona already declares for
# the RTC probe, and drivers/bme280.c reaches them through the shared i2c
# task): because **autostart is what a persona is for**. The rule set in
# phase 11 and kept since is that a program starts by itself only on an
# appliance persona, never in the general build. Everything Q0-Q6 builds works
# on rp2350-wifi by hand, and that is how it is tested; this persona adds
# unattendedness and subtracts a compiler.
#
# Same RP2350 silicon, arch and linker script as every other RP2350 persona --
# a board file, not a new LUGALOS_TARGET, selected via the rp2350-sensor
# preset.

set(CONFIG_PALLOC_MAX_PAGES 128)
set(CONFIG_BALLOC_ARENA_PAGES 4)

# UART0: the console, GP0/GP1, unchanged from every other RP2350 persona. Kept
# even though the appliance runs headless -- a sensor node that cannot be
# asked what it is doing is a sensor node nobody can debug.
set(CONFIG_UART0_BASE     0x40070000)
set(CONFIG_UART0_TX_GPIO  0)
set(CONFIG_UART0_RX_GPIO  1)

# Heartbeat LED. NOT CONFIG_LED_ONBOARD_GPIO and NOT GP25: on a Pico 2 **W**
# the user LED hangs off the wireless module (WL_GPIO0) and GP25 is the
# CYW43439's chip select -- the same finding cmake/board-rp2350-wifi.cmake and
# the clock persona both record. GP9 is free on every RP2350W board this
# project has met.
set(CONFIG_LED_EXT_GPIO   9)

# I2C0 on GP4/GP5: the BME280, at 0x76 or 0x77 depending on its SDO strapping
# (drivers/bme280.c probes both).
#
# These are the same pins and the same peripheral instance every other RP2350
# persona declares for a DS1307/DS3231 -- deliberately, not incidentally.
# drivers/i2c_rtc.c owns the bus and arbitrates it through the shared "i2c"
# task, so the sensor is one more address on a bus that already has an owner
# rather than a second bus master. The CONFIG_I2C_RTC_* names are historical:
# they name the bus, not the part on it. No RTC is fitted on this board; the
# probe finds none, says so once at boot, and the kernel clock runs from its
# own monotonic counter until NTP or the network says otherwise.
#
# Wiring, four wires:
#
#   BME280 VCC  -> 3V3 (pin 36)   -- 3.3V. Most breakouts have a regulator and
#                  tolerate 5V, but the Pico has 3V3 right there, so use it.
#   BME280 GND  -> GND (pin 38)
#   BME280 SDA  -> GP4 (pin 6)
#   BME280 SCL  -> GP5 (pin 7)
#   BME280 SDO  -> leave as the module strapped it. Pulled low on most
#                  breakouts, giving address 0x76; a module that pulls it high
#                  answers at 0x77 and is found just the same.
#   BME280 CSB  -> leave alone. It selects I2C when high, and every breakout
#                  that exposes I2C pulls it up already.
#
# No external pull-ups are needed: drivers/i2c_rtc.c enables the RP2350's
# internal ones, and they have carried a DS3231 and an AT24C32 on the clock
# persona's identical wiring.
set(CONFIG_I2C_RTC_BASE     0x40090000) # I2C0
set(CONFIG_I2C_RTC_SDA_GPIO 4)
set(CONFIG_I2C_RTC_SCL_GPIO 5)

# CYW43439 wireless, R5. GP23/24/25/29 are internal to the Pico 2 W module on
# every board carrying one -- see cmake/board-rp2350-wifi.cmake for the pin
# table and the PIO/gSPI reasoning, which is identical here.
set(CONFIG_WL_ON_GPIO   23)
set(CONFIG_WL_DATA_GPIO 24)
set(CONFIG_WL_CS_GPIO   25)
set(CONFIG_WL_CLK_GPIO  29)

# Deliberately no CONFIG_SPI1_*: no SD card is fitted. The filesystem is
# /flash0 alone (flashfs.uf2), which is how the clock persona has run since
# I7a. LUGALOS_ENABLE_SPISD=OFF in the preset matches.
#
# Deliberately no CONFIG_UART1_*, no CONFIG_ETH_*, no display, no DCF77: none
# of it is fitted, and a pin reservation for a part that does not exist is how
# GP16 became a bus fight on the gateway persona once already.
