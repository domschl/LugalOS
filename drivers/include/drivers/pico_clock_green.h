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

#include <stdbool.h>
#include <stdint.h>

/* GPIO/ADC bring-up, the three baseboard buttons, and a blanked display
 * buffer. Call once at boot, mirroring st7735_init()/tm1638_init()
 * (kernel/main.c). */
void pico_clock_green_init(void);

/* ---------------------------------------------------------- buttons ---- */
/*
 * The baseboard's three buttons (phase17 C1). Polled from inside the row-scan
 * loop rather than by a task of their own -- see the driver -- and delivered
 * as events rather than levels, because every consumer wants "SET was pressed
 * briefly", not "SET is down right now".
 */
typedef enum {
    CLOCK_KEY_SET = 0,
    CLOCK_KEY_UP,
    CLOCK_KEY_DOWN,
    CLOCK_KEY_COUNT
} clock_key_t;

typedef enum {
    CLOCK_PRESS_SHORT = 0,   /* released before 400 ms */
    CLOCK_PRESS_LONG,        /* released after 400 ms; one event, on release */
    CLOCK_PRESS_REPEAT       /* UP/DOWN held: 5/s after 600 ms */
} clock_press_t;

/* Key events are returned by the frame op (`clock_hw_scan_frame()`, the
 * appliance's own seam in drivers/pico_clock_internal.h) rather than polled
 * from out here: they exist only while something is scanning, and whatever is
 * scanning is the thing that should be given them. Phase 17b removed the
 * standalone `pico_clock_green_pop_key()`, which had no caller outside the
 * driver and no way to be correct for one. */

/* ------------------------------------------------- indicator LEDs ------ */
/*
 * The ten status LEDs down the left edge and the seven weekday LEDs along the
 * top, which are part of the same frame buffer as the digits but survive a
 * redraw of them. Vendor labels are kept except where this project has a
 * better use for the lamp than the label describes: "MoveOn" means nothing
 * here and carries the DCF-77 status, and "Alarm On" carries the network
 * status, since there is no alarm feature to claim it and a headless
 * appliance badly wants a way to say whether it is on the network.
 */
typedef enum {
    CLOCK_IND_DCF = 0,       /* vendor "MoveOn"   -- our DCF-77 status LED */
    CLOCK_IND_WIFI,          /* vendor "Alarm On" -- our network status LED */
    CLOCK_IND_COUNTDOWN,
    CLOCK_IND_F,
    CLOCK_IND_C,
    CLOCK_IND_AM,
    CLOCK_IND_PM,
    CLOCK_IND_COUNTUP,
    CLOCK_IND_CHIME,
    CLOCK_IND_AUTOLIGHT,
    CLOCK_IND_COUNT
} clock_indicator_t;

/* 1 = Monday .. 7 = Sunday; 0 lights none. Computed from the date by the
 * caller (kernel/time.h's time_weekday()), never read from the DS3231's own
 * weekday register -- that one is a free-running counter the chip never
 * checks against the date. */
void pico_clock_green_set_weekday(unsigned dow);
void pico_clock_green_indicator(clock_indicator_t ind, bool on);

/* --------------------------------------------------- diagnostics ------- */

/* Print every button event for `secs`, with the press duration, plus the raw
 * pin levels -- so a miswired or inverted button is visible immediately
 * rather than as "the menu does nothing". Drives the row scan meanwhile so
 * the display stays lit. Ctrl-C stops. Behind `(clock-keys [secs])`. */
void pico_clock_green_keys(unsigned secs);

/* Light every weekday LED in turn, then every indicator LED in turn, then all
 * of them together -- the whole of C1's LED surface in one pass, named on the
 * console as it goes. Behind `(clock-leds)`. */
void pico_clock_green_led_walk(void);

/* Show `str` for `secs`: centred if it fits the 24-column text area, scrolled
 * right-to-left on repeat if it does not. The C2 font check -- letters are
 * either legible on the real panel or they are not, and that is not something
 * a build can tell you. Behind `(clock-text "..." [secs])`. */
void pico_clock_green_show_text(const char *str, unsigned secs);

/* The DCF-77 signal monitor on the panel (D3): 24 columns, one per second,
 * newest on the right, height from that second's quality score, with the DCF
 * indicator LED following the pulse. The instrument for the interference
 * measurement in plan/phase17_clock_ui_and_dcf77.md section 3 -- it runs with
 * the display ON by definition, because it *is* the display. Behind
 * `(dcf-monitor [secs] [dark])`.
 *
 * `dark` blanks the panel entirely -- zeros latched, OE closed, and the row
 * scan not run at all -- which is the controlled half of the D5 interference
 * measurement: two runs of the same code, the same decoder and the same
 * scoring, differing only in whether the display is switching. */
void pico_clock_green_dcf_monitor(unsigned secs, bool dark);

/* Boot progress on a board whose only outputs are the panel and the buzzer:
 * ONE click per call, and `stage` LEDs lit along the top row. The marks are
 * consecutive, so the number of clicks heard is the number of steps reached --
 * quicker to count than a group of clicks per stage, with nothing to keep
 * track of between groups. The matrix
 * half **latches** -- shifted once, latched, OE held open, no scanning -- so
 * if the boot hangs, the last stage reached stays lit and the panel says
 * where it stopped. A blinking heartbeat cannot do that; it just stops, and
 * every failure looks identical.
 *
 * Safe before pico_clock_green_init(): the earliest stages run before there
 * is a display driver at all, and fall back to the clicks alone. Compiled out
 * entirely unless CONFIG_CLOCK_BOOT_BEACON. */
#if CONFIG_CLOCK_BOOT_BEACON
void pico_clock_green_boot_mark(unsigned stage);
#define CLOCK_BOOT_MARK(n) pico_clock_green_boot_mark(n)
#else
#define CLOCK_BOOT_MARK(n) ((void)0)
#endif

/* Raw 12-bit LDR reading (0-4095), the same single-shot conversion the
 * (task-internal) scan loop samples once per frame for auto-brightness. A
 * diagnostic primitive, not something the display loop needs -- exposed
 * so LDR polarity and the auto-brightness ladder's thresholds
 * (`AUTO_DARKER_AT[]`) can be checked against real ambient light on real
 * hardware instead of assumed correct from the register-level port
 * (plan/phase11_pico_clock_green.md L2, and phase 17 section 10). */
uint16_t pico_clock_green_read_light(void);

/* L4 (plan/phase11_pico_clock_green.md): the blocking appliance loop behind
 * the `(clock)` Lisp primitive. Alternates the display between time (most
 * of the time) and temperature (briefly, only when the DS3231 is actually
 * detected), driving the ~1kHz row scan continuously. Returns on Ctrl-C,
 * same console_interrupt_requested()/_clear() convention chess_ui.c already
 * uses ([[standardized_interrupt_polling]]) -- not a new mechanism.
 * Allocates nothing on the heap, so there is nothing to free on return
 * ([[heap_stateless_user_programs]] is satisfied trivially, not by effort).
 *
 * Phase 17b, plan/phase17b_clock_task_split.md: this loop runs in the
 * CALLER's task -- the shell/Lisp task, exactly where chess's UI loop runs --
 * and reaches the panel through one chan_call() per ~8 ms frame. Under M4.5
 * it was the other way round: the whole loop was served inside the clock
 * task as a single long call, which is why `clockstats` used to read
 * `calls=1` for an entire session and now advances ~125 times a second.
 *
 * What that bought is the reason for the phase: the clock task is now a thin
 * hardware server like every other RP2350 driver task, small enough to run
 * confined in U-mode, and `clockisotest` can put its domain on trial the way
 * `st7735isotest` does. */
void pico_clock_green_run(void);

/* Light one row of the matrix as a pure DC load: shifted out once, latched,
 * OE held open, no multiplexing and no PWM -- current without switching
 * noise. Used as the quiet-sync indicator (plan/phase17_clock_ui_and_dcf77.md
 * section 3, M5) and by that phase's M0 experiment, which needs to raise the
 * 3V3 load on a Pico 2 W -- where the regulator's PWM-mode pin sits behind
 * the wireless chip -- without adding a single switching edge for the
 * receiver to hear. Direct hardware access, like pico_clock_green_init():
 * meant for use while the appliance loop is NOT running. */
void pico_clock_green_static_load(bool on);

/* Must run after sched_init(); pico_clock_green_init() itself stays a
 * direct-hardware call (it runs before a task table exists). Returns the
 * task's pid, or -1. */
int pico_clock_green_task_start(void);

// M4.5 verify: how many chan_call()s the shared "clock" task has served
// since boot -- see drivers/spisd_rp2350.c's g_blk_calls comment.
uint32_t pico_clock_green_task_call_count(void);

#endif /* DRIVERS_PICO_CLOCK_GREEN_H */
