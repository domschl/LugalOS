#include "drivers/charger.h"
#include "drivers/i2c_bus.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <string.h>

const char *charger_mode_name(charge_mode_t m) {
    switch (m) {
    case CHARGE_OFF:  return "off";
    case CHARGE_SLOW: return "slow";
    case CHARGE_FAST: return "fast";
    default:          return "?";
    }
}

bool charger_parse_mode(const char *s, charge_mode_t *m) {
    if (!s || !m) return false;
    if (strcmp(s, "off") == 0)  { *m = CHARGE_OFF;  return true; }
    if (strcmp(s, "slow") == 0) { *m = CHARGE_SLOW; return true; }
    if (strcmp(s, "fast") == 0) { *m = CHARGE_FAST; return true; }
    return false;
}

#if defined(CONFIG_BOARD_TAB5)

#define EXP_ADDR      0x44u
#define EXP_IN_STA    0x0Fu
#define BIT_PWROFF    (1u << 4)
#define BIT_NQC       (1u << 5)
#define BIT_CHG_STAT  (1u << 6)
#define BIT_CHG_EN    (1u << 7)

bool charger_present(void) { return true; }

bool charger_set_mode(charge_mode_t m) {
    uint8_t bits = m == CHARGE_OFF  ? BIT_NQC
                 : m == CHARGE_SLOW ? (uint8_t)(BIT_CHG_EN | BIT_NQC)
                 :                    BIT_CHG_EN;
    return i2c_tab5_expander_update(EXP_ADDR, BIT_CHG_EN | BIT_NQC, bits);
}

bool charger_get_mode(charge_mode_t *m) {
    int v = i2c_tab5_expander_read(EXP_ADDR, 0x05u);
    if (v < 0 || !m) return false;
    *m = !(v & BIT_CHG_EN) ? CHARGE_OFF : (v & BIT_NQC) ? CHARGE_SLOW : CHARGE_FAST;
    return true;
}

int charger_stat_raw(void) {
    int v = i2c_tab5_expander_read(EXP_ADDR, EXP_IN_STA);
    return v < 0 ? -1 : (v & BIT_CHG_STAT) ? 1 : 0;
}

/* M5Unified's Power_Class::powerOff(): ten edges 50 ms apart. */
void charger_power_off(void) {
    for (int i = 0; i < 10; i++) {
        i2c_tab5_expander_update(EXP_ADDR, BIT_PWROFF, (i & 1) ? BIT_PWROFF : 0u);
        time_delay_us(50000);
    }
}

#else

bool charger_present(void) { return false; }
bool charger_set_mode(charge_mode_t m) { (void)m; return false; }
bool charger_get_mode(charge_mode_t *m) { (void)m; return false; }
int  charger_stat_raw(void) { return -1; }
void charger_power_off(void) { }

#endif
