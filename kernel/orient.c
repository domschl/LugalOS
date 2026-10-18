#include "kernel/orient.h"
#include "drivers/bmi270.h"
#include "kernel/console.h"
#include "kernel/identity.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <string.h>

#if defined(CONFIG_BOARD_TAB5)
#include "drivers/dsi_esp32p4.h"
#endif

/* The rotation that reads upright when gravity points along each IMU axis
 * (+x, -x, +y, -y), measured on the board: CONFIG_IMU_ROT_*. */
#if defined(CONFIG_IMU_ROT_XPOS)
static const int k_rot[4] = { CONFIG_IMU_ROT_XPOS, CONFIG_IMU_ROT_XNEG,
                              CONFIG_IMU_ROT_YPOS, CONFIG_IMU_ROT_YNEG };
#define ORIENT_PRESENT 1
#else
static const int k_rot[4] = { 270, 90, 0, 180 };
#define ORIENT_PRESENT 0
#endif

#define FLAT_MG        800      /* |z| above this: lying flat, keep the display */
#define MIN_MG         500      /* the in-plane axis must carry at least this much */
#define MOVING_DDPS    600      /* rotating faster than 60 deg/s: wait */
#define HOLD_MS        600u     /* a new direction must last this long */

void orient_state_init(orient_state_t *st, int shown) {
    st->cand = -1;
    st->cand_since_ms = 0;
    st->shown = shown;
}

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

int orient_step(orient_state_t *st, int32_t ax, int32_t ay, int32_t az,
                int32_t gx, int32_t gy, int32_t gz, uint64_t now_ms) {
    int dir = -1;
    bool moving = iabs(gx) > MOVING_DDPS || iabs(gy) > MOVING_DDPS || iabs(gz) > MOVING_DDPS;
    if (!moving && iabs(az) <= FLAT_MG) {
        int64_t x2 = (int64_t)ax * ax, y2 = (int64_t)ay * ay;
        /* dominant by more than 30 deg: a^2 > 3 b^2 */
        if (x2 > 3 * y2 && iabs(ax) >= MIN_MG) dir = ax > 0 ? 0 : 1;
        else if (y2 > 3 * x2 && iabs(ay) >= MIN_MG) dir = ay > 0 ? 2 : 3;
    }
    int rot = dir >= 0 ? k_rot[dir] : -1;
    if (rot < 0 || rot == st->shown) {
        st->cand = -1;
        return -1;
    }
    if (rot != st->cand) {
        st->cand = rot;
        st->cand_since_ms = now_ms;
        return -1;
    }
    if (now_ms - st->cand_since_ms < HOLD_MS) return -1;
    st->cand = -1;
    st->shown = rot;
    return rot;
}

uint32_t orient_selftest(bool report) {
    /* traces at 100 ms: {ax, ay, az, gz} per step; expect the final return */
    typedef struct { int32_t ax, ay, az, gz; } smp_t;
    static const struct {
        const char *what;
        int start;
        smp_t s[10];
        int n;
        int want_last;      /* what the last step returns */
        int want_switches;  /* how many steps return >= 0 */
    } cases[] = {
        { "held upright in landscape", 0, { {1000,0,0,0},{1000,0,0,0},{1000,0,0,0},{1000,0,0,0},
          {1000,0,0,0},{1000,0,0,0},{1000,0,0,0},{1000,0,0,0} }, 8, -1, 1 },
        { "portrait clockwise", 270, { {0,-1000,0,0},{0,-1000,0,0},{0,-1000,0,0},{0,-1000,0,0},
          {0,-1000,0,0},{0,-1000,0,0},{0,-1000,0,0},{0,-1000,0,0} }, 8, -1, 1 },
        { "flat on the table", 270, { {0,-100,-990,0},{0,-100,-990,0},{0,-100,-990,0},{0,-100,-990,0},
          {0,-100,-990,0},{0,-100,-990,0},{0,-100,-990,0},{0,-100,-990,0} }, 8, -1, 0 },
        { "tilted 20 deg only", 270, { {940,-342,0,0},{940,-342,0,0},{940,-342,0,0},{940,-342,0,0},
          {940,-342,0,0},{940,-342,0,0},{940,-342,0,0},{940,-342,0,0} }, 8, -1, 0 },
        { "while turning", 270, { {0,-1000,0,900},{0,-1000,0,900},{0,-1000,0,900},{0,-1000,0,900},
          {0,-1000,0,900},{0,-1000,0,900},{0,-1000,0,900},{0,-1000,0,900} }, 8, -1, 0 },
        { "brief tip, back", 270, { {0,-1000,0,0},{0,-1000,0,0},{0,-1000,0,0},{1000,0,0,0},
          {1000,0,0,0},{0,-1000,0,0},{0,-1000,0,0},{1000,0,0,0} }, 8, -1, 0 },
        { "upside-down landscape", 270, { {-1000,0,0,0},{-1000,0,0,0},{-1000,0,0,0},{-1000,0,0,0},
          {-1000,0,0,0},{-1000,0,0,0},{-1000,0,0,0},{-1000,0,0,0} }, 8, -1, 1 },
    };
    uint32_t failed = 0;
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        orient_state_t st;
        orient_state_init(&st, cases[c].start);
        int last = -1, switches = 0;
        for (int i = 0; i < cases[c].n; i++) {
            const smp_t *p = &cases[c].s[i];
            last = orient_step(&st, p->ax, p->ay, p->az, 0, 0, p->gz, (uint64_t)i * 100u);
            if (last >= 0) switches++;
        }
        if (switches != cases[c].want_switches || last != cases[c].want_last) {
            failed++;
            if (report) cprintf("  orient '%s': %d switches, want %d\n", cases[c].what, switches,
                                cases[c].want_switches);
        }
    }
    if (report) cprintf("orient selftest: %lu case%s failed\n", (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

/* --- The task and the command ------------------------------------------- */

typedef enum { ORIENT_OFF, ORIENT_AUTO, ORIENT_PAUSED } orient_mode_t;
static volatile orient_mode_t g_mode = ORIENT_OFF;
static int g_task = -1;

static int display_rot(void) {
#if defined(CONFIG_BOARD_TAB5)
    return dsi_lcd_get_orientation();
#else
    return 0;
#endif
}

static void display_set(int rot) {
#if defined(CONFIG_BOARD_TAB5)
    dsi_lcd_set_orientation(rot);
    console_sync_readers();
#else
    (void)rot;
#endif
}

static void orient_task(void *arg) {
    (void)arg;
    orient_state_t st;
    orient_state_init(&st, display_rot());
    for (;;) {
        if (g_mode != ORIENT_AUTO) {
            task_sleep_ms(500);
            orient_state_init(&st, display_rot());
            continue;
        }
        task_sleep_ms(100);
        bmi270_reading_t r;
        if (!bmi270_read(&r)) continue;
        st.shown = display_rot();
        int rot = orient_step(&st, r.ax, r.ay, r.az, r.gx, r.gy, r.gz, time_get_ms());
        if (rot >= 0 && g_mode == ORIENT_AUTO) display_set(rot);
    }
}

void orient_init(void) {
    if (!ORIENT_PRESENT || !bmi270_is_detected()) return;
    bool on = false;
    if (node_orient_auto(&on) && on) g_mode = ORIENT_AUTO;
    /* Turning the display runs the terminal's whole repaint and frame
     * conversion on this task, the same depth as a shell's: 16 KB. */
    g_task = task_create_sized("orient", orient_task, NULL, 4);
}

void orient_manual(void) {
    if (g_mode == ORIENT_AUTO) g_mode = ORIENT_PAUSED;
}

static const char *mode_name(orient_mode_t m) {
    return m == ORIENT_AUTO ? "auto" : m == ORIENT_PAUSED ? "paused (a manual turn; `orient auto` resumes)" : "off";
}

void orient_cmd(const char *arg) {
    while (*arg == ' ') arg++;
    if (!ORIENT_PRESENT || g_task < 0) { cprintf("orient: no IMU-driven display on this board\n"); return; }
    if (!*arg) {
        cprintf("orient: %s, display at %d deg\n", mode_name(g_mode), display_rot());
        return;
    }
    bool on;
    if (strcmp(arg, "auto") == 0) on = true;
    else if (strcmp(arg, "off") == 0) on = false;
    else { cprintf("usage: orient [auto|off]\n"); return; }
    g_mode = on ? ORIENT_AUTO : ORIENT_OFF;
    if (node_identity_set_orient_auto(on) != NODE_ID_OK) cprintf("orient: set, but not stored\n");
    cprintf("orient: %s\n", mode_name(g_mode));
}

static void imu_print(const bmi270_reading_t *r) {
    char ax[16], ay[16], az[16], gx[16], gy[16], gz[16];
    sensor_format_channel(SENSOR_CHAN_ACCEL_X, r->ax, ax, sizeof ax);
    sensor_format_channel(SENSOR_CHAN_ACCEL_Y, r->ay, ay, sizeof ay);
    sensor_format_channel(SENSOR_CHAN_ACCEL_Z, r->az, az, sizeof az);
    sensor_format_channel(SENSOR_CHAN_GYRO_X, r->gx, gx, sizeof gx);
    sensor_format_channel(SENSOR_CHAN_GYRO_Y, r->gy, gy, sizeof gy);
    sensor_format_channel(SENSOR_CHAN_GYRO_Z, r->gz, gz, sizeof gz);
    int32_t p, rl;
    bmi270_tilt(r, &p, &rl);
    cprintf("accel %s %s %s  gyro %s %s %s  pitch %ld.%ld roll %ld.%ld\n",
            ax, ay, az, gx, gy, gz,
            (long)(p / 10), (long)((p < 0 ? -p : p) % 10),
            (long)(rl / 10), (long)((rl < 0 ? -rl : rl) % 10));
}

void imu_cmd(const char *arg) {
    while (*arg == ' ') arg++;
    if (!bmi270_is_detected()) { cprintf("imu: no IMU on this board\n"); return; }
    if (strcmp(arg, "reload") == 0) {
        bmi270_reload();
        uint64_t t0 = time_get_ms();
        bmi270_reading_t r;
        bool ok = bmi270_read(&r);
        cprintf("imu: microcode %s in %lu ms\n", ok ? "loaded" : "NOT loaded",
                (unsigned long)(time_get_ms() - t0));
        return;
    }
    if (strncmp(arg, "watch", 5) == 0) {
        const char *s = arg + 5;
        while (*s == ' ') s++;
        uint32_t secs = 0;
        while (*s >= '0' && *s <= '9') secs = secs * 10u + (uint32_t)(*s++ - '0');
        if (!secs) secs = 10;
        cprintf("imu: 5 readings a second for %lu s (q to stop)\n", (unsigned long)secs);
        for (uint32_t i = 0; i < secs * 5u; i++) {
            bmi270_reading_t r;
            if (bmi270_read(&r)) imu_print(&r);
            else cprintf("imu: read failed\n");
            uint64_t until = time_get_ms() + 200u;
            while (time_get_ms() < until) {
                if (console_has_char() && console_getc() == 'q') return;
                task_sleep_ms(20);
            }
        }
        return;
    }
    if (*arg) { cprintf("usage: imu | imu watch [seconds] | imu reload\n"); return; }
    bmi270_reading_t r;
    if (!bmi270_read(&r)) { cprintf("imu: read failed\n"); return; }
    imu_print(&r);
}
