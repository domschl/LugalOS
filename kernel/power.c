#include "kernel/power.h"
#include "drivers/battery.h"
#include "drivers/charger.h"
#include "drivers/ina226.h"
#include "drivers/sensor_hub.h"
#include "fs/vfs.h"
#include "kernel/console.h"
#include "kernel/identity.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <string.h>

/* Below the warning voltage the menu bar shows "!"; below the cut-off for
 * CUTOFF_HOLD_MS while discharging, the board switches itself off -- before
 * the supervisor cuts it at the empty voltage, and with no write in flight.
 * The terminal voltage, not the corrected one: what browns the board out is
 * the sag under load. */
#if defined(CONFIG_BATT_EMPTY_MV)
#define WARN_MV   ((int32_t)CONFIG_BATT_EMPTY_MV + 600)
#define CUTOFF_MV ((int32_t)CONFIG_BATT_EMPTY_MV + 200)
#else
#define WARN_MV   0
#define CUTOFF_MV 0
#endif
#define CUTOFF_HOLD_MS 30000u
/* The charge limit's hysteresis: stop at the limit, resume this many % below. */
#define LIMIT_HYST_PCT 5

typedef struct {
    bool     have;           /* a monitor reading in the hub's cache */
    int32_t  mv, ma, mw;
    bool     have_soc;
    int32_t  soc_pm;
    int32_t  minutes;        /* -1: not meaningful */
    uint32_t age_s;
} power_now_t;

/* accepted: a shell sets the mode and the limit with single stores; the
 * power task alone keeps held, warned and low_since, and its next round
 * puts the charger right after a change. */
static struct {
    charge_mode_t mode;      /* configured */
    uint8_t       limit;     /* %, 0 = none */
    bool          held;      /* charging paused by the limit */
    bool          warned;
    uint64_t      low_since_ms;
    int           pid;
} g = { .mode = CHARGE_FAST, .pid = -1 };

static void power_now(power_now_t *p) {
    memset(p, 0, sizeof *p);
    p->minutes = -1;
    p->have = sensor_hub_get_dev(&ina226_sensor_dev, SENSOR_CHAN_VOLTAGE, &p->mv, &p->age_s) &&
              sensor_hub_get_dev(&ina226_sensor_dev, SENSOR_CHAN_CURRENT, &p->ma, NULL) &&
              sensor_hub_get_dev(&ina226_sensor_dev, SENSOR_CHAN_POWER, &p->mw, NULL);
    battery_t *b = battery_board();
    p->have_soc = battery_soc(b, &p->soc_pm);
    if (p->have_soc) p->minutes = battery_minutes_left(b);
}

static const char *power_state(const power_now_t *p) {
    if (p->ma > 20) return "charging";
    if (p->ma < -20) return "discharging";
    return "idle";
}

/* --- `power log` ------------------------------------------------------- */

/* One CSV line a second, for fitting the battery curve (tools/batt_fit.py).
 * A task of its own, so the shell stays free for the hours a discharge takes. */
/* accepted: `power log` starts the task and `power log stop` sets stop; the
 * task clears active when it ends. Two shells starting one at the same
 * instant could both start. */
static struct {
    volatile bool active;
    volatile bool stop;
    char     path[96];
    uint32_t seconds;       /* 0: until stopped */
    uint32_t lines;
} g_log;

static void power_log_task(void *arg) {
    (void)arg;
    static const char hdr[] = "t_s,mv,ma,mw,soc_pm,chg_stat\n";
    if (vfs_write(g_log.path, hdr, sizeof hdr - 1) < 0) {
        printk("[power] log: cannot write %s\n", g_log.path);
        g_log.active = false;
        return;
    }
    uint64_t t0 = time_get_ms();
    for (uint32_t t = 0; !g_log.stop && (g_log.seconds == 0 || t < g_log.seconds); t++) {
        power_now_t p;
        power_now(&p);
        char line[64];
        int n = ksnprintf(line, sizeof line, "%lu,%ld,%ld,%ld,%ld,%d\n",
                          (unsigned long)t, (long)p.mv, (long)p.ma, (long)p.mw,
                          (long)(p.have_soc ? p.soc_pm : -1), charger_stat_raw());
        if (n > 0 && vfs_append(g_log.path, line, (uint32_t)n) >= 0) g_log.lines++;
        uint64_t next = t0 + (uint64_t)(t + 1u) * 1000u, now = time_get_ms();
        if (next > now) task_sleep_ms((uint32_t)(next - now));
    }
    printk("[power] log: %lu lines in %s\n", (unsigned long)g_log.lines, g_log.path);
    g_log.active = false;
}

static void power_log_cmd(const char *arg) {
    while (*arg == ' ') arg++;
    if (strcmp(arg, "stop") == 0) {
        if (!g_log.active) { cprintf("power log: not running\n"); return; }
        g_log.stop = true;
        cprintf("power log: stopping (%lu lines in %s)\n", (unsigned long)g_log.lines, g_log.path);
        return;
    }
    if (!*arg) {
        if (g_log.active) cprintf("power log: %lu lines in %s\n", (unsigned long)g_log.lines, g_log.path);
        else cprintf("usage: power log FILE [seconds] | power log stop\n");
        return;
    }
    if (g_log.active) { cprintf("power log: already logging to %s\n", g_log.path); return; }
    uint32_t n = 0;
    while (arg[n] && arg[n] != ' ' && n + 1 < sizeof g_log.path) { g_log.path[n] = arg[n]; n++; }
    g_log.path[n] = '\0';
    const char *s = arg + n;
    while (*s == ' ') s++;
    uint32_t secs = 0;
    while (*s >= '0' && *s <= '9') secs = secs * 10u + (uint32_t)(*s++ - '0');
    g_log.seconds = secs;
    g_log.lines = 0;
    g_log.stop = false;
    g_log.active = true;
    if (task_create("power_log", power_log_task, NULL) < 0) {
        g_log.active = false;
        cprintf("power log: no task\n");
        return;
    }
    if (secs) cprintf("power log: %s, one line a second for %lu s\n", g_log.path, (unsigned long)secs);
    else cprintf("power log: %s, one line a second until `power log stop`\n", g_log.path);
}

/* --- Charging, the limit, switching off -------------------------------- */

static bool power_save(void) {
    return node_identity_set_power((uint8_t)g.mode, g.limit) == NODE_ID_OK;
}

static bool power_set_mode(charge_mode_t m) {
    g.mode = m;
    g.held = false;
    return charger_set_mode(m);
}

static void power_off_now(const char *why) {
    printk("[power] switching off: %s\n", why);
    if (g_log.active) {
        g_log.stop = true;
        for (int i = 0; i < 40 && g_log.active; i++) task_sleep_ms(50);
    }
    /* Files are written through and their FSInfo synced at close; what the
     * pause covers is a write another task has in flight right now. */
    task_sleep_ms(500);
    charger_power_off();
    printk("[power] the supervisor did not switch the board off\n");
}

static void power_task(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(2000);
        power_now_t p;
        power_now(&p);
        if (!p.have) continue;
        uint64_t now = time_get_ms();

        if (g.limit && g.mode != CHARGE_OFF && p.have_soc) {
            if (!g.held && p.soc_pm >= (int32_t)g.limit * 10) {
                if (charger_set_mode(CHARGE_OFF)) {
                    g.held = true;
                    printk("[power] at %u %%, the charge limit: charging paused\n", (unsigned)g.limit);
                }
            } else if (g.held && p.soc_pm <= (int32_t)(g.limit - LIMIT_HYST_PCT) * 10) {
                if (charger_set_mode(g.mode)) {
                    g.held = false;
                    printk("[power] below %u %%: charging resumed\n", (unsigned)(g.limit - LIMIT_HYST_PCT));
                }
            }
        }

        battery_t *b = battery_board();
        if (!b || !WARN_MV) continue;
        bool discharging = p.ma < -20;
        if (discharging && p.mv < WARN_MV && !g.warned) {
            g.warned = true;
            b->low = true;
            printk("[power] battery low: %ld mV\n", (long)p.mv);
        } else if (g.warned && (!discharging || p.mv > WARN_MV + 200)) {
            g.warned = false;
            b->low = false;
        }
        if (discharging && p.mv < CUTOFF_MV) {
            if (!g.low_since_ms) g.low_since_ms = now;
            else if (now - g.low_since_ms >= CUTOFF_HOLD_MS) power_off_now("battery below cut-off for 30 s");
        } else {
            g.low_since_ms = 0;
        }
    }
}

void power_init(void) {
    if (!charger_present() && !battery_board()) return;
    uint8_t mode = 0, limit = 0;
    if (node_power_settings(&mode, &limit) && mode <= CHARGE_FAST && limit <= 100) {
        g.limit = limit;
        if (charger_present()) power_set_mode((charge_mode_t)mode);
    } else if (charger_present()) {
        charge_mode_t m;
        if (charger_get_mode(&m)) g.mode = m;
    }
    g.pid = task_create_sized("power", power_task, NULL, 1);
}

/* --- What `power` shows and /dev/power reads --------------------------- */

static void power_print(void) {
    power_now_t p;
    power_now(&p);
    if (!p.have) {
        cprintf(ina226_is_detected() ? "power: the monitor has not been read yet\n"
                                     : "power: no battery or power monitor on this board\n");
        return;
    }
    char v[24], i[24], w[24];
    sensor_format_channel(SENSOR_CHAN_VOLTAGE, p.mv, v, sizeof v);
    sensor_format_channel(SENSOR_CHAN_CURRENT, p.ma, i, sizeof i);
    sensor_format_channel(SENSOR_CHAN_POWER, p.mw, w, sizeof w);
    cprintf("%s  %s  %s  %s", battery_board() ? "battery" : "ina226", v, i, w);
    if (p.have_soc) {
        cprintf("  %ld.%ld %%  %s", (long)(p.soc_pm / 10), (long)(p.soc_pm % 10), power_state(&p));
        if (p.minutes > 0) {
            cprintf(", %ld h %02ld min to %s", (long)(p.minutes / 60), (long)(p.minutes % 60),
                    p.ma > 0 ? "full" : "empty");
        }
    }
    cprintf("\n");
    if (charger_present()) {
        cprintf("charger  %s%s", charger_mode_name(g.mode), g.held ? " (paused at the limit)" : "");
        if (g.limit) cprintf(", limit %u %%", (unsigned)g.limit);
        cprintf(", CHG_STAT %d\n", charger_stat_raw());
    }
    cprintf("  read %lu s ago, every 1 s\n", (unsigned long)p.age_s);
}

uint32_t power_ctl_read(char *buf, uint32_t cap) {
    power_now_t p;
    power_now(&p);
    uint32_t n = 0;
    int k;
    if (!p.have) {
        k = ksnprintf(buf, cap, "state=none\n");
    } else {
        k = ksnprintf(buf, cap, "mv=%ld\nma=%ld\nmw=%ld\nstate=%s\nage_s=%lu\n",
                      (long)p.mv, (long)p.ma, (long)p.mw, power_state(&p), (unsigned long)p.age_s);
    }
    if (k > 0) n += (uint32_t)k;
    if (p.have_soc && n < cap) {
        k = ksnprintf(buf + n, cap - n, "soc_pm=%ld\nminutes=%ld\n", (long)p.soc_pm, (long)p.minutes);
        if (k > 0) n += (uint32_t)k;
    }
    if (charger_present() && n < cap) {
        k = ksnprintf(buf + n, cap - n, "charge=%s\nheld=%s\nlimit=%u\nchg_stat=%d\n",
                      charger_mode_name(g.mode), g.held ? "yes" : "no",
                      (unsigned)g.limit, charger_stat_raw());
        if (k > 0) n += (uint32_t)k;
    }
    return n;
}

/* The commands `power` and /dev/power share. Returns NULL on success, or
 * why not. */
static const char *power_do(const char *verb, const char *arg) {
    if (strcmp(verb, "charge") == 0) {
        charge_mode_t m;
        if (!charger_present()) return "no charger on this board";
        if (!charger_parse_mode(arg, &m)) return "charge off|slow|fast";
        if (!power_set_mode(m)) return "the charger did not answer";
        return power_save() ? NULL : "set, but not stored";
    }
    if (strcmp(verb, "limit") == 0) {
        if (!charger_present()) return "no charger on this board";
        unsigned v = 0;
        if (strcmp(arg, "off") == 0) {
            v = 0;
        } else {
            const char *s = arg;
            if (!*s) return "limit N|off";
            while (*s >= '0' && *s <= '9') v = v * 10u + (unsigned)(*s++ - '0');
            if (*s || v < 20 || v > 100) return "limit 20..100 or off";
            if (v == 100) v = 0;
        }
        g.limit = (uint8_t)v;
        if (g.held) power_set_mode(g.mode);
        return power_save() ? NULL : "set, but not stored";
    }
    if (strcmp(verb, "off") == 0) {
        if (!charger_present()) return "this board cannot switch itself off";
        power_off_now("requested");
        return "the supervisor did not switch the board off";
    }
    return "unknown command";
}

/* Splits "verb arg" in place of a copy of at most 31 bytes. */
static const char *power_line(const char *line, uint32_t len) {
    char b[32];
    uint32_t n = len < sizeof b - 1 ? len : (uint32_t)sizeof b - 1;
    memcpy(b, line, n);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r' || b[n - 1] == ' ')) n--;
    b[n] = '\0';
    char *arg = b;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = '\0';
    while (*arg == ' ') arg++;
    return power_do(b, arg);
}

int power_ctl_write(const char *buf, uint32_t len) {
    return power_line(buf, len) ? -1 : (int)len;
}

void power_cmd(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) { power_print(); return; }
    if (strncmp(arg, "log", 3) == 0 && (arg[3] == '\0' || arg[3] == ' ')) { power_log_cmd(arg + 3); return; }
    if (strcmp(arg, "charge") == 0 || strcmp(arg, "limit") == 0) {
        if (!charger_present()) { cprintf("power: no charger on this board\n"); return; }
        cprintf("charger %s, limit %s%u%s\n", charger_mode_name(g.mode),
                g.limit ? "" : "none", (unsigned)g.limit, g.limit ? " %" : "");
        return;
    }
    if (strncmp(arg, "charge ", 7) == 0 || strncmp(arg, "limit ", 6) == 0 || strcmp(arg, "off") == 0) {
        const char *err = power_line(arg, (uint32_t)strlen(arg));
        if (err) cprintf("power: %s\n", err);
        else power_print();
        return;
    }
    cprintf("usage: power | power charge [off|slow|fast] | power limit [N|off] | power off\n"
            "       power log FILE [seconds] | power log stop\n");
}
