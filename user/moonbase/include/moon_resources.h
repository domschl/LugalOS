#ifndef MOON_RESOURCES_H
#define MOON_RESOURCES_H

#include <stdbool.h>
#include <stdint.h>
#include "moon_fixed.h"
#include "moon_grid.h"
#include "moon_structures.h"

/* Simulation Tick Frequency */
#define MOON_SIM_TICK_RATE_HZ     4           /* 4 ticks per second (250 ms per tick) */
#define MOON_SIM_TICK_INTERVAL_MS (1000 / MOON_SIM_TICK_RATE_HZ)

/* Full Lunar Day/Night Cycle Duration in Simulation Ticks */
/* 180 seconds total: 90s daytime (sunlight), 90s cryogenic night */
#define MOON_LUNAR_DAY_TICKS      (180 * MOON_SIM_TICK_RATE_HZ)

/* Casualty banner stays up for 8 seconds */
#define MOON_CREW_LOST_BANNER_TICKS (8 * MOON_SIM_TICK_RATE_HZ)

/* Emergency regolith scooping: with no producing Sinterer the Lander tops the
 * stock up to this level (kg), so the base can never be stuck without the means
 * to build a Sinterer. */
#define MOON_REGOLITH_RESERVE_KG  15
#define MOON_SCOOP_MILLI_PER_TICK 25      /* 0.1 kg/s */

/* Life Support & Colony Capacity Constants */
#define MOON_LANDER_CREW          4
#define MOON_HABITAT_CREW         8

/* The numbers a difficulty level sets (flash table, copied into the sim state) */
typedef struct {
    const char *name;
    uint16_t start_regolith_kg;
    uint16_t start_o2_kg;
    uint16_t start_water_l;
    uint16_t start_battery_kwh;
    uint8_t  energy_pct;           /* kWh a kW moves in one game second: a lunar night lasts
                                    * 90 s, so 100 would need about twenty batteries */
    uint8_t  sinterer_kg_per_min;  /* regolith a powered Sinterer bakes */
    uint8_t  crew_use_pct;         /* O2 and water per colonist, 100 = 1 kg and 2 L per 30 s */
    uint8_t  deposit_s_per_unit;   /* Seconds an Extractor takes to draw one unit (1% of a tile) from a deposit */
} moon_rules_t;

enum { MOON_DIFFICULTY_EASY = 0, MOON_DIFFICULTY_NORMAL, MOON_DIFFICULTY_HARD, MOON_DIFFICULTY_COUNT };
extern const moon_rules_t g_moon_rules[MOON_DIFFICULTY_COUNT];

/* Resource System State */
typedef struct moon_resources_s {
    moon_rules_t rules;             /* The difficulty this run plays by */

    /* Day / Night Cycle */
    uint32_t cycle_tick;            /* 0..MOON_LUNAR_DAY_TICKS - 1 */
    uint32_t lunar_day;             /* Day count (1, 2, 3...) */
    fixed32_t solar_intensity;      /* 0.0 (night) .. 1.0 (zenith noon) in 16.16 */
    int32_t  sun_elevation_deg;     /* 0..360 degrees */
    bool     is_night;              /* true during cryogenic lunar night */

    /* Power Grid (kW / kWh) */
    int32_t  power_produced_kw;     /* Current generation across connected grid */
    int32_t  power_consumed_kw;     /* Current active consumption */
    int32_t  power_net_kw;          /* produced - consumed */
    fixed32_t battery_stored_kwh;   /* 16.16 fixed-point current storage */
    int32_t  battery_capacity_kwh;  /* Maximum battery capacity */
    bool     power_brownout;        /* true if battery empty and demand > gen */

    /* Oxygen (kg) */
    fixed32_t o2_stored_kg;         /* 16.16 fixed-point stored O2 */
    int32_t  o2_capacity_kg;
    int32_t  o2_rate_kg_per_tick;   /* Net delta */

    /* Water (L) */
    fixed32_t water_stored_l;       /* 16.16 fixed-point stored water */
    int32_t  water_capacity_l;
    int32_t  water_rate_l_per_tick; /* Net delta */

    /* Regolith / Pavers (kg) */
    fixed32_t regolith_stored_kg;   /* 16.16 fixed-point building stock */
    int32_t  regolith_capacity_kg;
    int32_t  regolith_rate_kg_per_tick;

    /* Colony Life Support */
    uint16_t population;            /* Active colonists */
    uint16_t max_population;        /* Housing capacity */
    uint8_t  life_support_pct;      /* 0..100% health */
    uint16_t casualty_timer_ticks;  /* Timer at 0% life support before crew loss */
    uint16_t growth_timer_ticks;    /* Timer at >95% life support before new arrival */
    bool     warning_crew_lost;     /* True for MOON_CREW_LOST_BANNER_TICKS after a casualty */
    uint16_t crew_lost_ticks;       /* Remaining ticks of the casualty banner */
    bool     warning_power;         /* Low power / blackout */
    bool     warning_o2;            /* Oxygen low (under 15 kg) or empty */
    bool     warning_water;         /* Water low (under 20 L) or empty */
    bool     warning_cold;          /* Cryogenic freezing in unheated habitat */

    /* Run statistics for the end-of-game summary */
    uint16_t peak_population;
    uint8_t  death_cause;           /* MOON_CAUSE_*: what killed the last colonist */
    uint32_t ticks_since_blackout;  /* Ticks since power last failed */
} moon_resources_t;

enum { MOON_CAUSE_NONE = 0, MOON_CAUSE_OXYGEN, MOON_CAUSE_WATER, MOON_CAUSE_BLACKOUT };

/* Act I is complete when the base has run itself for a while: enough crew, O2 and
 * water produced at least as fast as they are used, and no blackout for a whole
 * lunar day. */
#define MOON_ACT1_DAY   6          /* lunar_day reached: five full days survived */
#define MOON_ACT1_CREW  8

bool moon_resources_act1_complete(const moon_resources_t *res);

/* Resource System Lifecycle & Updates */
/* Fresh state for a difficulty level; stocks come from that level's rules */
void moon_resources_init(moon_resources_t *res, int difficulty);

/* Set the starting stocks from res->rules (used by init and by the balance harness) */
void moon_resources_apply_start(moon_resources_t *res);

/* Discrete simulation tick (called every 250 ms) */
void moon_resources_tick(moon_resources_t *res,
                         moon_grid_t *grid,
                         moon_structure_mgr_t *mgr);

/* Query whether a structure is connected to the Lander power grid */
void moon_resources_solve_power_grid(moon_grid_t *grid,
                                     moon_structure_mgr_t *mgr);

/* Earth-equivalent 24 h reading of the lunar cycle: dawn is 06:00, zenith 12:00,
 * dusk 18:00 and the middle of the night 00:00. next_in_min is the base time
 * until the next dawn or dusk. */
typedef struct {
    int  minutes;        /* 0..1439 since 00:00 */
    int  next_in_min;    /* Minutes of base time until the next sunrise or sunset */
    bool next_is_dusk;   /* The next event is sunset (false: sunrise) */
} moon_clock_t;

moon_clock_t moon_resources_clock(const moon_resources_t *res);

#endif /* MOON_RESOURCES_H */
