#include "kernel/orient.h"
#include "drivers/bmi270.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

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
