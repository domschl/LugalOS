#include "drivers/battery.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
#include <string.h>

#define SOC_FULL_PPM 1000000

/* The usual Li-ion/Li-Po resting-voltage curve, per cell 3.00 V (0 %) to
 * 4.20 V (100 %), with the voltage normalised to 0..1000 over that span.
 * battery_soc_from_ocv_ppm() lays it over the board's own empty..full pack
 * range instead, so the curve's shape is kept and its ends are the board's
 * measured ones. A measured discharge (tools/batt_fit.py) replaces this. */
static const struct { int16_t u; int16_t soc; } k_curve[] = {
    {    0,    0 }, {  508,   50 }, {  575,  100 }, {  592,  150 }, {  608,  200 },
    {  625,  250 }, {  642,  300 }, {  658,  350 }, {  667,  400 }, {  683,  450 },
    {  700,  500 }, {  708,  550 }, {  725,  600 }, {  758,  650 }, {  792,  700 },
    {  817,  750 }, {  850,  800 }, {  900,  850 }, {  925,  900 }, {  958,  950 },
    { 1000, 1000 },
};
#define CURVE_N (sizeof(k_curve) / sizeof(k_curve[0]))

int32_t battery_soc_from_ocv_ppm(const battery_params_t *p, int32_t ocv_mv) {
    int32_t span = (int32_t)p->full_mv - (int32_t)p->empty_mv;
    if (span <= 0) return 0;
    /* u in 1/1000000 of the span, so interpolation keeps ppm resolution */
    int64_t u = ((int64_t)(ocv_mv - (int32_t)p->empty_mv) * 1000000) / span;
    if (u <= 0) return 0;
    if (u >= 1000000) return SOC_FULL_PPM;
    for (unsigned i = 1; i < CURVE_N; i++) {
        int64_t u1 = (int64_t)k_curve[i].u * 1000;
        if (u > u1) continue;
        int64_t u0 = (int64_t)k_curve[i - 1].u * 1000;
        int64_t s0 = (int64_t)k_curve[i - 1].soc * 1000, s1 = (int64_t)k_curve[i].soc * 1000;
        return (int32_t)(s0 + (s1 - s0) * (u - u0) / (u1 - u0));
    }
    return SOC_FULL_PPM;
}

static int32_t ocv_mv(const battery_params_t *p, int32_t mv, int32_t ma) {
    return mv - (int32_t)(((int64_t)ma * p->rint_mohm) / 1000);
}

void battery_update(battery_t *b, int32_t mv, int32_t ma, uint64_t now_ms) {
    int32_t target = battery_soc_from_ocv_ppm(&b->p, ocv_mv(&b->p, mv, ma));
    uint64_t dt = now_ms - b->last_ms;
    /* A gap of over a minute (sampling stopped, a long busy spell) is a new
     * start: the current over it is unknown. */
    if (!b->started || now_ms < b->last_ms || dt > 60000u) {
        b->soc_ppm = target;
        b->started = true;
    } else {
        if (b->p.capacity_mah) {
            /* ppm = mA * ms * 1e6 / (mAh * 3600000 ms/h) */
            b->soc_ppm += (int32_t)(((int64_t)ma * (int64_t)dt * 10) /
                                    ((int64_t)b->p.capacity_mah * 36));
        }
        b->soc_ppm += (target - b->soc_ppm) / 256;
        if (b->soc_ppm < 0) b->soc_ppm = 0;
        if (b->soc_ppm > SOC_FULL_PPM) b->soc_ppm = SOC_FULL_PPM;
    }
    b->last_ms = now_ms;
    b->last_mv = mv;
    b->last_ma = ma;
}

bool battery_soc(const battery_t *b, int32_t *soc_pm) {
    if (!b || !b->started) return false;
    if (soc_pm) *soc_pm = (b->soc_ppm + 500) / 1000;
    return true;
}

int32_t battery_minutes_left(const battery_t *b) {
    if (!b || !b->started || !b->p.capacity_mah) return -1;
    int64_t ma = b->last_ma;
    int64_t ppm = ma < 0 ? b->soc_ppm : SOC_FULL_PPM - b->soc_ppm;
    if (ma < 0) ma = -ma;
    if (ma < 20) return -1;
    /* mAh left = ppm * capacity / 1e6; minutes = mAh * 60 / mA */
    return (int32_t)((ppm * (int64_t)b->p.capacity_mah * 60) / (ma * 1000000));
}

#if defined(CONFIG_BATT_CELLS)
static battery_t g_board = {
    .p = {
        .cells        = CONFIG_BATT_CELLS,
        .empty_mv     = CONFIG_BATT_EMPTY_MV,
        .full_mv      = CONFIG_BATT_FULL_MV,
        .rint_mohm    = CONFIG_BATT_RINT_MOHM,
        .capacity_mah = CONFIG_BATT_CAPACITY_MAH,
    },
};
battery_t *battery_board(void) { return &g_board; }
#else
battery_t *battery_board(void) { return NULL; }
#endif

bool battery_board_charging(void) {
    battery_t *b = battery_board();
    return b && b->started && b->last_ma > 20;
}

uint32_t battery_indicator(char *buf, uint32_t cap) {
    int32_t pm = 0;
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    if (!battery_soc(battery_board(), &pm)) return 0;
    battery_t *b = battery_board();
    int n = ksnprintf(buf, cap, "%ld%%%s", (long)((pm + 5) / 10),
                      battery_board_charging() ? "+" : b->low ? "!" : "");
    return n > 0 ? (uint32_t)n : 0u;
}

uint32_t battery_selftest(bool report) {
    uint32_t failed = 0;
    /* The Tab5's pack: 2S, 6.00 V empty, 8.23 V full */
    const battery_params_t tab5 = { 2, 6000, 8230, 150, 2000 };

    static const struct { int32_t ocv_mv, want_ppm; } curve[] = {
        { 5000,       0 },        /* below empty */
        { 6000,       0 },
        { 7561,  500000 },        /* u = 700 exactly: the 50 % knot */
        { 8230, 1000000 },
        { 9000, 1000000 },        /* above full */
    };
    for (unsigned i = 0; i < sizeof(curve) / sizeof(curve[0]); i++) {
        int32_t got = battery_soc_from_ocv_ppm(&tab5, curve[i].ocv_mv);
        if (got != curve[i].want_ppm) {
            failed++;
            if (report) cprintf("  battery curve %u: %ld mV -> %ld ppm, want %ld\n", i,
                                (long)curve[i].ocv_mv, (long)got, (long)curve[i].want_ppm);
        }
    }
    /* Monotonic across the whole range */
    int32_t prev = -1;
    for (int32_t mv = 5900; mv <= 8300; mv += 7) {
        int32_t s = battery_soc_from_ocv_ppm(&tab5, mv);
        if (s < prev) {
            failed++;
            if (report) cprintf("  battery curve not monotonic at %ld mV\n", (long)mv);
            break;
        }
        prev = s;
    }

    /* Under 400 mA of load the terminal reads 60 mV low; the first sample
     * still starts at the resting level's 50 %. */
    battery_t b;
    memset(&b, 0, sizeof b);
    b.p = tab5;
    battery_update(&b, 7561 - 60, -400, 1000);
    int32_t pm = 0;
    if (!battery_soc(&b, &pm) || pm != 500) {
        failed++;
        if (report) cprintf("  battery IR compensation: %ld, want 500\n", (long)pm);
    }

    /* One hour at 1 A from 2000 mAh with the voltage held at 50 %: the count
     * alone would fall to 0 %, the voltage alone stays at 50 %; the blend
     * settles 256 steps of 139 ppm below the voltage, about 46.4 %. */
    battery_params_t norint = tab5;
    norint.rint_mohm = 0;
    memset(&b, 0, sizeof b);
    b.p = norint;
    for (uint32_t t = 0; t <= 3600; t++) battery_update(&b, 7561, -1000, 1000u + t * 1000u);
    battery_soc(&b, &pm);
    if (pm < 460 || pm > 470) {
        failed++;
        if (report) cprintf("  battery blend: %ld, want 460..470\n", (long)pm);
    }

    /* 50 % of 2000 mAh at 1 A: an hour */
    memset(&b, 0, sizeof b);
    b.p = norint;
    battery_update(&b, 7561, -1000, 1000);
    int32_t mins = battery_minutes_left(&b);
    if (mins != 60) {
        failed++;
        if (report) cprintf("  battery minutes left: %ld, want 60\n", (long)mins);
    }

    if (report) cprintf("battery selftest: %lu case%s failed\n",
                        (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}
