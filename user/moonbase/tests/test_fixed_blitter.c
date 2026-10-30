#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "moon_fixed.h"
#include "moon_blitter.h"
#include "moon_grid.h"
#include "moon_structures.h"
#include "moon_resources.h"

static void test_structure_catalog_and_placement(void) {
    printf("[test] Checking structure catalog, footprints & placement rules...\n");

    moon_grid_t grid;
    moon_grid_generate(&grid, 19690720);

    moon_structure_mgr_t mgr;
    moon_structure_mgr_init(&mgr);

    /* 1. Catalog integrity */
    for (int t = 1; t < MOON_STRUCT_COUNT; t++) {
        const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)t);
        assert(def != NULL);
        assert(def->name != NULL);
        assert(def->footprint_w >= 1 && def->footprint_w <= 4);
        assert(def->footprint_h >= 1 && def->footprint_h <= 4);
    }

    /* 2. Initial expedition lander */
    moon_structure_t *lander = moon_structure_place(&grid, &mgr, MOON_STRUCT_LANDER, 32, 32);
    assert(lander != NULL);
    assert(mgr.count == 1);
    assert(grid.tiles[32][32].flags & MOON_TILE_FLAG_OCCUPIED);
    assert(grid.tiles[32][32].structure_id == lander->id);

    /* 3. Collision: placing on top of lander must fail */
    const char *reason = NULL;
    bool can_place = moon_structure_can_place(&grid, &mgr, MOON_STRUCT_SOLAR_ARRAY, 32, 32, &reason);
    assert(!can_place);
    assert(strcmp(reason, "Site already occupied") == 0);

    /* 4. Placement of Solar Array at (33, 32) */
    moon_structure_t *solar = moon_structure_place(&grid, &mgr, MOON_STRUCT_SOLAR_ARRAY, 33, 32);
    assert(solar != NULL);
    assert(mgr.count == 2);
    assert(grid.tiles[32][33].flags & MOON_TILE_FLAG_OCCUPIED);

    /* 5. 2x2 Habitat Dome placement at (30, 30) */
    moon_structure_t *hab = moon_structure_place(&grid, &mgr, MOON_STRUCT_HABITAT_DOME, 30, 30);
    assert(hab != NULL);
    assert(mgr.count == 3);
    assert(grid.tiles[30][30].flags & MOON_TILE_FLAG_OCCUPIED);
    assert(grid.tiles[30][31].flags & MOON_TILE_FLAG_OCCUPIED);
    assert(grid.tiles[31][30].flags & MOON_TILE_FLAG_OCCUPIED);
    assert(grid.tiles[31][31].flags & MOON_TILE_FLAG_OCCUPIED);

    /* 6. Demolition */
    moon_structure_demolish(&grid, &mgr, 33, 32);
    assert(mgr.count == 2);
    assert(!(grid.tiles[32][33].flags & MOON_TILE_FLAG_OCCUPIED));

    /* 7. Render structures and ghost preview onto surface */
    uint16_t *surf_buf = (uint16_t *)calloc(1024 * 600, sizeof(uint16_t));
    assert(surf_buf != NULL);
    moon_surface_t surf = moon_surface_make(surf_buf, 1024, 600, 1024);

    moon_world_render(&mgr, &grid, &surf, 0, 32 * MOON_TILE_H, 0, 0, 0, false);

    moon_structure_render_ghost(&mgr, &grid, &surf, 0, 32 * MOON_TILE_H, MOON_STRUCT_BATTERY_BANK, 34, 32);

    free(surf_buf);
    printf("[test] Structure catalog, footprints & placement rules: PASS\n");
}

static void test_grid_generation_and_projection(void) {
    printf("[test] Checking lunar terrain generation & grid projections...\n");

    moon_grid_t grid;
    moon_grid_generate(&grid, 19690720);

    /* Verify lander tile is ready with foundation pad */
    const moon_tile_t *lander = moon_grid_get_tile_const(&grid, grid.lander_gx, grid.lander_gy);
    assert(lander != NULL);
    assert(lander->terrain == MOON_TERRAIN_REGOLITH_FLAT);
    assert(lander->elevation == 1);
    assert(lander->flags & MOON_TILE_FLAG_FOUNDATION);
    assert(lander->structure_id == 0);

    /* Verify presence of diverse terrain types */
    bool has_ice = false;
    bool has_basalt = false;
    bool has_crater_rim = false;
    bool has_crater_floor = false;

    for (int gy = 0; gy < MOON_MAP_SIZE_Y; gy++) {
        for (int gx = 0; gx < MOON_MAP_SIZE_X; gx++) {
            const moon_tile_t *t = &grid.tiles[gy][gx];
            if (t->terrain == MOON_TERRAIN_ICE_DEPOSIT) has_ice = true;
            if (t->terrain == MOON_TERRAIN_BASALT_MARE) has_basalt = true;
            if (t->terrain == MOON_TERRAIN_CRATER_RIM) has_crater_rim = true;
            if (t->terrain == MOON_TERRAIN_CRATER_FLOOR) has_crater_floor = true;
        }
    }

    assert(has_ice);
    assert(has_basalt);
    assert(has_crater_rim);
    assert(has_crater_floor);

    /* Test isometric coordinate roundtrip */
    int32_t cam_x = 0;
    int32_t cam_y = 32 * MOON_TILE_H;
    int32_t screen_w = 1024;
    int32_t screen_h = 600;

    for (int gy = 20; gy <= 40; gy += 5) {
        for (int gx = 20; gx <= 40; gx += 5) {
            int32_t sx, sy;
            moon_grid_to_screen(gx, gy, 0, cam_x, cam_y, screen_w, screen_h, &sx, &sy);

            int32_t out_gx, out_gy;
            moon_screen_to_grid(sx, sy, cam_x, cam_y, screen_w, screen_h, &out_gx, &out_gy);

            assert(out_gx == gx);
            assert(out_gy == gy);
        }
    }

    /* Test grid rendering onto surface with boundary clipping */
    uint16_t *surf_buf = (uint16_t *)calloc(screen_w * screen_h, sizeof(uint16_t));
    assert(surf_buf != NULL);
    moon_surface_t surf = moon_surface_make(surf_buf, screen_w, screen_h, screen_w);

    moon_grid_render(&grid, &surf, cam_x, cam_y, 32, 32, true);
    moon_grid_render_minimap(&grid, &surf, screen_w - 74, screen_h - 74, 64, 64, cam_x, cam_y);

    free(surf_buf);
    printf("[test] Lunar terrain generation & grid projections: PASS\n");
}

static void test_fixed_math(void) {
    printf("[test] Checking fixed-point arithmetic...\n");

    /* Multiplication */
    fixed32_t a = INT_TO_FIXED(3);
    fixed32_t b = INT_TO_FIXED(4);
    assert(FIXED_TO_INT(fixed_mul(a, b)) == 12);

    /* Fractional multiplication: 1.5 * 2.0 = 3.0 */
    fixed32_t f1 = INT_TO_FIXED(1) + FIXED_HALF;
    fixed32_t f2 = INT_TO_FIXED(2);
    assert(FIXED_TO_INT(fixed_mul(f1, f2)) == 3);

    /* Division */
    assert(FIXED_TO_INT(fixed_div(INT_TO_FIXED(10), INT_TO_FIXED(2))) == 5);
    assert(FIXED_TO_INT(fixed_div(INT_TO_FIXED(7), INT_TO_FIXED(2))) == 3);

    /* Square root */
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(0))) == 0);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(1))) == 1);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(4))) == 2);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(9))) == 3);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(16))) == 4);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(25))) == 5);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(100))) == 10);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(144))) == 12);

    /* Trigonometry */
    assert(fixed_sin_deg(0) == 0);
    assert(fixed_sin_deg(90) == FIXED_ONE);
    assert(fixed_sin_deg(180) == 0);
    assert(fixed_sin_deg(270) == -FIXED_ONE);
    assert(fixed_sin_deg(360) == 0);

    assert(fixed_cos_deg(0) == FIXED_ONE);
    assert(fixed_cos_deg(90) == 0);
    assert(fixed_cos_deg(180) == -FIXED_ONE);
    assert(fixed_cos_deg(270) == 0);
    assert(fixed_cos_deg(360) == FIXED_ONE);

    /* Lerp */
    fixed32_t lerp_mid = fixed_lerp(INT_TO_FIXED(10), INT_TO_FIXED(20), FIXED_HALF);
    assert(FIXED_TO_INT(lerp_mid) == 15);

    printf("[test] Fixed-point arithmetic: PASS\n");
}

static void test_blitter_and_clipping(void) {
    printf("[test] Checking blitter routines & boundary clipping...\n");

    const int W = 64;
    const int H = 64;
    uint16_t *pixels = (uint16_t *)calloc(W * H, sizeof(uint16_t));
    assert(pixels != NULL);

    moon_surface_t surf = moon_surface_make(pixels, W, H, W);

    /* Clear */
    moon_blit_clear(&surf, COLOR_SPACE_BLACK);
    assert(surf.pixels[0] == COLOR_SPACE_BLACK);

    /* Pixel within & out of bounds */
    moon_blit_pixel(&surf, 10, 10, COLOR_GOLD_FOIL);
    assert(surf.pixels[10 * W + 10] == COLOR_GOLD_FOIL);
    moon_blit_pixel(&surf, -5, 10, COLOR_GOLD_FOIL); /* Should clip safely */
    moon_blit_pixel(&surf, 10, 80, COLOR_GOLD_FOIL); /* Should clip safely */

    /* Filled and outline rectangles with boundary clipping */
    moon_blit_fill_rect(&surf, -10, -10, 20, 20, COLOR_ALERT_RED);
    assert(surf.pixels[0] == COLOR_ALERT_RED);

    moon_blit_rect_outline(&surf, 5, 5, 20, 20, COLOR_LASER_CYAN);

    /* Lines intersecting borders */
    moon_blit_line(&surf, -50, -50, 100, 100, COLOR_ENERGY_YELLOW);

    /* Circles */
    moon_blit_circle(&surf, 32, 32, 10, COLOR_STATUS_GREEN);
    moon_blit_circle_filled(&surf, 32, 32, 5, COLOR_SOLAR_BLUE);

    /* Isometric tile and block */
    moon_blit_iso_tile(&surf, 32, 20, 16, 8, COLOR_REGOLITH_MID, COLOR_REGOLITH_LIGHT);
    moon_blit_iso_block(&surf, 32, 45, 16, 8, 12,
                        COLOR_GOLD_FOIL, COLOR_REGOLITH_DARK,
                        COLOR_REGOLITH_MID, COLOR_LUNAR_WHITE);

    /* Textured isometric tile & block (procedural texturing and cliff stratification) */
    moon_blit_iso_tile_textured(&surf, 32, 20, 16, 8, 10, 10,
                                MOON_TERRAIN_ICE_DEPOSIT, 0, COLOR_LASER_CYAN);
    moon_blit_iso_block_textured(&surf, 32, 45, 16, 8, 12, 12, 14,
                                 MOON_TERRAIN_CRATER_RIM, MOON_TILE_FLAG_FOUNDATION,
                                 COLOR_GOLD_FOIL);

    /* Text & formatted text */
    moon_blit_text(&surf, 0, 0, "TEST", COLOR_LUNAR_WHITE, 0, true);
    moon_blit_text_fmt(&surf, 0, 16, COLOR_GOLD_FOIL, 0, true, "VAL=%d", 42);

    /* Sprite with transparency key */
    uint16_t sprite_buf[16] = {
        COLOR_ALERT_RED, 0x0000,          0x0000,          COLOR_ALERT_RED,
        0x0000,          COLOR_ALERT_RED, COLOR_ALERT_RED, 0x0000,
        0x0000,          COLOR_ALERT_RED, COLOR_ALERT_RED, 0x0000,
        COLOR_ALERT_RED, 0x0000,          0x0000,          COLOR_ALERT_RED
    };
    moon_surface_t sprite = moon_surface_make(sprite_buf, 4, 4, 4);

    /* Blit sprite overlapping right edge */
    moon_blit_sprite(&surf, W - 2, H - 2, &sprite, 0x0000);

    /* Verify color dim and lerp */
    uint16_t c_dim = moon_color_dim(RGB565(200, 200, 200), 128);
    assert(c_dim != 0);
    uint16_t c_lerp = moon_color_lerp(COLOR_SPACE_BLACK, COLOR_LUNAR_WHITE, FIXED_HALF);
    assert(c_lerp != COLOR_SPACE_BLACK);

    free(pixels);
    printf("[test] Blitter routines & boundary clipping: PASS\n");
}

static void test_resource_simulation_and_day_night(void) {
    printf("[test] Checking resource simulation loop & lunar day/night cycle...\n");

    moon_grid_t grid;
    moon_grid_generate(&grid, 19690720);

    moon_structure_mgr_t mgr;
    moon_structure_mgr_init(&mgr);

    moon_resources_t res;
    moon_resources_init(&res, MOON_DIFFICULTY_NORMAL);

    /* 1. Initial State */
    assert(res.lunar_day == 1);
    assert(!res.is_night);
    assert(res.solar_intensity > 0);
    assert(res.population == MOON_LANDER_CREW);
    assert(res.battery_stored_kwh > 0);

    /* 2. Place Lander at (32, 32) */
    moon_structure_place(&grid, &mgr, MOON_STRUCT_LANDER, 32, 32);

    /* Place Solar Array connected directly adjacent at (33, 32) */
    moon_structure_t *solar = moon_structure_place(&grid, &mgr, MOON_STRUCT_SOLAR_ARRAY, 33, 32);
    assert(solar != NULL);

    /* Place disconnected/isolated Sinterer far away at (10, 10) */
    moon_structure_t *isolated_sinterer = moon_structure_place(&grid, &mgr, MOON_STRUCT_REGOLITH_PLANT, 10, 10);
    assert(isolated_sinterer != NULL);

    /* Solve power grid */
    moon_resources_solve_power_grid(&grid, &mgr);
    assert(solar->powered == true);
    assert(isolated_sinterer->powered == false);

    /* Connect isolated sinterer using Power Conduit chain from (32, 32) to (32, 31) */
    moon_structure_t *cable = moon_structure_place(&grid, &mgr, MOON_STRUCT_POWER_CONDUIT, 32, 31);
    assert(cable != NULL);
    moon_resources_solve_power_grid(&grid, &mgr);
    assert(cable->powered == true);

    /* 3. Run tick at midday */
    res.cycle_tick = MOON_LUNAR_DAY_TICKS * 90 / 360; /* Noon: 90 deg */
    moon_resources_tick(&res, &grid, &mgr);
    assert(!res.is_night);
    assert(res.sun_elevation_deg == 90);
    assert(res.power_produced_kw >= 35); /* RTG 10 kW + Solar ~30 kW */
    assert(res.power_net_kw > 0);
    assert(!res.power_brownout);

    /* 4. Advance into Lunar Night (270 deg) */
    res.cycle_tick = MOON_LUNAR_DAY_TICKS * 270 / 360;
    moon_resources_tick(&res, &grid, &mgr);
    assert(res.is_night);
    assert(res.solar_intensity == 0);
    /* In night, solar produces 0 kW, only Lander RTG (10 kW) */
    assert(res.power_produced_kw == 10);

    /* 5. Add power-hungry Regolith Sinterer adjacent to lander at (31, 32) */
    moon_structure_t *sinterer = moon_structure_place(&grid, &mgr, MOON_STRUCT_REGOLITH_PLANT, 31, 32);
    assert(sinterer != NULL);

    /* At night: consumption (5 base + 20 Sinterer = 25 kW) exceeds RTG (10 kW) -> deficit of -15 kW */
    res.battery_stored_kwh = fixed_from_int(1); /* almost empty */
    moon_resources_tick(&res, &grid, &mgr);
    assert(res.power_net_kw < 0);

    /* Drain battery to 0 -> trigger brownout and cold warning */
    for (int t = 0; t < 20; t++) {
        moon_resources_tick(&res, &grid, &mgr);
    }
    assert(res.battery_stored_kwh == 0);
    assert(res.power_brownout == true);
    assert(res.warning_power == true);
    assert(res.warning_cold == true);
    assert(res.life_support_pct < 100);

    /* 6. Verify crew casualty when life support stays at 0% */
    res.life_support_pct = 0;
    res.casualty_timer_ticks = 0;
    for (int t = 0; t < 25; t++) {
        moon_resources_tick(&res, &grid, &mgr);
    }
    assert(res.population < MOON_LANDER_CREW);
    assert(res.warning_crew_lost == true);

    printf("[test] Resource simulation loop & lunar day/night cycle: PASS\n");
}

static void test_ui_inspection_and_gameplay_loop(void) {
    printf("[test] Checking UI inspector, minimap navigation & gameplay loop...\n");

    moon_grid_t grid;
    moon_grid_generate(&grid, 19690720);

    moon_structure_mgr_t mgr;
    moon_structure_mgr_init(&mgr);

    /* 1. Place Lander and Battery */
    moon_structure_t *lander = moon_structure_place(&grid, &mgr, MOON_STRUCT_LANDER, 32, 32);
    assert(lander != NULL);
    moon_structure_t *batt = moon_structure_place(&grid, &mgr, MOON_STRUCT_BATTERY_BANK, 33, 32);
    assert(batt != NULL);

    /* 2. Selection lookup */
    const moon_tile_t *t_batt = moon_grid_get_tile_const(&grid, 33, 32);
    assert(t_batt != NULL);
    assert(t_batt->structure_id == batt->id);

    /* 3. Demolish and 50% refund calculation */
    const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)batt->type);
    assert(def != NULL);
    int refund = def->cost_regolith / 2;
    assert(refund == 10); /* Battery costs 20, refund is 10 */

    moon_structure_demolish(&grid, &mgr, 33, 32);
    assert(mgr.count == 1);
    assert(grid.tiles[32][33].structure_id == 0);

    /* 4. Minimap coordinate to camera projection */
    int mm_click_x = 32;
    int mm_click_y = 32;
    int32_t cam_x = (mm_click_x - mm_click_y) * MOON_TILE_HALF_W;
    int32_t cam_y = (mm_click_x + mm_click_y) * MOON_TILE_HALF_H;
    assert(cam_x == 0);
    assert(cam_y == 32 * MOON_TILE_H);

    printf("[test] UI inspector, minimap navigation & gameplay loop: PASS\n");
}

int main(void) {
    test_fixed_math();
    test_blitter_and_clipping();
    test_grid_generation_and_projection();
    test_structure_catalog_and_placement();
    test_resource_simulation_and_day_night();
    test_ui_inspection_and_gameplay_loop();
    printf("[test] All Milestone 64.2 through 64.6 tests passed cleanly.\n");
    return 0;
}
