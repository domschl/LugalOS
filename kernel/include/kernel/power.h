#ifndef KERNEL_POWER_H
#define KERNEL_POWER_H

#include <stdint.h>

/* The board's power: battery, charge level, and (later) the charger.
 *
 * Readers take the sensor hub's cache (drivers/ina226.c, drivers/battery.c)
 * and never touch the I2C bus, so /dev/power is safe to read from the 9P
 * task. */

/* `power` and its subcommands; arg is what follows "power" ("" for none). */
void power_cmd(const char *arg);

/* /dev/power: key=value lines (mv=, ma=, mw=, soc_pm=, state=, minutes=). */
uint32_t power_ctl_read(char *buf, uint32_t cap);

#endif /* KERNEL_POWER_H */
