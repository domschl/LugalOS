/*
 * LugalOS Hardware Driver: Waveshare Pico-Clock-Green LED matrix (L2,
 * plan/phase11_pico_clock_green.md).
 *
 * Two SM16106 column shift registers + one SM5166P row-address decoder
 * driving an 8-row x 24-column LED matrix, plus an LDR (GP26/ADC0) for
 * auto-brightness. Protocol reverse-engineered from the vendor firmware at
 * ~/gith/Pico-Clock-Green (Pico-Clock-Green.c, define.h, ziku.h) -- see the
 * plan doc's L0 section for the citations.
 */

#ifndef DRIVERS_PICO_CLOCK_GREEN_H
#define DRIVERS_PICO_CLOCK_GREEN_H

/* GPIO/ADC bring-up and a blanked display buffer. Call once at boot,
 * mirroring st7735_init()/tm1638_init() (kernel/main.c). */
void pico_clock_green_init(void);

/* Render into the internal 8x24 buffer. Neither of these touches the
 * physical LEDs directly -- pico_clock_green_scan_step() is what shifts
 * the buffer out; call one of these first, then keep calling scan_step().
 * hour is 0-23, minute is 0-59 (24h only, out-of-range values are clamped
 * rather than producing garbage on the matrix). temp_c may be negative. */
void pico_clock_green_show_time(unsigned hour, unsigned minute);
void pico_clock_green_show_temperature_c(int temp_c);
void pico_clock_green_clear(void);

/* Advances the row scan by one row (0..7): shifts that row's column data
 * out to the two SM16106s (SDI/CLK), latches it (LE), addresses the row on
 * the SM5166P (A0-A2), and un-blanks (OE) for either the full row period
 * (bright ambient) or a short software-PWM pulse (dim ambient, sampled
 * from the LDR once per full 8-row frame). Call in a tight loop at roughly
 * 1kHz for a flicker-free ~125Hz refresh (matches the vendor firmware's
 * own cadence) -- see plan/phase11_pico_clock_green.md L2 and L4. */
void pico_clock_green_scan_step(void);

/* L4 (plan/phase11_pico_clock_green.md): the blocking appliance loop behind
 * the `(clock)` Lisp primitive. Alternates the display between time (most
 * of the time) and temperature (briefly, only when the DS3231 is actually
 * detected), driving pico_clock_green_scan_step() continuously. Returns on
 * Ctrl-C, same console_interrupt_requested()/_clear() convention chess_ui.c
 * already uses ([[standardized_interrupt_polling]]) -- not a new mechanism.
 * Allocates nothing on the heap, so there is nothing to free on return
 * ([[heap_stateless_user_programs]] is satisfied trivially, not by effort). */
void pico_clock_green_run(void);

#endif /* DRIVERS_PICO_CLOCK_GREEN_H */
