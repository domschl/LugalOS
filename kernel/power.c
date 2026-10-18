#include "kernel/power.h"
#include "drivers/battery.h"
#include "drivers/ina226.h"
#include "drivers/sensor_hub.h"
#include "fs/vfs.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

typedef struct {
    bool    have;           /* a monitor reading in the hub's cache */
    int32_t mv, ma, mw;
    bool    have_soc;
    int32_t soc_pm;
    int32_t minutes;        /* -1: not meaningful */
    uint32_t age_s;
} power_now_t;

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

static void fmt_chan(sensor_chan_t ch, int32_t v, char *buf, uint32_t cap) {
    sensor_format_channel(ch, v, buf, cap);
}

static void power_print(void) {
    power_now_t p;
    power_now(&p);
    if (!p.have) {
        cprintf(ina226_is_detected() ? "power: the monitor has not been read yet\n"
                                     : "power: no battery or power monitor on this board\n");
        return;
    }
    char v[24], i[24], w[24];
    fmt_chan(SENSOR_CHAN_VOLTAGE, p.mv, v, sizeof v);
    fmt_chan(SENSOR_CHAN_CURRENT, p.ma, i, sizeof i);
    fmt_chan(SENSOR_CHAN_POWER, p.mw, w, sizeof w);
    cprintf("%s  %s  %s  %s", battery_board() ? "battery" : "ina226", v, i, w);
    if (p.have_soc) {
        cprintf("  %ld.%ld %%  %s", (long)(p.soc_pm / 10), (long)(p.soc_pm % 10), power_state(&p));
        if (p.minutes >= 0) {
            cprintf(", %ld h %02ld min to %s", (long)(p.minutes / 60), (long)(p.minutes % 60),
                    p.ma > 0 ? "full" : "empty");
        }
    }
    cprintf("\n  read %lu s ago, every 1 s\n", (unsigned long)p.age_s);
}

uint32_t power_ctl_read(char *buf, uint32_t cap) {
    power_now_t p;
    power_now(&p);
    if (!p.have) return (uint32_t)ksnprintf(buf, cap, "state=none\n");
    int n = ksnprintf(buf, cap, "mv=%ld\nma=%ld\nmw=%ld\nstate=%s\nage_s=%lu\n",
                      (long)p.mv, (long)p.ma, (long)p.mw, power_state(&p), (unsigned long)p.age_s);
    if (n < 0) return 0;
    if (p.have_soc && (uint32_t)n < cap) {
        int m = ksnprintf(buf + n, cap - (uint32_t)n, "soc_pm=%ld\nminutes=%ld\n",
                          (long)p.soc_pm, (long)p.minutes);
        if (m > 0) n += m;
    }
    return (uint32_t)n;
}

/* `power log FILE [s]`: one CSV line a second, for fitting the battery curve
 * (tools/batt_fit.py). A task of its own, so the shell stays free for the
 * hours a discharge takes; `power log stop` ends it early. */
static struct {
    volatile bool active;
    volatile bool stop;
    char     path[96];
    uint32_t seconds;       /* 0: until stopped */
    uint32_t lines;
} g_log;

static void power_log_task(void *arg) {
    (void)arg;
    static const char hdr[] = "t_s,mv,ma,mw,soc_pm\n";
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
        int n = ksnprintf(line, sizeof line, "%lu,%ld,%ld,%ld,%ld\n",
                          (unsigned long)t, (long)p.mv, (long)p.ma, (long)p.mw,
                          (long)(p.have_soc ? p.soc_pm : -1));
        if (n > 0 && vfs_append(g_log.path, line, (uint32_t)n) >= 0) g_log.lines++;
        uint64_t next = t0 + (uint64_t)(t + 1u) * 1000u, now = time_get_ms();
        if (next > now) task_sleep_ms(next - now);
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

void power_cmd(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) { power_print(); return; }
    if (strncmp(arg, "log", 3) == 0 && (arg[3] == '\0' || arg[3] == ' ')) { power_log_cmd(arg + 3); return; }
    cprintf("usage: power | power log FILE [seconds] | power log stop\n");
}
