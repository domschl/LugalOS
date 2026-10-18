#ifndef DRIVERS_BATTERY_H
#define DRIVERS_BATTERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The battery's state of charge, from a pack voltage and current measured
 * elsewhere (drivers/ina226.c on the Tab5). Pure integer arithmetic.
 *
 * Two estimates, blended:
 *
 *  - From the voltage. The resting (open-circuit) voltage says how full a
 *    Li-ion cell is, but under load the terminal voltage sags by I * R_int,
 *    and while charging it rises by as much. V_ocv = V - I * R_int (current
 *    positive into the battery) undoes that, and a table of the usual Li-ion
 *    discharge curve, stretched between the board's empty and full pack
 *    voltages, turns V_ocv into a charge level. Good on average, noisy from
 *    second to second, and wrong by a few percent whenever R_int is.
 *
 *  - By counting charge. I * dt over the pack capacity moves the level
 *    smoothly and exactly as the current says, but drifts, and needs a
 *    starting point.
 *
 * The count carries the level from sample to sample, and every sample pulls
 * it 1/256 of the way toward the voltage estimate. At one sample a second
 * that is a time constant of about four minutes: long enough that a load
 * step (the backlight) does not make the number jump, short enough that a
 * wrong capacity or a wrong start is corrected within the hour. The first
 * sample starts from the voltage estimate.
 *
 * Levels are in parts per million inside (1000000 = full), so that a few
 * hundred mA for one second still moves the count; the public value is in
 * 0.1 % (SENSOR_CHAN_BATTERY). */

typedef struct {
    uint16_t cells;          /* in series */
    uint16_t empty_mv;       /* pack voltage at 0 %, at rest */
    uint16_t full_mv;        /* pack voltage at 100 %, at rest */
    uint16_t rint_mohm;      /* pack internal resistance */
    uint32_t capacity_mah;
} battery_params_t;

typedef struct {
    battery_params_t p;
    bool     started;
    int32_t  soc_ppm;        /* the blended level */
    uint64_t last_ms;
    int32_t  last_mv, last_ma;
} battery_t;

/* The level the voltage alone says, in ppm, for a resting pack voltage. */
int32_t battery_soc_from_ocv_ppm(const battery_params_t *p, int32_t ocv_mv);

/* One measurement at time now_ms: pack voltage and current (+ = charging). */
void    battery_update(battery_t *b, int32_t mv, int32_t ma, uint64_t now_ms);

/* State of charge in 0.1 % (0..1000); false before the first update. */
bool    battery_soc(const battery_t *b, int32_t *soc_pm);

/* Minutes until empty at the last current, or until full while charging
 * (capacity-based, ignoring the charger's slow final phase); -1 when the
 * current is too small to say. */
int32_t battery_minutes_left(const battery_t *b);

/* Golden vectors for the curve and the blend; failures count. */
uint32_t battery_selftest(bool report);

/* The board's own battery, fed by its monitor (drivers/ina226.c), or NULL
 * on a board without one (CONFIG_BATT_CELLS unset). */
battery_t *battery_board(void);

/* "71%" or "71%+" while charging, for the menu bar; 0 (and "") without a
 * board battery or before its first reading. */
uint32_t battery_indicator(char *buf, uint32_t cap);

/* Whether the board battery is charging: current into it above 20 mA. */
bool battery_board_charging(void);

#endif /* DRIVERS_BATTERY_H */
