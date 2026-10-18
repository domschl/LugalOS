#ifndef KERNEL_POWER_H
#define KERNEL_POWER_H

#include <stdint.h>

/* The board's power: battery, charge level, and (later) the charger.
 *
 * Readers take the sensor hub's cache (drivers/ina226.c, drivers/battery.c)
 * and never touch the I2C bus, so /dev/power is safe to read from the 9P
 * task. */

/* Applies the stored charge mode and limit and starts the task that keeps the
 * limit and switches the board off on a flat battery. After the sensor hub's
 * sampler; a no-op without a battery or charger. */
void power_init(void);

/* `power` and its subcommands; arg is what follows "power" ("" for none). */
void power_cmd(const char *arg);

/* /dev/power: key=value lines (mv=, ma=, mw=, state=, soc_pm=, minutes=,
 * charge=, held=, limit=, chg_stat=). */
uint32_t power_ctl_read(char *buf, uint32_t cap);

/* /dev/power commands, one per write: `charge off|slow|fast`, `limit N|off`,
 * `off`. Returns len, or -1 for a command refused. */
int power_ctl_write(const char *buf, uint32_t len);

#endif /* KERNEL_POWER_H */
