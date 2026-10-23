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

/* Life Support & Colony Capacity Constants */
#define MOON_LANDER_CREW          4
#define MOON_HABITAT_CREW         8

/* Resource System State */
typedef struct moon_resources_s {
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
    bool     warning_crew_lost;     /* Flag set when casualty occurs */
    bool     warning_power;         /* Low power / blackout */
    bool     warning_o2;            /* Oxygen depleted */
    bool     warning_water;         /* Water depleted */
    bool     warning_cold;          /* Cryogenic freezing in unheated habitat */
} moon_resources_t;

/* Resource System Lifecycle & Updates */
void moon_resources_init(moon_resources_t *res);

/* Discrete simulation tick (called every 250 ms) */
void moon_resources_tick(moon_resources_t *res,
                         moon_grid_t *grid,
                         moon_structure_mgr_t *mgr);

/* Query whether a structure is connected to the Lander power grid */
void moon_resources_solve_power_grid(moon_grid_t *grid,
                                     moon_structure_mgr_t *mgr);

#endif /* MOON_RESOURCES_H */
