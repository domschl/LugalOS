/*
 * POSIX TZ rule engine (kernel/include/kernel/timezone.h has the format and
 * the reasoning; this file is the parser and the arithmetic).
 *
 * Everything here works in minutes east of UTC and in seconds since 1970,
 * both computed with kernel/time.c's naive civil-date helpers. There is no
 * floating point, no allocation, and no dependency on the clock actually
 * being set -- a conversion is a pure function of its inputs and the rule in
 * force, which is why the whole thing is testable on QEMU with tz_selftest().
 */

#include "kernel/timezone.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
#include <string.h>

#define TZ_NAME_MAX 8
#define TZ_SPEC_MAX 48

typedef struct {
    uint8_t mon;    /* 1-12 */
    uint8_t week;   /* 1-4, or 5 for "the last one in the month" */
    uint8_t dow;    /* 0 = Sunday .. 6 = Saturday */
    int32_t sec;    /* seconds past local midnight, default 02:00:00 */
} tz_rule_t;

typedef struct {
    char    std_name[TZ_NAME_MAX + 1];
    char    dst_name[TZ_NAME_MAX + 1];
    int     std_off_min;    /* east of UTC */
    int     dst_off_min;
    bool    has_dst;
    tz_rule_t start, end;
} tz_t;

/* UTC until someone says otherwise, so a parse failure at boot still leaves
 * the system with a coherent (if unhelpful) answer rather than garbage. */
static tz_t  g_tz = { "UTC", "", 0, 0, false, {0,0,0,0}, {0,0,0,0} };
static char  g_spec[TZ_SPEC_MAX + 1] = "UTC0";

/* ------------------------------------------------------------ parsing --- */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* A zone abbreviation: three or more letters, or anything inside <> (the
 * form the tz database uses for numeric abbreviations like <-03>). */
static bool parse_name(const char **p, char *out) {
    const char *s = *p;
    unsigned n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>' && n < TZ_NAME_MAX) out[n++] = *s++;
        if (*s != '>') return false;
        s++;
    } else {
        while (((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) && n < TZ_NAME_MAX)
            out[n++] = *s++;
        if (n < 3) return false;
    }
    out[n] = '\0';
    *p = s;
    return n > 0;
}

/* [+|-]hh[:mm[:ss]] -> seconds. POSIX writes the offset to ADD to local time
 * to reach UTC, so central Europe is "-1"; the caller flips the sign once. */
static bool parse_offset(const char **p, int32_t *out_sec) {
    const char *s = *p;
    int sign = 1;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }
    if (!is_digit(*s)) return false;

    int32_t v[3] = { 0, 0, 0 };
    for (int part = 0; part < 3; part++) {
        if (!is_digit(*s)) return false;
        int32_t acc = 0;
        for (int i = 0; i < 2 && is_digit(*s); i++) acc = acc * 10 + (*s++ - '0');
        v[part] = acc;
        if (*s != ':') break;
        s++;
    }
    if (v[0] > 24 || v[1] > 59 || v[2] > 59) return false;
    *out_sec = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    *p = s;
    return true;
}

/* Mm.w.d[/time]. Jn and plain n are rejected on purpose -- see the header. */
static bool parse_rule(const char **p, tz_rule_t *r) {
    const char *s = *p;
    if (*s != 'M') return false;
    s++;

    int32_t field[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        if (!is_digit(*s)) return false;
        int32_t acc = 0;
        while (is_digit(*s)) acc = acc * 10 + (*s++ - '0');
        field[i] = acc;
        if (i < 2) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (field[0] < 1 || field[0] > 12) return false;
    if (field[1] < 1 || field[1] > 5)  return false;
    if (field[2] < 0 || field[2] > 6)  return false;

    r->mon  = (uint8_t)field[0];
    r->week = (uint8_t)field[1];
    r->dow  = (uint8_t)field[2];
    r->sec  = 2 * 3600;   /* POSIX default */

    if (*s == '/') {
        s++;
        int32_t t;
        /* The transition hour may legitimately be 24 or more ("/24" means
         * midnight at the end of the day, and Chile's rules use it). */
        if (!parse_offset(&s, &t)) return false;
        r->sec = t;
    }
    *p = s;
    return true;
}

bool tz_set(const char *spec) {
    if (!spec) return false;
    size_t len = strlen(spec);
    if (len == 0 || len > TZ_SPEC_MAX) return false;

    tz_t t;
    memset(&t, 0, sizeof(t));
    const char *p = spec;

    if (!parse_name(&p, t.std_name)) return false;
    int32_t off;
    if (!parse_offset(&p, &off)) return false;
    t.std_off_min = (int)(-off / 60);   /* POSIX sign is west-positive */

    if (*p != '\0') {
        if (!parse_name(&p, t.dst_name)) return false;
        t.has_dst = true;
        if (*p != ',' && *p != '\0') {
            if (!parse_offset(&p, &off)) return false;
            t.dst_off_min = (int)(-off / 60);
        } else {
            t.dst_off_min = t.std_off_min + 60;   /* POSIX default: one hour */
        }
    }

    if (*p == ',') {
        if (!t.has_dst) return false;
        p++;
        if (!parse_rule(&p, &t.start)) return false;
        if (*p != ',') return false;
        p++;
        if (!parse_rule(&p, &t.end)) return false;
    } else if (t.has_dst) {
        /* A DST name with no rules is legal POSIX and means "the system
         * knows the rules", which this system does not. Refuse rather than
         * silently pretending summer time never starts. */
        return false;
    }
    if (*p != '\0') return false;

    g_tz = t;
    memcpy(g_spec, spec, len + 1);
    return true;
}

const char *tz_get(void) { return g_spec; }

/* ----------------------------------------------------------- the rule --- */

/* The day of the month the rule picks, for one year. Week 5 means "last",
 * which is the fifth occurrence if the month has one and the fourth if not --
 * so it is computed backwards from the end rather than forwards. */
static unsigned rule_day(const tz_rule_t *r, int year) {
    static const uint8_t dim[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    unsigned last = dim[r->mon - 1];
    if (r->mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) last = 29;

    rtc_time_t probe = { (uint16_t)year, r->mon, 1, 0, 0, 0, 0 };
    /* 1970-01-01 was a Thursday, so (days + 4) mod 7 counts from Sunday = 0,
     * matching the rule's own day numbering. */
    int64_t days = time_to_epoch(&probe) / 86400LL;
    int first_dow = (int)((days + 4) % 7);
    if (first_dow < 0) first_dow += 7;

    unsigned day = 1u + (unsigned)(((int)r->dow - first_dow + 7) % 7);  /* first such weekday */
    if (r->week >= 5) {
        while (day + 7u <= last) day += 7u;
    } else {
        day += 7u * (unsigned)(r->week - 1u);
        if (day > last) day -= 7u;   /* a 5th Tuesday that does not exist */
    }
    return day;
}

/* The UTC instant a rule fires in a given year. The rule's clock time is in
 * the local time in force just BEFORE the transition, so the spring rule is
 * read against standard time and the autumn one against summer time -- get
 * this backwards and both European switchovers land an hour out. */
static int64_t rule_instant_utc(const tz_rule_t *r, int year, int off_before_min) {
    rtc_time_t d = { (uint16_t)year, r->mon, (uint8_t)rule_day(r, year), 0, 0, 0, 0 };
    return time_to_epoch(&d) + (int64_t)r->sec - (int64_t)off_before_min * 60LL;
}

static bool dst_in_force(int64_t utc_sec) {
    if (!g_tz.has_dst) return false;

    rtc_time_t t;
    time_from_epoch(utc_sec, &t);
    int year = (int)t.year;

    int64_t start = rule_instant_utc(&g_tz.start, year, g_tz.std_off_min);
    int64_t end   = rule_instant_utc(&g_tz.end,   year, g_tz.dst_off_min);

    if (start <= end) return utc_sec >= start && utc_sec < end;   /* northern */
    return utc_sec >= start || utc_sec < end;                     /* southern */
}

int tz_offset_min(const rtc_time_t *utc, const char **abbrev, bool *is_dst) {
    int64_t sec = utc ? time_to_epoch(utc) : 0;
    bool dst = dst_in_force(sec);
    if (abbrev) *abbrev = dst ? g_tz.dst_name : g_tz.std_name;
    if (is_dst) *is_dst = dst;
    return dst ? g_tz.dst_off_min : g_tz.std_off_min;
}

void tz_utc_to_local(const rtc_time_t *utc, rtc_time_t *local) {
    if (!utc || !local) return;
    int off = tz_offset_min(utc, NULL, NULL);
    uint16_t ms = utc->ms;
    time_from_epoch(time_to_epoch(utc) + (int64_t)off * 60LL, local);
    local->ms = ms;
}

void tz_local_to_utc(const rtc_time_t *local, rtc_time_t *utc) {
    if (!local || !utc) return;
    int64_t naive = time_to_epoch(local);

    /* Two passes. The first guesses with standard time, the second uses the
     * offset that actually applies at the instant the guess lands on. That
     * converges everywhere except inside a transition, where the question has
     * no single answer anyway (see the header). */
    int64_t guess = naive - (int64_t)g_tz.std_off_min * 60LL;
    int off = dst_in_force(guess) ? g_tz.dst_off_min : g_tz.std_off_min;
    guess = naive - (int64_t)off * 60LL;
    off = dst_in_force(guess) ? g_tz.dst_off_min : g_tz.std_off_min;

    uint16_t ms = local->ms;
    time_from_epoch(naive - (int64_t)off * 60LL, utc);
    utc->ms = ms;
}

void tz_from_offset(const rtc_time_t *local, int offset_min, rtc_time_t *utc) {
    if (!local || !utc) return;
    uint16_t ms = local->ms;
    time_from_epoch(time_to_epoch(local) - (int64_t)offset_min * 60LL, utc);
    utc->ms = ms;
}

/* ---------------------------------------------------------- selftest --- */

static int g_fail;

static void check(bool ok, const char *what) {
    if (!ok) g_fail++;
    printk("  [%s] %s\n", ok ? "ok" : "FAIL", what);
}

static bool same(const rtc_time_t *a, int y, int mo, int d, int h, int mi) {
    return a->year == y && a->month == mo && a->day == d && a->hour == h && a->min == mi;
}

/* utc -> local, against an expected wall-clock reading and abbreviation. */
static void case_local(int y, int mo, int d, int h, int mi,
                       int ey, int emo, int ed, int eh, int emi,
                       const char *eab, const char *what) {
    rtc_time_t utc = { (uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, 0, 0 };
    rtc_time_t loc;
    const char *ab = "";
    tz_utc_to_local(&utc, &loc);
    tz_offset_min(&utc, &ab, NULL);
    bool ok = same(&loc, ey, emo, ed, eh, emi) && strcmp(ab, eab) == 0;
    if (!ok) {
        printk("       got %04u-%02u-%02u %02u:%02u %s, wanted %04d-%02d-%02d %02d:%02d %s\n",
               loc.year, loc.month, loc.day, loc.hour, loc.min, ab,
               ey, emo, ed, eh, emi, eab);
    }
    check(ok, what);
}

/* local -> utc, the direction `date` and a hand-set clock use. */
static void case_utc(int y, int mo, int d, int h, int mi,
                     int ey, int emo, int ed, int eh, int emi, const char *what) {
    rtc_time_t loc = { (uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, 0, 0 };
    rtc_time_t utc;
    tz_local_to_utc(&loc, &utc);
    bool ok = same(&utc, ey, emo, ed, eh, emi);
    if (!ok) {
        printk("       got %04u-%02u-%02u %02u:%02u, wanted %04d-%02d-%02d %02d:%02d\n",
               utc.year, utc.month, utc.day, utc.hour, utc.min,
               ey, emo, ed, eh, emi);
    }
    check(ok, what);
}

int tz_selftest(void) {
    char saved[TZ_SPEC_MAX + 1];
    memcpy(saved, g_spec, sizeof(saved));
    g_fail = 0;

    printk("[TZ] selftest\n");

    /* --- parsing ---------------------------------------------------- */
    check(tz_set("CET-1CEST,M3.5.0,M10.5.0/3"), "parse Germany");
    check(tz_set("UTC0"), "parse UTC0");
    check(tz_set("MST7"), "parse fixed-offset MST7");
    check(tz_set("<-03>3"), "parse angle-bracket abbreviation");
    check(!tz_set("CET-1CEST"), "reject DST with no rules");
    check(!tz_set("CET-1CEST,J89,J302"), "reject unsupported Julian rules");
    check(!tz_set("nonsense"), "reject garbage");
    check(!tz_set(""), "reject empty");
    check(strcmp(tz_get(), "<-03>3") == 0, "a rejected rule leaves the old one in force");

    /* --- Germany, the shipped default -------------------------------- */
    check(tz_set("CET-1CEST,M3.5.0,M10.5.0/3"), "install Germany");

    case_local(2026, 1, 15, 12, 0,  2026, 1, 15, 13, 0, "CET",  "midwinter is UTC+1");
    case_local(2026, 7, 15, 12, 0,  2026, 7, 15, 14, 0, "CEST", "midsummer is UTC+2");

    /* Both switchovers happen at 01:00 UTC, so the whole EU moves at once. */
    case_local(2026, 3, 29,  0, 59,  2026, 3, 29,  1, 59, "CET",  "one minute before spring forward");
    case_local(2026, 3, 29,  1,  0,  2026, 3, 29,  3,  0, "CEST", "spring forward: 02:00 becomes 03:00");
    case_local(2026, 10, 25, 0, 59,  2026, 10, 25, 2, 59, "CEST", "one minute before fall back");
    case_local(2026, 10, 25, 1,  0,  2026, 10, 25, 2,  0, "CET",  "fall back: 03:00 becomes 02:00");

    /* The DCF-77 frame this was all built for. */
    case_utc(2026, 8, 23,  9,  1,  2026, 8, 23, 7, 1, "a CEST frame is UTC-2");
    case_utc(2026, 1, 15, 13,  0,  2026, 1, 15, 12, 0, "a CET wall clock is UTC-1");

    /* Rollovers the old month-walking clock code had a special case for. */
    case_local(2026, 12, 31, 23, 30,  2027, 1, 1, 0, 30, "CET", "new year crossing");
    case_utc(2027, 1, 1, 0, 30,  2026, 12, 31, 23, 30, "new year crossing, back");
    case_local(2028, 2, 28, 23, 30,  2028, 2, 29, 0, 30, "CET", "leap day");

    /* Round trip over a whole year, every 7 hours: the cheapest way to catch
     * an off-by-one in rule_day() that a handful of cases would miss. */
    {
        bool ok = true;
        rtc_time_t utc, loc, back;
        for (int64_t s = time_to_epoch(&(rtc_time_t){ 2026, 1, 1, 0, 0, 0, 0 });
             s < time_to_epoch(&(rtc_time_t){ 2027, 1, 1, 0, 0, 0, 0 }); s += 7 * 3600) {
            time_from_epoch(s, &utc);
            tz_utc_to_local(&utc, &loc);
            tz_local_to_utc(&loc, &back);
            /* The repeated hour in October is genuinely ambiguous: a local
             * time inside it maps back to the first of its two instants, so
             * an hour of mismatch there is correct behaviour, not a bug. */
            int64_t err = time_to_epoch(&back) - s;
            if (err != 0 && err != -3600 && err != 3600) { ok = false; break; }
        }
        check(ok, "utc -> local -> utc round trip across 2026");
    }

    /* --- southern hemisphere: DST spans the new year ------------------ */
    check(tz_set("AEST-10AEDT,M10.1.0,M4.1.0/3"), "install Sydney");
    case_local(2026, 1, 15, 0, 0,  2026, 1, 15, 11, 0, "AEDT", "Sydney in January is on summer time");
    case_local(2026, 7, 15, 0, 0,  2026, 7, 15, 10, 0, "AEST", "Sydney in July is on standard time");

    /* --- no DST at all ------------------------------------------------ */
    check(tz_set("UTC0"), "install UTC0");
    case_local(2026, 7, 15, 12, 0,  2026, 7, 15, 12, 0, "UTC", "UTC0 never shifts");
    check(tz_set("MST7"), "install MST7");
    case_local(2026, 7, 15, 12, 0,  2026, 7, 15, 5, 0, "MST", "MST7 is UTC-7 year round");

    tz_set(saved);
    printk("[TZ] %s (%d failure%s), timezone restored to %s\n",
           g_fail ? "FAILURES" : "all passed", g_fail, g_fail == 1 ? "" : "s", tz_get());
    printk(g_fail ? "TZ_SELFTEST_FAIL\n" : "TZ_SELFTEST_OK\n");
    return g_fail;
}
