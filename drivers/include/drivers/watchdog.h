#ifndef LUGALOS_DRIVERS_WATCHDOG_H
#define LUGALOS_DRIVERS_WATCHDOG_H

#include <stdbool.h>

/* The hardware watchdog (45.8; drivers/wdt_esp32c6.c is the one backend so far). Armed once the
 * scheduler exists, fed by a task at normal priority: the board resets itself when ordinary tasks
 * stop running. */
void watchdog_start(void);
/* `wdt`: armed or not, feeds, and the cause of the last reset. */
void watchdog_report(void);
/* `wdt test`: stop feeding, to prove the reset path. */
void watchdog_withhold(void);
/* The cause of the last reset, as the ROM reports it. */
const char *watchdog_reset_reason(void);
bool watchdog_last_reset_was_watchdog(void);

#endif
