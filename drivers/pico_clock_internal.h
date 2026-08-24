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
 * Everything here is the CLIENT side of the clock task's wire (phase 17b,
 * plan/phase17b_clock_task_split.md). It used to be the `_hw_` layer, called
 * from inside the task itself; now the appliance loop runs in the caller's
 * task and each of these routes through `chan_call("clock", ...)`, falling
 * back to direct hardware access whenever the task is not alive. Nothing here
 * may be called from inside the task -- that would be the task calling
 * itself.
 */

#ifndef DRIVERS_PICO_CLOCK_INTERNAL_H
#define DRIVERS_PICO_CLOCK_INTERNAL_H

#include "drivers/pico_clock_green.h"
#include <stdbool.h>
#include <stdint.h>

/* One whole frame of the matrix: eight rows, ~8 ms, with the buttons polled
 * on the same cadence they always were. The heartbeat everything else hangs
 * off -- any loop that holds the display must call this or the panel goes
 * dark -- and, since phase 17b, the unit of work that keeps the row timing
 * inside the driver while the policy lives out here.
 *
 * A frame rather than a row because a row is ~1 ms and this is a channel
 * call: one call per row would put `chan_call()` on a ~1 kHz hot path, which
 * is exactly what phase 12's M4.5 refused to do -- and answered by moving the
 * whole appliance INTO the task instead. Eight rows per call is ~125 calls a
 * second, and every microsecond-sensitive loop stays on the far side of it.
 *
 * Returns how many key events happened during the frame, written to `ev`. */
typedef struct {
    clock_key_t   key;
    clock_press_t press;
    unsigned      held_ms;   /* how long it was down; 0 for a repeat */
} clock_event_t;

#define CLOCK_EVENTS_MAX 8u

unsigned clock_hw_scan_frame(clock_event_t *ev, unsigned max);

void clock_hw_clear(void);
void clock_hw_blank(void);                 /* buffer cleared AND OE closed */
void clock_hw_show_time(unsigned hour, unsigned minute, bool colon);
void clock_hw_show_temperature_c(int temp_c);
void clock_hw_draw_text(const char *s);    /* centred, clipped */

/* The same text with its left edge at column `col`, which may be off either
 * end of the panel -- one frame of a scroll. The scroll itself (the loop over
 * `col`, when to stop, what a keypress means) is policy and lives in
 * drivers/pico_clock_app.c; this draws the one frame of it that needs pixels.
 *
 * The two columns the text may occupy, for a caller working out where a
 * scroll starts and ends. Columns 0-1 are the indicator LEDs, which is why
 * text starts at 2 and not 0. */
#define CLOCK_TEXT_COL_FIRST 2
#define CLOCK_TEXT_COL_LAST  23

void clock_hw_draw_text_at(int col, const char *s);

/* The DCF-77 signal monitor's bar chart: one column per second, newest on the
 * right, height 0-7 from the score. */
void clock_hw_draw_bars(const uint8_t *scores, unsigned n);

void clock_hw_set_weekday(unsigned dow);
void clock_hw_indicator(clock_indicator_t ind, bool on);

/* Raw idle levels of the three button pins: bit 0 = SET, 1 = UP, 2 = DOWN,
 * 1 = released (they are active-low with pull-ups). Diagnostics only -- a 0
 * here with nothing pressed is a stuck button or a pull-up that is not doing
 * its job, and no amount of event watching says so as plainly. */
uint8_t clock_hw_pin_levels(void);

/* -1 = automatic (the LDR decides, as it always has); 1..7 = a fixed level,
 * where 7 is full brightness. Anything else is ignored. */
void clock_hw_set_brightness(int level);

/* The buzzer, held for `ms` while the row scan keeps running -- so a beep
 * does not stop the display. A no-op if the board has no buzzer pin. */
void clock_hw_beep(unsigned ms);

/* The appliance loop, in drivers/pico_clock_app.c. Runs in whatever task
 * evaluated `(clock)` -- the shell/Lisp task, which is exactly where chess's
 * own UI loop runs -- and reaches the panel only through the calls above.
 * Returns on Ctrl-C. */
void clock_app_run(void);

#endif /* DRIVERS_PICO_CLOCK_INTERNAL_H */
