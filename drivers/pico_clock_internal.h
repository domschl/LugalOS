/*
 * The seam between the Pico-Clock-Green's hardware and its user interface
 * (C3, plan/phase17_clock_ui_and_dcf77.md).
 *
 * drivers/pico_clock_green_rp2350.c owns the pins, the row scan, the buttons
 * and the frame buffer. drivers/pico_clock_app.c owns what is on the screen
 * and what the buttons mean. This header is everything the second needs from
 * the first, and nothing else -- the public
 * drivers/include/drivers/pico_clock_green.h keeps only what the rest of the
 * system uses.
 *
 * Everything here is the `_hw_` layer: it runs INSIDE the clock task and must
 * never be reached through the channel, because that would be the task
 * calling itself. The public facades in the driver are the ones that route.
 */

#ifndef DRIVERS_PICO_CLOCK_INTERNAL_H
#define DRIVERS_PICO_CLOCK_INTERNAL_H

#include "drivers/pico_clock_green.h"
#include <stdbool.h>
#include <stdint.h>

/* One row of the matrix, ~1 ms of it. The heartbeat everything else hangs
 * off: any loop that holds the display must call this or the panel goes
 * dark. */
void clock_hw_scan_step(void);

void clock_hw_clear(void);
void clock_hw_blank(void);                 /* buffer cleared AND OE closed */
void clock_hw_show_time(unsigned hour, unsigned minute, bool colon);
void clock_hw_show_temperature_c(int temp_c);
void clock_hw_draw_text(const char *s);    /* centred, clipped */
bool clock_hw_scroll_text(const char *s);  /* once; false = Ctrl-C */

/* The DCF-77 signal monitor's bar chart: one column per second, newest on the
 * right, height 0-7 from the score. */
void clock_hw_draw_bars(const uint8_t *scores, unsigned n);

void clock_hw_set_weekday(unsigned dow);
void clock_hw_indicator(clock_indicator_t ind, bool on);

void clock_hw_buttons_poll(uint64_t now_ms);
bool clock_hw_key_pop(clock_key_t *key, clock_press_t *press);

/* -1 = automatic (the LDR decides, as it always has); 1..7 = a fixed level,
 * where 7 is full brightness. Anything else is ignored. */
void clock_hw_set_brightness(int level);

/* The buzzer, held for `ms` while the row scan keeps running -- so a beep
 * does not stop the display. A no-op if the board has no buzzer pin. */
void clock_hw_beep(unsigned ms);

/* The appliance loop, in drivers/pico_clock_app.c. Called by the clock task
 * in place of the old in-driver loop. Returns on Ctrl-C. */
void clock_app_run(void);

#endif /* DRIVERS_PICO_CLOCK_INTERNAL_H */
