#include "moon_resources.h"
#include <string.h>

/* Settled with the headless runs in tests/balance.c (`make balance`): over twelve maps
 * a scripted good player survives and completes Act I, a careless one (Habitat before
 * power) and an idle one lose. */
const moon_rules_t g_moon_rules[MOON_DIFFICULTY_COUNT] = {
    /*                      regolith o2  water battery energy sinterer crew deposit */
    [MOON_DIFFICULTY_EASY]   = { "CADET",     120, 70, 100, 100,  5, 45,  80, 12 },
    [MOON_DIFFICULTY_NORMAL] = { "ENGINEER",   80, 50,  75,  40,  8, 30, 100,  8 },
    [MOON_DIFFICULTY_HARD]   = { "COMMANDER",  50, 40,  60,  20, 10, 30, 130,  6 },
};

/* Ground work of an Extractor (ice or mare) or a Sinterer (quarrying): one unit of the
 * richest tile beside it is taken every rules.deposit_s_per_unit seconds, or per kilogram
 * of regolith baked. False (and `depleted`) when the ground around it is spent. */
static bool draw_from_ground(const moon_resources_t *res, moon_grid_t *grid, moon_structure_t *s) {
    moon_tile_t *tile = moon_structure_ore_tile(grid, (moon_structure_type_t)s->type, s->gx, s->gy);
    s->depleted = (tile == NULL);
    if (!tile) return false;

    bool take;
    if (s->type == MOON_STRUCT_O2_EXTRACTOR) {
        take = (++s->work >= (unsigned)res->rules.deposit_s_per_unit * MOON_SIM_TICK_RATE_HZ);
        if (take) s->work = 0;
    } else {
        /* a unit per kilogram: kg/min is added every tick, a kilogram is 60 s of ticks */
        s->work = (uint16_t)(s->work + res->rules.sinterer_kg_per_min);
        take = (s->work >= 60 * MOON_SIM_TICK_RATE_HZ);
        if (take) s->work = (uint16_t)(s->work - 60 * MOON_SIM_TICK_RATE_HZ);
    }
    if (take) tile->resource_val--;
    return true;
}

void moon_resources_apply_start(moon_resources_t *res) {
    res->regolith_stored_kg = fixed_from_int(res->rules.start_regolith_kg);
    res->o2_stored_kg = fixed_from_int(res->rules.start_o2_kg);
    res->water_stored_l = fixed_from_int(res->rules.start_water_l);
    res->battery_stored_kwh = fixed_from_int(res->rules.start_battery_kwh);
}

void moon_resources_init(moon_resources_t *res, int difficulty) {
    if (!res) return;
    if (difficulty < 0 || difficulty >= MOON_DIFFICULTY_COUNT) difficulty = MOON_DIFFICULTY_NORMAL;
    memset(res, 0, sizeof(*res));
    res->rules = g_moon_rules[difficulty];

    /* Start at dawn of Day 1 (angle 20 degrees, sun rising) */
    res->lunar_day = 1;
    res->cycle_tick = MOON_LUNAR_DAY_TICKS * 20 / 360;
    res->sun_elevation_deg = 20;
    res->solar_intensity = moon_sin(20);
    res->is_night = false;

    /* Baseline Power */
    res->power_produced_kw = 10;
    res->power_consumed_kw = 5;
    res->power_net_kw = 5;
    res->battery_capacity_kwh = 50;
    res->power_brownout = false;

    /* Starting Life Support & Materials */
    res->o2_capacity_kg = 100;

    res->water_capacity_l = 150;

    res->regolith_capacity_kg = 250;

    /* Population */
    res->population = MOON_LANDER_CREW;
    res->peak_population = MOON_LANDER_CREW;
    res->death_cause = MOON_CAUSE_NONE;
    res->ticks_since_blackout = 0;
    res->max_population = MOON_LANDER_CREW;
    res->life_support_pct = 100;
    moon_resources_apply_start(res);
}

void moon_resources_solve_power_grid(moon_grid_t *grid,
                                     moon_structure_mgr_t *mgr) {
    if (!grid || !mgr) return;
    mgr->solved_revision = mgr->revision;

    /* 1. Mark all structures unpowered by default */
    for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
        if (mgr->list[i].id != 0) {
            mgr->list[i].powered = false;
        }
    }

    /* 2. Locate the Expedition Lander (primary power anchor) */
    int lander_idx = -1;
    for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
        if (mgr->list[i].id != 0 && mgr->list[i].type == MOON_STRUCT_LANDER) {
            lander_idx = i;
            break;
        }
    }

    if (lander_idx < 0) return; /* No power grid without Lander */

    /* 3. BFS over the structure graph (two structures are linked when any of
     *    their footprint tiles share an edge). Small, fixed-size working set:
     *    safe on a small task stack. */
    uint8_t queue[MOON_MAX_STRUCTURES];
    uint8_t visited[MOON_MAX_STRUCTURES];
    int q_head = 0;
    int q_tail = 0;
    memset(visited, 0, sizeof(visited));

    moon_structure_t *lander = &mgr->list[lander_idx];
    lander->powered = true;
    visited[lander_idx] = 1;
    queue[q_tail++] = (uint8_t)lander_idx;

    /* Direction deltas: North, South, East, West */
    static const int dirs[4][2] = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} };

    while (q_head < q_tail) {
        const moon_structure_t *cur = &mgr->list[queue[q_head++]];

        for (int dy = 0; dy < cur->footprint_h; dy++) {
            for (int dx = 0; dx < cur->footprint_w; dx++) {
                for (int d = 0; d < 4; d++) {
                    int nx = cur->gx + dx + dirs[d][0];
                    int ny = cur->gy + dy + dirs[d][1];
                    const moon_tile_t *tile = moon_grid_get_tile_const(grid, nx, ny);
                    if (!tile) continue;
                    uint8_t sid = tile->structure_id;
                    if (sid == 0 || sid >= MOON_MAX_STRUCTURES) continue;
                    if (visited[sid] || mgr->list[sid].id != sid) continue;
                    visited[sid] = 1;
                    mgr->list[sid].powered = true;
                    queue[q_tail++] = sid;
                }
            }
        }
    }
}

void moon_resources_tick(moon_resources_t *res,
                         moon_grid_t *grid,
                         moon_structure_mgr_t *mgr) {
    if (!res || !grid || !mgr) return;

    /* 1. Advance Lunar Day / Night Solar Cycle */
    res->cycle_tick++;
    if (res->cycle_tick >= MOON_LUNAR_DAY_TICKS) {
        res->cycle_tick = 0;
        res->lunar_day++;
    }

    int32_t angle = (int32_t)((int64_t)res->cycle_tick * 360 / MOON_LUNAR_DAY_TICKS);
    res->sun_elevation_deg = angle;

    if (angle >= 0 && angle < 180) {
        /* Daytime: Sun ascends from 0 deg (dawn) to 90 deg (zenith) to 180 deg (dusk) */
        res->is_night = false;
        res->solar_intensity = moon_sin(angle);
    } else {
        /* Cryogenic Lunar Night */
        res->is_night = true;
        res->solar_intensity = 0;
    }

    /* 2. Re-solve power grid connectivity only when the structures changed */
    if (mgr->solved_revision != mgr->revision) {
        moon_resources_solve_power_grid(grid, mgr);
    }

    /* 3. Aggregate Generation, Capacity, and Consumption */
    int32_t total_gen_kw = 0;
    int32_t total_use_kw = 0;
    int32_t total_battery_cap = 0;
    int32_t total_housing = 0;

    int32_t net_o2_milli = 0;
    int32_t net_water_milli = 0;
    int32_t net_regolith_milli = 0;

    for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
        moon_structure_t *s = &mgr->list[i];
        if (s->id == 0 || !s->active) continue;

        const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)s->type);
        if (!def) continue;

        /* Battery capacity counts whether powered or not */
        total_battery_cap += def->power_storage;

        if (s->type == MOON_STRUCT_LANDER) {
            total_housing += MOON_LANDER_CREW;
            total_gen_kw += def->power_kw; /* Baseline RTG generator */
            total_use_kw += 5;             /* Base command & comms */
        } else if (s->type == MOON_STRUCT_HABITAT_DOME) {
            total_housing += MOON_HABITAT_CREW;
        }

        /* If connected to power grid */
        if (s->powered) {
            if (s->type == MOON_STRUCT_SOLAR_ARRAY) {
                /* Solar panels generate proportional to sun elevation */
                int32_t solar_kw = fixed_to_int((int64_t)def->power_kw * res->solar_intensity);
                total_gen_kw += solar_kw;
            } else if (def->power_kw < 0 && !s->depleted) {
                /* Consumer (an Extractor or Sinterer with nothing left to draw idles) */
                total_use_kw += (-def->power_kw);
            }

            /* Resource production from active industrial modules */
            if (!res->power_brownout) {
                if (s->type == MOON_STRUCT_O2_EXTRACTOR) {
                    if (draw_from_ground(res, grid, s)) {
                        net_o2_milli += (def->rate_o2 * 1000 / (MOON_SIM_TICK_RATE_HZ * 60));
                        net_water_milli += (def->rate_water * 1000 / (MOON_SIM_TICK_RATE_HZ * 60));
                    }
                } else if (s->type == MOON_STRUCT_REGOLITH_PLANT) {
                    if (draw_from_ground(res, grid, s)) {
                        net_regolith_milli += (res->rules.sinterer_kg_per_min * 1000 / (MOON_SIM_TICK_RATE_HZ * 60));
                    }
                }
            }
        }
    }

    res->max_population = (uint16_t)total_housing;
    res->battery_capacity_kwh = total_battery_cap;
    res->power_produced_kw = total_gen_kw;
    res->power_consumed_kw = total_use_kw;
    res->power_net_kw = total_gen_kw - total_use_kw;

    /* 4. Power Balance & Battery Charging / Discharging */
    /* One tick moves kW * (1/4 s) * MOON_ENERGY_PCT/100 kWh */
    fixed32_t delta_kwh = fixed_div(fixed_from_int(res->power_net_kw * res->rules.energy_pct),
                                    fixed_from_int(100 * MOON_SIM_TICK_RATE_HZ));

    res->battery_stored_kwh += delta_kwh;
    fixed32_t max_cap = fixed_from_int(res->battery_capacity_kwh);
    if (res->battery_stored_kwh > max_cap) {
        res->battery_stored_kwh = max_cap;   /* also after a battery was recycled */
    }

    if (res->power_net_kw >= 0) {
        res->power_brownout = false;
        res->warning_power = false;
    } else if (res->battery_stored_kwh <= 0) {
        res->battery_stored_kwh = 0;
        res->power_brownout = true;
        res->warning_power = true;
    } else {
        res->power_brownout = false;
        res->warning_power = res->battery_stored_kwh < fixed_from_int(res->battery_capacity_kwh / 5);
    }

    /* 5. Life Support: Population Consumption */
    /* 1 kg O2 per colonist per 120s day; 2 L Water per colonist per 120s day */
    int32_t pop = res->population;
    int32_t pop_o2_consume_milli = (pop * 1000 * res->rules.crew_use_pct / 100) / (MOON_SIM_TICK_RATE_HZ * 30);
    int32_t pop_water_consume_milli = (pop * 2000 * res->rules.crew_use_pct / 100) / (MOON_SIM_TICK_RATE_HZ * 30);

    net_o2_milli -= pop_o2_consume_milli;
    net_water_milli -= pop_water_consume_milli;

    res->o2_rate_kg_per_tick = net_o2_milli;
    res->water_rate_l_per_tick = net_water_milli;
    res->regolith_rate_kg_per_tick = net_regolith_milli;

    /* Update O2 */
    fixed32_t delta_o2 = fixed_div(fixed_from_int(net_o2_milli), fixed_from_int(1000));
    res->o2_stored_kg += delta_o2;
    if (res->o2_stored_kg > fixed_from_int(res->o2_capacity_kg)) {
        res->o2_stored_kg = fixed_from_int(res->o2_capacity_kg);
    }
    if (res->o2_stored_kg <= 0) {
        res->o2_stored_kg = 0;
        res->warning_o2 = true;
    } else {
        res->warning_o2 = (res->o2_stored_kg < fixed_from_int(15));
    }

    /* Update Water */
    fixed32_t delta_water = fixed_div(fixed_from_int(net_water_milli), fixed_from_int(1000));
    res->water_stored_l += delta_water;
    if (res->water_stored_l > fixed_from_int(res->water_capacity_l)) {
        res->water_stored_l = fixed_from_int(res->water_capacity_l);
    }
    if (res->water_stored_l <= 0) {
        res->water_stored_l = 0;
        res->warning_water = true;
    } else {
        res->warning_water = (res->water_stored_l < fixed_from_int(20));
    }

    /* Update Regolith building stock */
    fixed32_t delta_reg = fixed_div(fixed_from_int(net_regolith_milli), fixed_from_int(1000));
    res->regolith_stored_kg += delta_reg;
    if (res->regolith_stored_kg > fixed_from_int(res->regolith_capacity_kg)) {
        res->regolith_stored_kg = fixed_from_int(res->regolith_capacity_kg);
    }
    if (res->regolith_stored_kg < 0) {
        res->regolith_stored_kg = 0;
    }

    /* Emergency scooping by the Lander when nothing else produces regolith */
    if (net_regolith_milli <= 0 && res->regolith_stored_kg < fixed_from_int(MOON_REGOLITH_RESERVE_KG)) {
        res->regolith_stored_kg += fixed_div(fixed_from_int(MOON_SCOOP_MILLI_PER_TICK), fixed_from_int(1000));
        if (res->regolith_stored_kg > fixed_from_int(MOON_REGOLITH_RESERVE_KG)) {
            res->regolith_stored_kg = fixed_from_int(MOON_REGOLITH_RESERVE_KG);
        }
    }

    if (res->power_brownout) res->ticks_since_blackout = 0;
    else if (res->ticks_since_blackout < UINT32_MAX) res->ticks_since_blackout++;
    if (res->population > res->peak_population) res->peak_population = res->population;

    if (res->crew_lost_ticks > 0) res->crew_lost_ticks--;
    res->warning_crew_lost = (res->crew_lost_ticks > 0);

    /* 6. Life Support Health Calculation */
    bool stressed = false;

    /* Freeze warning if night falls and base is blacked out (heaters off) */
    if (res->is_night && res->power_brownout) {
        res->warning_cold = true;
        stressed = true;
    } else {
        res->warning_cold = false;
    }

    /* Low reserves (battery, O2, water) are only warnings: stress needs an actual
     * blackout or an empty O2 or water tank. */
    if (res->o2_stored_kg <= 0 || res->water_stored_l <= 0 || res->power_brownout) {
        stressed = true;
    }

    if (stressed) {
        if (res->life_support_pct > 0) {
            res->life_support_pct--;
        } else if (res->population > 0) {
            /* At 0% life support: casualties occur every 5 seconds (20 ticks at 4Hz) */
            res->casualty_timer_ticks++;
            if (res->casualty_timer_ticks >= (MOON_SIM_TICK_RATE_HZ * 5)) {
                res->casualty_timer_ticks = 0;
                res->population--;
                res->crew_lost_ticks = MOON_CREW_LOST_BANNER_TICKS;
                res->death_cause = res->o2_stored_kg <= 0 ? MOON_CAUSE_OXYGEN :
                                   res->water_stored_l <= 0 ? MOON_CAUSE_WATER : MOON_CAUSE_BLACKOUT;
            }
        }
        res->growth_timer_ticks = 0;
    } else {
        res->casualty_timer_ticks = 0;
        if (res->life_support_pct < 100) {
            res->life_support_pct++;
        }

        /* Population growth when life support is excellent and housing capacity exists */
        if (res->life_support_pct >= 95 && res->population < res->max_population) {
            res->growth_timer_ticks++;
            if (res->growth_timer_ticks >= (MOON_SIM_TICK_RATE_HZ * 45)) {
                res->growth_timer_ticks = 0;
                res->population++;
            }
        } else {
            res->growth_timer_ticks = 0;
        }
    }
}

moon_clock_t moon_resources_clock(const moon_resources_t *res) {
    moon_clock_t c;
    const int64_t day = MOON_LUNAR_DAY_TICKS;
    int64_t t = res->cycle_tick;
    c.minutes = (int)((t * 1440 / day + 360) % 1440);
    int64_t until = (t < day / 2) ? (day / 2 - t) : (day - t);
    c.next_is_dusk = (t < day / 2);
    c.next_in_min = (int)(until * 1440 / day);
    return c;
}

bool moon_resources_act1_complete(const moon_resources_t *res) {
    return res->lunar_day >= MOON_ACT1_DAY &&
           res->population >= MOON_ACT1_CREW &&
           res->o2_rate_kg_per_tick >= 0 && res->water_rate_l_per_tick >= 0 &&
           res->ticks_since_blackout >= MOON_LUNAR_DAY_TICKS;
}
