/*
 * The Pico-Clock-Green appliance loop (C3, plan/phase17_clock_ui_and_
 * dcf77.md): buttons in, pixels out, and the menu state machine in between.
 *
 * This file is the only place the three halves meet. It owns no policy --
 * drivers/pico_clock_ui.c decides what should be on the panel -- and it owns
 * no registers: drivers/pico_clock_green_rp2350.c does. What it owns is the
 * loop, the ~1 ms cadence, and the I2C reads the state machine is not allowed
 * to perform for itself.
 *
 * The 542-line driver was already the wrong place for a menu, and phase 11's
 * comment saying so is why this file exists rather than another 400 lines
 * being added there.
 */

/* First, necessarily: the CONFIG_* guards below are defined there. */
#include "lugalos_config.h"

#include "drivers/pico_clock_internal.h"
#include "drivers/pico_clock_ui.h"
#include "pico_clock_font.h"
#include "drivers/i2c_rtc.h"
#if CONFIG_ENABLE_DCF77
#include "drivers/dcf77_service.h"
#endif
#include "net/netif.h"
#include "net/ip.h"
#include "kernel/identity.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/timezone.h"

#ifndef CONFIG_CLOCK_TEMP_OFFSET_C
#define CONFIG_CLOCK_TEMP_OFFSET_C (-2)
#endif
#ifndef CONFIG_CLOCK_COLON_BLINK
#define CONFIG_CLOCK_COLON_BLINK 0
#endif

/* One turn of the loop is one frame of scan: eight rows, ~8 ms, taken in a
 * single call into the clock task (phase 17b). Before the split this loop ran
 * a row at a time inside that task; the cadence a person can see is the same,
 * and the thing that changed is which side of the channel it runs on. */

/* The scroll animation's step, and **abortable by a keypress**. Both are
 * corrections from phase 17's first hardware run: "TEMPERATURE" is 55
 * columns, which at the vendor's 60 ms/column is 4.6 seconds, and the first
 * version did not sample the buttons at all while it ran -- so backing out of
 * the TEMPERATURE item looked like SET-long doing nothing. The first event
 * ends the scroll and is *kept*, so the press still does what it was going to
 * do. Pressing a button to skip an animation is what everyone expects. */
#define SCROLL_STEP_MS   45u

/* How often to go to the RTC over I2C -- deliberately *not* every scan step.
 * Found on real hardware in phase 11, not predicted: a 100 kHz transaction
 * blocks for the best part of a millisecond, which against a 1 ms row period
 * is a visible stutter on whichever row it lands on. Once a second means one
 * row in a thousand pays it.
 *
 * While a menu is open it drops to 250 ms, because there the reading is not
 * the point -- responsiveness is -- and the menu screens mostly do not read
 * the clock at all. */
#define READ_INTERVAL_IDLE_MS 1000u
#define READ_INTERVAL_MENU_MS  250u

/* Short. The buzzer is a self-oscillating type with no volume control, so
 * duration is the only lever there is, and 120 ms was reported as simply loud
 * (user, 2026-08-23). 40 ms still reads as a deliberate confirmation rather
 * than a glitch, and the BEEP menu item turns it off entirely. */
#define CLICK_MS   10u
#define CONFIRM_MS 40u

/* Key events, as they come back from each frame op.
 *
 * A queue rather than a straight hand-off because the frame op is the only
 * thing that produces events and it is called from three places in this file
 * (the main loop, the scroll, anything else that holds the display): whoever
 * pumps a frame collects whatever came with it, and the main loop drains the
 * queue when it gets there. Sized for two frames' worth of the driver's own
 * eight-entry ring, which no human hand can outrun at 125 frames a second. */
#define APP_EV_MAX (2u * CLOCK_EVENTS_MAX)
static clock_event_t g_ev[APP_EV_MAX];
static unsigned      g_ev_n;

/* One frame of display, and whatever the buttons did during it. */
static void pump_frame(void) {
    clock_event_t ev[CLOCK_EVENTS_MAX];
    unsigned n = clock_hw_scan_frame(ev, CLOCK_EVENTS_MAX);
    for (unsigned i = 0; i < n && g_ev_n < APP_EV_MAX; i++) g_ev[g_ev_n++] = ev[i];
}

static bool pop_event(clock_event_t *out) {
    if (g_ev_n == 0) return false;
    *out = g_ev[0];
    for (unsigned i = 1; i < g_ev_n; i++) g_ev[i - 1] = g_ev[i];
    g_ev_n--;
    return true;
}

/* Scroll `s` right-to-left once. False means Ctrl-C asked to stop; a keypress
 * also ends it, but returns true with the event still queued (see
 * SCROLL_STEP_MS above). */
static bool scroll_once(const char *s) {
    unsigned w = clock_font_text_width(s);
    for (int dest = CLOCK_TEXT_COL_LAST + 1;
         dest > CLOCK_TEXT_COL_FIRST - (int)w; dest--) {
        clock_hw_draw_text_at(dest, s);
        uint64_t until = time_get_ms() + SCROLL_STEP_MS;
        while (time_get_ms() < until) {
            pump_frame();
            if (g_ev_n) return true;   /* skipped; the event is kept */
            if (console_interrupt_requested()) { console_interrupt_clear(); return false; }
        }
    }
    return true;
}

/* Blink an editable value by simply not drawing it on the dark half of the
 * cycle: the state machine says "this is being edited" and the rate lives
 * here, where the frame timing already is. */
static void render(const ui_screen_t *scr, bool editing) {
    switch (scr->kind) {
    case UI_SCR_TIME:
        clock_hw_show_time((unsigned)(scr->value / 100), (unsigned)(scr->value % 100),
                           scr->colon);
        break;
    case UI_SCR_TEMP:
        clock_hw_show_temperature_c(scr->value);
        break;
    case UI_SCR_TEXT:
        clock_hw_clear();
        if (!editing || scr->colon) clock_hw_draw_text(scr->text);
        break;
    case UI_SCR_BARS:
        clock_hw_clear();
        clock_hw_draw_bars(scr->bars, scr->bars_n);
        break;
    case UI_SCR_SCROLL:
    case UI_SCR_BLANK:
    default:
        clock_hw_clear();
        break;
    }
}

/* Write a completed SET TIME back. The edit is in LOCAL time, because that is
 * what a person typed; the kernel clock and the DS3231 both hold UTC, so the
 * conversion happens here and exactly once (see kernel/timezone.h). Seconds
 * are zeroed, which is the only honest answer -- nobody sets a clock to a
 * particular second by pressing a button five times. */
static void commit_time(const ui_state_t *st) {
    rtc_time_t local = {
        .year  = (uint16_t)(2000u + st->edit[UI_FIELD_YEAR]),
        .month = st->edit[UI_FIELD_MONTH],
        .day   = st->edit[UI_FIELD_DAY],
        .hour  = st->edit[UI_FIELD_HOUR],
        .min   = st->edit[UI_FIELD_MIN],
        .sec   = 0,
        .ms    = 0,
    };
    rtc_time_t utc;
    tz_local_to_utc(&local, &utc);
    time_set_utc(&utc);
    i2c_rtc_write_time(&utc);
}

void clock_app_run(void) {
    /* Matches chess_run()'s own precedent (chess_ui.c): a stale interrupt
     * latched by an unrelated earlier Ctrl-C must not abort this run before
     * it does anything. */
    console_interrupt_clear();

    const ui_settings_t defaults = {
        .brightness   = -1,                             /* the LDR decides */
        .temp_offset  = CONFIG_CLOCK_TEMP_OFFSET_C,
        .hour12       = false,
        .click        = false,                          /* see the plan: off */
        .confirm_beep = true,
#if CONFIG_ENABLE_DCF77
        .auto_sync    = (CONFIG_DCF77_AUTO_ENABLED != 0),
#else
        .auto_sync    = false,
#endif
    };

    ui_state_t st;
    ui_inputs_t in;
    ui_screen_t scr;
    uint64_t now = time_get_ms();

    clock_ui_reset(&st, &defaults, now);
    clock_hw_set_brightness(defaults.brightness);

    /* Seeded rather than left at zero: the first frame is drawn before the
     * first I2C read completes, and a clock that flashes 00:00 on entry looks
     * broken even for the 10 ms it lasts. */
    in.local = (rtc_time_t){ 2026, 1, 1, 0, 0, 0, 0 };
    in.have_temp = false;
    in.temp_c = 0;
    in.quality = 0;
    in.quality_n = 0;
    in.dcf = UI_DCF_ABSENT;
    in.dcf_age_s = 0xFFFFFFFFu;
    in.dcf_left_s = 0;
    {
        rtc_time_t utc;
        if (i2c_rtc_read_time(&utc)) tz_utc_to_local(&utc, &in.local);
    }

    uint64_t next_read = 0;
    ui_mode_t last_mode = (ui_mode_t)0xFF;
    bool need_render = true;

    /* Outside the DCF guard on purpose: the network lamp is not a DCF
     * feature, and a persona with a radio but no receiver still wants it.
     *
     * Whether credentials exist is read once, here, and not in the loop:
     * node_wlan_ssid() goes through idstore_read(), which puts a 4 KB record
     * buffer on the stack and re-reads the sector -- fine occasionally,
     * absurd twenty times a second. It cannot change underneath us either,
     * since writing the identity record reboots this board. */
    char net_ssid[NODE_WLAN_SSID_MAX + 1];
    bool net_have_creds = node_wlan_ssid(net_ssid, sizeof(net_ssid));
    uint64_t next_net_led_ms = 0;
    bool net_led_on = false;

#if CONFIG_ENABLE_DCF77
    /* The receiver is listened to continuously, not only during a sync: the
     * signal screen then has live data the moment it is opened, and a sync
     * asked for after good reception completes at once (see
     * drivers/dcf77_service.h). Nothing is ever written without a request. */
    dcf_status_t dcf;
    dcf77_service_status(&dcf);
    uint64_t next_led_ms = 0;
    bool led_on = false;
    bool was_in_sync_item = false;
#endif

    for (;;) {
        now = time_get_ms();

#if CONFIG_ENABLE_DCF77
        /* Every tick, before anything else: the decoder wants an evenly
         * spaced sample far more than it wants a recent one, and this is the
         * only part of the loop that runs unconditionally. */
        dcf77_service_feed(now);
#endif

        {
            clock_event_t e;
            while (pop_event(&e)) {
                unsigned act = clock_ui_key(&st, &in, e.key, e.press, now);
                if (act & UI_ACT_COMMIT_TIME)  commit_time(&st);
#if CONFIG_ENABLE_DCF77
                if (act & UI_ACT_SET_AUTO) dcf77_service_set_auto(st.set.auto_sync);
#endif
                if (act & UI_ACT_APPLY_BRIGHT) {
                    /* While inside BRIGHTNESS the value being previewed is
                     * the edit copy, not the committed one -- the point is to
                     * see it before choosing it. */
                    clock_hw_set_brightness(st.mode == UI_MODE_ITEM
                                              ? st.edit_scalar : st.set.brightness);
                }
                if ((act & UI_ACT_CONFIRM) && st.set.confirm_beep) clock_hw_beep(CONFIRM_MS);
                else if ((act & UI_ACT_CLICK) && st.set.click)     clock_hw_beep(CLICK_MS);
                need_render = true;
            }
        }

#if CONFIG_ENABLE_DCF77
        /* Entering DCF SYNC NOW is the request; the machine only records that
         * it was asked, because asking is an action and the machine performs
         * none. Leaving the item cancels a sync still in progress -- walking
         * away from that screen is the "any button aborts" the plan asks for,
         * and leaving one running invisibly would be worse than not offering
         * it at all. */
        if (st.sync_asked) {
            st.sync_asked = false;
            dcf77_service_request_sync(0);
            need_render = true;
        }
        {
            bool in_sync_item = (st.mode == UI_MODE_ITEM && st.item == UI_ITEM_SYNC);
            if (was_in_sync_item && !in_sync_item) dcf77_service_cancel();
            was_in_sync_item = in_sync_item;
        }
#endif

        clock_ui_tick(&st, now);
        if (st.mode != last_mode) { last_mode = st.mode; need_render = true; }

        if (need_render || now >= next_read) {
#if CONFIG_ENABLE_DCF77
            /* Before the screen is decided, not after it is drawn. With this
             * at the bottom of the block the bars lagged a frame and the
             * first frame of the signal screen read "NO SIG", because the
             * scores it was asking about had not been fetched yet. */
            dcf77_service_status(&dcf);
            in.quality = dcf.decoder.quality;
            in.quality_n = dcf.decoder.quality_count;
            switch (dcf.state) {
            case DCF_SYNCING: in.dcf = UI_DCF_SYNCING; break;
            case DCF_DONE:    in.dcf = UI_DCF_DONE;    break;
            case DCF_FAILED:  in.dcf = UI_DCF_FAILED;  break;
            default:          in.dcf = UI_DCF_IDLE;    break;
            }
            in.dcf_age_s  = dcf77_service_age_s();
            in.dcf_left_s = dcf.timeout_left_s;
#endif
            /* Only go to the bus for what the current screen actually needs.
             * The temperature read is a second transaction and there is no
             * reason to pay for it while the panel is showing the time. */
            clock_ui_screen(&st, &in, now, &scr);
            if (scr.kind == UI_SCR_TIME || scr.kind == UI_SCR_TEXT) {
                rtc_time_t utc;
                if (i2c_rtc_read_time(&utc)) tz_utc_to_local(&utc, &in.local);
            }
            if (scr.kind == UI_SCR_TEMP || st.mode == UI_MODE_SHORTCUT_TEMP ||
                (st.mode == UI_MODE_ITEM && st.item == UI_ITEM_TEMP)) {
                int t;
                in.have_temp = i2c_rtc_read_temperature_c(&t);
                if (in.have_temp) in.temp_c = t;
            }

            clock_ui_screen(&st, &in, now, &scr);

            if (scr.kind == UI_SCR_SCROLL) {
                /* Blocking, and correctly so: a label scrolls once and the
                 * loop has nothing else to do meanwhile. It still runs the
                 * scan and still honours Ctrl-C. */
                if (!scroll_once(scr.text)) break;
                clock_ui_scroll_done(&st, time_get_ms());
                need_render = true;
                continue;
            }

            bool editing = (st.mode == UI_MODE_ITEM && st.item == UI_ITEM_TIMESET);
            render(&scr, editing);

            clock_hw_indicator(CLOCK_IND_C, scr.kind == UI_SCR_TEMP);
            clock_hw_indicator(CLOCK_IND_AM, scr.am);
            clock_hw_indicator(CLOCK_IND_PM, scr.pm);
            clock_hw_set_weekday(scr.weekday ? time_weekday(&in.local) : 0);

            need_render = false;
            /* A blinking value needs looking at several times a second; the
             * clock face is content with one look a second unless the colon
             * is blinking too. */
            uint32_t period = (st.mode == UI_MODE_IDLE)
                                ? (CONFIG_CLOCK_COLON_BLINK ? 100u : READ_INTERVAL_IDLE_MS)
                                : READ_INTERVAL_MENU_MS;
            if (editing) period = 100u;
            next_read = now + period;
        }

        /* The network indicator, deliberately speaking the same language as
         * the DCF lamp below it -- blink while trying, solid when it is
         * actually usable -- so the two read as one status column rather than
         * two conventions.
         *
         *   solid : associated *and* addressed, i.e. someone can reach this
         *           board. Carrier alone would be a half-truth; an associated
         *           board with no address answers nothing.
         *   blink : credentials are stored, so it is trying and has not got
         *           there yet -- which is also what a rejoin after an AP
         *           reboot looks like from the outside.
         *   off   : nothing stored, so nothing is expected of it.
         *
         * Read through net/netif.h rather than from the CYW43 driver: this is
         * "am I on a network", not "is the radio associated", and a wired
         * persona should light the same lamp for the same reason. Carrier is
         * now genuinely carrier -- the driver decodes the firmware's link
         * events -- so this lamp goes out when a link drops instead of
         * staying on because a BSSID was once seen.
         */
        if (now >= next_net_led_ms) {
            netif_t *nif = netif_default();
            bool up = nif && netif_link_up(nif) && net_configured();
            bool want_net;
            if (up) {
                want_net = true;
                next_net_led_ms = now + 1000u;
            } else if (net_have_creds) {
                want_net = ((now / 500u) & 1u) == 0;
                next_net_led_ms = now + 50u;
            } else {
                want_net = false;
                next_net_led_ms = now + 1000u;
            }
            if (want_net != net_led_on) {
                net_led_on = want_net;
                clock_hw_indicator(CLOCK_IND_WIFI, want_net);
            }
        }

#if CONFIG_ENABLE_DCF77
        /* The DCF indicator, on its own cadence -- it has to be able to blink
         * once a second while the rest of the screen sits still for a minute.
         *
         *   on the signal screen : mirrors the second pulse itself, which is
         *                          the most useful signal feedback there is --
         *                          a person can see at a glance whether it is
         *                          ticking once a second or stuttering
         *   syncing              : a slow blink
         *   synced < 48 h ago    : solid
         *   otherwise            : off (never synced, or gone stale)
         */
        if (now >= next_led_ms) {
            dcf_status_t s2;
            dcf77_service_status(&s2);
            bool want;
            if (st.mode == UI_MODE_ITEM && st.item == UI_ITEM_SIG) {
                want = s2.pulse;
                next_led_ms = now + 10u;
            } else if (s2.state == DCF_SYNCING) {
                want = ((now / 500u) & 1u) == 0;
                next_led_ms = now + 50u;
            } else {
                uint32_t age = dcf77_service_age_s();
                want = (age != 0xFFFFFFFFu) && (age < 48u * 3600u);
                next_led_ms = now + 1000u;
            }
            if (want != led_on) { led_on = want; clock_hw_indicator(CLOCK_IND_DCF, want); }
        }
#endif

        /* The frame, and the only place this loop waits. Its ~8 ms is the
         * loop's period: everything above runs once per frame, and the ops it
         * issues are short next to the frame that follows them. */
        pump_frame();

        if (console_interrupt_requested()) { console_interrupt_clear(); break; }
    }

    clock_hw_indicator(CLOCK_IND_C, false);
    clock_hw_indicator(CLOCK_IND_DCF, false);
    clock_hw_indicator(CLOCK_IND_AM, false);
    clock_hw_indicator(CLOCK_IND_PM, false);
    clock_hw_set_weekday(0);
    clock_hw_set_brightness(-1);
    clock_hw_blank();
}
