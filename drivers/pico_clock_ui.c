/* The menu state machine. See drivers/include/drivers/pico_clock_ui.h for the
 * navigation contract and for why none of this touches hardware. */

#include "drivers/pico_clock_ui.h"
#include "kernel/printk.h"
#include <string.h>

#define INACTIVITY_MS      10000u
#define SHORTCUT_TEMP_MS    2000u
#define SHORTCUT_DATE_MS    2000u
#define SHORTCUT_YEAR_MS    1500u

/* Short labels are what stays on the panel, so they all have to fit the 22
 * columns the font leaves; full names scroll once and can be any length.
 * "24H" rather than the "12/24" the plan first wrote: at 24 columns that one
 * label would have had to scroll every time, and a short label that scrolls
 * is not a short label. */
static const struct { const char *shortname, *fullname; } ITEM[UI_ITEM_COUNT] = {
    [UI_ITEM_TEMP]    = { "TEMP", "TEMPERATURE" },
    [UI_ITEM_DATE]    = { "DATE", "DATE" },
    [UI_ITEM_BRIGHT]  = { "BRT",  "BRIGHTNESS" },
    [UI_ITEM_TIMESET] = { "TSET", "SET TIME" },
    [UI_ITEM_OFFSET]  = { "OFFS", "TEMP OFFSET" },
    [UI_ITEM_SIG]     = { "SIG",  "DCF SIGNAL" },
    [UI_ITEM_SYNC]    = { "SYNC", "DCF SYNC NOW" },
    [UI_ITEM_LAST]    = { "LAST", "LAST SYNC" },
    [UI_ITEM_AUTO]    = { "AUTO", "NIGHTLY SYNC" },
    [UI_ITEM_HOUR12]  = { "24H",  "HOUR FORMAT" },
    [UI_ITEM_BEEP]    = { "BEEP", "BEEP ON KEYS" },
    [UI_ITEM_EXIT]    = { "EXIT", "EXIT" },
};

const char *clock_ui_item_short(ui_item_t i) {
    return ((unsigned)i < UI_ITEM_COUNT) ? ITEM[i].shortname : "";
}
const char *clock_ui_item_full(ui_item_t i) {
    return ((unsigned)i < UI_ITEM_COUNT) ? ITEM[i].fullname : "";
}

/* ------------------------------------------------------------ helpers -- */

static uint8_t days_in_month(unsigned year, unsigned month) {
    static const uint8_t d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 1 || month > 12) return 31;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return d[month - 1];
}

/* Every adjustable value wraps rather than clamping. On three buttons,
 * hitting the end of a range and having nothing happen reads as a broken
 * button; wrapping is always recoverable and needs no second direction. */
static int wrap(int v, int lo, int hi) {
    if (v < lo) return hi;
    if (v > hi) return lo;
    return v;
}

/* `scroll` says whether the item's full name should play. It should when you
 * ARRIVE at an item -- that is what the name is for -- and it should not when
 * you come back OUT of one, which was the other half of why backing out of
 * TEMPERATURE felt broken: you had just been there, and the reward for
 * leaving was being told its name again at length. */
static void enter_menu(ui_state_t *st, ui_item_t item, uint64_t now, bool scroll) {
    st->mode = UI_MODE_MENU;
    st->item = item;
    st->pending_scroll = scroll;
    st->deadline_ms = 0;
    st->last_input_ms = now;
}

static void go_idle(ui_state_t *st, uint64_t now) {
    st->mode = UI_MODE_IDLE;
    st->pending_scroll = false;
    st->deadline_ms = 0;
    st->last_input_ms = now;
}

/* Load the current time into the edit buffer. Done on entry to SET TIME and
 * nowhere else, so an abandoned edit leaves the clock untouched and a
 * restarted one starts from what the clock says now, not from what it said
 * the last time somebody looked. */
static void load_edit(ui_state_t *st, const ui_inputs_t *in) {
    st->edit[UI_FIELD_HOUR]  = in->local.hour;
    st->edit[UI_FIELD_MIN]   = in->local.min;
    st->edit[UI_FIELD_DAY]   = in->local.day ? in->local.day : 1;
    st->edit[UI_FIELD_MONTH] = in->local.month ? in->local.month : 1;
    st->edit[UI_FIELD_YEAR]  = (uint8_t)(in->local.year >= 2000
                                            ? (in->local.year - 2000) % 100u : 0);
    st->field = UI_FIELD_HOUR;
}

static void enter_item(ui_state_t *st, const ui_inputs_t *in, uint64_t now) {
    st->mode = UI_MODE_ITEM;
    st->pending_scroll = false;
    st->deadline_ms = 0;
    st->last_input_ms = now;

    switch (st->item) {
    case UI_ITEM_TIMESET: load_edit(st, in); break;
    /* Entering DCF SYNC NOW *is* the request -- pressing SET on an item
     * called "sync now" and then having to confirm it would be asking the
     * same question twice. The screen then reports what happened, and any
     * press leaves. */
    case UI_ITEM_SYNC:    st->sync_asked = true; break;
    case UI_ITEM_BRIGHT:  st->edit_scalar = st->set.brightness; break;
    case UI_ITEM_OFFSET:  st->edit_scalar = st->set.temp_offset; break;
    case UI_ITEM_HOUR12:  st->edit_scalar = st->set.hour12 ? 12 : 24; break;
    case UI_ITEM_BEEP:    st->edit_scalar = st->set.confirm_beep ? 1 : 0; break;
    case UI_ITEM_AUTO:    st->edit_scalar = st->set.auto_sync ? 1 : 0; break;
    default: break;
    }
}

/* ------------------------------------------------------------- entry --- */

void clock_ui_reset(ui_state_t *st, const ui_settings_t *defaults, uint64_t now) {
    memset(st, 0, sizeof(*st));
    if (defaults) st->set = *defaults;
    st->mode = UI_MODE_IDLE;
    st->last_input_ms = now;
}

void clock_ui_scroll_done(ui_state_t *st, uint64_t now) {
    st->pending_scroll = false;
    /* The scroll took real time, and it was not the user idling. Without
     * this, a 1.5 s label plus a moment's thought trips the 10 s timeout
     * while someone is still reading the label. */
    st->last_input_ms = now;
}

unsigned clock_ui_tick(ui_state_t *st, uint64_t now) {
    if (st->mode == UI_MODE_IDLE) return 0;

    if (st->deadline_ms && now >= st->deadline_ms) {
        if (st->mode == UI_MODE_SHORTCUT_DATE) {
            st->mode = UI_MODE_SHORTCUT_YEAR;
            st->deadline_ms = now + SHORTCUT_YEAR_MS;
        } else {
            go_idle(st, now);
        }
        return 0;
    }

    /* Inactivity. Deliberately abandons an edit rather than committing it: a
     * clock that sets itself to whatever was on screen when its owner walked
     * away is worse than one that did nothing.
     *
     * DCF SIGNAL is exempt: it exists to be stared at while an antenna is
     * moved around, and timing it out after ten seconds would break the one
     * job it has. It is read-only, so nothing can be left half-done in it. */
    bool watching = (st->mode == UI_MODE_ITEM &&
                     (st->item == UI_ITEM_SIG || st->item == UI_ITEM_SYNC));
    if (!watching && !st->pending_scroll && (now - st->last_input_ms) >= INACTIVITY_MS) {
        go_idle(st, now);
    }
    return 0;
}

unsigned clock_ui_key(ui_state_t *st, const ui_inputs_t *in,
                      clock_key_t key, clock_press_t press, uint64_t now) {
    unsigned act = UI_ACT_CLICK;
    st->last_input_ms = now;

    /* A shortcut screen: any key at all returns to the clock, and the key is
     * consumed doing so rather than also doing whatever it normally does. */
    if (st->mode == UI_MODE_SHORTCUT_TEMP || st->mode == UI_MODE_SHORTCUT_DATE ||
        st->mode == UI_MODE_SHORTCUT_YEAR) {
        go_idle(st, now);
        return act;
    }

    if (st->mode == UI_MODE_IDLE) {
        if (press != CLOCK_PRESS_SHORT) return 0;
        switch (key) {
        case CLOCK_KEY_UP:
            st->mode = UI_MODE_SHORTCUT_TEMP;
            st->deadline_ms = now + SHORTCUT_TEMP_MS;
            return act;
        case CLOCK_KEY_DOWN:
            st->mode = UI_MODE_SHORTCUT_DATE;
            st->deadline_ms = now + SHORTCUT_DATE_MS;
            return act;
        case CLOCK_KEY_SET:
            enter_menu(st, (ui_item_t)0, now, true);   /* the first item */
            return act;
        default:
            return 0;
        }
    }

    if (st->mode == UI_MODE_MENU) {
        if (key == CLOCK_KEY_SET) {
            if (press == CLOCK_PRESS_LONG) { go_idle(st, now); return act; }
            if (press != CLOCK_PRESS_SHORT) return 0;
            if (st->item == UI_ITEM_EXIT) { go_idle(st, now); return act; }
            enter_item(st, in, now);
            return act;
        }
        if (press == CLOCK_PRESS_LONG) return 0;   /* long UP/DOWN: nothing */
        int next = (int)st->item + (key == CLOCK_KEY_UP ? -1 : +1);
        enter_menu(st, (ui_item_t)wrap(next, 0, UI_ITEM_COUNT - 1), now, true);
        return act;
    }

    /* Inside an item. */
    if (key == CLOCK_KEY_SET && press == CLOCK_PRESS_LONG) {
        /* Abandon, and go back one level rather than out. Nothing has been
         * written anywhere -- every item edits a copy and only the confirm
         * path below hands it over -- so this is also the "let me look at a
         * different setting instead" route, which is why it stops at the menu
         * while confirming goes all the way to the clock. */
        enter_menu(st, st->item, now, false);
        return act;
    }

    switch (st->item) {
    case UI_ITEM_TEMP:
    case UI_ITEM_DATE:
    case UI_ITEM_SIG:
    case UI_ITEM_LAST:
    case UI_ITEM_SYNC:
        /* Nothing to confirm, so any press is "seen it" -- and seeing it was
         * the whole errand. Straight to the clock, exactly as the same
         * screens reached by an idle shortcut behave. (SET long has already
         * been handled above and goes back a level instead.) */
        go_idle(st, now);
        return act;

    case UI_ITEM_BRIGHT:
        if (key == CLOCK_KEY_SET) {
            if (press != CLOCK_PRESS_SHORT) return 0;
            st->set.brightness = st->edit_scalar;
            go_idle(st, now);
            return act | UI_ACT_CONFIRM | UI_ACT_APPLY_BRIGHT;
        }
        /* -1 is "automatic", and it sits below 1 in the same cycle so one
         * button reaches it: ... 6, 7, AUTO, 1, 2 ... */
        if (key == CLOCK_KEY_UP) {
            st->edit_scalar = (int8_t)(st->edit_scalar < 0 ? 7 :
                                       (st->edit_scalar <= 1 ? -1 : st->edit_scalar - 1));
        } else {
            st->edit_scalar = (int8_t)(st->edit_scalar < 0 ? 1 :
                                       (st->edit_scalar >= 7 ? -1 : st->edit_scalar + 1));
        }
        /* Applied live, so the panel shows the brightness being chosen rather
         * than describing it. */
        return act | UI_ACT_APPLY_BRIGHT;

    case UI_ITEM_OFFSET:
        if (key == CLOCK_KEY_SET) {
            if (press != CLOCK_PRESS_SHORT) return 0;
            st->set.temp_offset = st->edit_scalar;
            go_idle(st, now);
            return act | UI_ACT_CONFIRM;
        }
        st->edit_scalar = (int8_t)wrap(st->edit_scalar + (key == CLOCK_KEY_UP ? 1 : -1), -9, 9);
        return act;

    case UI_ITEM_HOUR12:
        if (key == CLOCK_KEY_SET) {
            if (press != CLOCK_PRESS_SHORT) return 0;
            st->set.hour12 = (st->edit_scalar == 12);
            go_idle(st, now);
            return act | UI_ACT_CONFIRM;
        }
        st->edit_scalar = (int8_t)(st->edit_scalar == 12 ? 24 : 12);
        return act;

    case UI_ITEM_AUTO:
        if (key == CLOCK_KEY_SET) {
            if (press != CLOCK_PRESS_SHORT) return 0;
            st->set.auto_sync = (st->edit_scalar != 0);
            go_idle(st, now);
            return act | UI_ACT_CONFIRM | UI_ACT_SET_AUTO;
        }
        st->edit_scalar = (int8_t)(st->edit_scalar ? 0 : 1);
        return act;

    case UI_ITEM_BEEP:
        if (key == CLOCK_KEY_SET) {
            if (press != CLOCK_PRESS_SHORT) return 0;
            st->set.confirm_beep = (st->edit_scalar != 0);
            go_idle(st, now);
            /* No CONFIRM here: beeping to confirm that beeping is now off
             * would be the last thing it ever did, and confusing. */
            return act;
        }
        st->edit_scalar = (int8_t)(st->edit_scalar ? 0 : 1);
        return act;

    case UI_ITEM_TIMESET:
        if (key == CLOCK_KEY_SET) {
            if (press != CLOCK_PRESS_SHORT) return 0;
            if (st->field + 1 < UI_FIELD_COUNT) {
                st->field = (ui_field_t)(st->field + 1);
                return act;
            }
            /* Last field confirmed: the whole edit is committed at once, so a
             * half-set clock is never written. */
            go_idle(st, now);
            return act | UI_ACT_CONFIRM | UI_ACT_COMMIT_TIME;
        }
        {
            int delta = (key == CLOCK_KEY_UP) ? +1 : -1;
            int v = st->edit[st->field] + delta;
            switch (st->field) {
            case UI_FIELD_HOUR:  v = wrap(v, 0, 23); break;
            case UI_FIELD_MIN:   v = wrap(v, 0, 59); break;
            case UI_FIELD_DAY:   v = wrap(v, 1, days_in_month(2000u + st->edit[UI_FIELD_YEAR],
                                                              st->edit[UI_FIELD_MONTH])); break;
            case UI_FIELD_MONTH: v = wrap(v, 1, 12); break;
            default:             v = wrap(v, 0, 99); break;
            }
            st->edit[st->field] = (uint8_t)v;
            /* A day that no longer exists after changing month or year -- 31
             * February, or 29 February in a common year -- is pulled back
             * rather than left to be rejected at commit time. */
            {
                uint8_t dim = days_in_month(2000u + st->edit[UI_FIELD_YEAR],
                                            st->edit[UI_FIELD_MONTH]);
                if (st->edit[UI_FIELD_DAY] > dim) st->edit[UI_FIELD_DAY] = dim;
            }
        }
        return act;

    default:
        go_idle(st, now);
        return act;
    }
}

/* ------------------------------------------------------------ screen --- */

static void put_str(char *dst, const char *src) {
    unsigned i = 0;
    while (src[i] && i < 15u) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void put_u2(char *dst, unsigned v) {
    dst[0] = (char)('0' + (v / 10u) % 10u);
    dst[1] = (char)('0' + v % 10u);
}

void clock_ui_screen(const ui_state_t *st, const ui_inputs_t *in,
                     uint64_t now, ui_screen_t *out) {
    memset(out, 0, sizeof(*out));

    if (st->pending_scroll) {
        out->kind = UI_SCR_SCROLL;
        put_str(out->text, clock_ui_item_full(st->item));
        return;
    }

    switch (st->mode) {
    case UI_MODE_MENU:
        out->kind = UI_SCR_TEXT;
        put_str(out->text, clock_ui_item_short(st->item));
        return;

    case UI_MODE_SHORTCUT_TEMP:
        goto temperature;

    case UI_MODE_SHORTCUT_DATE:
        out->kind = UI_SCR_TEXT;
        put_u2(&out->text[0], in->local.day);
        out->text[2] = '.';
        put_u2(&out->text[3], in->local.month);
        out->text[5] = '\0';
        return;

    case UI_MODE_SHORTCUT_YEAR:
        out->kind = UI_SCR_TEXT;
        put_u2(&out->text[0], (unsigned)(in->local.year / 100u));
        put_u2(&out->text[2], (unsigned)(in->local.year % 100u));
        out->text[4] = '\0';
        return;

    case UI_MODE_ITEM:
        switch (st->item) {
        case UI_ITEM_TEMP:
            goto temperature;
        case UI_ITEM_DATE:
            out->kind = UI_SCR_TEXT;
            put_u2(&out->text[0], in->local.day);
            out->text[2] = '.';
            put_u2(&out->text[3], in->local.month);
            out->text[5] = '\0';
            return;
        case UI_ITEM_BRIGHT:
            out->kind = UI_SCR_TEXT;
            if (st->edit_scalar < 0) put_str(out->text, "BR A");
            else { put_str(out->text, "BR "); out->text[3] = (char)('0' + st->edit_scalar);
                   out->text[4] = '\0'; }
            return;
        case UI_ITEM_OFFSET:
            out->kind = UI_SCR_TEXT;
            put_str(out->text, "OF");
            out->text[2] = (st->edit_scalar < 0) ? '-' : ' ';
            out->text[3] = (char)('0' + (st->edit_scalar < 0 ? -st->edit_scalar
                                                             : st->edit_scalar));
            out->text[4] = '\0';
            return;
        case UI_ITEM_SIG:
            /* The one screen that is a picture rather than a value: 24
             * columns, one per second, newest on the right. Nothing to
             * confirm, so any press dismisses it -- but it is also the one
             * screen someone deliberately watches for a while, which is why
             * it is exempt from the inactivity timeout (clock_ui_tick). */
            if (!in->quality || in->quality_n == 0) {
                out->kind = UI_SCR_TEXT;
                put_str(out->text, "NO SIG");
                return;
            }
            out->kind = UI_SCR_BARS;
            out->bars = in->quality;
            out->bars_n = in->quality_n;
            return;
        case UI_ITEM_SYNC:
            out->kind = UI_SCR_TEXT;
            switch (in->dcf) {
            case UI_DCF_ABSENT:  put_str(out->text, "NO RX"); break;
            case UI_DCF_DONE:    put_str(out->text, "OK");    break;
            case UI_DCF_FAILED:  put_str(out->text, "FAIL");  break;
            case UI_DCF_SYNCING:
                /* A five-minute wait with no feedback reads as a hang, so the
                 * screen counts down: minutes while there are minutes left,
                 * then seconds. */
                if (in->dcf_left_s >= 60u) {
                    unsigned m = in->dcf_left_s / 60u;
                    put_u2(&out->text[0], m);
                    out->text[2] = 'M';
                    out->text[3] = '\0';
                } else {
                    put_u2(&out->text[0], in->dcf_left_s);
                    out->text[2] = 'S';
                    out->text[3] = '\0';
                }
                break;
            default:             put_str(out->text, "WAIT"); break;
            }
            return;
        case UI_ITEM_LAST:
            out->kind = UI_SCR_TEXT;
            if (in->dcf == UI_DCF_ABSENT)        put_str(out->text, "NO RX");
            else if (in->dcf_age_s == 0xFFFFFFFFu) put_str(out->text, "NONE");
            else if (in->dcf_age_s < 3600u) {
                put_u2(&out->text[0], in->dcf_age_s / 60u);
                out->text[2] = 'M'; out->text[3] = '\0';
            } else if (in->dcf_age_s < 48u * 3600u) {
                put_u2(&out->text[0], in->dcf_age_s / 3600u);
                out->text[2] = 'H'; out->text[3] = '\0';
            } else {
                put_u2(&out->text[0], in->dcf_age_s / 86400u);
                out->text[2] = 'D'; out->text[3] = '\0';
            }
            return;
        case UI_ITEM_AUTO:
            out->kind = UI_SCR_TEXT;
            put_str(out->text, st->edit_scalar ? "ON" : "OFF");
            return;
        case UI_ITEM_HOUR12:
            out->kind = UI_SCR_TEXT;
            put_str(out->text, st->edit_scalar == 12 ? "12H" : "24H");
            return;
        case UI_ITEM_BEEP:
            out->kind = UI_SCR_TEXT;
            put_str(out->text, st->edit_scalar ? "ON" : "OFF");
            return;
        case UI_ITEM_TIMESET: {
            /* One field at a time, named, because five bare numbers in a row
             * on a 22-column panel are indistinguishable from each other. */
            static const char *const LABEL[UI_FIELD_COUNT] = { "H", "M", "D", "MO", "Y" };
            out->kind = UI_SCR_TEXT;
            put_str(out->text, LABEL[st->field]);
            unsigned n = (unsigned)strlen(out->text);
            put_u2(&out->text[n], st->edit[st->field]);
            out->text[n + 2] = '\0';
            /* The caller blinks it, at a rate it owns; the machine only says
             * that this is an editable value rather than a settled one. */
            out->colon = ((now / 400u) & 1u) == 0;
            return;
        }
        default:
            out->kind = UI_SCR_TEXT;
            put_str(out->text, clock_ui_item_short(st->item));
            return;
        }

    case UI_MODE_IDLE:
    default: {
        unsigned h = in->local.hour;
        out->kind = UI_SCR_TIME;
        out->weekday = true;
        out->colon = true;
        if (st->set.hour12) {
            out->am = (h < 12);
            out->pm = !out->am;
            h = h % 12u;
            if (h == 0) h = 12;
        }
        out->value = (int)(h * 100u + in->local.min);
        return;
    }
    }

temperature:
    out->kind = UI_SCR_TEMP;
    if (!in->have_temp) { out->kind = UI_SCR_TEXT; put_str(out->text, "NO C"); return; }
    out->value = in->temp_c + st->set.temp_offset;
}

/* ---------------------------------------------------------- selftest --- */

static int g_fail;

static void check(bool ok, const char *what) {
    if (!ok) g_fail++;
    printk("  [%s] %s\n", ok ? "ok" : "FAIL", what);
}

/* A whole test is "press these keys, then look at the panel", which is what a
 * person does, so the helpers are shaped that way too. */
static ui_state_t  T_st;
static ui_inputs_t T_in;
static uint64_t    T_now;
static unsigned    T_act;

static void t_reset(void) {
    ui_settings_t def = { .brightness = -1, .temp_offset = -2, .hour12 = false,
                          .click = false, .confirm_beep = true,
                          .auto_sync = true };
    T_now = 1000;
    clock_ui_reset(&T_st, &def, T_now);
    memset(&T_in, 0, sizeof(T_in));
    T_in.local = (rtc_time_t){ 2026, 8, 23, 14, 35, 0, 0 };
    T_in.have_temp = true;
    T_in.temp_c = 24;
    T_act = 0;
}

static void t_key(clock_key_t k, clock_press_t p) {
    T_now += 50;
    T_act = clock_ui_key(&T_st, &T_in, k, p, T_now);
    /* A caller always finishes any scroll the machine asks for before the
     * next key; the test does the same so the states line up. */
    if (T_st.pending_scroll) { T_now += 1500; clock_ui_scroll_done(&T_st, T_now); }
}

static void t_wait(uint64_t ms) {
    T_now += ms;
    clock_ui_tick(&T_st, T_now);
}

static const char *t_text(void) {
    static ui_screen_t scr;
    clock_ui_screen(&T_st, &T_in, T_now, &scr);
    return scr.text;
}

static ui_screen_kind_t t_kind(void) {
    ui_screen_t scr;
    clock_ui_screen(&T_st, &T_in, T_now, &scr);
    return scr.kind;
}

static int t_value(void) {
    ui_screen_t scr;
    clock_ui_screen(&T_st, &T_in, T_now, &scr);
    return scr.value;
}

/* Walk to an item from idle, by name, so a reordering of the menu does not
 * silently retarget every test below. */
static bool t_goto(ui_item_t want) {
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    for (unsigned i = 0; i < UI_ITEM_COUNT; i++) {
        if (T_st.item == want) return true;
        t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    }
    return false;
}

int clock_ui_selftest(void) {
    g_fail = 0;
    printk("[ClockUI] selftest\n");

    /* --- the idle screen and its shortcuts ---------------------------- */
    t_reset();
    check(t_kind() == UI_SCR_TIME && t_value() == 1435, "idle shows the time");

    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(t_kind() == UI_SCR_TEMP && t_value() == 22,
          "UP shows the temperature, with the offset applied");
    t_wait(2100);
    check(t_kind() == UI_SCR_TIME, "the temperature screen times out");

    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "23.08") == 0, "DOWN shows the date");
    t_wait(2100);
    check(strcmp(t_text(), "2026") == 0, "...then the year");
    t_wait(1600);
    check(t_kind() == UI_SCR_TIME, "...then back to the clock");

    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(t_kind() == UI_SCR_TIME, "any key leaves a shortcut screen at once");

    /* --- navigation --------------------------------------------------- */
    t_reset();
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(T_st.mode == UI_MODE_MENU && T_st.item == (ui_item_t)0 &&
          strcmp(t_text(), "SIG") == 0,
          "SET opens the menu at the first item, which is the signal monitor");
    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "SYNC") == 0, "DOWN moves to the next item");
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "EXIT") == 0, "UP past the first item wraps to the last");
    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "SIG") == 0, "...and DOWN past the last wraps back");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_LONG);
    check(T_st.mode == UI_MODE_IDLE, "SET long leaves the menu");

    t_reset();
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_wait(10100);
    check(T_st.mode == UI_MODE_IDLE, "10 s of inactivity returns to the clock");

    /* The full name scrolls once on entry, then the short label stays. */
    t_reset();
    T_now += 50;
    clock_ui_key(&T_st, &T_in, CLOCK_KEY_SET, CLOCK_PRESS_SHORT, T_now);
    check(t_kind() == UI_SCR_SCROLL && strcmp(t_text(), "DCF SIGNAL") == 0,
          "entering an item scrolls its full name");
    clock_ui_scroll_done(&T_st, T_now);
    check(t_kind() == UI_SCR_TEXT && strcmp(t_text(), "SIG") == 0,
          "...then settles on the short label");

    /* --- brightness ---------------------------------------------------- */
    t_reset();
    check(t_goto(UI_ITEM_BRIGHT), "reach BRIGHTNESS");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "BR A") == 0, "brightness starts on automatic");
    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "BR 1") == 0 && (T_act & UI_ACT_APPLY_BRIGHT),
          "DOWN leaves automatic for level 1, applied live");
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "BR A") == 0, "UP from level 1 returns to automatic");
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "BR 7") == 0, "...and UP again wraps to the brightest");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(T_st.set.brightness == 7 && (T_act & UI_ACT_CONFIRM) &&
          T_st.mode == UI_MODE_IDLE,
          "SET commits the brightness and finishes, back at the clock");

    t_reset();
    t_goto(UI_ITEM_BRIGHT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_LONG);
    check(T_st.set.brightness == -1 && T_st.mode == UI_MODE_MENU,
          "SET long abandons the change");

    /* --- temperature offset -------------------------------------------- */
    t_reset();
    check(t_goto(UI_ITEM_OFFSET), "reach TEMP OFFSET");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "OF-2") == 0, "offset starts at the board default");
    for (int i = 0; i < 3; i++) t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "OF 1") == 0, "UP raises it through zero");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(T_st.set.temp_offset == 1 && T_st.mode == UI_MODE_IDLE,
          "SET commits the offset and finishes");
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(t_value() == 25, "the committed offset changes what the shortcut shows");

    /* --- hour format ---------------------------------------------------- */
    t_reset();
    check(t_goto(UI_ITEM_HOUR12), "reach HOUR FORMAT");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "24H") == 0, "starts on 24-hour");
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    {
        ui_screen_t scr;
        clock_ui_screen(&T_st, &T_in, T_now, &scr);
        check(scr.value == 235 && scr.pm && !scr.am,
              "12-hour mode shows 2:35 with PM lit");
    }

    /* --- setting the time ----------------------------------------------- */
    t_reset();
    check(t_goto(UI_ITEM_TIMESET), "reach SET TIME");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "H14") == 0, "SET TIME starts at the current hour");
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "H15") == 0, "UP advances the hour");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "M35") == 0, "SET moves on to the minute");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "D23") == 0, "...then the day");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "MO08") == 0, "...then the month");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "Y26") == 0, "...then the year");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check((T_act & UI_ACT_COMMIT_TIME) && T_st.edit[UI_FIELD_HOUR] == 15 &&
          T_st.mode == UI_MODE_IDLE,
          "confirming the last field commits the whole edit and finishes");

    t_reset();
    t_goto(UI_ITEM_TIMESET);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_LONG);
    check(!(T_act & UI_ACT_COMMIT_TIME) && T_st.mode == UI_MODE_MENU,
          "SET long abandons a half-finished time without writing it");

    t_reset();
    t_goto(UI_ITEM_TIMESET);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_wait(10100);
    check(T_st.mode == UI_MODE_IDLE, "an abandoned edit also times out");

    /* A day that stops existing when the month changes under it. */
    t_reset();
    T_in.local = (rtc_time_t){ 2026, 1, 31, 12, 0, 0, 0 };
    t_goto(UI_ITEM_TIMESET);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);   /* on the day field */
    check(strcmp(t_text(), "D31") == 0, "31 January");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);   /* month */
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);    /* -> February */
    check(T_st.edit[UI_FIELD_DAY] == 28, "31 February is pulled back to the 28th");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);   /* year */
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);    /* 2026 -> 2027 */
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);    /* -> 2028, a leap year */
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);    /* and back off it */
    check(T_st.edit[UI_FIELD_DAY] == 28, "the day stays legal across a leap year");

    /* --- the beep, and turning it off -------------------------------------- */
    t_reset();
    check(t_goto(UI_ITEM_BEEP), "reach BEEP ON KEYS");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "ON") == 0, "the confirm beep starts on");
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(!T_st.set.confirm_beep && !(T_act & UI_ACT_CONFIRM) &&
          T_st.mode == UI_MODE_IDLE,
          "turning the beep off does not beep on the way out");

    /* --- coming back out of an item ---------------------------------------- */
    /* The bug this pair pins down: SET long DID leave the TEMPERATURE item,
     * and then replayed its 4.6-second name scroll, during which the buttons
     * were not even sampled. It read as "SET long does nothing". */
    t_reset();
    t_goto(UI_ITEM_TEMP);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(T_st.mode == UI_MODE_ITEM, "inside TEMPERATURE");
    T_now += 50;
    T_act = clock_ui_key(&T_st, &T_in, CLOCK_KEY_SET, CLOCK_PRESS_LONG, T_now);
    check(T_st.mode == UI_MODE_MENU, "SET long leaves the item");
    check(!T_st.pending_scroll && strcmp(t_text(), "TEMP") == 0,
          "...straight to the short label, with no name scroll on the way out");

    t_reset();
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    T_now += 50;
    T_act = clock_ui_key(&T_st, &T_in, CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT, T_now);
    check(T_st.pending_scroll, "arriving at a new item still scrolls its name");

    /* --- short finishes, long steps back ------------------------------------ */
    /* The rule the whole menu rests on, and the one place it used to break:
     * confirming a value used to land back on the item just finished, where
     * SET reopened it -- the only screen in the UI where completing something
     * pointed you at doing it again (user, 2026-08-23). */
    t_reset();
    t_goto(UI_ITEM_BRIGHT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(T_st.mode == UI_MODE_IDLE, "confirming leaves the menu entirely");

    t_reset();
    t_goto(UI_ITEM_BRIGHT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_LONG);
    check(T_st.mode == UI_MODE_MENU && T_st.item == UI_ITEM_BRIGHT &&
          T_st.set.brightness == -1,
          "abandoning stops at the menu instead, so another item is one step away");

    t_reset();
    t_goto(UI_ITEM_TEMP);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(T_st.mode == UI_MODE_IDLE,
          "a read-only item has nothing to confirm, so dismissing it finishes too");

    /* --- the signal monitor ------------------------------------------------ */
    /* Two presses from the clock face, and that is the whole point: a
     * finished clock has no console, so the only way to aim the antenna is
     * through these buttons. */
    t_reset();
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(T_st.mode == UI_MODE_ITEM && T_st.item == UI_ITEM_SIG,
          "SET, SET from the clock reaches the signal monitor");

    t_reset();
    check(t_goto(UI_ITEM_SIG), "reach DCF SIGNAL");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "NO SIG") == 0,
          "with no receiver the signal screen says so rather than drawing nothing");
    {
        static const uint8_t q[4] = { 7, 6, 0, 7 };
        T_in.quality = q;
        T_in.quality_n = 4;
        ui_screen_t scr;
        clock_ui_screen(&T_st, &T_in, T_now, &scr);
        check(scr.kind == UI_SCR_BARS && scr.bars_n == 4 && scr.bars[2] == 0,
              "with a receiver it hands the caller the per-second scores");
    }
    t_wait(20000);
    check(T_st.mode == UI_MODE_ITEM,
          "the signal screen does not time out -- it exists to be watched");
    t_key(CLOCK_KEY_UP, CLOCK_PRESS_SHORT);
    check(T_st.mode == UI_MODE_IDLE, "...and any press dismisses it");

    /* --- the sync items ------------------------------------------------- */
    t_reset();
    T_in.dcf = UI_DCF_IDLE;
    T_in.dcf_age_s = 0xFFFFFFFFu;
    check(t_goto(UI_ITEM_SYNC), "reach DCF SYNC NOW");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(T_st.sync_asked, "entering the item is the request -- no second confirmation");
    T_st.sync_asked = false;
    T_in.dcf = UI_DCF_SYNCING;
    T_in.dcf_left_s = 240;
    check(strcmp(t_text(), "04M") == 0, "while syncing it counts down in minutes");
    T_in.dcf_left_s = 9;
    check(strcmp(t_text(), "09S") == 0, "...then in seconds");
    T_in.dcf = UI_DCF_DONE;
    check(strcmp(t_text(), "OK") == 0, "and says so when it lands");
    T_in.dcf = UI_DCF_FAILED;
    check(strcmp(t_text(), "FAIL") == 0, "...or when it does not");
    t_wait(20000);
    check(T_st.mode == UI_MODE_ITEM, "a sync in progress is not timed out from under you");

    t_reset();
    T_in.dcf = UI_DCF_IDLE;
    check(t_goto(UI_ITEM_LAST), "reach LAST SYNC");
    T_in.dcf_age_s = 0xFFFFFFFFu;
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "NONE") == 0, "a clock that has never synced says NONE");
    T_in.dcf_age_s = 45 * 60;
    check(strcmp(t_text(), "45M") == 0, "minutes for the first hour");
    T_in.dcf_age_s = 13 * 3600;
    check(strcmp(t_text(), "13H") == 0, "hours after that");
    T_in.dcf_age_s = 3 * 86400;
    check(strcmp(t_text(), "03D") == 0, "days once it is properly stale");

    t_reset();
    check(t_goto(UI_ITEM_AUTO), "reach NIGHTLY SYNC");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "ON") == 0, "the nightly sync is on by default");
    t_key(CLOCK_KEY_DOWN, CLOCK_PRESS_SHORT);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(!T_st.set.auto_sync && (T_act & UI_ACT_SET_AUTO) && T_st.mode == UI_MODE_IDLE,
          "turning it off commits and finishes");

    /* A build with no receiver must not show four items that cannot work. */
    t_reset();
    T_in.dcf = UI_DCF_ABSENT;
    t_goto(UI_ITEM_SYNC);
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(strcmp(t_text(), "NO RX") == 0, "with no receiver the sync item says so");

    /* --- EXIT ------------------------------------------------------------ */
    t_reset();
    check(t_goto(UI_ITEM_EXIT), "reach EXIT");
    t_key(CLOCK_KEY_SET, CLOCK_PRESS_SHORT);
    check(T_st.mode == UI_MODE_IDLE, "EXIT returns to the clock");

    /* --- keys that must do nothing ---------------------------------------- */
    t_reset();
    T_now += 50;
    T_act = clock_ui_key(&T_st, &T_in, CLOCK_KEY_UP, CLOCK_PRESS_LONG, T_now);
    check(T_st.mode == UI_MODE_IDLE && T_act == 0,
          "a long UP on the idle screen does nothing at all");

    printk("[ClockUI] %s (%d failure%s)\n", g_fail ? "FAILURES" : "all passed",
           g_fail, g_fail == 1 ? "" : "s");
    printk(g_fail ? "CLOCKUI_SELFTEST_FAIL\n" : "CLOCKUI_SELFTEST_OK\n");
    return g_fail;
}
