/*
 * LugalOS Hardware Driver: DCF-77 longwave time-signal receiver (D2,
 * plan/phase17_clock_ui_and_dcf77.md).
 *
 * A 4-wire receiver module (VDD/GND/OUT/PON) on two free RP2350 header pins.
 * OUT carries the demodulated second pulse -- the 77.5 kHz carrier from
 * Mainflingen is attenuated to ~15% for 100ms (bit 0) or 200ms (bit 1) at
 * the start of every second, and not at all in second 59, which is how the
 * minute boundary is found. PON enables the receiver.
 *
 * This header is the pin/power/raw-level layer only. The frame decoder (D1)
 * is deliberately a separate, target-independent file so it can be tested on
 * QEMU without a radio -- see plan/phase17_clock_ui_and_dcf77.md D1.
 */

#ifndef DRIVERS_DCF77_H
#define DRIVERS_DCF77_H

#include <stdbool.h>
#include <stdint.h>

/* Pin bring-up. Leaves the receiver powered DOWN: PON is asserted only for
 * the duration of a sync or a probe, both to save the module's ~1-2 mA and
 * to keep its own oscillator off the air the rest of the time. Call once at
 * boot, mirroring pico_clock_green_init() (kernel/main.c). */
void dcf77_init(void);

/* PON. `on` means "receiver enabled", which is a LOW pin level when
 * CONFIG_DCF77_PON_ACTIVE_LOW is set -- and the driver may have flipped that
 * at runtime, see dcf77_pon_active_low(). */
void dcf77_power(bool on);

/* The OUT pin as wired, with no polarity correction applied: true = pin
 * high. Whether "high" means "carrier attenuated" (a pulse) or the opposite
 * is a property of the module, resolved by dcf77_probe() rather than
 * assumed -- which is why this deliberately returns the raw level and not a
 * cooked "pulse active" boolean. */
bool dcf77_raw_level(void);

/* What the driver currently believes about the two polarities. Both start
 * from the board file's CONFIG_DCF77_PON_ACTIVE_LOW / "OUT pulses high" and
 * can be corrected by dcf77_probe(). */
bool dcf77_pon_active_low(void);
bool dcf77_out_pulse_is_high(void);

/* D0-in-system (plan/phase17_clock_ui_and_dcf77.md): power the receiver up,
 * work out both polarities from what the pin actually does, then print every
 * pulse it sees -- width, spacing, and the bit that width would decode to --
 * for `secs` seconds, followed by a summary. Console output (cprintf, like
 * i2c_scan_bus()), not the kernel log: this is a table a human reads live
 * while moving an antenna around.
 *
 * Leaves the receiver powered down on return. Returns early on Ctrl-C
 * ([[standardized_interrupt_polling]]), like every other long-running
 * hardware loop in this tree. */
void dcf77_probe(unsigned secs);

/* Electrical-only pin diagnostic: register readback, a pull-up/no-pull/
 * pull-down test on OUT, an ADC voltage for each (GP26/27/28 are ADC
 * channels 0/1/2, so the pad voltage is measurable rather than inferable),
 * and a drive-and-read-back test on PON. Written after the first hardware
 * probe found OUT stuck low *against the internal pull-up*, which is not the
 * "nothing connected" case and needed a measurement to explain. Read-only on
 * OUT: it never drives that pin, since a powered module driving it low would
 * make that a deliberate driver-against-driver short. */
void dcf77_pin_report(void);

/* Sweep PON through driven-low, driven-high and floating, watching OUT both
 * digitally and with the ADC in each state. Needs no radio signal to be
 * useful: if OUT's voltage moves when PON moves, the module is powered and
 * PON reaches it, and the remaining question is reception rather than
 * wiring. In the floating state -- where neither pin is driven, so both are
 * safe to read -- it watches the PON pin too, which catches OUT and PON
 * being swapped without anyone rewiring anything. `secs_per_state` defaults
 * to 20 and is clamped to 5..120; each state also gets a 15 s warm-up,
 * deliberately longer than dcf77_probe()'s 5 s (receiver datasheets quote
 * warm-up anywhere from ~1 s to ~30 s). */
void dcf77_hunt(unsigned secs_per_state);

/* Compare the DCF-77 OUT pin against this persona's other unused pins, using
 * pulls only -- nothing is ever driven, and every pad is saved and restored.
 * Answers whether the load found on OUT arrived with the module or belongs
 * to the baseboard: GP27/GP28 were taken as free from the vendor firmware's
 * pin list, which records only the pins that firmware uses and is not proof
 * the board leaves the others unconnected. */
void dcf77_free_pin_survey(void);

/* Mirror the OUT line onto the LED matrix -- one row lights while OUT is
 * high -- so the signal can be watched directly instead of read about. An LED
 * wired to OUT itself is a poor idea on a micropower module: ~1 kohm of
 * output on-resistance against an 85 uA budget will not drive one, and trying
 * loads the output being observed. The matrix is built to be driven, and the
 * row is lit statically (no multiplexing), so the mirror adds no switching
 * noise of its own. `secs` is clamped to 5..900. */
void dcf77_mirror(unsigned secs);

/* Assert PON and sample OUT from that same instant, with no warm-up delay,
 * printing a 100 ms-per-character trace. Exists because dcf77_hunt() waits
 * out a 15 s warm-up *before* it starts watching, which skips the very thing
 * an RC8000 is documented to do on being enabled: drive OUT high for a few
 * seconds. A multi-second startup high proves the module is alive and
 * responds to PON without needing any radio signal at all. Needs PON on its
 * own GPIO; if it is strapped at the module there is nothing to toggle. */
void dcf77_power_on_capture(unsigned secs);

/* Drive the OUT pin and read it back -- the one question every passive test
 * cannot answer, since a stuck-low pad and a healthy pad watching a device
 * drive low look identical when you only ever look. The only function here
 * that drives OUT: 2 mA drive against the ~1 kohm measured passively is about
 * 3 mA of contention, held for 50 ms, inside both parts' limits -- but with
 * the module's OUT wire unplugged it is a cleaner experiment still. */
void dcf77_drive_test(void);

/* Watch OUT for minutes rather than seconds, printing every pulse as it
 * arrives and a progress line every 10 s -- the tool for an antenna that has
 * to be moved around while someone watches a number. `with_load` lights one
 * matrix row as a pure DC load (no multiplexing, no switching edges), which
 * is the M0 experiment from plan/phase17_clock_ui_and_dcf77.md section 3: the
 * Pico's buck regulator idles in PFM at light load and its ripple is a
 * documented killer for this module, and on a Pico 2 W the PWM-mode pin is
 * behind the CYW43 chip where this tree cannot reach it. `secs` is clamped to
 * 10..1800. */
void dcf77_listen(unsigned secs, bool with_load);

/* Feed the live pin into the portable frame decoder (drivers/dcf77_decode.c)
 * and report the decoded time, rather than the pulse widths every other
 * function here reports. `set_rtc` writes the kernel clock and the DS3231 on
 * success; without it nothing is changed and the run is purely diagnostic.
 * Nothing is ever written on failure. `secs` defaults to 300 and is raised to
 * whatever two frames plus the warm-up actually need -- a shorter timeout
 * cannot succeed, because a time is only believed once a second frame agrees
 * with the first. */
void dcf77_sync(unsigned secs, bool set_rtc);

#endif /* DRIVERS_DCF77_H */
