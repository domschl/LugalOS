/*
 * The Pico-Clock-Green menu, as a pure state machine (C3,
 * plan/phase17_clock_ui_and_dcf77.md).
 *
 * Nothing in this file or its implementation touches hardware, reads a clock,
 * or allocates. A key event goes in, the state changes, and a *description*
 * of what should be on the panel comes out; the caller (drivers/
 * pico_clock_app.c) is what turns that description into pixels and what
 * carries out the actions it asks for. The same shape as the DCF-77 split,
 * and for the same reason: the fiddly part of a menu is the state machine,
 * and it should not be debugged by flashing a board and pressing buttons.
 * `clockuiselftest` drives every path below from synthetic key sequences, on
 * every target, in milliseconds -- see kernel/shell.c.
 *
 * It is built on QEMU targets too, which have no display and no buttons.
 * That is not waste: it is what makes the test run there.
 *
 * Navigation, which is the whole contract:
 *
 *   idle          UP short    -> temperature for 2 s
 *                 DOWN short  -> DD.MM for 2 s, then YYYY for 1.5 s
 *                 SET short   -> the menu
 *   menu          UP/DOWN     -> previous/next item (its full name scrolls
 *                                once on arrival, then the short label stays)
 *                 SET short   -> enter the item
 *                 SET long    -> back to idle
 *   in an item    UP/DOWN     -> adjust
 *                 SET short   -> confirm (or advance, for a multi-field item
 *                                like SET TIME) -- and on the final confirm,
 *                                straight back to the CLOCK
 *                 SET long    -> abandon, back to the menu
 *   anywhere      10 s idle   -> back to the clock, changing nothing
 *
 * SET short is "forward" everywhere and SET long is "back one level"
 * everywhere. Confirming is the end of the forward path, so it ends the whole
 * errand rather than dropping you back onto the item you just finished --
 * which was the one screen in the UI where completing something left the same
 * button pointed at doing it again (user, 2026-08-23).
 *
 * The two exits are not redundant: confirm means "done, put my clock back",
 * long-press means "not that one, let me look at another", and each is worth
 * a button. Changing two settings in a row therefore costs a second trip
 * through the menu, which for a clock is the right way round.
 */

#ifndef DRIVERS_PICO_CLOCK_UI_H
#define DRIVERS_PICO_CLOCK_UI_H

#include "kernel/time.h"
#include "drivers/pico_clock_green.h"
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------ items ---- */

/*
 * Menu order is this enum's order, and it is chosen around one fact: on a
 * finished clock there is no console, so anything not reachable from these
 * three buttons does not exist.
 *
 * DCF SIGNAL comes first because it is the screen you need with both hands on
 * an antenna, and burying it five presses deep would have made the one tool
 * for aiming the receiver the hardest thing in the menu to reach. TEMPERATURE
 * and DATE come last, not out of neglect but because they are already a
 * single press from the idle screen -- they are in the menu for completeness,
 * not as the way anyone reaches them.
 */
typedef enum {
    UI_ITEM_SIG = 0,    /* SIG   DCF SIGNAL   */
    UI_ITEM_SYNC,       /* SYNC  DCF SYNC NOW */
    UI_ITEM_LAST,       /* LAST  LAST SYNC    */
    UI_ITEM_AUTO,       /* AUTO  NIGHTLY SYNC */
    UI_ITEM_BRIGHT,     /* BRT   BRIGHTNESS   */
    UI_ITEM_TIMESET,    /* TSET  SET TIME     */
    UI_ITEM_OFFSET,     /* OFFS  TEMP OFFSET  */
    UI_ITEM_HOUR12,     /* 24H   HOUR FORMAT  */
    UI_ITEM_BEEP,       /* BEEP  BEEP ON KEYS */
    UI_ITEM_TEMP,       /* TEMP  TEMPERATURE  -- also UP from the clock */
    UI_ITEM_DATE,       /* DATE  DATE         -- also DOWN from the clock */
    UI_ITEM_EXIT,       /* EXIT               */
    UI_ITEM_COUNT
    /* SYNC / LAST / SIG / AUTO join this list in D4, once there is a sync
     * controller for them to drive. An item that is in the menu and does
     * nothing is worse than one that is not there yet. */
} ui_item_t;

const char *clock_ui_item_short(ui_item_t item);
const char *clock_ui_item_full(ui_item_t item);

/* --------------------------------------------------------- settings ---- */

/*
 * Held in RAM and lost at reset. There is nowhere to persist them on this
 * persona -- no SD card, and the DS3231's handful of spare registers are not
 * something this phase claims (§5 of the plan). Every one of them has a
 * board-file default that is a sensible permanent answer, so losing the
 * runtime value costs a preference, never a correct clock.
 */
typedef struct {
    int8_t brightness;     /* -1 = automatic (LDR), else 1..7 fixed */
    int8_t temp_offset;    /* -9..+9 degrees C, display path only */
    bool   hour12;         /* 12-hour clock, driving the AM/PM indicators */
    bool   click;          /* a click on every accepted keypress */
    bool   confirm_beep;   /* a beep when a value is committed */
    bool   auto_sync;      /* the nightly DCF-77 sync */
} ui_settings_t;

/* ------------------------------------------------------------ state ---- */

typedef enum {
    UI_MODE_IDLE = 0,
    UI_MODE_SHORTCUT_TEMP,
    UI_MODE_SHORTCUT_DATE,
    UI_MODE_SHORTCUT_YEAR,
    UI_MODE_MENU,
    UI_MODE_ITEM
} ui_mode_t;

/* SET TIME walks these in order. Year is two digits, within 2000. */
typedef enum {
    UI_FIELD_HOUR = 0, UI_FIELD_MIN, UI_FIELD_DAY,
    UI_FIELD_MONTH, UI_FIELD_YEAR, UI_FIELD_COUNT
} ui_field_t;

typedef struct {
    ui_mode_t     mode;
    ui_item_t     item;
    ui_field_t    field;
    ui_settings_t set;

    /* The value being edited, so an abandoned edit changes nothing. */
    uint8_t  edit[UI_FIELD_COUNT];
    int8_t   edit_scalar;      /* brightness / offset while inside that item */

    bool     sync_asked;       /* set on entry to DCF SYNC NOW; the caller
                                * clears it once it has made the request */
    bool     pending_scroll;   /* a full name is waiting to be scrolled */
    uint64_t deadline_ms;      /* shortcut screens; 0 = none */
    uint64_t last_input_ms;    /* for the inactivity timeout */
} ui_state_t;

/* What the caller must supply before asking for a screen: the readings the
 * state machine is not allowed to go and get for itself. */
typedef enum {
    UI_DCF_ABSENT = 0,   /* no receiver in this build */
    UI_DCF_IDLE,         /* listening; nothing will be written */
    UI_DCF_SYNCING,
    UI_DCF_DONE,
    UI_DCF_FAILED
} ui_dcf_t;

typedef struct {
    rtc_time_t local;      /* the current local time */
    bool       have_temp;
    int        temp_c;     /* RAW -- the offset is applied by the machine */

    /* DCF-77, for the SIG/SYNC/LAST screens. Supplied like everything else
     * rather than fetched, so the machine stays testable with no radio in the
     * room -- and typed as the UI's own enum rather than the driver's, so the
     * pure layer keeps building on targets that have no receiver code at
     * all. */
    const uint8_t *quality;    /* newest last; NULL if there is no receiver */
    unsigned       quality_n;
    ui_dcf_t       dcf;
    uint32_t       dcf_age_s;   /* since the last successful sync; ~0 = never */
    uint32_t       dcf_left_s;  /* while syncing: time before it gives up */
} ui_inputs_t;

/* ----------------------------------------------------------- screen ---- */

typedef enum {
    UI_SCR_TIME = 0,   /* value = hour*100 + minute */
    UI_SCR_TEMP,       /* value = degrees C, offset already applied */
    UI_SCR_TEXT,       /* text, static */
    UI_SCR_BARS,       /* one column per second of DCF-77 signal quality */
    UI_SCR_SCROLL,     /* text, scrolled once; then call clock_ui_scroll_done() */
    UI_SCR_BLANK
} ui_screen_kind_t;

typedef struct {
    ui_screen_kind_t kind;
    char  text[16];
    int   value;
    bool  colon;       /* TIME: draw the colon this frame */
    bool  am, pm;      /* TIME: which indicator to light, 12-hour mode only */
    bool  weekday;     /* light the weekday LED for the current date */
    const uint8_t *bars;   /* UI_SCR_BARS: per-second scores, newest last */
    unsigned       bars_n;
} ui_screen_t;

/* --------------------------------------------------------- actions ----- */
/* A bitmask, because committing a value both changes something and deserves
 * a beep. The caller performs these; the machine never does. */
#define UI_ACT_CLICK        (1u << 0)   /* an accepted keypress */
#define UI_ACT_CONFIRM      (1u << 1)   /* a value was committed */
#define UI_ACT_COMMIT_TIME  (1u << 2)   /* write st->edit[] to the clock */
#define UI_ACT_APPLY_BRIGHT (1u << 3)   /* st->set.brightness changed */
#define UI_ACT_SYNC_NOW     (1u << 4)   /* ask the receiver to set the clock */
#define UI_ACT_SET_AUTO     (1u << 5)   /* st->set.auto_sync changed */

void     clock_ui_reset(ui_state_t *st, const ui_settings_t *defaults, uint64_t now_ms);
unsigned clock_ui_key(ui_state_t *st, const ui_inputs_t *in,
                      clock_key_t key, clock_press_t press, uint64_t now_ms);
unsigned clock_ui_tick(ui_state_t *st, uint64_t now_ms);
void     clock_ui_screen(const ui_state_t *st, const ui_inputs_t *in,
                         uint64_t now_ms, ui_screen_t *out);
/* Called once the caller has finished scrolling a full name. */
void     clock_ui_scroll_done(ui_state_t *st, uint64_t now_ms);

/* The whole menu driven from synthetic key sequences. Returns the number of
 * failures (0 = all passed). No display, no buttons, no clock. */
int clock_ui_selftest(void);

#endif /* DRIVERS_PICO_CLOCK_UI_H */
