#ifndef KERNEL_ORIENT_H
#define KERNEL_ORIENT_H

#include <stdbool.h>
#include <stdint.h>

/* The IMU from the shell: `imu`, `imu watch [s]`, `imu reload`. */
void imu_cmd(const char *arg);

/* Auto-orientation: the display turns with the board, from the IMU's
 * gravity vector. `orient`, `orient auto|off`; stored in the identity record,
 * off unless set. A manual turn (lcd orient, Super+O, the reader) pauses it
 * until `orient auto`. */
void orient_cmd(const char *arg);

/* Starts the task (on a board with an IMU and a rotatable display) and
 * applies the stored mode. */
void orient_init(void);

/* Called by every manual orientation change. */
void orient_manual(void);

/* The decision, separate from the IMU and the display so it can be tested.
 * Feed one sample at a time (accel in mg, gyro in 0.1 deg/s); returns the
 * rotation to switch to, or -1 to leave the display as it is. */
typedef struct {
    int      cand;          /* candidate rotation, -1 = none */
    uint64_t cand_since_ms;
    int      shown;         /* the rotation last returned (or set) */
} orient_state_t;

void orient_state_init(orient_state_t *st, int shown);
int  orient_step(orient_state_t *st, int32_t ax, int32_t ay, int32_t az,
                 int32_t gx, int32_t gy, int32_t gz, uint64_t now_ms);

uint32_t orient_selftest(bool report);

#endif /* KERNEL_ORIENT_H */
