#ifndef DRIVERS_CHARGER_H
#define DRIVERS_CHARGER_H

#include <stdbool.h>
#include <stdint.h>

/* The board's battery charger and its power switch.
 *
 * On the M5Stack Tab5: an IP2326 2S charger, controlled through the PI4IOE
 * expander at 0x44 -- P7 CHG_EN (1 = charge), P5 nCHG_QC_EN (0 = the fast
 * mode), P6 CHG_STAT (input) -- and the PMS150G power supervisor, which
 * switches the board off after a train of pulses on P4 PWROFF_PULSE. The
 * modes follow M5Unified's setChargeCurrent(): off, slow (its "500 mA"),
 * fast (its "1000 mA"). Elsewhere every call reports no charger. */

typedef enum {
    CHARGE_OFF  = 0,
    CHARGE_SLOW = 1,
    CHARGE_FAST = 2,
} charge_mode_t;

bool charger_present(void);
bool charger_set_mode(charge_mode_t m);
/* Read back from the expander, not remembered: what the pins are. */
bool charger_get_mode(charge_mode_t *m);
const char *charger_mode_name(charge_mode_t m);
bool charger_parse_mode(const char *s, charge_mode_t *m);

/* CHG_STAT as read (0 or 1), or -1 without a charger or on a bus error. */
int  charger_stat_raw(void);

/* Switches the board off. Returns only if that did not happen. */
void charger_power_off(void);

#endif /* DRIVERS_CHARGER_H */
