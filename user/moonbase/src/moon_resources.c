#include "moon_resources.h"
#include <string.h>

void moon_resources_init(moon_resources_t *res) {
    if (!res) return;
    memset(res, 0, sizeof(*res));

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
    res->battery_stored_kwh = fixed_from_int(40);
    res->power_brownout = false;

    /* Starting Life Support & Materials */
    res->o2_capacity_kg = 100;
    res->o2_stored_kg = fixed_from_int(50);

    res->water_capacity_l = 150;
    res->water_stored_l = fixed_from_int(75);

    res->regolith_capacity_kg = 250;
    res->regolith_stored_kg = fixed_from_int(60);

    /* Population */
    res->population = MOON_LANDER_CREW;
    res->max_population = MOON_LANDER_CREW;
    res->life_support_pct = 100;
}

void moon_resources_solve_power_grid(moon_grid_t *grid,
                                     moon_structure_mgr_t *mgr) {
    if (!grid || !mgr) return;

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

    /* 3. BFS traversal across orthogonal building and conduit footprints */
    uint8_t visited[MOON_MAP_SIZE_Y][MOON_MAP_SIZE_X];
    memset(visited, 0, sizeof(visited));

    /* Simple ring queue for BFS */
    typedef struct { uint8_t gx, gy; } grid_pt_t;
    grid_pt_t queue[MOON_MAP_SIZE_X * MOON_MAP_SIZE_Y];
    int q_head = 0;
    int q_tail = 0;

    /* Seed BFS with all tiles under the Lander */
    moon_structure_t *lander = &mgr->list[lander_idx];
    lander->powered = true;
    for (int dy = 0; dy < lander->footprint_h; dy++) {
        for (int dx = 0; dx < lander->footprint_w; dx++) {
            int gx = lander->gx + dx;
            int gy = lander->gy + dy;
            if (gx < MOON_MAP_SIZE_X && gy < MOON_MAP_SIZE_Y) {
                visited[gy][gx] = 1;
                queue[q_tail++] = (grid_pt_t){ (uint8_t)gx, (uint8_t)gy };
            }
        }
    }

    /* Direction deltas: North, South, East, West */
    static const int dirs[4][2] = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} };

    while (q_head < q_tail) {
        grid_pt_t curr = queue[q_head++];

        for (int d = 0; d < 4; d++) {
            int nx = curr.gx + dirs[d][0];
            int ny = curr.gy + dirs[d][1];

            if (nx < 0 || nx >= MOON_MAP_SIZE_X || ny < 0 || ny >= MOON_MAP_SIZE_Y) {
                continue;
            }
            if (visited[ny][nx]) continue;

            /* Check if neighbor tile contains any structure or conduit */
            const moon_tile_t *tile = moon_grid_get_tile_const(grid, nx, ny);
            if (!tile || tile->structure_id == 0) continue;

            /* Mark tile visited */
            visited[ny][nx] = 1;
            queue[q_tail++] = (grid_pt_t){ (uint8_t)nx, (uint8_t)ny };

            /* Mark the structure owning this tile as powered */
            uint8_t struct_id = tile->structure_id;
            for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
                if (mgr->list[i].id == struct_id) {
                    mgr->list[i].powered = true;
                    break;
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

    /* 2. Solve Power Grid Connectivity */
    moon_resources_solve_power_grid(grid, mgr);

    /* 3. Aggregate Generation, Capacity, and Consumption */
    int32_t total_gen_kw = 0;
    int32_t total_use_kw = 0;
    int32_t total_battery_cap = 0;
    int32_t total_housing = 0;

    int32_t net_o2_milli = 0;
    int32_t net_water_milli = 0;
    int32_t net_regolith_milli = 0;

    for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
        const moon_structure_t *s = &mgr->list[i];
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
            } else if (def->power_kw < 0) {
                /* Consumer */
                total_use_kw += (-def->power_kw);
            }

            /* Resource production from active industrial modules */
            if (!res->power_brownout) {
                if (s->type == MOON_STRUCT_O2_EXTRACTOR) {
                    net_o2_milli += (def->rate_o2 * 1000 / (MOON_SIM_TICK_RATE_HZ * 60));
                    net_water_milli += (def->rate_water * 1000 / (MOON_SIM_TICK_RATE_HZ * 60));
                } else if (s->type == MOON_STRUCT_REGOLITH_PLANT) {
                    net_regolith_milli += (30 * 1000 / (MOON_SIM_TICK_RATE_HZ * 60));
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
    /* Net power in kW applied for 1 tick (0.25 s) in kWh: (kW * (1/4 s)) / 3600 = kW / 14400 */
    /* Scaled by game factor 120x so lunar night forces meaningful battery reserves */
    int32_t net_energy_unit = res->power_net_kw * 10; /* kWh * 10 / sec */
    fixed32_t delta_kwh = fixed_div(fixed_from_int(net_energy_unit), fixed_from_int(MOON_SIM_TICK_RATE_HZ * 10));

    if (res->power_net_kw >= 0) {
        /* Surplus power charges battery */
        res->battery_stored_kwh += delta_kwh;
        fixed32_t max_cap = fixed_from_int(res->battery_capacity_kwh);
        if (res->battery_stored_kwh > max_cap) {
            res->battery_stored_kwh = max_cap;
        }
        res->power_brownout = false;
        res->warning_power = false;
    } else {
        /* Deficit discharges battery */
        res->battery_stored_kwh += delta_kwh; /* delta_kwh is negative */
        if (res->battery_stored_kwh <= 0) {
            res->battery_stored_kwh = 0;
            res->power_brownout = true;
            res->warning_power = true;
        } else if (res->battery_stored_kwh < fixed_from_int(res->battery_capacity_kwh / 5)) {
            res->warning_power = true;
        } else {
            res->warning_power = false;
        }
    }

    /* 5. Life Support: Population Consumption */
    /* 1 kg O2 per colonist per 120s day; 2 L Water per colonist per 120s day */
    int32_t pop = res->population;
    int32_t pop_o2_consume_milli = (pop * 1000) / (MOON_SIM_TICK_RATE_HZ * 30);
    int32_t pop_water_consume_milli = (pop * 2000) / (MOON_SIM_TICK_RATE_HZ * 30);

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

    /* 6. Life Support Health Calculation */
    bool stressed = false;

    /* Freeze warning if night falls and base is blacked out (heaters off) */
    if (res->is_night && res->power_brownout) {
        res->warning_cold = true;
        stressed = true;
    } else {
        res->warning_cold = false;
    }

    if (res->warning_o2 || res->warning_water || res->warning_power) {
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
                res->warning_crew_lost = true;
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
