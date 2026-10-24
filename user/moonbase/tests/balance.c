/* Headless balance runs: scripted players against the real game input path.
 * `bin/balance` prints a per-day table for each strategy; `bin/balance check`
 * (part of `make test`) asserts the targets recorded in plan/phase64. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "moon_game.h"

static moon_game_t g;

static void key(uint32_t k) {
    moon_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MOON_EVENT_KEY_DOWN; ev.key = k;
    moon_game_handle_event(&g, &ev);
}

static void step_ms(int ms) {
    for (int t = 0; t < ms; t += 50) moon_game_update(&g, 50);
}

static const moon_tile_t *tile(int x, int y) { return moon_grid_get_tile_const(&g.grid, x, y); }
static bool ok_to_build(moon_structure_type_t t, int x, int y) {
    return moon_structure_can_place(&g.grid, &g.structs, t, x, y, NULL);
}

static int count_of(moon_structure_type_t t) {
    int n = 0;
    for (int i = 1; i < MOON_MAX_STRUCTURES; i++) if (g.structs.list[i].id && g.structs.list[i].type == t) n++;
    return n;
}

/* Build `type` on tile (x,y) through the keyboard path; false if the game refused */
static bool build(moon_structure_type_t type, int x, int y) {
    static const uint32_t keys[MOON_STRUCT_COUNT] = {
        [MOON_STRUCT_SOLAR_ARRAY] = MOON_KEY_1, [MOON_STRUCT_BATTERY_BANK] = MOON_KEY_2,
        [MOON_STRUCT_HABITAT_DOME] = MOON_KEY_3, [MOON_STRUCT_REGOLITH_PLANT] = MOON_KEY_4,
        [MOON_STRUCT_O2_EXTRACTOR] = MOON_KEY_5, [MOON_STRUCT_POWER_CONDUIT] = MOON_KEY_6,
    };
    int before = g.structs.count;
    g.selected_tile_x = x; g.selected_tile_y = y;
    g.last_slot = -1;
    key(keys[type]);
    return g.structs.count == before + 1;
}

static bool adjacent_to_network(int x, int y, int w, int h) {
    for (int dy = -1; dy <= h; dy++)
        for (int dx = -1; dx <= w; dx++) {
            if ((dx >= 0 && dx < w) && (dy >= 0 && dy < h)) continue;
            if ((dx < 0 || dx >= w) && (dy < 0 || dy >= h)) continue; /* corners do not conduct */
            const moon_tile_t *t = tile(x + dx, y + dy);
            if (t && t->structure_id) return true;
        }
    return false;
}

/* Nearest buildable site next to the existing network, scanning outward from the lander */
static bool find_site(moon_structure_type_t type, int *ox, int *oy) {
    const moon_structure_def_t *d = moon_structure_get_def(type);
    for (int r = 1; r < 14; r++)
        for (int y = 32 - r; y <= 32 + r; y++)
            for (int x = 32 - r; x <= 32 + r; x++) {
                if (abs(x - 32) != r && abs(y - 32) != r) continue;
                if (ok_to_build(type, x, y) && adjacent_to_network(x, y, d->footprint_w, d->footprint_h)) {
                    *ox = x; *oy = y; return true;
                }
            }
    return false;
}

/* ---- Route to the ice: conduits from the network to a tile beside the deposit ---- */
static int s_route[256][2], s_route_len, s_route_next;
static int s_ext_x = -1, s_ext_y = -1;

static bool plan_route(void) {
    static int dist[MOON_MAP_SIZE_Y][MOON_MAP_SIZE_X], from[MOON_MAP_SIZE_Y][MOON_MAP_SIZE_X];
    static int q[MOON_MAP_SIZE_X * MOON_MAP_SIZE_Y];
    memset(dist, -1, sizeof(dist));
    int h = 0, t = 0;
    for (int y = 0; y < MOON_MAP_SIZE_Y; y++)
        for (int x = 0; x < MOON_MAP_SIZE_X; x++)
            if (tile(x, y)->structure_id) { dist[y][x] = 0; from[y][x] = -1; q[t++] = y * MOON_MAP_SIZE_X + x; }
    static const int d4[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
    while (h < t) {
        int c = q[h++], cx = c % MOON_MAP_SIZE_X, cy = c / MOON_MAP_SIZE_X;
        /* a free tile beside the deposit that can hold the extractor ends the search */
        if (dist[cy][cx] > 0 && ok_to_build(MOON_STRUCT_O2_EXTRACTOR, cx, cy)) {
            /* walk back to the network, then store the path network-to-site; the last
             * tile is the extractor's, the ones before it are conduits */
            int n = 0, px = cx, py = cy;
            static int rev[256][2];
            while (dist[py][px] > 0) { rev[n][0] = px; rev[n][1] = py; n++; int f = from[py][px]; px = f % MOON_MAP_SIZE_X; py = f / MOON_MAP_SIZE_X; }
            for (int i = 0; i < n; i++) { s_route[i][0] = rev[n - 1 - i][0]; s_route[i][1] = rev[n - 1 - i][1]; }
            s_route_len = n - 1;
            s_ext_x = cx; s_ext_y = cy; s_route_next = 0;
            return true;
        }
        for (int k = 0; k < 4; k++) {
            int nx = cx + d4[k][0], ny = cy + d4[k][1];
            if (nx < 0 || ny < 0 || nx >= MOON_MAP_SIZE_X || ny >= MOON_MAP_SIZE_Y || dist[ny][nx] >= 0) continue;
            if (!ok_to_build(MOON_STRUCT_POWER_CONDUIT, nx, ny)) continue;
            dist[ny][nx] = dist[cy][cx] + 1; from[ny][nx] = cy * MOON_MAP_SIZE_X + cx; q[t++] = ny * MOON_MAP_SIZE_X + nx;
        }
    }
    return false;
}

/* ---- Players ---- */

typedef enum { PLAYER_IDLE, PLAYER_GOOD, PLAYER_HABITAT_FIRST, PLAYER_SLOW } player_t;

static int regolith(void) { return fixed_to_int(g.resources.regolith_stored_kg); }

/* Load of the planned base in kW: the lander's 5 plus every consumer */
static int load_with(int extractors, int sinterers, int habitats, int extra_batteries_kw) {
    return 5 + extractors * 25 + sinterers * 20 + habitats * 15 + extra_batteries_kw;
}

/* Solar arrays that carry `load` kW through the day and refill the batteries for the
 * night: the night is as long as the day, so the average solar output (about 19 kW an
 * array) must cover twice the load less the RTG's two 10 kW shares. Battery capacity
 * (kWh) carries it through a 90 s night after the RTG's 10 kW. */
static int solar_for(int load) { return load > 10 ? (2 * load - 20 + 18) / 19 : 0; }
static int kwh_for_night(int load) { return load > 10 ? (load - 10) * 90 * g.resources.rules.energy_pct / 100 : 0; }

static bool build_site(moon_structure_type_t t, int cost) {
    int x, y;
    return regolith() >= cost && find_site(t, &x, &y) && build(t, x, y);
}

/* Route of conduits to the ice and an extractor at its end (planned once per extractor) */
static void build_extractor(void) {
    if (s_ext_x < 0 && !plan_route()) return;
    if (s_route_next < s_route_len) {
        if (tile(s_route[s_route_next][0], s_route[s_route_next][1])->structure_id) s_route_next++;
        else if (regolith() >= 2 && build(MOON_STRUCT_POWER_CONDUIT, s_route[s_route_next][0], s_route[s_route_next][1])) s_route_next++;
    } else if (regolith() >= 25) {
        if (build(MOON_STRUCT_O2_EXTRACTOR, s_ext_x, s_ext_y) || tile(s_ext_x, s_ext_y)->structure_id) s_ext_x = -1; /* plan the next one afresh */
    }
}

/* Supply first: returns true when it spent this turn (or wants to wait for regolith) */
static bool ensure_supply(int load) {
    if (count_of(MOON_STRUCT_SOLAR_ARRAY) < solar_for(load)) { build_site(MOON_STRUCT_SOLAR_ARRAY, 10); return true; }
    if (g.resources.battery_capacity_kwh < kwh_for_night(load) + 20) { build_site(MOON_STRUCT_BATTERY_BANK, 20); return true; }
    return false;
}

/* One decision, at most one build per call */
static void decide(player_t who) {
    if (who == PLAYER_IDLE) return;

    if (who == PLAYER_HABITAT_FIRST && count_of(MOON_STRUCT_HABITAT_DOME) == 0) {
        build_site(MOON_STRUCT_HABITAT_DOME, 50);
        return;
    }

    /* 1. Water and oxygen: power for the extractor, a route of conduits, the extractor */
    if (count_of(MOON_STRUCT_O2_EXTRACTOR) == 0) {
        /* only solar until the extractor is covered; batteries wait for income */
        if (count_of(MOON_STRUCT_SOLAR_ARRAY) < solar_for(load_with(1, 0, 0, 0))) { build_site(MOON_STRUCT_SOLAR_ARRAY, 10); return; }
        build_extractor();
        return;
    }

    int habitats = count_of(MOON_STRUCT_HABITAT_DOME);
    /* 2. Income: power for the sinterer first */
    if (count_of(MOON_STRUCT_REGOLITH_PLANT) == 0) {
        if (count_of(MOON_STRUCT_SOLAR_ARRAY) < solar_for(load_with(1, 1, habitats, 0))) { build_site(MOON_STRUCT_SOLAR_ARRAY, 10); return; }
        build_site(MOON_STRUCT_REGOLITH_PLANT, 15);
        return;
    }

    /* 3. Storage for the night, then housing with its power already provided */
    if (ensure_supply(load_with(1, 1, habitats, 0))) return;
    if (habitats == 0) {
        if (ensure_supply(load_with(1, 1, 1, 0))) return;
        build_site(MOON_STRUCT_HABITAT_DOME, 50);
        return;
    }

    /* 4. Water or oxygen going down: another extractor, with its power */
    int ex = count_of(MOON_STRUCT_O2_EXTRACTOR);
    if (ex < 5 && (ex < 2 || g.resources.water_rate_l_per_tick < 0 || g.resources.o2_rate_kg_per_tick < 0)) {
        if (ensure_supply(load_with(ex + 1, 1, 1, 0))) return;
        build_extractor();
    }
}

typedef struct { int day_dead, day_end, peak_pop, act1_day, objective; } result_t;
typedef struct { int alive[2], act1[2], median_day[2], careless_dead, idle_median_death, objectives_done; } level_result_t;

static uint32_t s_seed;   /* 0 = the default map */
static const moon_rules_t *s_override;   /* replaces the level's rules when set */

static result_t run(player_t who, int days, bool verbose, const char *name) {
    memset(&g, 0, sizeof(g));
    g.seed_request = s_seed;
    moon_game_init(&g, 1024, 600);
    if (s_override) { g.resources.rules = *s_override; moon_resources_apply_start(&g.resources); }
    s_ext_x = -1; s_route_len = s_route_next = 0;
    result_t out = { 0, 0, 0, 0, 0 };
    if (verbose) printf("-- %s\n day  pwr(prod/use)  batt/cap   O2    H2O   rgl  crew  LS  struct\n", name);
    int total_s = days * 180;
    for (int sec = 0; sec < total_s && g.running; sec++) {
        if (g.state == MOON_STATE_GAME_OVER) { out.day_dead = g.resources.lunar_day; break; }
        if (g.state == MOON_STATE_VICTORY) { out.act1_day = g.resources.lunar_day; key(MOON_KEY_C); }
        if (who != PLAYER_SLOW || sec % 8 == 0) decide(who == PLAYER_SLOW ? PLAYER_GOOD : who);
        step_ms(1000);
        if (verbose && sec % 30 == 29) {
            const moon_resources_t *r = &g.resources;
            printf(" %2d%s  %4d/%-4d      %4d/%-4d  %4d  %4d  %4d   %d  %3d  %2d\n", (int)r->lunar_day, r->is_night ? "n" : "d",
                   r->power_produced_kw, r->power_consumed_kw, fixed_to_int(r->battery_stored_kwh), r->battery_capacity_kwh,
                   fixed_to_int(r->o2_stored_kg), fixed_to_int(r->water_stored_l), regolith(), r->population, r->life_support_pct, g.structs.count);
        }
    }
    if (verbose) printf(" end: state %d day %d pop %d conduits %d, extractor %d, sinterer %d, solar %d, battery %d, habitat %d, cause %d\n",
                        (int)g.state, (int)g.resources.lunar_day, g.resources.population, count_of(MOON_STRUCT_POWER_CONDUIT),
                        count_of(MOON_STRUCT_O2_EXTRACTOR), count_of(MOON_STRUCT_REGOLITH_PLANT), count_of(MOON_STRUCT_SOLAR_ARRAY),
                        count_of(MOON_STRUCT_BATTERY_BANK), count_of(MOON_STRUCT_HABITAT_DOME), g.resources.death_cause);
    out.objective = g.objective;
    out.day_end = g.resources.lunar_day;
    out.peak_pop = g.resources.peak_population;
    if (g.state == MOON_STATE_GAME_OVER && !out.day_dead) out.day_dead = g.resources.lunar_day;
    return out;
}

/* Twelve maps per level: the good player (decides every second), a slow one (every 8 s),
 * a careless one (Habitat first) and an idle one */
static level_result_t measure_level(int lv, int seeds, int days) {
    level_result_t r;
    memset(&r, 0, sizeof(r));
    int act1_days[2][32], idle_days[32];
    s_override = &g_moon_rules[lv];
    for (int s = 0; s < seeds; s++) {
        s_seed = 1000u + (uint32_t)s * 7919u;
        for (int k = 0; k < 2; k++) {
            result_t x = run(k ? PLAYER_SLOW : PLAYER_GOOD, days, false, "");
            if (!x.day_dead) r.alive[k]++;
            if (x.act1_day) r.act1[k]++;
            if (k == 0 && x.objective == MOON_OBJECTIVES) r.objectives_done++;
            act1_days[k][s] = x.act1_day ? x.act1_day : 99;
        }
        if (run(PLAYER_HABITAT_FIRST, days, false, "").day_dead) r.careless_dead++;
        idle_days[s] = run(PLAYER_IDLE, days, false, "").day_dead;
    }
    for (int i = 0; i < seeds; i++) for (int j = i + 1; j < seeds; j++) {
        for (int k = 0; k < 2; k++) if (act1_days[k][j] < act1_days[k][i]) { int x = act1_days[k][i]; act1_days[k][i] = act1_days[k][j]; act1_days[k][j] = x; }
        if (idle_days[j] < idle_days[i]) { int x = idle_days[i]; idle_days[i] = idle_days[j]; idle_days[j] = x; }
    }
    r.median_day[0] = act1_days[0][seeds / 2];
    r.median_day[1] = act1_days[1][seeds / 2];
    r.idle_median_death = idle_days[seeds / 2];
    return r;
}

static int days_arg(int argc, char **argv) { return argc > 2 ? atoi(argv[2]) : 8; }

int main(int argc, char **argv) {
    bool check = (argc > 1 && strcmp(argv[1], "check") == 0);
    int days = (argc > 2) ? atoi(argv[2]) : 8;
    if (argc > 1 && strcmp(argv[1], "sweep") == 0) {
        enum { SEEDS = 8 };
        printf("energy%% start_rgl sint | good: alive act1 median-day | careless: dead | idle: median death day\n");
        static const int pcts[] = { 5, 8, 10, 12, 15 }, rgls[] = { 60, 80, 100 };
        for (unsigned a = 0; a < 5; a++) for (unsigned b = 0; b < 3; b++) {
            moon_rules_t r = g_moon_rules[MOON_DIFFICULTY_NORMAL];
            r.energy_pct = (uint8_t)pcts[a]; r.start_regolith_kg = (uint16_t)rgls[b];
            s_override = &r;
            int alive = 0, act1 = 0, bad_dead = 0, days[SEEDS], idle_days[SEEDS];
            for (int s = 0; s < SEEDS; s++) {
                s_seed = 1000u + (uint32_t)s * 7919u;
                result_t good = run(PLAYER_GOOD, days_arg(argc, argv), false, "");
                result_t bad = run(PLAYER_HABITAT_FIRST, days_arg(argc, argv), false, "");
                result_t idle = run(PLAYER_IDLE, days_arg(argc, argv), false, "");
                if (!good.day_dead) alive++;
                if (good.act1_day) act1++;
                days[s] = good.act1_day ? good.act1_day : 99;
                if (bad.day_dead) bad_dead++;
                idle_days[s] = idle.day_dead;
            }
            for (int i = 0; i < SEEDS; i++) for (int j = i + 1; j < SEEDS; j++) {
                if (days[j] < days[i]) { int x = days[i]; days[i] = days[j]; days[j] = x; }
                if (idle_days[j] < idle_days[i]) { int x = idle_days[i]; idle_days[i] = idle_days[j]; idle_days[j] = x; }
            }
            printf("%5d %9d %4d | %d/%d  %d/%d  %3d       | %d/%d     | %d\n", pcts[a], rgls[b], r.sinterer_kg_per_min,
                   alive, SEEDS, act1, SEEDS, days[SEEDS / 2], bad_dead, SEEDS, idle_days[SEEDS / 2]);
        }
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "levels") == 0 || check)) {
        enum { SEEDS = 12 };
        if (!check) printf("%-10s %-30s %-30s %-16s %s\n", "level", "good (decides every 1 s)", "slow (every 8 s)", "careless dies", "idle dies");
        level_result_t res[MOON_DIFFICULTY_COUNT];
        for (int lv = 0; lv < MOON_DIFFICULTY_COUNT; lv++) {
            res[lv] = measure_level(lv, SEEDS, check ? 16 : days_arg(argc, argv));
            const level_result_t *r = &res[lv];
            if (!check) {
                printf("%-10s alive %2d/%d  Act I %2d/%d  day %2d    alive %2d/%d  Act I %2d/%d  day %2d    %2d/%d             day %d\n", g_moon_rules[lv].name,
                       r->alive[0], SEEDS, r->act1[0], SEEDS, r->median_day[0], r->alive[1], SEEDS, r->act1[1], SEEDS, r->median_day[1],
                       r->careless_dead, SEEDS, r->idle_median_death);
            }
        }
        if (check) {
            /* Targets (plan/phase64 5.8, 64.13): over twelve maps */
            const level_result_t *e = &res[MOON_DIFFICULTY_EASY], *n = &res[MOON_DIFFICULTY_NORMAL], *h = &res[MOON_DIFFICULTY_HARD];
            assert(n->alive[0] == SEEDS && n->act1[0] == SEEDS);            /* a good player always wins on Normal ... */
            assert(n->objectives_done == SEEDS && e->objectives_done == SEEDS); /* ... following the objective path to its end */
            assert(n->alive[1] >= SEEDS - 1 && n->act1[1] >= SEEDS - 1);    /* ... and so does a slow one */
            assert(n->careless_dead >= SEEDS - 1);                          /* Habitat before power loses */
            assert(n->idle_median_death <= 3);                              /* doing nothing loses within three days */
            assert(e->alive[0] == SEEDS && e->act1[0] >= SEEDS - 1);        /* Easy is won */
            assert(e->careless_dead < n->careless_dead);                    /* ... and forgives mistakes */
            assert(e->idle_median_death >= n->idle_median_death);
            assert(h->alive[0] >= SEEDS * 2 / 3 && h->act1[0] >= SEEDS / 2); /* Hard is winnable but not on every map */
            assert(h->act1[0] < SEEDS || h->act1[1] < SEEDS);               /* ... and punishes a slow player or a bad map */
            assert(h->careless_dead >= SEEDS - 1);
            assert(h->idle_median_death <= n->idle_median_death);
            printf("[test] Balance: good player wins every level, careless and idle players lose on Normal and Hard: PASS\n");
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "seeds") == 0) {
        int lv = atoi(argv[2]);
        s_override = &g_moon_rules[lv];
        for (int s = 0; s < 12; s++) {
            s_seed = 1000u + (uint32_t)s * 7919u;
            result_t r = run(PLAYER_GOOD, 16, false, "");
            printf("seed %5u: dead day %2d act1 %2d  route %2d conduits, cause %d, structures %d\n", (unsigned)s_seed, r.day_dead, r.act1_day, s_route_len, g.resources.death_cause, g.structs.count);
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "one") == 0) {
        moon_rules_t r = g_moon_rules[MOON_DIFFICULTY_NORMAL];
        r.energy_pct = (uint8_t)atoi(argv[2]); r.start_regolith_kg = (uint16_t)atoi(argv[3]); r.sinterer_kg_per_min = (uint8_t)atoi(argv[4]);
        s_override = &r;
        run(PLAYER_GOOD, days, true, "good");
        return 0;
    }
    if (!check) {
        run(PLAYER_IDLE, days, true, "idle (builds nothing)");
        run(PLAYER_HABITAT_FIRST, days, true, "habitat first, then the good order");
        run(PLAYER_GOOD, days, true, "good opening: life support, income, power, housing");
    }
    return 0;
}
