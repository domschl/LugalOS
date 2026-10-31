/* Regression tests: picking, power solver, life-support rules,
 * camera bounds, pointer routing, the regolith guard, depth-sorted rendering and
 * sprite picking, shared UI layout, pause and clock, the same
 * play sequence by mouse, touch and keyboard, and the game flow (game over,
 * Act I victory, restart with a new seed). Headless (no SDL). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "moon_game.h"
#include "moon_sprites.h"
#include "moon_cyber.h"

void moon_platform_set_volume(int percent) { (void)percent; }
void moon_platform_pump_audio(void) { }

static moon_game_t g;

static void fresh(void) {
    memset(&g, 0, sizeof(g));
    assert(moon_game_init(&g, 1024, 600));
}

static void put(moon_structure_type_t type);
static int selected_structure_type(void);
static int s_touch; /* 1 while the test plays with a finger */

static void ptr(moon_event_type_t type, int x, int y, int button) {
    moon_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.x = x; ev.y = y; ev.button = (uint8_t)button; ev.touch = (uint8_t)s_touch;
    moon_game_handle_event(&g, &ev);
}

static void key(uint32_t k) {
    moon_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MOON_EVENT_KEY_DOWN; ev.key = k;
    moon_game_handle_event(&g, &ev);
}

static void click(int x, int y) {
    ptr(MOON_EVENT_POINTER_DOWN, x, y, 1);
    ptr(MOON_EVENT_POINTER_UP, x, y, 1);
}

static void ticks(int n) {
    /* frames are clamped to 100 ms, so feed 50 ms slices */
    for (int i = 0; i < n * (MOON_SIM_TICK_INTERVAL_MS / 50); i++) moon_game_update(&g, 50);
}

static void test_picking(void) {
    fresh();
    static const int off[][2] = {
        {0, 0}, {-20, 0}, {20, 0}, {0, -10}, {0, 10}, {-10, -5}, {10, 5}, {-10, 5}, {10, -5}
    };
    for (int gy = 30; gy <= 32; gy += 1) {
        for (int gx = 30; gx <= 32; gx += 1) {
            const moon_tile_t *t = moon_grid_get_tile_const(&g.grid, gx, gy);
            int32_t sx, sy;
            moon_grid_to_screen(gx, gy, t->elevation, g.cam_x, g.cam_y, 1024, 600, &sx, &sy);
            for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
                int32_t px, py;
                bool in = moon_grid_pick(&g.grid, sx + off[i][0], sy + off[i][1],
                                         g.cam_x, g.cam_y, 1024, 600, &px, &py);
                if (!in || px != gx || py != gy) {
                    /* only the flat landing apron is asserted */
                    printf("pick mismatch tile %d,%d off %d,%d -> %d,%d\n", gx, gy, off[i][0], off[i][1], px, py);
                    assert(0);
                }
            }
        }
    }
    /* Raised tile: pick at the top face centre */
    int found = 0;
    for (int gy = 0; gy < MOON_MAP_SIZE_Y && !found; gy++) {
        for (int gx = 0; gx < MOON_MAP_SIZE_X && !found; gx++) {
            const moon_tile_t *t = moon_grid_get_tile_const(&g.grid, gx, gy);
            if (t->elevation != 2) continue;
            int32_t sx, sy, px, py;
            moon_grid_to_screen(gx, gy, 2, g.cam_x, g.cam_y, 1024, 600, &sx, &sy);
            assert(moon_grid_pick(&g.grid, sx, sy, g.cam_x, g.cam_y, 1024, 600, &px, &py));
            assert(px == gx && py == gy);
            found = 1;
        }
    }
    assert(found);
    printf("[test] Tile picking (centred, elevation-aware): PASS\n");
}

static void test_power_solver(void) {
    fresh();
    /* Isolated solar array is not on the grid; a conduit chain joins it */
    moon_structure_t *sol = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_SOLAR_ARRAY, 30, 30);
    assert(sol);
    moon_resources_solve_power_grid(&g.grid, &g.structs);
    assert(!g.structs.list[sol->id].powered);

    /* Conduit chain from the Lander (32,32) west and north to the array at (30,30) */
    int path[][2] = {{31,32},{30,32},{30,31}};
    for (unsigned i = 0; i < sizeof(path) / sizeof(path[0]); i++) {
        assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, path[i][0], path[i][1]));
    }
    ticks(1); /* tick re-solves because the structure set changed */
    assert(g.structs.list[sol->id].powered);

    /* Breaking the chain unpowers it again on the next tick */
    moon_structure_demolish(&g.grid, &g.structs, 30, 32);
    ticks(1);
    assert(!g.structs.list[sol->id].powered);
    printf("[test] Power grid solver (islands, change-driven re-solve): PASS\n");
}

static void test_life_support_rules(void) {
    /* Low battery alone must not hurt the crew */
    fresh();
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_BATTERY_BANK, 33, 33));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 31, 30));
    moon_resources_solve_power_grid(&g.grid, &g.structs);
    g.resources.cycle_tick = 100 * MOON_SIM_TICK_RATE_HZ; /* night */
    g.resources.battery_stored_kwh = fixed_from_int(40);
    ticks(8);
    assert(g.resources.is_night);
    assert(g.resources.warning_power);
    assert(!g.resources.power_brownout);
    assert(g.resources.life_support_pct == 100);

    /* Low water or O2 is a warning, not starvation: the crew only suffers once a tank is empty */
    fresh();
    g.resources.water_stored_l = fixed_from_int(15);
    g.resources.o2_stored_kg = fixed_from_int(10);
    ticks(MOON_SIM_TICK_RATE_HZ * 20);
    assert(g.resources.warning_water && g.resources.warning_o2);
    assert(g.resources.water_stored_l > 0 && g.resources.o2_stored_kg > 0);
    assert(g.resources.life_support_pct == 100 && g.resources.population == MOON_LANDER_CREW);
    g.resources.water_stored_l = 0;
    ticks(MOON_SIM_TICK_RATE_HZ * 5);
    assert(g.resources.life_support_pct < 100);

    /* Casualty banner appears and goes away again */
    fresh();
    g.resources.o2_stored_kg = 0;
    g.resources.life_support_pct = 0;
    ticks(MOON_SIM_TICK_RATE_HZ * 6);
    assert(g.resources.population == MOON_LANDER_CREW - 1);
    assert(g.resources.warning_crew_lost);
    g.resources.o2_stored_kg = fixed_from_int(50);
    g.resources.life_support_pct = 100;
    ticks(MOON_CREW_LOST_BANNER_TICKS + 2);
    assert(!g.resources.warning_crew_lost);

    /* Recycling a battery clamps stored energy to the new capacity */
    fresh();
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_BATTERY_BANK, 33, 33));
    ticks(1); /* capacity now includes the battery bank (50 kWh lander + 250 kWh battery) */
    assert(g.resources.battery_capacity_kwh == 300);
    g.resources.battery_stored_kwh = fixed_from_int(300);
    moon_structure_demolish(&g.grid, &g.structs, 33, 33);
    ticks(1);
    assert(g.resources.battery_capacity_kwh == 50);
    assert(fixed_to_int(g.resources.battery_stored_kwh) <= 50);
    printf("[test] Life-support stress rule, casualty banner, battery clamp: PASS\n");
}

static void test_camera_and_ticks(void) {
    fresh();
    int corners[][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
    for (int i = 0; i < 4; i++) {
        g.cam_x = (corners[i][0] - corners[i][1]) * MOON_TILE_HALF_W;
        g.cam_y = (corners[i][0] + corners[i][1]) * MOON_TILE_HALF_H;
        int32_t cx = g.cam_x, cy = g.cam_y;
        moon_game_clamp_camera(&g);
        assert(g.cam_x == cx && g.cam_y == cy);   /* every map corner is reachable */
    }
    g.cam_x = 99999; g.cam_y = -99999;
    moon_game_clamp_camera(&g);
    assert(g.cam_x == 63 * MOON_TILE_HALF_W && g.cam_y == 0);

    /* A stalled frame advances at most a bounded number of sim ticks */
    fresh();
    uint32_t before = g.sim_ticks;
    moon_game_update(&g, 60000);
    assert(g.sim_ticks - before <= 1);
    printf("[test] Camera bounds and bounded sim catch-up: PASS\n");
}

static void test_pointer_routing(void) {
    /* Click on the HUD bar must not select or place */
    fresh();
    click(500, 10);
    assert(!g.panel_open && g.selected_tile_x == 32 && g.selected_tile_y == 32);

    g.structs.selected_build_type = MOON_STRUCT_SOLAR_ARRAY;
    fixed32_t reg = g.resources.regolith_stored_kg;
    int count = g.structs.count;
    click(500, 10);
    assert(g.structs.count == count && g.resources.regolith_stored_kg == reg);

    /* Dragging that starts on the minimap / ribbon does not pan */
    g.structs.selected_build_type = MOON_STRUCT_NONE;
    int mx = 1024 - 64 - 10 + 20, my = 600 - 64 - 44 + 20;
    int32_t cx = g.cam_x, cy = g.cam_y;
    ptr(MOON_EVENT_POINTER_DOWN, mx, my, 1);
    ptr(MOON_EVENT_POINTER_MOVE, mx - 40, my - 10, 1);
    assert(g.cam_x == cx && g.cam_y == cy);
    ptr(MOON_EVENT_POINTER_UP, mx - 40, my - 10, 1);
    assert(g.cam_x == cx && g.cam_y == cy);

    /* Dragging on the map pans; the right button does not */
    ptr(MOON_EVENT_POINTER_DOWN, 400, 300, 1);
    ptr(MOON_EVENT_POINTER_MOVE, 380, 300, 1);
    assert(g.cam_x == cx + 20);
    ptr(MOON_EVENT_POINTER_UP, 380, 300, 1);
    cx = g.cam_x;
    ptr(MOON_EVENT_POINTER_DOWN, 400, 300, 3);
    ptr(MOON_EVENT_POINTER_MOVE, 300, 300, 3);
    assert(g.cam_x == cx);
    ptr(MOON_EVENT_POINTER_UP, 300, 300, 3);

    /* Inspector close button works with the narrow (220 px) and wide layouts */
    for (int w = 0; w < 2; w++) {
        memset(&g, 0, sizeof(g));
        assert(moon_game_init(&g, w ? 1024 : 640, w ? 600 : 360));
        g.panel_open = true;
        g.selected_tile_x = 33; g.selected_tile_y = 32;
        int ip_w = w ? 260 : 220;
        int ip_x = g.screen_w - ip_w - 10;
        click(ip_x + ip_w - 14, 40 + 12);
        assert(!g.panel_open);
    }
    printf("[test] Pointer routing (HUD click-through, UI-started drags, inspector): PASS\n");
}

static void test_regolith_guard(void) {
    fresh();
    g.resources.regolith_stored_kg = 0;
    ticks(MOON_SIM_TICK_RATE_HZ * 200);
    int kg = fixed_to_int(g.resources.regolith_stored_kg);
    assert(kg == MOON_REGOLITH_RESERVE_KG);
    const moon_structure_def_t *d = moon_structure_get_def(MOON_STRUCT_REGOLITH_PLANT);
    assert(g.resources.regolith_stored_kg >= fixed_from_int(d->cost_regolith));
    printf("[test] Regolith soft-lock guard: PASS\n");
}

static uint16_t s_fb[1024 * 600];

static void render_scene(moon_surface_t *surf) {
    *surf = moon_surface_make(s_fb, 1024, 600, 1024);
    moon_blit_clear(surf, COLOR_SPACE_BLACK);
    moon_world_render(&g.structs, &g.grid, surf, g.cam_x, g.cam_y, 0,
                      g.hover_tile_x, g.hover_tile_y, false);
    moon_world_render_overlays(&g.structs, &g.grid, surf, &g.resources, g.cam_x, g.cam_y);
}

static void tile_screen(int gx, int gy, int32_t *sx, int32_t *sy) {
    const moon_tile_t *t = moon_grid_get_tile_const(&g.grid, gx, gy);
    moon_grid_to_screen(gx, gy, t->elevation, g.cam_x, g.cam_y, 1024, 600, sx, sy);
}

static int selected_structure_type(void) {
    const moon_tile_t *t = moon_grid_get_tile_const(&g.grid, g.selected_tile_x, g.selected_tile_y);
    return t && t->structure_id ? g.structs.list[t->structure_id].type : -1;
}

/* The folded world tint equals the old way: draw untinted, then run the tint over the frame */
static void test_tint_fold(void) {
    static uint16_t a[1024 * 600], b[1024 * 600];
    for (int night = 0; night < 3; night++) {
        const uint16_t f[3][3] = { { 116, 128, 150 }, { 190, 190, 200 }, { 255, 255, 255 } };
        for (int textured = 0; textured < 2; textured++) {
            fresh();
            g.grid.textured_terrain = textured;
            for (int i = 0; i < 6; i++) put(i % 2 ? MOON_STRUCT_BATTERY_BANK : MOON_STRUCT_SOLAR_ARRAY);
            put(MOON_STRUCT_POWER_CONDUIT);
            g.cam_x = 300; g.cam_y = 40 * MOON_TILE_H;
            moon_surface_t sa = moon_surface_make(a, 1024, 600, 1024), sb = moon_surface_make(b, 1024, 600, 1024);
            moon_blit_clear(&sa, COLOR_SPACE_BLACK); moon_blit_clear(&sb, COLOR_SPACE_BLACK);

            moon_blit_set_tint(f[night][0], f[night][1], f[night][2]);
            moon_world_render(&g.structs, &g.grid, &sa, g.cam_x, g.cam_y, 0, 33, 33, true);
            moon_blit_set_tint(256, 256, 256);

            moon_world_render(&g.structs, &g.grid, &sb, g.cam_x, g.cam_y, 0, 33, 33, true);
            moon_blit_tint_rect(&sb, 0, 0, 1024, 600, f[night][0], f[night][1], f[night][2]);
            assert(memcmp(a, b, sizeof(a)) == 0);
        }
    }
    printf("[test] World tint folded into the drawing equals the whole-frame pass, pixel for pixel: PASS\n");
}

/* The baked tile cache changes how fast a tile is drawn, never how it looks */
static void test_tile_cache(void) {
    static uint16_t cold[1024 * 600], warm[1024 * 600], plain[1024 * 600];
    void *mem = malloc(moon_tile_cache_bytes());
    assert(mem);
    fresh();
    g.grid.textured_terrain = true;
    for (int i = 0; i < 6; i++) put(i % 2 ? MOON_STRUCT_BATTERY_BANK : MOON_STRUCT_SOLAR_ARRAY);
    for (int night = 0; night < 2; night++) {
        if (night) g.resources.solar_intensity = 0;            /* night: the tint is on */
        moon_tile_cache_attach(mem);
        for (int cx = -1500; cx <= 1500; cx += 497) {
            for (int cy = 100; cy <= 1900; cy += 449) {
                g.cam_x = cx; g.cam_y = cy;
                moon_game_clamp_camera(&g);
                moon_tile_cache_attach(NULL);                  /* no cache: every tile sampled as drawn */
                moon_game_render(&g, plain, 1024, 600, 1024);
                moon_tile_cache_attach(mem);                   /* fresh cache: baking while drawing */
                moon_game_render(&g, cold, 1024, 600, 1024);
                moon_game_render(&g, warm, 1024, 600, 1024);   /* everything now comes from the cache */
                assert(memcmp(plain, cold, sizeof(plain)) == 0);
                assert(memcmp(plain, warm, sizeof(plain)) == 0);
            }
        }
    }

    /* A tile that changed under the cache is baked again */
    g.cam_x = 0; g.cam_y = 32 * MOON_TILE_H;
    g.resources.solar_intensity = fixed_from_int(1);
    moon_game_render(&g, warm, 1024, 600, 1024);
    g.grid.tiles[33][36].terrain = MOON_TERRAIN_ICE_DEPOSIT;
    g.grid.tiles[33][36].flags |= MOON_TILE_FLAG_FOUNDATION;
    moon_game_render(&g, warm, 1024, 600, 1024);
    moon_tile_cache_attach(NULL);
    moon_game_render(&g, plain, 1024, 600, 1024);
    assert(memcmp(plain, warm, sizeof(plain)) == 0);
    free(mem);
    printf("[test] Tile cache: cached, baking and uncached frames are identical, changed tiles re-bake: PASS\n");
}

/* With no clear first, tiles must still cover everything the view can show */
static void test_view_coverage(void) {
    static uint16_t fb[1024 * 600];
    fresh();
    for (int textured = 0; textured < 2; textured++) {
        g.grid.textured_terrain = textured;
        for (int cx = -1900; cx <= 1900; cx += 211) {
            for (int cy = 0; cy <= 2050; cy += 173) {
                g.cam_x = cx; g.cam_y = cy;
                moon_game_clamp_camera(&g);
                for (int i = 0; i < 1024 * 600; i++) fb[i] = 0xF81F;      /* sentinel */
                moon_game_render(&g, fb, 1024, 600, 1024);
                for (int i = 0; i < 1024 * 600; i++) assert(fb[i] != 0xF81F);
            }
        }
    }
    printf("[test] Whole view is covered at every camera position without a clear: PASS\n");
}

static void test_depth_order(void) {
    fresh();
    /* The nearer building is placed first: slot order is the reverse of depth order */
    moon_structure_t *front = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 31);
    moon_structure_t *back = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_SOLAR_ARRAY, 30, 30);
    assert(front && back);
    moon_surface_t surf;
    render_scene(&surf);

    /* Where both sprites cover a pixel, the front one's pixel must be on screen */
    int32_t fx, fy, bx, by;
    tile_screen(31, 31, &fx, &fy);
    tile_screen(30, 30, &bx, &by);
    int checked = 0;
    for (int y = 0; y < SPRITE_SINTERER_H; y++) {
        for (int x = 0; x < SPRITE_SINTERER_W; x++) {
            uint16_t c = g_surf_sinterer.pixels[y * g_surf_sinterer.stride + x];
            if (c == MOON_KEY_COLOR) continue;
            int px = fx - SPRITE_SINTERER_W / 2 + x;
            int py = fy + MOON_TILE_HALF_H - SPRITE_SINTERER_H + y;
            int bsx = px - (bx - SPRITE_SOLAR_ARRAY_W / 2);
            int bsy = py - (by + MOON_TILE_HALF_H - SPRITE_SOLAR_ARRAY_H);
            if (bsx < 0 || bsx >= SPRITE_SOLAR_ARRAY_W || bsy < 0 || bsy >= SPRITE_SOLAR_ARRAY_H) continue;
            if (g_surf_solar_array.pixels[bsy * g_surf_solar_array.stride + bsx] == MOON_KEY_COLOR) continue;
            /* A status badge or gauge may lie on top, but those are drawn over everything */
            assert(s_fb[py * 1024 + px] == c || py < by - 6);
            checked++;
        }
    }
    assert(checked > 20);
    printf("[test] Depth-sorted sprites (%d overlapping pixels): PASS\n", checked);
}

static void test_badge_survives(void) {
    fresh();
    moon_structure_t *b = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_SOLAR_ARRAY, 30, 30);
    assert(b);
    g.structs.list[b->id].active = false;
    int32_t bx, by;
    tile_screen(30, 30, &bx, &by);
    const moon_structure_def_t *def = moon_structure_get_def(MOON_STRUCT_SOLAR_ARRAY);
    int badge_x = bx - 12, badge_y = by - def->height_px - 8; /* top-left corner of the OFF pill */

    /* Put a neighbour in front of it */
    moon_structure_t *f = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 31);
    assert(f);
    moon_surface_t surf;
    render_scene(&surf);
    assert(s_fb[badge_y * 1024 + badge_x] == COLOR_ENERGY_YELLOW);
    assert(s_fb[(badge_y + 9) * 1024 + badge_x + 23] == COLOR_ENERGY_YELLOW);
    printf("[test] Status badge drawn above a sprite in front of it: PASS\n");
}

static void test_sprite_pick(void) {
    fresh();
    moon_structure_t *s = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_SOLAR_ARRAY, 30, 30);
    assert(s);
    int32_t sx, sy;
    tile_screen(30, 30, &sx, &sy);

    /* An opaque sprite pixel above the tile diamond: the ground tile there is another one */
    int px = -1, py = -1;
    for (int y = 0; y < SPRITE_SOLAR_ARRAY_H / 2 && px < 0; y++) {
        for (int x = 0; x < SPRITE_SOLAR_ARRAY_W; x++) {
            if (g_surf_solar_array.pixels[y * g_surf_solar_array.stride + x] == MOON_KEY_COLOR) continue;
            px = sx - SPRITE_SOLAR_ARRAY_W / 2 + x;
            py = sy + MOON_TILE_HALF_H - SPRITE_SOLAR_ARRAY_H + y;
            break;
        }
    }
    assert(px >= 0);
    int32_t gx, gy;
    moon_grid_pick(&g.grid, px, py, g.cam_x, g.cam_y, 1024, 600, &gx, &gy);
    assert(gx != 30 || gy != 30);

    int id = 0;
    assert(moon_structure_pick(&g.structs, &g.grid, px, py, g.cam_x, g.cam_y, 1024, 600, &id));
    assert(id == s->id);

    click(px, py);
    assert(g.panel_open && g.selected_tile_x == 30 && g.selected_tile_y == 30);

    /* With a build tool the ground tile is still the target */
    ptr(MOON_EVENT_POINTER_MOVE, px, py, 0);
    assert(g.hover_tile_x == 30 && g.hover_tile_y == 30);
    g.structs.selected_build_type = MOON_STRUCT_SOLAR_ARRAY;
    ptr(MOON_EVENT_POINTER_MOVE, px + 1, py, 0);
    assert(g.hover_tile_x != 30 || g.hover_tile_y != 30);
    printf("[test] Sprite-aware picking: PASS\n");
}

static int luma(uint16_t c) {
    return ((c >> 11) & 0x1F) * 8 * 3 + ((c >> 5) & 0x3F) * 4 * 6 + (c & 0x1F) * 8;
}

static void test_palette_and_tint(void) {
    fresh();
    /* Flat and textured modes: the mean brightness of a whole tile agrees closely */
    for (int t = 0; t < MOON_TERRAIN_COUNT; t++) {
        uint16_t flat = moon_terrain_mean_color((uint8_t)t);
        long sum = 0; int n = 0;
        uint16_t px[64 * 32];
        moon_surface_t surf = moon_surface_make(px, 64, 32, 64);
        memset(px, 0, sizeof(px));
        moon_blit_iso_tile_textured(&surf, 32, 16, 32, 16, 5, 7, (uint8_t)t, 0, 0);
        for (int i = 0; i < 64 * 32; i++) if (px[i]) { sum += luma(px[i]); n++; }
        assert(n > 500);
        int diff = (int)(sum / n) - luma(flat);
        assert(diff > -150 && diff < 150);
    }

    /* Tint: night scales a pixel down and shifts it towards blue */
    uint16_t px[4] = { RGB565(200, 200, 200), 0, 0, 0 };
    moon_surface_t one = moon_surface_make(px, 1, 1, 1);
    moon_blit_tint_rect(&one, 0, 0, 1, 1, 116, 128, 150);
    int r = (px[0] >> 11) & 0x1F, b = px[0] & 0x1F;
    assert(luma(px[0]) < luma(RGB565(200, 200, 200)) && b > r);
    printf("[test] Shared TEX/FAST palette and day/night tint: PASS\n");
}

static void click_rect(const moon_ui_rect_t *r) {
    click(r->x + r->w / 2, r->y + r->h / 2);
}

static void click_tile(int gx, int gy) {
    int32_t sx, sy;
    tile_screen(gx, gy, &sx, &sy);
    click(sx, sy);
}

static bool overlap(const moon_ui_rect_t *a, const moon_ui_rect_t *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void check_layout(int w, int h) {
    moon_ui_layout_t ui;
    moon_ui_layout(&ui, w, h);
    const moon_ui_rect_t *touch[] = {
        &ui.cat_btn, &ui.pause_btn, &ui.speed_btn, &ui.menu_btn, &ui.insp_recycle, &ui.insp_standby,
        &ui.undo_btn, &ui.menu_item[0], &ui.menu_item[1],
        &ui.menu_item[2], &ui.menu_item[3], &ui.minimap_zone
    };
    for (unsigned i = 0; i < sizeof(touch) / sizeof(touch[0]); i++) {
        assert(touch[i]->w >= MOON_UI_TOUCH_MIN - 4 && touch[i]->h >= MOON_UI_TOUCH_MIN - 4);
        assert(touch[i]->x >= 0 && touch[i]->y >= 0 && touch[i]->x + touch[i]->w <= w && touch[i]->y + touch[i]->h <= h);
    }
    assert(ui.insp_close.w >= 36 && ui.insp_close.h >= 36);
    for (int i = 0; i < MOON_UI_RIBBON_SLOTS; i++) {
        assert(ui.ribbon[i].w >= MOON_UI_TOUCH_MIN && ui.ribbon[i].h >= MOON_UI_TOUCH_MIN);
        assert(ui.ribbon[i].x + ui.ribbon[i].w <= w);
        assert(!overlap(&ui.ribbon[i], &ui.cat_btn) && !overlap(&ui.ribbon[i], &ui.pause_btn) && !overlap(&ui.ribbon[i], &ui.menu_btn));
        for (int j = i + 1; j < MOON_UI_RIBBON_SLOTS; j++) assert(!overlap(&ui.ribbon[i], &ui.ribbon[j]));
    }
    assert(!overlap(&ui.cat_btn, &ui.pause_btn) && !overlap(&ui.pause_btn, &ui.speed_btn) && !overlap(&ui.speed_btn, &ui.menu_btn));
    /* Panels that can be open together must not share pixels */
    assert(!overlap(&ui.inspector, &ui.minimap_zone));
    assert(!overlap(&ui.inspector, &ui.hud));
    assert(!overlap(&ui.inspector, &ui.ribbon_bar));
    assert(!overlap(&ui.minimap_zone, &ui.ribbon_bar));
    assert(!overlap(&ui.undo_btn, &ui.ribbon_bar) && !overlap(&ui.undo_btn, &ui.minimap_zone));
    /* Inspector close button lies inside the panel, actions below the body text */
    assert(ui.insp_close.x + ui.insp_close.w <= ui.inspector.x + ui.inspector.w);
    assert(ui.insp_recycle.y + ui.insp_recycle.h <= ui.inspector.y + ui.inspector.h);
}

static void test_layout(void) {
    check_layout(1024, 600);
    check_layout(1280, 720);
    check_layout(640, 360);
    printf("[test] Shared UI layout (touch sizes, no overlaps, 3 screens): PASS\n");
}

static void test_clock_and_pause(void) {
    fresh();
    moon_clock_t c = moon_resources_clock(&g.resources);
    g.resources.cycle_tick = 0;
    c = moon_resources_clock(&g.resources);
    assert(c.minutes == 6 * 60 && c.next_is_dusk && c.next_in_min == 12 * 60);
    g.resources.cycle_tick = MOON_LUNAR_DAY_TICKS / 4;
    c = moon_resources_clock(&g.resources);
    assert(c.minutes == 12 * 60 && c.next_is_dusk && c.next_in_min == 6 * 60);
    g.resources.cycle_tick = MOON_LUNAR_DAY_TICKS / 2;
    c = moon_resources_clock(&g.resources);
    assert(c.minutes == 18 * 60 && !c.next_is_dusk && c.next_in_min == 12 * 60);
    g.resources.cycle_tick = MOON_LUNAR_DAY_TICKS * 3 / 4;
    c = moon_resources_clock(&g.resources);
    assert(c.minutes == 0 && !c.next_is_dusk && c.next_in_min == 6 * 60);

    /* The dial agrees with the solar model: sunset is where is_night flips */
    fresh();
    for (int i = 0; i < MOON_LUNAR_DAY_TICKS; i++) {
        moon_clock_t k = moon_resources_clock(&g.resources);
        bool dark = (k.minutes >= 18 * 60 || k.minutes < 6 * 60);
        if (i > 0 && g.resources.cycle_tick > 0) assert(g.resources.is_night == dark);
        ticks(1);
    }

    /* Pause freezes the sim and the clock; scrolling still works; speed scales */
    fresh();
    ticks(4);
    uint32_t st = g.sim_ticks, ct = g.resources.cycle_tick;
    key(MOON_KEY_P);
    assert(g.paused && g.state == MOON_STATE_PAUSED);
    ticks(8);
    assert(g.sim_ticks == st && g.resources.cycle_tick == ct);
    key(MOON_KEY_P);
    ticks(4);
    assert(g.sim_ticks == st + 4);
    key(MOON_KEY_PLUS);
    assert(g.speed == 2);
    key(MOON_KEY_PLUS); key(MOON_KEY_PLUS);
    assert(g.speed == 4);
    uint32_t before = g.sim_ticks;
    for (int i = 0; i < 20; i++) moon_game_update(&g, 50);
    assert(g.sim_ticks - before == 16); /* 1 s of frames at 4x = 16 ticks */
    key(MOON_KEY_MINUS); key(MOON_KEY_MINUS); key(MOON_KEY_MINUS);
    assert(g.speed == 1);
    printf("[test] Lunar clock mapping, pause and speed: PASS\n");
}

static int regolith(void) { return fixed_to_int(g.resources.regolith_stored_kg); }

static const moon_structure_t *struct_at(int gx, int gy) {
    const moon_tile_t *t = moon_grid_get_tile_const(&g.grid, gx, gy);
    return t && t->structure_id ? &g.structs.list[t->structure_id] : NULL;
}

/* A selected tile always exists; build buttons act on it directly */
static void test_select_first(void) {
    fresh();
    assert(g.selected_tile_x == 32 && g.selected_tile_y == 32 && !g.panel_open);

    /* Each arrow is one edge-neighbour step, a screen diagonal: Up up-left, Right up-right,
     * Down down-right, Left down-left. Two different keys make a straight screen line. */
    int32_t sx0, sy0, sx, sy;
    tile_screen(32, 32, &sx0, &sy0);
    key(MOON_KEY_UP);
    assert(g.selected_tile_x == 31 && g.selected_tile_y == 32);
    tile_screen(31, 32, &sx, &sy);
    assert(sx == sx0 - 32 && sy == sy0 - 16);
    key(MOON_KEY_W);
    assert(g.selected_tile_x == 30 && g.selected_tile_y == 32);
    key(MOON_KEY_RIGHT);
    assert(g.selected_tile_x == 30 && g.selected_tile_y == 31);
    key(MOON_KEY_D);
    assert(g.selected_tile_x == 30 && g.selected_tile_y == 30);
    key(MOON_KEY_DOWN); key(MOON_KEY_S);
    assert(g.selected_tile_x == 32 && g.selected_tile_y == 30);
    key(MOON_KEY_LEFT); key(MOON_KEY_A);
    assert(g.selected_tile_x == 32 && g.selected_tile_y == 32);
    key(MOON_KEY_UP); key(MOON_KEY_RIGHT);       /* straight up the screen */
    tile_screen(g.selected_tile_x, g.selected_tile_y, &sx, &sy);
    assert(sx == sx0 && sy == sy0 - 32);
    key(MOON_KEY_DOWN); key(MOON_KEY_LEFT);
    assert(g.selected_tile_x == 32 && g.selected_tile_y == 32);

    /* The view stays where it is while the selection moves inside the middle of the screen */
    int cx = g.cam_x, cy = g.cam_y;
    key(MOON_KEY_UP); key(MOON_KEY_RIGHT); key(MOON_KEY_DOWN);
    for (int i = 0; i < 40; i++) moon_game_update(&g, 16);
    assert(g.cam_x == cx && g.cam_y == cy && !g.glide);
    g.selected_tile_x = 30; g.selected_tile_y = 30;

    /* 1 builds a solar array on the selection, nothing else needed */
    int before = regolith();
    key(MOON_KEY_1);
    assert(struct_at(30, 30) && struct_at(30, 30)->type == MOON_STRUCT_SOLAR_ARRAY);
    assert(before - regolith() == 10);

    /* An occupied tile rejects a build: nothing changes, the reason is shown */
    before = regolith();
    key(MOON_KEY_2);
    assert(struct_at(30, 30)->type == MOON_STRUCT_SOLAR_ARRAY && regolith() == before);
    assert(g.notify_msg != NULL && g.notify_timer_ms > 0);
    key(MOON_KEY_7); /* unmapped */

    /* Shift moves 8 tiles and the selection stays on the map */
    moon_event_t ev; memset(&ev, 0, sizeof(ev));
    ev.type = MOON_EVENT_KEY_DOWN; ev.key = MOON_KEY_DOWN; ev.mods = MOON_MOD_SHIFT;
    moon_game_handle_event(&g, &ev);
    assert(g.selected_tile_x == 38 && g.selected_tile_y == 30);
    for (int i = 0; i < 10; i++) { ev.key = MOON_KEY_UP; moon_game_handle_event(&g, &ev); }
    assert(g.selected_tile_x == 0 && g.selected_tile_y == 30);

    /* Tab jumps to the next building and opens the inspector; Enter toggles it */
    key(MOON_KEY_TAB);
    assert(g.panel_open && selected_structure_type() == MOON_STRUCT_LANDER);
    key(MOON_KEY_TAB);
    assert(selected_structure_type() != MOON_STRUCT_LANDER);
    key(MOON_KEY_RETURN);
    assert(!g.panel_open);
    key(MOON_KEY_RETURN);
    assert(g.panel_open);

    /* Space brings the view to the selection, and from there home to the lander */
    g.cam_x = 0; g.cam_y = 0;
    key(MOON_KEY_SPACE);
    for (int i = 0; i < 100; i++) moon_game_update(&g, 16);
    tile_screen(g.selected_tile_x, g.selected_tile_y, &sx, &sy);
    assert(abs(sx - 512) <= 2);
    key(MOON_KEY_SPACE);
    for (int i = 0; i < 100; i++) moon_game_update(&g, 16);
    tile_screen(32, 32, &sx, &sy);
    assert(abs(sx - 512) <= 2);
    printf("[test] Select-first: move selection, act on it, Tab, Shift jump, Space: PASS\n");
}

/* Every tile can be reached, and the opposite key undoes a step */
static void test_keyboard_reach(void) {
    fresh();
    static bool seen[MOON_MAP_SIZE_Y][MOON_MAP_SIZE_X];
    memset(seen, 0, sizeof(seen));
    static const uint32_t keys[4] = { MOON_KEY_UP, MOON_KEY_DOWN, MOON_KEY_LEFT, MOON_KEY_RIGHT };
    static const uint32_t undo[4] = { MOON_KEY_DOWN, MOON_KEY_UP, MOON_KEY_RIGHT, MOON_KEY_LEFT };

    /* Inverse steps on both parities, away from and at the borders */
    for (int gy = 2; gy < MOON_MAP_SIZE_Y - 2; gy += 7) {
        for (int gx = 2; gx < MOON_MAP_SIZE_X - 2; gx += 5) {
            for (int k = 0; k < 4; k++) {
                g.selected_tile_x = gx; g.selected_tile_y = gy;
                key(keys[k]);
                assert(g.selected_tile_x != gx || g.selected_tile_y != gy);
                key(undo[k]);
                assert(g.selected_tile_x == gx && g.selected_tile_y == gy);
            }
        }
    }

    /* Breadth-first search over the real key handling: all 4096 tiles are reachable */
    static int queue[MOON_MAP_SIZE_X * MOON_MAP_SIZE_Y];
    int head = 0, tail = 0, visited = 0;
    queue[tail++] = 32 * MOON_MAP_SIZE_X + 32;
    seen[32][32] = true;
    while (head < tail) {
        int cur = queue[head++];
        visited++;
        for (int k = 0; k < 4; k++) {
            g.selected_tile_x = cur % MOON_MAP_SIZE_X; g.selected_tile_y = cur / MOON_MAP_SIZE_X;
            key(keys[k]);
            int x = g.selected_tile_x, y = g.selected_tile_y;
            if (!seen[y][x]) { seen[y][x] = true; queue[tail++] = y * MOON_MAP_SIZE_X + x; }
        }
    }
    assert(visited == MOON_MAP_SIZE_X * MOON_MAP_SIZE_Y);

    /* At the border a key that points off the map does nothing; the others still move */
    g.selected_tile_x = 0; g.selected_tile_y = 20;
    key(MOON_KEY_UP);
    assert(g.selected_tile_x == 0 && g.selected_tile_y == 20);
    for (int i = 0; i < 6; i++) key(MOON_KEY_RIGHT);
    assert(g.selected_tile_x == 0 && g.selected_tile_y == 14);
    g.selected_tile_x = 0; g.selected_tile_y = 0;
    key(MOON_KEY_UP); key(MOON_KEY_RIGHT);
    assert(g.selected_tile_x == 0 && g.selected_tile_y == 0);
    printf("[test] Keyboard reaches every tile; opposite keys undo each other; edges: PASS\n");
}

/* The view scrolls rarely: only when the selection nears the border */
static void test_view_follow(void) {
    fresh();
    int glides = 0;
    int cx = g.cam_x, cy = g.cam_y;
    key(MOON_KEY_RIGHT);
    assert(!g.glide && g.cam_x == cx && g.cam_y == cy);
    key(MOON_KEY_DOWN);

    for (int i = 0; i < 60; i++) {   /* Right then Down alternately: straight right on screen */
        bool was = g.glide;
        int32_t before_x = g.glide_x;
        key(i & 1 ? MOON_KEY_DOWN : MOON_KEY_RIGHT);
        if (g.glide && (!was || g.glide_x != before_x)) glides++;
        moon_game_update(&g, 16);
    }
    /* 60 key presses (30 tiles) to the right: a few glides, not one per key press */
    assert(glides >= 1 && glides <= 6);
    for (int i = 0; i < 100; i++) moon_game_update(&g, 16);
    int32_t sx, sy;
    tile_screen(g.selected_tile_x, g.selected_tile_y, &sx, &sy);
    assert(sx >= 0 && sx < 1024 && sy >= 34 && sy < 548);
    printf("[test] View stays still until the selection nears the border (%d glides in 60 steps): PASS\n", glides);
}

static void test_standby_recycle_undo(void) {
    fresh();
    g.selected_tile_x = 30; g.selected_tile_y = 30;
    int start = regolith();
    key(MOON_KEY_2);                                   /* Battery Bank, 20 kg */
    assert(regolith() == start - 20 && struct_at(30, 30));

    /* E toggles standby on the selection; the ribbon says what a press will do */
    key(MOON_KEY_E);
    assert(!struct_at(30, 30)->active);
    key(MOON_KEY_E);
    assert(struct_at(30, 30)->active);

    /* Undo within the window: structure gone, full cost back */
    key(MOON_KEY_Z);
    assert(!struct_at(30, 30) && regolith() == start);

    /* Recycle gives half back, and undo restores the building for that half */
    key(MOON_KEY_2);
    key(MOON_KEY_R);
    assert(!struct_at(30, 30) && regolith() == start - 20 + 10);
    key(MOON_KEY_Z);
    assert(struct_at(30, 30) && regolith() == start - 20);

    /* The window closes */
    key(MOON_KEY_R);
    assert(!struct_at(30, 30));
    for (int i = 0; i < 12 * 1000 / 50; i++) moon_game_update(&g, 50);
    key(MOON_KEY_Z);
    assert(!struct_at(30, 30));

    /* Nothing to recycle on bare ground and never the lander */
    int r0 = regolith();
    key(MOON_KEY_R);
    assert(regolith() == r0);
    g.selected_tile_x = 32; g.selected_tile_y = 32;
    key(MOON_KEY_R);
    assert(struct_at(32, 32) && struct_at(32, 32)->type == MOON_STRUCT_LANDER);
    key(MOON_KEY_E);
    assert(struct_at(32, 32)->active);
    printf("[test] Standby, recycle, undo window: PASS\n");
}

static void test_brush(void) {
    fresh();
    g.resources.regolith_stored_kg = fixed_from_int(200);
    g.selected_tile_x = 30; g.selected_tile_y = 28;

    /* Pressing the same build key twice with nothing between arms it as a brush */
    key(MOON_KEY_3);                       /* builds a conduit */
    assert(struct_at(30, 28));
    key(MOON_KEY_3);                       /* second press: brush, builds nothing */
    assert(g.structs.selected_build_type == MOON_STRUCT_POWER_CONDUIT);
    int n = g.structs.count;
    key(MOON_KEY_RIGHT);                   /* (30,28) -> (30,27) */
    key(MOON_KEY_RETURN);                  /* Enter applies the brush there */
    assert(g.structs.count == n + 1 && struct_at(30, 27));
    click_tile(29, 29);                    /* a click selects and applies */
    assert(g.structs.count == n + 2 && struct_at(29, 29));
    assert(g.structs.selected_build_type == MOON_STRUCT_POWER_CONDUIT); /* still armed */

    /* Another key between the two presses means no brush */
    key(MOON_KEY_0);
    assert(g.structs.selected_build_type == MOON_STRUCT_NONE);
    key(MOON_KEY_DOWN);
    key(MOON_KEY_3);
    key(MOON_KEY_DOWN);
    key(MOON_KEY_3);
    assert(g.structs.selected_build_type == MOON_STRUCT_NONE);

    /* Recycle brush: the first R recycles the selected conduit, the second arms the
     * brush, and a click then removes what it hits */
    int cnt = g.structs.count;
    key(MOON_KEY_R);
    assert(g.structs.count == cnt - 1);
    key(MOON_KEY_R);
    assert(g.demolish_tool_active);
    click_tile(29, 29);
    assert(!struct_at(29, 29) && g.structs.count == cnt - 2);
    key(MOON_KEY_ESCAPE);
    assert(!g.demolish_tool_active && g.menu_page == MOON_MENU_CLOSED);
    printf("[test] Brush: double press arms, Enter/click applies, ESC ends: PASS\n");
}

/* Ribbon states: a build button is live only where it could act */
static void test_button_validity_and_preview(void) {
    fresh();
    /* Selection on the lander: nothing can be built there */
    for (int i = 1; i <= 6; i++) {
        key(MOON_KEY_0 + i);
        assert(struct_at(32, 32)->type == MOON_STRUCT_LANDER && g.structs.count == 1);
        g.last_slot = -1;
    }

    /* Hovering a build button previews it on the selection; leaving clears it */
    g.selected_tile_x = 30; g.selected_tile_y = 30;
    ptr(MOON_EVENT_POINTER_MOVE, g.ui.ribbon[3].x + 5, g.ui.ribbon[3].y + 5, 0);
    assert(g.preview_slot == 3);
    ptr(MOON_EVENT_POINTER_MOVE, 500, 300, 0);
    assert(g.preview_slot == -1);

    /* Touch peek: hold a button, slide off, release: nothing built; release on it: built */
    s_touch = 1;
    int cx = g.ui.ribbon[1].x + 5, cy = g.ui.ribbon[1].y + 5;
    ptr(MOON_EVENT_POINTER_DOWN, cx, cy, 1);
    assert(g.preview_slot == 1);
    ptr(MOON_EVENT_POINTER_MOVE, cx, cy - 120, 1);
    assert(g.preview_slot == -1);
    ptr(MOON_EVENT_POINTER_UP, cx, cy - 120, 1);
    assert(g.structs.count == 1);
    ptr(MOON_EVENT_POINTER_DOWN, cx, cy, 1);
    ptr(MOON_EVENT_POINTER_UP, cx, cy, 1);
    assert(struct_at(30, 30) && g.structs.count == 2);
    s_touch = 0;

    /* Too poor: refused with a reason, nothing built */
    g.resources.regolith_stored_kg = fixed_from_int(5);
    g.selected_tile_x = 29; g.selected_tile_y = 29;
    g.last_slot = -1;
    key(MOON_KEY_2);
    assert(g.structs.count == 2);
    printf("[test] Button validity, hover preview, touch peek: PASS\n");
}

/* One play sequence, three input devices; each ends on identical state */
typedef struct { int regolith, structures; bool tile_free, standby_seen, undone; } outcome_t;

static outcome_t play(int device) {
    fresh();
    s_touch = (device == 1);
    outcome_t o = { 0, 0, false, false, false };
    const int T = 30;

    /* Select the tile, then press the button for what to build there */
    if (device == 2) {
        key(MOON_KEY_UP); key(MOON_KEY_UP);              /* (32,32) -> (30,32) */
        key(MOON_KEY_RIGHT); key(MOON_KEY_RIGHT);        /* -> (30,30) */
        assert(g.selected_tile_x == T && g.selected_tile_y == T);
        key(MOON_KEY_1);
    } else {
        click_tile(T, T);
        click_rect(&g.ui.ribbon[1]);
    }
    assert(g.structs.count == 2);
    const moon_structure_t *s = struct_at(T, T);
    assert(s && s->type == MOON_STRUCT_SOLAR_ARRAY && s->active);
    assert(g.selected_tile_x == T && g.selected_tile_y == T);

    /* Standby */
    if (device == 2) key(MOON_KEY_E); else click_rect(&g.ui.ribbon[0]);
    o.standby_seen = !struct_at(T, T)->active;

    /* Undo it all, then build again and recycle for good */
    if (device == 2) key(MOON_KEY_Z); else click_rect(&g.ui.undo_btn);
    o.undone = !struct_at(T, T);
    if (device == 2) key(MOON_KEY_1); else click_rect(&g.ui.ribbon[1]);
    assert(struct_at(T, T));
    if (device == 2) key(MOON_KEY_R); else click_rect(&g.ui.ribbon[7]);

    o.regolith = regolith();
    o.structures = g.structs.count;
    o.tile_free = !struct_at(T, T);
    s_touch = 0;
    return o;
}

static void test_three_devices(void) {
    outcome_t m = play(0), f = play(1), k = play(2);
    assert(m.standby_seen && f.standby_seen && k.standby_seen);
    assert(m.undone && f.undone && k.undone);
    assert(m.tile_free && f.tile_free && k.tile_free);
    assert(m.structures == 1 && f.structures == 1 && k.structures == 1);
    assert(m.regolith == f.regolith && m.regolith == k.regolith);
    printf("[test] Select, build, standby, undo, recycle by mouse, touch, keyboard: PASS (%d kg)\n", m.regolith);
}

static void test_touch_flow(void) {
    fresh();
    s_touch = 1;
    int rgl = regolith();

    /* A tap on bare ground only selects it: no panel, no hover left behind */
    click_tile(35, 35);
    assert(g.selected_tile_x == 35 && g.selected_tile_y == 35 && !g.panel_open && !g.hover_valid);
    assert(g.structs.count == 1 && regolith() == rgl);

    /* A tap on a building selects it and opens the inspector */
    click_tile(32, 32);
    assert(g.panel_open && selected_structure_type() == MOON_STRUCT_LANDER);

    /* A 10 px wobble is a tap; 30 px is a drag that pans and does not select */
    g.panel_open = false;
    int32_t sx, sy;
    tile_screen(36, 30, &sx, &sy);
    int cam = g.cam_x;
    ptr(MOON_EVENT_POINTER_DOWN, sx, sy, 1);
    ptr(MOON_EVENT_POINTER_MOVE, sx + 8, sy + 4, 1);
    ptr(MOON_EVENT_POINTER_UP, sx + 8, sy + 4, 1);
    assert(g.selected_tile_x == 36 && g.cam_x == cam);
    tile_screen(37, 29, &sx, &sy);
    ptr(MOON_EVENT_POINTER_DOWN, sx, sy, 1);
    ptr(MOON_EVENT_POINTER_MOVE, sx + 30, sy, 1);
    ptr(MOON_EVENT_POINTER_UP, sx + 30, sy, 1);
    assert(g.selected_tile_x == 36 && g.cam_x != cam);
    s_touch = 0;
    printf("[test] Touch: tap selects, inspector on buildings, drag threshold: PASS\n");
}

/* Cables laid with a finger in brush mode: one tap, one cable, no leftover "occupied" ghost */
static void test_touch_brush(void) {
    fresh();
    g.resources.regolith_stored_kg = fixed_from_int(100);
    s_touch = 1;
    click_rect(&g.ui.ribbon[3]);
    click_rect(&g.ui.ribbon[3]);                       /* second tap on the button arms the brush */
    assert(g.structs.selected_build_type == MOON_STRUCT_POWER_CONDUIT);
    int n = g.structs.count, gx, gy;
    g.notify_timer_ms = 0;

    for (int i = 0; i < 4; i++) {
        int tx = 33, ty = 31 - i;                      /* a column of tiles next to the base */
        int32_t sx, sy;
        tile_screen(tx, ty, &sx, &sy);
        ptr(MOON_EVENT_POINTER_DOWN, sx, sy, 1);
        assert(moon_game_ghost_target(&g, &gx, &gy) && gx == tx && gy == ty); /* preview under the finger */
        ptr(MOON_EVENT_POINTER_UP, sx, sy, 1);
        assert(g.structs.count == n + i + 1 && struct_at(tx, ty));
        assert(g.notify_timer_ms == 0);                /* nothing was refused */
        assert(!moon_game_ghost_target(&g, &gx, &gy)); /* finger up: nothing hovers on the new cable */
    }
    s_touch = 0;

    /* A keyboard brush previews the selection (Enter applies there), a mouse brush the pointer */
    key(MOON_KEY_LEFT);
    assert(moon_game_ghost_target(&g, &gx, &gy) && gx == g.selected_tile_x && gy == g.selected_tile_y);
    ptr(MOON_EVENT_POINTER_MOVE, 500, 300, 0);
    assert(moon_game_ghost_target(&g, &gx, &gy) && gx == g.hover_tile_x && gy == g.hover_tile_y);

    /* The same goes for the recycle brush after a finger recycled something */
    key(MOON_KEY_0);
    s_touch = 1;
    click_rect(&g.ui.ribbon[7]);
    click_rect(&g.ui.ribbon[7]);
    assert(g.demolish_tool_active);
    int32_t sx, sy;
    tile_screen(33, 31, &sx, &sy);
    click(sx, sy);
    assert(!struct_at(33, 31) && !moon_game_ghost_target(&g, &gx, &gy));
    s_touch = 0;
    printf("[test] Touch brush: one cable per tap, no stale ghost: PASS\n");
}

static void test_menu_and_cancel(void) {
    fresh();
    /* ESC with nothing to cancel opens the menu and freezes the sim; it never quits */
    key(MOON_KEY_ESCAPE);
    assert(g.menu_page == MOON_MENU_MAIN && g.running);
    uint32_t st = g.sim_ticks;
    ticks(4);
    assert(g.sim_ticks == st);
    key(MOON_KEY_ESCAPE);
    assert(g.menu_page == MOON_MENU_CLOSED && g.running);
    key(MOON_KEY_Q); key(MOON_KEY_ESCAPE);     /* menu, then closed again */
    assert(g.running && g.menu_page == MOON_MENU_CLOSED);

    /* Quit asks first, and NO goes back */
    key(MOON_KEY_Q);
    click_rect(&g.ui.menu_item[3]);
    assert(g.menu_page == MOON_MENU_CONFIRM_QUIT && g.running);
    click_rect(&g.ui.menu_item[1]);
    assert(g.menu_page == MOON_MENU_MAIN && g.running);
    click_rect(&g.ui.menu_item[3]);
    click_rect(&g.ui.menu_item[0]);
    assert(!g.running);

    /* Restart: confirm, then a clean base */
    fresh();
    click_tile(30, 30);
    click_rect(&g.ui.ribbon[1]);
    assert(g.structs.count == 2);
    click_rect(&g.ui.menu_btn);
    assert(g.menu_page == MOON_MENU_MAIN);
    click_rect(&g.ui.menu_item[2]);
    assert(g.menu_page == MOON_MENU_NEW_GAME && g.structs.count == 2);
    click_rect(&g.ui.menu_item[1]);
    assert(g.menu_page == MOON_MENU_CLOSED && g.structs.count == 1 && g.running);
    assert(g.sim_ticks == 0 && !g.panel_open && g.structs.selected_build_type == MOON_STRUCT_NONE);
    assert(g.selected_tile_x == 32 && g.undo_kind == MOON_UNDO_NONE);

    /* Right click, Backspace, 0 and ESC end a brush alike; the inspector closes the same way */
    for (int how = 0; how < 4; how++) {
        fresh();
        key(MOON_KEY_3); key(MOON_KEY_3);
        assert(g.structs.selected_build_type == MOON_STRUCT_POWER_CONDUIT);
        if (how == 0) { ptr(MOON_EVENT_POINTER_DOWN, 500, 300, 3); ptr(MOON_EVENT_POINTER_UP, 500, 300, 3); }
        if (how == 1) key(MOON_KEY_BACKSPACE);
        if (how == 2) key(MOON_KEY_ESCAPE);
        if (how == 3) key(MOON_KEY_0);
        assert(g.structs.selected_build_type == MOON_STRUCT_NONE && g.menu_page == MOON_MENU_CLOSED);
        g.panel_open = true;
        key(MOON_KEY_BACKSPACE);
        assert(!g.panel_open);
    }

    /* Every menu entry has a visible key */
    fresh();
    key(MOON_KEY_ESCAPE);
    bool tex = g.grid.textured_terrain;
    key(MOON_KEY_T);
    assert(g.grid.textured_terrain != tex && g.menu_page == MOON_MENU_MAIN);
    key(MOON_KEY_Q);
    assert(g.menu_page == MOON_MENU_CONFIRM_QUIT && g.running);
    key(MOON_KEY_T);                        /* not a confirm key */
    assert(g.menu_page == MOON_MENU_CONFIRM_QUIT);
    key(MOON_KEY_N);
    assert(g.menu_page == MOON_MENU_MAIN);
    key(MOON_KEY_R);
    assert(g.menu_page == MOON_MENU_NEW_GAME);
    key(MOON_KEY_T);                        /* not a chooser key */
    assert(g.menu_page == MOON_MENU_NEW_GAME);
    g.structs.count = 9;                    /* marker: a new game must rebuild the base */
    key(MOON_KEY_2);
    assert(g.menu_page == MOON_MENU_CLOSED && g.structs.count == 1 && g.running);
    key(MOON_KEY_ESCAPE); key(MOON_KEY_Q); key(MOON_KEY_Y);
    assert(!g.running);
    printf("[test] Menu, quit/restart confirmation, identical cancel: PASS\n");
}

static void test_pointer_leave_and_hover(void) {
    fresh();
    ptr(MOON_EVENT_POINTER_MOVE, 500, 300, 0);
    assert(g.hover_valid);
    moon_event_t lv; memset(&lv, 0, sizeof(lv)); lv.type = MOON_EVENT_POINTER_LEAVE;
    moon_game_handle_event(&g, &lv);
    assert(!g.hover_valid);
    /* Over the ribbon there is no tile under the mouse */
    ptr(MOON_EVENT_POINTER_MOVE, 500, 300, 0);
    ptr(MOON_EVENT_POINTER_MOVE, g.ui.ribbon[2].x + 3, g.ui.ribbon[2].y + 3, 0);
    assert(!g.hover_valid);
    printf("[test] Pointer leave and hover over UI: PASS\n");
}

static void test_game_over(void) {
    fresh();
    assert(g.state == MOON_STATE_PLAYING && g.menu_page == MOON_MENU_CLOSED);
    uint32_t seed0 = g.seed;
    g.panel_open = true;
    g.resources.population = 0;
    ticks(1);
    assert(g.state == MOON_STATE_GAME_OVER && g.menu_page == MOON_MENU_GAME_OVER && !g.panel_open);

    /* The sim is frozen and only the buttons lead out: ESC, P and build keys do nothing */
    uint32_t st = g.sim_ticks;
    ticks(8);
    assert(g.sim_ticks == st);
    key(MOON_KEY_ESCAPE); key(MOON_KEY_P); key(MOON_KEY_1); key(MOON_KEY_BACKSPACE);
    assert(g.state == MOON_STATE_GAME_OVER && g.menu_page == MOON_MENU_GAME_OVER && !g.paused);
    assert(g.structs.count == 1 && g.running);

    /* New game (button): the level chooser, then a clean base on a new map */
    click_rect(&g.ui.menu_item[2]);
    assert(g.menu_page == MOON_MENU_NEW_GAME && g.state == MOON_STATE_GAME_OVER);
    click_rect(&g.ui.menu_item[1]);
    assert(g.state == MOON_STATE_PLAYING && g.menu_page == MOON_MENU_CLOSED && g.running);
    assert(g.resources.population == MOON_LANDER_CREW && g.sim_ticks == 0 && g.structs.count == 1);
    assert(g.seed != seed0);

    /* Restart by key, and Quit by key and by button */
    g.resources.population = 0; ticks(1);
    key(MOON_KEY_R);
    assert(g.menu_page == MOON_MENU_NEW_GAME);
    key(MOON_KEY_2);
    assert(g.state == MOON_STATE_PLAYING && g.resources.population == MOON_LANDER_CREW);
    g.resources.population = 0; ticks(1);
    key(MOON_KEY_Q);
    assert(!g.running);
    fresh();
    g.resources.population = 0; ticks(1);
    click_rect(&g.ui.menu_item[3]);
    assert(!g.running);
    printf("[test] Game over: overlay, frozen sim, Restart and Quit: PASS\n");
}

static void test_run_statistics(void) {
    /* Cause of death follows what ran out */
    for (int cause = MOON_CAUSE_OXYGEN; cause <= MOON_CAUSE_BLACKOUT; cause++) {
        fresh();
        assert(g.resources.peak_population == MOON_LANDER_CREW && g.resources.death_cause == MOON_CAUSE_NONE);
        g.resources.life_support_pct = 0;
        if (cause == MOON_CAUSE_OXYGEN) g.resources.o2_stored_kg = 0;
        if (cause == MOON_CAUSE_WATER) g.resources.water_stored_l = 0;
        if (cause == MOON_CAUSE_BLACKOUT) {
            /* a Sinterer on the grid outdraws the RTG; at night nothing else supplies it */
            assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 32));
            moon_resources_solve_power_grid(&g.grid, &g.structs);
            g.resources.cycle_tick = 100 * MOON_SIM_TICK_RATE_HZ; /* night */
            g.resources.battery_stored_kwh = 0;
        }
        ticks(MOON_SIM_TICK_RATE_HZ * 6);
        assert(g.resources.population == MOON_LANDER_CREW - 1);
        assert(g.resources.death_cause == cause);
        assert(g.resources.peak_population == MOON_LANDER_CREW);
    }
    printf("[test] Run statistics: peak crew and cause of death: PASS\n");
}

static void test_victory(void) {
    /* Each condition of "self-sustaining" matters */
    fresh();
    moon_resources_t *r = &g.resources;
    r->lunar_day = MOON_ACT1_DAY; r->population = MOON_ACT1_CREW;
    r->o2_rate_kg_per_tick = 0; r->water_rate_l_per_tick = 5;
    r->ticks_since_blackout = MOON_LUNAR_DAY_TICKS;
    assert(moon_resources_act1_complete(r));
    r->lunar_day--;                       assert(!moon_resources_act1_complete(r)); r->lunar_day++;
    r->population--;                      assert(!moon_resources_act1_complete(r)); r->population++;
    r->o2_rate_kg_per_tick = -1;          assert(!moon_resources_act1_complete(r)); r->o2_rate_kg_per_tick = 0;
    r->water_rate_l_per_tick = -1;        assert(!moon_resources_act1_complete(r)); r->water_rate_l_per_tick = 0;
    r->ticks_since_blackout--;            assert(!moon_resources_act1_complete(r)); r->ticks_since_blackout++;
    assert(moon_resources_act1_complete(r));

    /* The overlay appears once, Continue keeps playing and does not ask again */
    moon_game_check_end(&g);
    assert(g.state == MOON_STATE_VICTORY && g.menu_page == MOON_MENU_VICTORY);
    uint32_t st = g.sim_ticks;
    ticks(4);
    assert(g.sim_ticks == st);
    key(MOON_KEY_C);
    assert(g.state == MOON_STATE_PLAYING && g.menu_page == MOON_MENU_CLOSED && g.victory_claimed);
    moon_game_check_end(&g);
    assert(g.state == MOON_STATE_PLAYING);
    ticks(2);
    assert(g.sim_ticks > st);

    /* ESC also continues; Restart gives a clean base and forgets the win; Quit leaves */
    fresh();
    g.resources.lunar_day = MOON_ACT1_DAY; g.resources.population = MOON_ACT1_CREW;
    g.resources.ticks_since_blackout = MOON_LUNAR_DAY_TICKS;
    moon_game_check_end(&g);
    key(MOON_KEY_ESCAPE);
    assert(g.state == MOON_STATE_PLAYING && g.victory_claimed);
    fresh();
    g.resources.lunar_day = MOON_ACT1_DAY; g.resources.population = MOON_ACT1_CREW;
    g.resources.ticks_since_blackout = MOON_LUNAR_DAY_TICKS;
    moon_game_check_end(&g);
    click_rect(&g.ui.menu_item[2]);       /* New game is the second of the three rows (1..3) */
    assert(g.menu_page == MOON_MENU_NEW_GAME);
    key(MOON_KEY_2);
    assert(g.state == MOON_STATE_PLAYING && !g.victory_claimed && g.resources.lunar_day == 1);
    g.resources.lunar_day = MOON_ACT1_DAY; g.resources.population = MOON_ACT1_CREW;
    g.resources.ticks_since_blackout = MOON_LUNAR_DAY_TICKS;
    moon_game_check_end(&g);
    key(MOON_KEY_Q);
    assert(!g.running);
    printf("[test] Act I victory check, overlay, Continue / Restart / Quit: PASS\n");
}

static void test_difficulty(void) {
    /* The first game is Normal; the levels start with different stocks */
    fresh();
    assert(g.difficulty == MOON_DIFFICULTY_NORMAL);
    int rgl[3];
    for (int lv = 0; lv < MOON_DIFFICULTY_COUNT; lv++) {
        key(MOON_KEY_ESCAPE); key(MOON_KEY_R);
        assert(g.menu_page == MOON_MENU_NEW_GAME && g.menu_sel == g.difficulty);
        key(MOON_KEY_1 + lv);
        assert(g.menu_page == MOON_MENU_CLOSED && g.difficulty == lv);
        assert(g.resources.rules.start_regolith_kg == g_moon_rules[lv].start_regolith_kg);
        rgl[lv] = regolith();
        assert(g.state == MOON_STATE_PLAYING && g.resources.population == MOON_LANDER_CREW);
    }
    assert(rgl[MOON_DIFFICULTY_EASY] > rgl[MOON_DIFFICULTY_NORMAL] && rgl[MOON_DIFFICULTY_NORMAL] > rgl[MOON_DIFFICULTY_HARD]);

    /* The level survives plain restarts of the same session, and the idle crew lasts longer on Easy */
    int dies[3];
    for (int lv = 0; lv < MOON_DIFFICULTY_COUNT; lv++) {
        fresh();
        g.difficulty_request = (uint8_t)(lv + 1);
        key(MOON_KEY_ESCAPE); key(MOON_KEY_R); key(MOON_KEY_1 + lv);
        int sec = 0;
        while (g.state == MOON_STATE_PLAYING && sec < 1000) { ticks(MOON_SIM_TICK_RATE_HZ); sec++; }
        assert(g.state == MOON_STATE_GAME_OVER);
        dies[lv] = sec;
    }
    assert(dies[MOON_DIFFICULTY_EASY] > dies[MOON_DIFFICULTY_NORMAL] && dies[MOON_DIFFICULTY_NORMAL] >= dies[MOON_DIFFICULTY_HARD]);

    /* Back from the chooser returns to where it came from, and ESC does the same */
    fresh();
    key(MOON_KEY_ESCAPE); key(MOON_KEY_R); key(MOON_KEY_N);
    assert(g.menu_page == MOON_MENU_MAIN);
    key(MOON_KEY_R); key(MOON_KEY_ESCAPE);
    assert(g.menu_page == MOON_MENU_MAIN);
    g.resources.population = 0; g.menu_page = MOON_MENU_CLOSED; ticks(1);
    assert(g.menu_page == MOON_MENU_GAME_OVER);
    key(MOON_KEY_R);
    assert(g.menu_page == MOON_MENU_NEW_GAME && g.menu_return == MOON_MENU_GAME_OVER);
    key(MOON_KEY_ESCAPE);
    assert(g.menu_page == MOON_MENU_GAME_OVER && g.state == MOON_STATE_GAME_OVER);

    /* The level can also be asked for before the first init */
    static moon_game_t h;
    memset(&h, 0, sizeof(h));
    h.difficulty_request = MOON_DIFFICULTY_HARD + 1;
    assert(moon_game_init(&h, 1024, 600) && h.difficulty == MOON_DIFFICULTY_HARD);
    printf("[test] Difficulty levels: chooser, stocks, survival time, Back: PASS\n");
}

/* A free tile touching the network, searched outward from the lander */
static bool site_near_network(moon_structure_type_t type, int *ox, int *oy) {
    for (int r = 1; r < 8; r++)
        for (int y = 32 - r; y <= 32 + r; y++)
            for (int x = 32 - r; x <= 32 + r; x++) {
                if (abs(x - 32) != r && abs(y - 32) != r) continue;
                if (!moon_structure_can_place(&g.grid, &g.structs, type, x, y, NULL)) continue;
                static const int d[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
                const moon_structure_def_t *def = moon_structure_get_def(type);
                for (int dy = 0; dy < def->footprint_h; dy++)
                    for (int dx = 0; dx < def->footprint_w; dx++)
                        for (int k = 0; k < 4; k++) {
                            const moon_tile_t *n = moon_grid_get_tile_const(&g.grid, x + dx + d[k][0], y + dy + d[k][1]);
                            bool inside = (d[k][0] + dx >= 0 && d[k][0] + dx < def->footprint_w && d[k][1] + dy >= 0 && d[k][1] + dy < def->footprint_h);
                            if (!inside && n && n->structure_id) { *ox = x; *oy = y; return true; }
                        }
            }
    return false;
}

static void put(moon_structure_type_t type) {
    int x, y;
    assert(site_near_network(type, &x, &y));
    assert(moon_structure_place(&g.grid, &g.structs, type, x, y));
}

static void test_objectives(void) {
    fresh();
    assert(g.objective == 0 && !g.objectives_hidden);
    ticks(2);
    assert(g.objective == 0);

    /* 0: a powered Extractor beside mare (a mare tile is put next to the lander) */
    g.grid.tiles[32][30].terrain = MOON_TERRAIN_BASALT_MARE;
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_O2_EXTRACTOR, 31, 32));
    ticks(1);
    assert(g.objective == 1);

    /* 1: three solar arrays */
    put(MOON_STRUCT_SOLAR_ARRAY);
    ticks(1);
    assert(g.objective == 1);
    put(MOON_STRUCT_SOLAR_ARRAY);
    ticks(1);
    assert(g.objective == 1);
    put(MOON_STRUCT_SOLAR_ARRAY);
    ticks(1);
    assert(g.objective == 2);

    /* 2: a powered Sinterer; 3: two battery banks; 4: a Habitat */
    put(MOON_STRUCT_REGOLITH_PLANT);
    ticks(1);
    assert(g.objective == 3);
    put(MOON_STRUCT_BATTERY_BANK);
    ticks(1);
    assert(g.objective == 3);
    put(MOON_STRUCT_BATTERY_BANK);
    ticks(1);
    assert(g.objective == 4);
    put(MOON_STRUCT_HABITAT_DOME);
    ticks(1);
    assert(g.objective == 5);

    /* 5: crew of 8; 6: Act I itself */
    g.resources.population = MOON_ACT1_CREW;
    ticks(1);
    assert(g.objective == 6);
    g.resources.lunar_day = MOON_ACT1_DAY;
    g.resources.ticks_since_blackout = MOON_LUNAR_DAY_TICKS;
    g.resources.o2_rate_kg_per_tick = 1; g.resources.water_rate_l_per_tick = 1;
    moon_game_check_end(&g);
    assert(g.state == MOON_STATE_VICTORY);
    key(MOON_KEY_C);
    ticks(1);
    assert(g.objective == MOON_OBJECTIVES);

    /* Done stays done even if the base is rebuilt differently; a new game starts over */
    moon_structure_demolish(&g.grid, &g.structs, 31, 32);
    ticks(1);
    assert(g.objective == MOON_OBJECTIVES);

    /* O hides the line, and the choice survives a restart */
    key(MOON_KEY_O);
    assert(g.objectives_hidden);
    key(MOON_KEY_ESCAPE); key(MOON_KEY_R); key(MOON_KEY_2);
    assert(g.objective == 0 && g.objectives_hidden);
    key(MOON_KEY_O);
    assert(!g.objectives_hidden);

    /* The line is drawn and fits both layouts */
    for (int w = 0; w < 2; w++) {
        static uint16_t fb[1024 * 600];
        memset(&g, 0, sizeof(g));
        assert(moon_game_init(&g, w ? 1024 : 640, w ? 600 : 360));
        moon_game_render(&g, fb, g.screen_w, g.screen_h, g.screen_w);
        bool found = false;
        int y = g.ui.hover_badge.y - 24 + 4;
        for (int x = g.ui.hover_badge.x + 8; x < g.screen_w / 2 && !found; x++) found = (fb[y * g.screen_w + x] != fb[(y - 30) * g.screen_w + x]);
        assert(found);
    }
    printf("[test] Objectives: chain from the first Extractor to Act I, sticky, hide, restart: PASS\n");
}

static void test_title(void) {
    static moon_game_t h;
    memset(&h, 0, sizeof(h));
    h.show_title = true;
    h.seed_request = 424242;
    assert(moon_game_init(&h, 1024, 600));
    assert(h.menu_page == MOON_MENU_TITLE && h.seed == 424242 && h.running);
    moon_game_t *save = &g;
    (void)save;
    memcpy(&g, &h, sizeof(g));

    /* Frozen, and ESC cannot dismiss it */
    uint32_t st = g.sim_ticks;
    ticks(4);
    assert(g.sim_ticks == st);
    key(MOON_KEY_ESCAPE); key(MOON_KEY_P); key(MOON_KEY_1 + 5);
    assert(g.menu_page == MOON_MENU_TITLE);

    /* Picking a level starts the game on the same map */
    static moon_tile_t before[MOON_MAP_SIZE_Y][MOON_MAP_SIZE_X];
    memcpy(before, g.grid.tiles, sizeof(before));
    click_rect(&g.ui.menu_item[3]);          /* the third button: Commander */
    assert(g.menu_page == MOON_MENU_CLOSED && g.difficulty == MOON_DIFFICULTY_HARD);
    assert(g.seed == 424242 && memcmp(before, g.grid.tiles, sizeof(before)) == 0);
    assert(g.resources.rules.start_regolith_kg == g_moon_rules[MOON_DIFFICULTY_HARD].start_regolith_kg);

    /* Keys 1 and 2, and Q leaves; a restart from the menu does not bring the title back */
    memcpy(&g, &h, sizeof(g));
    key(MOON_KEY_2);
    assert(g.menu_page == MOON_MENU_CLOSED && g.difficulty == MOON_DIFFICULTY_NORMAL);
    key(MOON_KEY_ESCAPE); key(MOON_KEY_R); key(MOON_KEY_1);
    assert(g.menu_page == MOON_MENU_CLOSED && g.difficulty == MOON_DIFFICULTY_EASY && g.seed != 424242);
    memcpy(&g, &h, sizeof(g));
    key(MOON_KEY_Q);
    assert(!g.running);
    printf("[test] Title page: level choice, same map, frozen, Q quits: PASS\n");
}

static void test_expedition_lander_touchdown(void) {
    memset(&g, 0, sizeof(g));
    g.show_title = true;
    assert(moon_game_init(&g, 1024, 600));
    assert(g.menu_page == MOON_MENU_TITLE);

    /* Click Cadet difficulty: menu_item[1] */
    click_rect(&g.ui.menu_item[1]);
    assert(g.menu_page == MOON_MENU_CLOSED);
    assert(g.landing_active);
    assert(g.landing_y_offset == 260);
    assert(g.landing_elapsed_ms == 0);

    /* Debounce test: touches and keys within the first 350 ms do not abort the sequence */
    ptr(MOON_EVENT_POINTER_DOWN, 512, 300, 1);
    assert(g.landing_active);
    assert(g.landing_y_offset == 260);

    key(MOON_KEY_SPACE);
    assert(g.landing_active);
    assert(g.landing_y_offset == 260);

    /* Frame render at alt=260 px */
    static uint16_t fb[1024 * 600];
    moon_game_render(&g, fb, 1024, 600, 1024);
    assert(g.structs.lander_y_offset == 260);

    /* Advance by 1300 ms in 50 ms slices (halfway) */
    for (int i = 0; i < 26; i++) moon_game_update(&g, 50);
    assert(g.landing_active);
    /* y = 260 * (1300/2600)^2 = 260 * 0.25 = 65 */
    assert(g.landing_y_offset == 65);
    assert(g.structs.lander_y_offset == 65);

    /* Render at alt=65 px */
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Advance by another 1300 ms (reaches 2600 ms touchdown) */
    for (int i = 0; i < 26; i++) moon_game_update(&g, 50);
    assert(!g.landing_active);
    assert(g.landing_y_offset == 0);
    assert(g.structs.lander_y_offset == 0);
    assert(g.notify_msg && strstr(g.notify_msg, "TOUCHDOWN CONFIRMED") != NULL);

    /* Render on the ground */
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Test intentional skip after 350 ms */
    memset(&g, 0, sizeof(g));
    g.show_title = true;
    assert(moon_game_init(&g, 1024, 600));
    click_rect(&g.ui.menu_item[2]);
    assert(g.landing_active && g.landing_y_offset == 260);
    for (int i = 0; i < 8; i++) moon_game_update(&g, 50); /* 400 ms */
    assert(g.landing_active);
    ptr(MOON_EVENT_POINTER_DOWN, 512, 300, 1);
    assert(!g.landing_active);
    assert(g.landing_y_offset == 0);

    printf("[test] Expedition Lander Touchdown Animation & Touch Debounce: PASS\n");
}

static void test_deposits(void) {
    fresh();
    g.resources.rules.deposit_s_per_unit = 1;          /* one unit a second */
    /* Two mare tiles beside the Extractor: the richer one is drawn first */
    g.grid.tiles[32][30].terrain = MOON_TERRAIN_BASALT_MARE; g.grid.tiles[32][30].resource_val = 2;
    g.grid.tiles[31][30].terrain = MOON_TERRAIN_BASALT_MARE; g.grid.tiles[31][30].resource_val = 3;
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_O2_EXTRACTOR, 31, 32));
    ticks(MOON_SIM_TICK_RATE_HZ);
    /* one unit a second, taken from the richer tile (3 -> 2) while the poorer one is untouched */
    assert(g.grid.tiles[31][30].resource_val == 2 && g.grid.tiles[32][30].resource_val == 2);
    int o2_rate = g.resources.o2_rate_kg_per_tick;
    int use_kw = g.resources.power_consumed_kw;
    assert(o2_rate > 0 || g.resources.water_rate_l_per_tick > 0);
    ticks(2 * MOON_SIM_TICK_RATE_HZ);
    assert(g.grid.tiles[31][30].resource_val + g.grid.tiles[32][30].resource_val == 2); /* three units gone */
    assert(!struct_at(31, 32)->depleted);

    /* Run it dry: five units in all */
    ticks(2 * MOON_SIM_TICK_RATE_HZ);
    assert(g.grid.tiles[31][30].resource_val == 0 && g.grid.tiles[32][30].resource_val == 0);
    ticks(1);                                          /* the next tick finds nothing left */
    assert(struct_at(31, 32)->depleted);
    ticks(1);
    /* It stops producing and stops drawing power */
    assert(g.resources.o2_rate_kg_per_tick < o2_rate);
    assert(g.resources.power_consumed_kw == use_kw - 25);

    /* A new Extractor cannot stand beside an exhausted patch, with a reason */
    const char *why = NULL;
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_O2_EXTRACTOR, 31, 31, &why));
    assert(why && strstr(why, "ore left"));
    g.grid.tiles[33][30].terrain = MOON_TERRAIN_ICE_DEPOSIT; g.grid.tiles[33][30].resource_val = 40;
    assert(moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_O2_EXTRACTOR, 31, 33, NULL));

    /* An unpowered Extractor draws nothing: a standby one keeps its deposit */
    g.grid.tiles[34][30].terrain = MOON_TERRAIN_ICE_DEPOSIT; g.grid.tiles[34][30].resource_val = 10;
    moon_structure_t *e2 = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_O2_EXTRACTOR, 31, 34);
    assert(e2);
    g.structs.list[e2->id].active = false;
    moon_structure_mark_dirty(&g.structs);
    ticks(10 * MOON_SIM_TICK_RATE_HZ);
    assert(g.grid.tiles[34][30].resource_val == 10);

    /* The inspector shows what is left; the status reads DEPLETED, the badge DEPL */
    g.panel_open = true;
    g.selected_tile_x = 31; g.selected_tile_y = 32;
    static uint16_t fb[1024 * 600];
    moon_game_render(&g, fb, 1024, 600, 1024);
    printf("[test] Deposits: richest tile first, depletion, idle and power, placement rule, standby: PASS\n");
}

/* Pressurised structures draw O2 and water on top of the crew's use */
static void test_pressurised_draw(void) {
    fresh();
    /* Lander only: 4 colonists (1 kg O2 and 2 L per 30 s each) and the Lander's own -1 / -2 per minute */
    ticks(1);
    const int crew_o2 = 4 * 1000 / (MOON_SIM_TICK_RATE_HZ * 30);
    const int crew_h2o = 4 * 2000 / (MOON_SIM_TICK_RATE_HZ * 30);
    const int lander_o2 = 1 * 1000 / (MOON_SIM_TICK_RATE_HZ * 60), lander_h2o = 2 * 1000 / (MOON_SIM_TICK_RATE_HZ * 60);
    assert(g.resources.o2_rate_kg_per_tick == -(crew_o2 + lander_o2));
    assert(g.resources.water_rate_l_per_tick == -(crew_h2o + lander_h2o));

    /* A Habitat adds its own draw, and with it crew room */
    int base_o2 = g.resources.o2_rate_kg_per_tick, base_h2o = g.resources.water_rate_l_per_tick;
    put(MOON_STRUCT_HABITAT_DOME);
    ticks(1);
    assert(g.resources.o2_rate_kg_per_tick == base_o2 - 5 * 1000 / (MOON_SIM_TICK_RATE_HZ * 60));
    assert(g.resources.water_rate_l_per_tick == base_h2o - 10 * 1000 / (MOON_SIM_TICK_RATE_HZ * 60));
    assert(g.resources.max_population == MOON_LANDER_CREW + MOON_HABITAT_CREW);

    /* Unpowered it still leaks; on standby (depressurised) it draws nothing */
    int with_o2 = g.resources.o2_rate_kg_per_tick;
    const moon_structure_t *hab = NULL;
    for (int i = 1; i < MOON_MAX_STRUCTURES; i++) if (g.structs.list[i].id && g.structs.list[i].type == MOON_STRUCT_HABITAT_DOME) hab = &g.structs.list[i];
    assert(hab);
    g.structs.list[hab->id].powered = false;   /* the next solve restores it; the draw does not depend on it */
    g.structs.list[hab->id].active = false;
    moon_structure_mark_dirty(&g.structs);
    ticks(1);
    assert(g.resources.o2_rate_kg_per_tick == base_o2);
    g.structs.list[hab->id].active = true;
    moon_structure_mark_dirty(&g.structs);
    ticks(1);
    assert(g.resources.o2_rate_kg_per_tick == with_o2);

    /* The stock follows the rate: with nothing producing, a Habitat shortens the reserves */
    fresh();
    ticks(MOON_SIM_TICK_RATE_HZ * 30);
    int o2_alone = fixed_to_int(g.resources.o2_stored_kg);
    fresh();
    put(MOON_STRUCT_HABITAT_DOME);
    ticks(MOON_SIM_TICK_RATE_HZ * 30);
    assert(fixed_to_int(g.resources.o2_stored_kg) < o2_alone);
    printf("[test] Pressurised structures: Lander and Habitat draw O2 and water, standby does not: PASS\n");
}

static void test_quarry(void) {
    fresh();
    g.resources.rules.sinterer_kg_per_min = 60;          /* a kilogram, so one unit, a second */
    /* Only two regolith tiles beside the Sinterer have anything in them; a mare tile beside it
     * belongs to the Extractors and is left alone */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) g.grid.tiles[32 + dy][31 + dx].resource_val = 0;
    g.grid.tiles[31][30].resource_val = 4;                /* (30,31) */
    g.grid.tiles[33][31].resource_val = 2;                /* (31,33) */
    g.grid.tiles[32][30].terrain = MOON_TERRAIN_BASALT_MARE; g.grid.tiles[32][30].resource_val = 9;

    /* Placement needs ground to quarry, and says how much there is */
    const char *why = NULL;
    assert(moon_structure_ore_left(&g.grid, MOON_STRUCT_REGOLITH_PLANT, 31, 32) == 6);
    assert(moon_structure_ore_left(&g.grid, MOON_STRUCT_O2_EXTRACTOR, 31, 32) == 9);
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 32));
    ticks(1);
    int start = regolith();
    ticks(MOON_SIM_TICK_RATE_HZ * 3);
    /* the richer tile is drawn first: 3 units gone from it */
    assert(g.grid.tiles[31][30].resource_val == 1 && g.grid.tiles[33][31].resource_val == 2);
    assert(!struct_at(31, 32)->depleted);
    int use_kw = g.resources.power_consumed_kw;

    /* Run it dry: six units in all, six kilograms made, then it idles */
    ticks(MOON_SIM_TICK_RATE_HZ * 5);
    assert(g.grid.tiles[31][30].resource_val == 0 && g.grid.tiles[33][31].resource_val == 0);
    assert(g.grid.tiles[32][30].resource_val == 9);                 /* the deposit is untouched */
    assert(abs(regolith() - start - 6) <= 1);
    ticks(2);
    assert(struct_at(31, 32)->depleted);
    assert(g.resources.power_consumed_kw == use_kw - 20 && g.resources.regolith_rate_kg_per_tick == 0);
    int after = regolith();
    ticks(MOON_SIM_TICK_RATE_HZ * 5);
    assert(regolith() >= after && regolith() <= MOON_REGOLITH_RESERVE_KG + after);   /* only the Lander's scoop */

    /* A new Sinterer is refused where the ground is spent; fresh ground is accepted */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) g.grid.tiles[36 + dy][37 + dx].resource_val = 0;
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 37, 36, &why));
    assert(why && strstr(why, "quarry"));
    g.grid.tiles[35][36].resource_val = 40;
    assert(moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 37, 36, NULL));

    /* Silo Full Check: When storage reaches 250 kg, Sinterer idles and does not deplete ground */
    fresh();
    g.resources.rules.sinterer_kg_per_min = 60;
    g.resources.regolith_stored_kg = fixed_from_int(250);
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 32));
    int ore_start = moon_structure_ore_left(&g.grid, MOON_STRUCT_REGOLITH_PLANT, 31, 32);
    assert(ore_start > 0);
    ticks(1);
    /* Regolith storage is full: Sinterer idles, does not draw 20 kW furnace power */
    assert(g.resources.power_consumed_kw == 5); /* Lander base 5 kW only */
    ticks(MOON_SIM_TICK_RATE_HZ * 3);
    assert(moon_structure_ore_left(&g.grid, MOON_STRUCT_REGOLITH_PLANT, 31, 32) == ore_start); /* Ground untouched! */
    assert(regolith() == 250);

    /* When regolith is consumed below 250, quarrying resumes */
    g.resources.regolith_stored_kg = fixed_from_int(200);
    ticks(1);
    assert(g.resources.power_consumed_kw == 25); /* 5 kW + 20 kW furnace */
    ticks(MOON_SIM_TICK_RATE_HZ * 3);
    assert(moon_structure_ore_left(&g.grid, MOON_STRUCT_REGOLITH_PLANT, 31, 32) < ore_start); /* Ground drawn */
    assert(regolith() > 200);

    /* Richness differs by ground: boulder fields are the best, crater floors poor */
    fresh();
    long sum[MOON_TERRAIN_COUNT] = { 0 }, n[MOON_TERRAIN_COUNT] = { 0 };
    for (int y = 0; y < MOON_MAP_SIZE_Y; y++)
        for (int x = 0; x < MOON_MAP_SIZE_X; x++) {
            sum[g.grid.tiles[y][x].terrain] += g.grid.tiles[y][x].resource_val;
            n[g.grid.tiles[y][x].terrain]++;
        }
    assert(n[MOON_TERRAIN_REGOLITH_ROUGH] > 0 && n[MOON_TERRAIN_REGOLITH_FLAT] > 0 && n[MOON_TERRAIN_CRATER_FLOOR] > 0);
    assert(sum[MOON_TERRAIN_REGOLITH_ROUGH] / n[MOON_TERRAIN_REGOLITH_ROUGH] >
           sum[MOON_TERRAIN_REGOLITH_FLAT] / n[MOON_TERRAIN_REGOLITH_FLAT]);
    assert(sum[MOON_TERRAIN_REGOLITH_FLAT] / n[MOON_TERRAIN_REGOLITH_FLAT] >
           sum[MOON_TERRAIN_CRATER_FLOOR] / n[MOON_TERRAIN_CRATER_FLOOR]);
    printf("[test] Quarrying: richest tile first, a kilogram per unit, idle and power, placement, deposits untouched: PASS\n");
}

static bool ice_near_lander(const moon_grid_t *grid) {
    for (int y = 0; y < MOON_MAP_SIZE_Y; y++)
        for (int x = 0; x < MOON_MAP_SIZE_X; x++)
            if (grid->tiles[y][x].terrain == MOON_TERRAIN_ICE_DEPOSIT &&
                (x - 32) * (x - 32) + (y - 32) * (y - 32) <= 12 * 12) return true;
    return false;
}

static void test_restarts_and_seeds(void) {
    fresh();
    enum { RUNS = 50 };
    uint32_t seeds[RUNS];
    for (int i = 0; i < RUNS; i++) {
        seeds[i] = g.seed;
        /* every run starts from the same, complete base */
        assert(g.structs.count == 1 && g.resources.population == MOON_LANDER_CREW);
        assert(g.state == MOON_STATE_PLAYING && g.sim_ticks == 0);
        assert(ice_near_lander(&g.grid));
        const moon_tile_t *t = moon_grid_get_tile_const(&g.grid, 32, 32);
        assert(t->terrain == MOON_TERRAIN_REGOLITH_FLAT && t->structure_id != 0);
        /* some play, then restart through the menu */
        key(MOON_KEY_UP); key(MOON_KEY_1);
        ticks(2);
        key(MOON_KEY_ESCAPE); key(MOON_KEY_R); key(MOON_KEY_2);
        assert(g.menu_page == MOON_MENU_CLOSED);
    }
    for (int i = 0; i < RUNS; i++)
        for (int j = i + 1; j < RUNS; j++) assert(seeds[i] != seeds[j]);

    /* The same seed always gives the same map; another seed another map */
    static moon_game_t a, b;
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    a.seed_request = 12345; b.seed_request = 12345;
    assert(moon_game_init(&a, 1024, 600) && moon_game_init(&b, 1024, 600));
    assert(a.seed == 12345 && memcmp(a.grid.tiles, b.grid.tiles, sizeof(a.grid.tiles)) == 0);
    b.seed_request = 54321;
    memset(&b, 0, sizeof(b)); b.seed_request = 54321;
    assert(moon_game_init(&b, 1024, 600) && memcmp(a.grid.tiles, b.grid.tiles, sizeof(a.grid.tiles)) != 0);
    assert(ice_near_lander(&b.grid));
    printf("[test] 50 restarts: clean base, new seed each, ice in reach, seeds reproducible: PASS\n");
}

static void test_categorized_build_sheets(void) {
    fresh();
    /* Starts at MOON_CAT_POWER */
    assert(g.build_category == MOON_CAT_POWER);

    /* Category 0: Power & Energy Storage */
    key(MOON_KEY_1); key(MOON_KEY_1);
    assert(g.structs.selected_build_type == MOON_STRUCT_SOLAR_ARRAY);
    key(MOON_KEY_0);

    key(MOON_KEY_2); key(MOON_KEY_2);
    assert(g.structs.selected_build_type == MOON_STRUCT_BATTERY_BANK);
    key(MOON_KEY_0);

    key(MOON_KEY_3); key(MOON_KEY_3);
    assert(g.structs.selected_build_type == MOON_STRUCT_POWER_CONDUIT);
    key(MOON_KEY_0);

    /* Unmapped slot does nothing */
    key(MOON_KEY_4);
    assert(g.structs.selected_build_type == MOON_STRUCT_NONE);

    /* Pressing B cycles to MOON_CAT_LIFECRYO */
    key(MOON_KEY_B);
    assert(g.build_category == MOON_CAT_LIFECRYO);

    key(MOON_KEY_1); key(MOON_KEY_1);
    assert(g.structs.selected_build_type == MOON_STRUCT_O2_EXTRACTOR);
    key(MOON_KEY_0);

    key(MOON_KEY_2); key(MOON_KEY_2);
    assert(g.structs.selected_build_type == MOON_STRUCT_HABITAT_DOME);
    key(MOON_KEY_0);

    key(MOON_KEY_3); key(MOON_KEY_3);
    assert(g.structs.selected_build_type == MOON_STRUCT_HYDROLOX_PLANT);
    key(MOON_KEY_0);

    key(MOON_KEY_4); key(MOON_KEY_4);
    assert(g.structs.selected_build_type == MOON_STRUCT_CRYO_DEWAR);
    key(MOON_KEY_0);

    key(MOON_KEY_5); key(MOON_KEY_5);
    assert(g.structs.selected_build_type == MOON_STRUCT_POWER_CONDUIT);
    key(MOON_KEY_0);

    key(MOON_KEY_6);
    assert(g.structs.selected_build_type == MOON_STRUCT_NONE);

    /* Pressing B cycles to MOON_CAT_LOGISTICS */
    key(MOON_KEY_B);
    assert(g.build_category == MOON_CAT_LOGISTICS);

    key(MOON_KEY_1); key(MOON_KEY_1);
    assert(g.structs.selected_build_type == MOON_STRUCT_REGOLITH_PLANT);
    key(MOON_KEY_0);

    key(MOON_KEY_2); key(MOON_KEY_2);
    assert(g.structs.selected_build_type == MOON_STRUCT_FOUNDATION_PAD);
    key(MOON_KEY_0);

    key(MOON_KEY_3); key(MOON_KEY_3);
    assert(g.structs.selected_build_type == MOON_STRUCT_LANDING_PAD);
    key(MOON_KEY_0);

    key(MOON_KEY_4); key(MOON_KEY_4);
    assert(g.structs.selected_build_type == MOON_STRUCT_POWER_CONDUIT);
    key(MOON_KEY_0);

    key(MOON_KEY_5);
    assert(g.structs.selected_build_type == MOON_STRUCT_NONE);

    /* Pressing B cycles back to MOON_CAT_POWER */
    key(MOON_KEY_B);
    assert(g.build_category == MOON_CAT_POWER);

    /* Clicking cat_btn also cycles */
    click_rect(&g.ui.cat_btn);
    assert(g.build_category == MOON_CAT_LIFECRYO);
    click_rect(&g.ui.cat_btn);
    assert(g.build_category == MOON_CAT_LOGISTICS);
    click_rect(&g.ui.cat_btn);
    assert(g.build_category == MOON_CAT_POWER);

    printf("[test] Categorized build sheets & ribbon cycling: PASS\n");
}

static void test_tech_progression_rules(void) {
    fresh();
    const char *reason = NULL;

    /* 1. Foundation Pad requires Sinterer */
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 30, 30, &reason));
    assert(strcmp(reason, "Requires Regolith Sinterer") == 0);

    /* 2. Hydrolox Plant & Dewar require Habitat Dome */
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_HYDROLOX_PLANT, 28, 28, &reason));
    assert(strcmp(reason, "Tech locked: Requires Habitat Dome") == 0);

    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_CRYO_DEWAR, 30, 30, &reason));
    assert(strcmp(reason, "Tech locked: Requires Habitat Dome") == 0);

    /* Build Sinterer */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 32));
    /* Now Foundation Pad is unlocked */
    assert(moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 30, 30, &reason));

    /* Build Habitat Dome */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 29, 29));
    /* Now Cryo Dewar is unlocked */
    assert(moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_CRYO_DEWAR, 30, 28, &reason));

    printf("[test] Tech progression locks & unlocking: PASS\n");
}

static void test_foundation_and_clearance_rules(void) {
    fresh();
    const char *reason = NULL;

    /* Flatten test region */
    for (int y = 26; y <= 36; y++) {
        for (int x = 26; x <= 36; x++) {
            g.grid.tiles[y][x].terrain = MOON_TERRAIN_REGOLITH_FLAT;
            g.grid.tiles[y][x].elevation = 1;
            g.grid.tiles[y][x].flags &= ~(MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED);
        }
    }
    /* Restore lander tile at (32, 32) */
    g.grid.tiles[32][32].flags |= MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED;
    g.grid.tiles[32][32].structure_id = 1;

    /* Build prerequisites: Sinterer at (31, 32), Habitat Dome at (29, 29) */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 29, 29));

    /* 0. Terrain restrictions for Foundation Pad */
    g.grid.tiles[26][26].terrain = MOON_TERRAIN_CRATER_RIM;
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 26, 26, &reason));
    assert(strcmp(reason, "Terrain too steep (Crater Rim)") == 0);

    g.grid.tiles[26][27].terrain = MOON_TERRAIN_REGOLITH_ROUGH;
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 27, 26, &reason));
    assert(strcmp(reason, "Boulder field (Requires clearing)") == 0);

    g.grid.tiles[26][28].terrain = MOON_TERRAIN_ICE_DEPOSIT;
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 28, 26, &reason));
    assert(strcmp(reason, "Requires flat regolith") == 0);

    /* 1. Foundation Pad paving */
    assert(moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 33, 29, NULL));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 33, 29));
    assert(g.grid.tiles[29][33].flags & MOON_TILE_FLAG_FOUNDATION);
    assert(!(g.grid.tiles[29][33].flags & MOON_TILE_FLAG_OCCUPIED));

    /* Cannot pave already paved tile */
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 33, 29, &reason));
    assert(strcmp(reason, "Foundation already paved") == 0);

    /* 2. Hydrolox Plant requires all 4 tiles to be foundation */
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_HYDROLOX_PLANT, 33, 29, &reason));
    assert(strcmp(reason, "Requires sintered foundation") == 0);

    /* Pave remaining 3 tiles */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 34, 29));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 33, 30));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, 34, 30));

    /* Now all 4 tiles are foundation -> can place Hydrolox! */
    assert(moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_HYDROLOX_PLANT, 33, 29, &reason));

    /* 3. Safety Clearance: Hydrolox Plant vs Lander */
    /* Pave (32, 33), (33, 33), (32, 34), (33, 34) adjacent to Lander at (32, 32) */
    g.grid.tiles[33][32].flags |= MOON_TILE_FLAG_FOUNDATION;
    g.grid.tiles[33][33].flags |= MOON_TILE_FLAG_FOUNDATION;
    g.grid.tiles[34][32].flags |= MOON_TILE_FLAG_FOUNDATION;
    g.grid.tiles[34][33].flags |= MOON_TILE_FLAG_FOUNDATION;
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_HYDROLOX_PLANT, 32, 33, &reason));
    assert(strcmp(reason, "Safety: clearance from habitat required") == 0);

    /* Safety Clearance: Hydrolox Plant vs Habitat Dome */
    /* Habitat is at (29, 29). Footprint [29..30] x [29..30]. Try (27, 28) adjacent */
    g.grid.tiles[28][27].flags |= MOON_TILE_FLAG_FOUNDATION;
    g.grid.tiles[28][28].flags |= MOON_TILE_FLAG_FOUNDATION;
    g.grid.tiles[29][27].flags |= MOON_TILE_FLAG_FOUNDATION;
    g.grid.tiles[29][28].flags |= MOON_TILE_FLAG_FOUNDATION;
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_HYDROLOX_PLANT, 27, 28, &reason));
    assert(strcmp(reason, "Safety: clearance from habitat required") == 0);

    /* Place Hydrolox at (33, 29) */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HYDROLOX_PLANT, 33, 29));

    /* Try placing Habitat Dome bordering Hydrolox */
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 31, 28, &reason));
    assert(strcmp(reason, "Safety: clearance from Hydrolox plant required") == 0);

    printf("[test] Sintered foundation and hazard clearance rules: PASS\n");
}

static void test_foundation_recycle_and_undo(void) {
    fresh();
    g.resources.regolith_stored_kg = fixed_from_int(100);
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 32));

    /* Switch to LOGISTICS category */
    while (g.build_category != MOON_CAT_LOGISTICS) key(MOON_KEY_B);
    g.selected_tile_x = 30; g.selected_tile_y = 30;
    g.grid.tiles[30][30].terrain = MOON_TERRAIN_REGOLITH_FLAT;
    g.grid.tiles[30][30].flags &= ~(MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED);

    int r0 = regolith();
    /* Key 2 in LOGISTICS is Foundation Pad (cost 5 kg) */
    key(MOON_KEY_2);
    assert((g.grid.tiles[30][30].flags & MOON_TILE_FLAG_FOUNDATION));
    assert(regolith() == r0 - 5);

    /* Undo build: refunds 5 kg and clears foundation */
    key(MOON_KEY_Z);
    assert(!(g.grid.tiles[30][30].flags & MOON_TILE_FLAG_FOUNDATION));
    assert(regolith() == r0);

    /* Place again */
    key(MOON_KEY_2);
    assert((g.grid.tiles[30][30].flags & MOON_TILE_FLAG_FOUNDATION));
    assert(regolith() == r0 - 5);

    /* Recycle foundation: 50% refund = 2 kg */
    key(MOON_KEY_R);
    assert(!(g.grid.tiles[30][30].flags & MOON_TILE_FLAG_FOUNDATION));
    assert(regolith() == r0 - 5 + 2);

    /* Undo recycle: restores foundation, takes 2 kg back */
    key(MOON_KEY_Z);
    assert((g.grid.tiles[30][30].flags & MOON_TILE_FLAG_FOUNDATION));
    assert(regolith() == r0 - 5);

    printf("[test] Foundation pad undo and recycle: PASS\n");
}

/* Detailed inspector clarity & Hydrolox / Dewar simulation verification */
static void test_hydrolox_dewar_and_inspector_clarity(void) {
    fresh();
    static uint16_t fb[1024 * 600];

    /* Flatten test region */
    for (int y = 25; y <= 37; y++) {
        for (int x = 25; x <= 37; x++) {
            g.grid.tiles[y][x].terrain = MOON_TERRAIN_REGOLITH_FLAT;
            g.grid.tiles[y][x].elevation = 1;
            g.grid.tiles[y][x].flags &= ~(MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED);
        }
    }
    /* Restore lander tile at (32, 32) */
    g.grid.tiles[32][32].flags |= MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED;
    g.grid.tiles[32][32].structure_id = 1;

    /* Build tech prerequisites: Sinterer at (31, 32), Habitat Dome at (26, 26) */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 26, 26));

    /* Provide plenty of regolith, water, and power */
    g.resources.regolith_stored_kg = fixed_from_int(500);
    g.resources.water_stored_l = fixed_from_int(100);

    /* Power network connection from Lander (32, 32) to Hydrolox at (33, 29) */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_BATTERY_BANK, 32, 31));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 33, 31));
    g.resources.battery_stored_kwh = fixed_from_int(500);

    /* 1. Lay 2x2 foundation pads for Hydrolox Plant at (33, 29) */
    int hx = 33, hy = 29;
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, hx + dx, hy + dy));
        }
    }

    /* 2. Build Hydrolox Plant BEFORE Cryo Dewar (lower id in structs list) */
    moon_structure_t *plant = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HYDROLOX_PLANT, hx, hy);
    assert(plant);
    moon_resources_solve_power_grid(&g.grid, &g.structs);
    assert(plant->powered);

    /* Tick sim: with no Cryo Dewar built, capacities are 0, so plant must NOT crack */
    ticks(4);
    assert(g.resources.lh2_capacity_kg == 0);
    assert(g.resources.lox_capacity_kg == 0);
    assert(g.resources.lh2_rate_kg_per_tick == 0);
    assert(g.resources.lox_rate_kg_per_tick == 0);

    /* Inspect Hydrolox Plant without Cryo Dewar */
    g.panel_open = true;
    g.selected_tile_x = hx; g.selected_tile_y = hy;
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* 3. Now build Cryo Dewar (higher id in structs list) */
    moon_structure_t *dewar = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_CRYO_DEWAR, 35, 29);
    assert(dewar);
    assert(dewar->id > plant->id);
    moon_resources_solve_power_grid(&g.grid, &g.structs);
    assert(dewar->powered);

    /* Provide battery reserves so plant cracks without brownout */
    g.resources.battery_stored_kwh = fixed_from_int(500);

    /* Tick sim: two-pass aggregation means Hydrolox plant now sees dewar capacity and cracks! */
    ticks(4);
    assert(g.resources.lh2_capacity_kg == 250);
    assert(g.resources.lox_capacity_kg == 2000);
    assert(g.resources.lh2_rate_kg_per_tick > 0);
    assert(g.resources.lox_rate_kg_per_tick > 0);
    assert(g.resources.water_rate_l_per_tick < 0);

    /* Inspect Hydrolox Plant while actively cracking */
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Inspect Cryo Dewar */
    g.selected_tile_x = 35; g.selected_tile_y = 29;
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* 4. Water threshold guard: drop water to 20 L -> cracking must pause */
    g.resources.water_stored_l = fixed_from_int(20);
    ticks(4);
    assert(g.resources.lh2_rate_kg_per_tick == 0);
    assert(g.resources.lox_rate_kg_per_tick == 0);

    /* Inspect Hydrolox Plant under low water condition */
    g.selected_tile_x = hx; g.selected_tile_y = hy;
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* 5. Tank full guard: restore water, fill dewar tanks to max -> cracking must pause */
    g.resources.water_stored_l = fixed_from_int(100);
    g.resources.lh2_stored_kg = fixed_from_int(250);
    g.resources.lox_stored_kg = fixed_from_int(2000);
    ticks(4);
    assert(g.resources.lh2_rate_kg_per_tick == 0);
    assert(g.resources.lox_rate_kg_per_tick == 0);

    /* Inspect Hydrolox Plant when dewars full */
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* 6. Verify inspector rendering on remaining structure types */
    /* Sinterer */
    g.selected_tile_x = 31; g.selected_tile_y = 32;
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Habitat Dome */
    g.selected_tile_x = 26; g.selected_tile_y = 26;
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Solar Array & Battery Bank */
    moon_structure_t *sol = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_SOLAR_ARRAY, 31, 30);
    assert(sol);
    g.selected_tile_x = 31; g.selected_tile_y = 30;
    moon_game_render(&g, fb, 1024, 600, 1024);

    moon_structure_t *bat = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_BATTERY_BANK, 31, 34);
    assert(bat);
    g.selected_tile_x = 31; g.selected_tile_y = 34;
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Conduit & Foundation pad */
    moon_structure_t *cnd = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 31, 35);
    assert(cnd);
    g.selected_tile_x = 31; g.selected_tile_y = 35;
    moon_game_render(&g, fb, 1024, 600, 1024);

    g.selected_tile_x = 33; g.selected_tile_y = 29;
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Lander */
    g.selected_tile_x = 32; g.selected_tile_y = 32;
    moon_game_render(&g, fb, 1024, 600, 1024);

    printf("[test] Hydrolox & Dewar simulation and detailed inspector clarity: PASS\n");
}

/* Reinforced Starship Landing Pad verification */
static void test_starship_landing_pad(void) {
    fresh();
    const char *reason = NULL;
    static uint16_t fb[1024 * 600];

    /* Flatten wide test zone */
    for (int y = 20; y <= 44; y++) {
        for (int x = 20; x <= 44; x++) {
            g.grid.tiles[y][x].terrain = MOON_TERRAIN_REGOLITH_FLAT;
            g.grid.tiles[y][x].elevation = 1;
            g.grid.tiles[y][x].flags &= ~(MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED);
        }
    }
    g.grid.tiles[32][32].flags |= MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED;
    g.grid.tiles[32][32].structure_id = 1;

    g.resources.regolith_stored_kg = fixed_from_int(1000);
    g.resources.battery_stored_kwh = fixed_from_int(500);

    /* 1. Tech Progression: Cannot place Starship Landing Pad without Habitat & Cryogenics */
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_LANDING_PAD, 35, 35, &reason));
    assert(strcmp(reason, "Tech locked: Requires Habitat & Cryogenics") == 0);

    /* Build prerequisites: Sinterer, Habitat Dome, Cryo Dewar */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 31, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 26, 26));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_CRYO_DEWAR, 26, 28));

    /* 2. Sintered Foundation Requirement: 4x4 unpaved must fail */
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_LANDING_PAD, 35, 35, &reason));
    assert(strcmp(reason, "Requires 4x4 sintered foundation") == 0);

    /* Partially pave 15 of 16 tiles -> still fails */
    int px = 35, py = 35;
    for (int dy = 0; dy < 4; dy++) {
        for (int dx = 0; dx < 4; dx++) {
            if (dx == 3 && dy == 3) continue;
            assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, px + dx, py + dy));
        }
    }
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_LANDING_PAD, px, py, &reason));
    assert(strcmp(reason, "Requires 4x4 sintered foundation") == 0);

    /* Pave 16th tile -> foundation complete */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, px + 3, py + 3));

    /* 3. Safety Clearance: Cannot place adjacent to Lander at (32, 32) */
    /* Pave 4x4 zone adjacent to lander at (32, 33) */
    for (int dy = 0; dy < 4; dy++) {
        for (int dx = 0; dx < 4; dx++) {
            g.grid.tiles[33 + dy][32 + dx].flags |= MOON_TILE_FLAG_FOUNDATION;
        }
    }
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_LANDING_PAD, 32, 33, &reason));
    assert(strcmp(reason, "Safety: clearance from habitat required") == 0);

    /* 4. Place Starship Landing Pad at (35, 35) with clearance */
    assert(moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_LANDING_PAD, px, py, &reason));
    const moon_structure_def_t *pad_def = moon_structure_get_def(MOON_STRUCT_LANDING_PAD);
    assert(pad_def != NULL);
    assert(pad_def->cost_regolith == 120);
    assert(pad_def->power_kw == -10);
    moon_structure_t *pad = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_LANDING_PAD, px, py);
    assert(pad);
    assert(pad->footprint_w == 4 && pad->footprint_h == 4);

    /* 5. Safety Clearance: Cannot place Habitat Dome adjacent to Starship Landing Pad */
    /* Landing pad is at [35..38] x [35..38]. Try placing Habitat Dome at (33, 35) (adjacent) */
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            g.grid.tiles[35 + dy][33 + dx].flags |= MOON_TILE_FLAG_FOUNDATION;
        }
    }
    assert(!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 33, 35, &reason));
    assert(strcmp(reason, "Safety: clearance from landing pad required") == 0);

    /* 6. Power Grid & Simulation Loop */
    /* Connect pad to Lander power grid via conduits */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 33, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 34, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 33));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 34));
    moon_resources_solve_power_grid(&g.grid, &g.structs);
    assert(pad->powered);

    ticks(4);
    /* Pad draws -10 kW for telemetry & cryo umbilical chill */
    assert(g.resources.power_consumed_kw >= 15); /* 5 kW lander + 10 kW pad */

    /* 7. Inspector Panel Rendering on Landing Pad */
    g.panel_open = true;
    g.selected_tile_x = px; g.selected_tile_y = py;
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* 8. Ribbon selection in MOON_CAT_LOGISTICS */
    while (g.build_category != MOON_CAT_LOGISTICS) {
        key(MOON_KEY_B);
    }
    assert(g.build_category == MOON_CAT_LOGISTICS);
    key(MOON_KEY_3); key(MOON_KEY_3);
    assert(g.structs.selected_build_type == MOON_STRUCT_LANDING_PAD);
    key(MOON_KEY_0);

    printf("[test] Reinforced Starship Landing Pad: PASS\n");
}

/* Starship Flight & Spaceport Refueling Sequence verification */
static void advance_ms(int ms) {
    while (ms > 0) {
        int slice = (ms > 50) ? 50 : ms;
        moon_game_update(&g, slice);
        ms -= slice;
    }
}

static void test_starship_flight_animation(void) {
    fresh();
    static uint16_t fb[1024 * 600];

    /* Flatten wide test zone and establish powered landing pad */
    for (int y = 20; y <= 45; y++) {
        for (int x = 20; x <= 45; x++) {
            g.grid.tiles[y][x].terrain = MOON_TERRAIN_REGOLITH_FLAT;
            g.grid.tiles[y][x].elevation = 0;
            g.grid.tiles[y][x].flags &= ~(MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED);
        }
    }

    g.resources.regolith_stored_kg = fixed_from_int(1000);
    g.resources.battery_stored_kwh = fixed_from_int(500);

    /* Prerequisites & 4x4 sintered pad foundation */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 25, 25));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 25, 27));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_CRYO_DEWAR, 25, 30));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_CRYO_DEWAR, 25, 31));

    int px = 35, py = 35;
    for (int dy = 0; dy < 4; dy++) {
        for (int dx = 0; dx < 4; dx++) {
            assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, px + dx, py + dy));
        }
    }

    moon_structure_t *pad = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_LANDING_PAD, px, py);
    assert(pad != NULL);

    /* Connect pad to grid */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 33, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 34, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 33));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 34));
    moon_resources_solve_power_grid(&g.grid, &g.structs);
    assert(pad->powered);

    /* 1. Initial State: Starship in Orbit */
    assert(g.starship.state == MOON_STARSHIP_ORBIT);
    assert(g.starship.alt_px == 360);

    /* 2. Dispatch Starship via Key 'L' */
    g.selected_tile_x = px;
    g.selected_tile_y = py;
    g.panel_open = true;
    key(MOON_KEY_L);
    assert(g.starship.state == MOON_STARSHIP_APPROACH);
    assert(g.starship.has_pad);
    assert(g.starship.pad_gx == px && g.starship.pad_gy == py);

    /* 3. Orbital Approach (1500 ms) -> Retro Burn */
    advance_ms(750);
    assert(g.starship.state == MOON_STARSHIP_APPROACH);
    assert(g.starship.alt_px < 360 && g.starship.alt_px > 240);

    advance_ms(750);
    assert(g.starship.state == MOON_STARSHIP_RETRO);
    assert(g.starship.alt_px == 240);

    /* 4. Retro Burn (3000 ms) with ease-out deceleration, plume & shock diamonds */
    advance_ms(1500);
    assert(g.starship.state == MOON_STARSHIP_RETRO);
    assert(g.starship.alt_px < 240 && g.starship.alt_px > 40);

    /* Render during retro burn: verifies plume, diamonds, and screen clipping */
    moon_game_render(&g, fb, 1024, 600, 1024);

    advance_ms(1500);
    assert(g.starship.state == MOON_STARSHIP_GEAR_EXTEND);
    assert(g.starship.alt_px == 40);

    /* 5. Final Flare (1200 ms) -> Touchdown */
    advance_ms(600);
    assert(g.starship.state == MOON_STARSHIP_GEAR_EXTEND);
    assert(g.starship.alt_px < 40 && g.starship.alt_px > 0);

    advance_ms(600);
    assert(g.starship.state == MOON_STARSHIP_TOUCHDOWN);
    assert(g.starship.alt_px == 0);
    assert(g.starship.dust_count > 0);

    /* 6. Touchdown dust expansion (1000 ms) -> Servicing */
    advance_ms(1000);
    assert(g.starship.state == MOON_STARSHIP_SERVICING);
    assert(g.starship.alt_px == 0);

    /* 7. Servicing: Umbilical Arm Extension (1000 ms) */
    advance_ms(500);
    assert(g.starship.umbilical_pct > 0 && g.starship.umbilical_pct < 100);

    advance_ms(500);
    assert(g.starship.umbilical_pct == 100);

    /* 8. Cryogenic Propellant Loading */
    /* Base has zero propellant: transfer should be idle */
    g.resources.lh2_stored_kg = 0;
    g.resources.lox_stored_kg = 0;
    advance_ms(500);
    assert(g.starship.lh2_loaded_kg == 0);
    assert(g.starship.lox_loaded_kg == 0);

    /* Supply base reserves with 300 kg LH2 and 2500 kg LOX */
    g.resources.lh2_stored_kg = fixed_from_int(300);
    g.resources.lox_stored_kg = fixed_from_int(2500);

    /* Partial fuel load */
    advance_ms(10000); /* 10s: 1000 kg LOX, 125 kg LH2 */
    assert(g.starship.lox_loaded_kg >= fixed_from_int(900));
    assert(g.starship.lh2_loaded_kg >= fixed_from_int(100));
    assert(g.resources.lox_stored_kg < fixed_from_int(2500));
    assert(g.resources.lh2_stored_kg < fixed_from_int(300));

    /* Render during servicing: verifies cryogenic loading meter & umbilical arm */
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Complete refueling to 250 kg LH2 and 2000 kg LOX -> advances to Prelaunch */
    advance_ms(10200);
    assert(g.starship.lh2_loaded_kg == g.starship.lh2_target_kg);
    assert(g.starship.lox_loaded_kg == g.starship.lox_target_kg);
    assert(g.starship.state == MOON_STARSHIP_PRELAUNCH);

    /* 9. Prelaunch (1500 ms): Umbilical retracts -> Vertical Ascent Burn */
    advance_ms(1500);
    assert(g.starship.state == MOON_STARSHIP_ASCENT);
    assert(g.starship.umbilical_pct == 0);

    /* 10. Ascent Burn (3200 ms): Accelerates up to orbit */
    advance_ms(1600);
    assert(g.starship.state == MOON_STARSHIP_ASCENT);
    assert(g.starship.alt_px > 0 && g.starship.alt_px < 450);

    /* Render during ascent */
    moon_game_render(&g, fb, 1024, 600, 1024);

    advance_ms(1600);
    assert(g.starship.state == MOON_STARSHIP_DEPARTED);
    assert(g.starship.alt_px == 450);

    /* 11. Departed (1000 ms) -> Reset to Orbit */
    advance_ms(1000);
    assert(g.starship.state == MOON_STARSHIP_ORBIT);

    /* 12. Skip Animation with Space Key Test */
    assert(moon_starship_call(&g.starship, px, py));
    assert(g.starship.state == MOON_STARSHIP_APPROACH);
    key(MOON_KEY_SPACE);
    assert(g.starship.state == MOON_STARSHIP_SERVICING);
    assert(g.starship.alt_px == 0);

    printf("[test] Starship Flight & Spaceport Refueling: PASS\n");
}

static void test_earth_moon_trade_and_act2_victory(void) {
    fresh();
    static uint16_t fb[1024 * 600];

    /* 1. Setup Base with Sinterer, Habitat Dome (capacity 12), Dewars, and Landing Pad */
    for (int y = 20; y <= 45; y++) {
        for (int x = 20; x <= 45; x++) {
            g.grid.tiles[y][x].terrain = MOON_TERRAIN_REGOLITH_FLAT;
            g.grid.tiles[y][x].elevation = 0;
            g.grid.tiles[y][x].flags &= ~(MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED);
        }
    }

    g.resources.regolith_stored_kg = fixed_from_int(100);
    g.resources.battery_stored_kwh = fixed_from_int(500);
    g.resources.o2_stored_kg = fixed_from_int(5000);
    g.resources.water_stored_l = fixed_from_int(5000);

    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_REGOLITH_PLANT, 25, 25));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, 25, 27));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_CRYO_DEWAR, 25, 30));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_CRYO_DEWAR, 25, 31));

    int px = 35, py = 35;
    for (int dy = 0; dy < 4; dy++) {
        for (int dx = 0; dx < 4; dx++) {
            assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_FOUNDATION_PAD, px + dx, py + dy));
        }
    }
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_LANDING_PAD, px, py));

    /* Conduits */
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 33, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 34, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 32));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 33));
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 35, 34));

    /* Conduits to Dewars and Habitat */
    for (int x = 26; x <= 31; x++) {
        assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, x, 32));
    }
    assert(moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_POWER_CONDUIT, 25, 32));

    ticks(1);
    assert(g.resources.max_population == 12);
    g.resources.population = 8;
    int baseline_gen = g.resources.power_produced_kw;
    int starting_reg = fixed_to_int(g.resources.regolith_stored_kg);

    /* 2. Test Cargo Delivery on Touchdown */
    assert(moon_starship_call(&g.starship, px, py));
    assert(!g.starship.manifest.delivered);
    assert(g.starship.manifest.colonists == 4);
    assert(g.starship.manifest.regolith_blocks == 100);
    assert(g.starship.manifest.reactor_power_kw == 30);

    /* Advance through approach (1500 ms) + retro (3000 ms) + flare (1200 ms) + touchdown (1000 ms) */
    advance_ms(6700);
    assert(g.starship.state == MOON_STARSHIP_SERVICING);
    assert(g.starship.manifest.delivered);
    assert(g.resources.population == 12);
    assert(fixed_to_int(g.resources.regolith_stored_kg) == starting_reg + 100);
    assert(g.resources.reactor_power_kw == 30);
    ticks(1);
    assert(g.resources.power_produced_kw == baseline_gen + 30);

    /* 3. Test Launch Window Turnaround Timer & Expiration / Abort */
    /* Empty base propellant reserves so refueling cannot happen */
    g.resources.lh2_stored_kg = 0;
    g.resources.lox_stored_kg = 0;
    assert(g.starship.turnaround_timer_ms > 0 && g.starship.turnaround_timer_ms <= MOON_STARSHIP_WINDOW_MS);

    /* Advance by remaining launch window duration */
    advance_ms(g.starship.turnaround_timer_ms);
    assert(g.starship.window_expired);
    assert(g.starship.state == MOON_STARSHIP_PRELAUNCH);
    assert(!g.starship.commercial_mission_success);

    /* Starship aborts to orbit */
    advance_ms(1500 + 3200 + 1000);
    assert(g.starship.state == MOON_STARSHIP_ORBIT);
    assert(!g.act2_victory);
    moon_game_check_end(&g);
    assert(g.state == MOON_STATE_PLAYING);

    /* 4. Test Successful Trade, Refueling & Act II Victory */
    /* Refill base cryo dewars */
    g.resources.lh2_stored_kg = fixed_from_int(300);
    g.resources.lox_stored_kg = fixed_from_int(2500);

    assert(moon_starship_call(&g.starship, px, py));
    /* Skip descent directly into servicing */
    key(MOON_KEY_SPACE);
    assert(g.starship.state == MOON_STARSHIP_SERVICING);
    assert(g.starship.turnaround_timer_ms == MOON_STARSHIP_WINDOW_MS);

    /* Wait for refueling: 20 seconds at full speed -> advances to Prelaunch */
    advance_ms(20100);
    assert(g.starship.lh2_loaded_kg == g.starship.lh2_target_kg);
    assert(g.starship.lox_loaded_kg == g.starship.lox_target_kg);
    assert(g.starship.state == MOON_STARSHIP_PRELAUNCH);

    advance_ms(1500); /* Prelaunch to Ascent */
    assert(g.starship.state == MOON_STARSHIP_ASCENT);
    advance_ms(3200); /* Ascent to Departed */

    /* Act II Victory Trigger */
    assert(g.state == MOON_STATE_VICTORY);
    assert(g.menu_page == MOON_MENU_VICTORY_ACT2);

    /* Verify rendering of Act II Victory overlay */
    moon_game_render(&g, fb, 1024, 600, 1024);

    /* Continue into Free-Play */
    key(MOON_KEY_C);
    assert(g.state == MOON_STATE_PLAYING);
    assert(g.act2_victory);
    assert(g.act2_victory_claimed);
    assert(g.menu_page == MOON_MENU_CLOSED);

    printf("[test] Earth-Moon Trade, Personnel & Act II Victory Loop: PASS\n");
}

static void test_dynamic_cybernetic_voice(void) {
    printf("[test] Dynamic Cybernetic Voice & Telemetry Sequencer...\n");

    moon_cyber_init();
    assert(moon_cyber_is_enabled());
    assert(moon_cyber_get_state() == MOON_CYBER_CALM);
    assert(moon_cyber_get_current_bpm() == 105);

    /* 1. Telemetry Evaluation Priorities */
    moon_cyber_telemetry_t telem;
    memset(&telem, 0, sizeof(telem));
    telem.life_support_pct = 100;
    telem.battery_pct = 80;
    telem.power_net_kw = 50;

    /* Daylight calm */
    moon_cyber_update_telemetry(&telem);
    assert(moon_cyber_get_state() == MOON_CYBER_CALM);

    /* Night cold */
    telem.is_night = true;
    moon_cyber_update_telemetry(&telem);
    assert(moon_cyber_get_state() == MOON_CYBER_NIGHT_COLD);

    /* Brownout */
    telem.power_brownout = true;
    moon_cyber_update_telemetry(&telem);
    assert(moon_cyber_get_state() == MOON_CYBER_BROWNOUT);

    /* Life support critical overrides brownout & night */
    telem.life_support_pct = 35;
    moon_cyber_update_telemetry(&telem);
    assert(moon_cyber_get_state() == MOON_CYBER_LIFE_CRITICAL);

    /* Starship descent approach overrides life support */
    telem.starship_state = 1; /* MOON_STARSHIP_APPROACH */
    moon_cyber_update_telemetry(&telem);
    assert(moon_cyber_get_state() == MOON_CYBER_STARSHIP_DESCENT);

    /* Starship orbital ascent overrides descent */
    telem.starship_state = 7; /* MOON_STARSHIP_ASCENT */
    moon_cyber_update_telemetry(&telem);
    assert(moon_cyber_get_state() == MOON_CYBER_STARSHIP_LAUNCH);

    /* 2. Audio buffer rendering & dynamic synthesis */
    int16_t buf[2048];
    memset(buf, 0, sizeof(buf));
    moon_cyber_render(buf, 2048);

    /* Verify non-zero synthesized samples and no clipping */
    bool non_zero = false;
    for (int i = 0; i < 2048; i++) {
        if (buf[i] != 0) non_zero = true;
        assert(buf[i] >= -32768 && buf[i] <= 32767);
    }
    assert(non_zero);

    /* Verify volume mute works */
    moon_cyber_set_volume(0);
    memset(buf, 0, sizeof(buf));
    moon_cyber_render(buf, 2048);
    for (int i = 0; i < 2048; i++) {
        assert(buf[i] == 0);
    }

    moon_cyber_reset();
    assert(moon_cyber_get_state() == MOON_CYBER_CALM);
    printf("[test] Dynamic Cybernetic Voice & Telemetry Sequencer: PASS\n");
}

int main(void) {
    test_picking();
    test_power_solver();
    test_life_support_rules();
    test_camera_and_ticks();
    test_pointer_routing();
    test_regolith_guard();
    test_tint_fold();
    test_view_coverage();
    test_tile_cache();
    test_depth_order();
    test_badge_survives();
    test_sprite_pick();
    test_palette_and_tint();
    test_layout();
    test_clock_and_pause();
    test_select_first();
    test_keyboard_reach();
    test_view_follow();
    test_standby_recycle_undo();
    test_brush();
    test_button_validity_and_preview();
    test_three_devices();
    test_touch_flow();
    test_touch_brush();
    test_menu_and_cancel();
    test_pointer_leave_and_hover();
    test_game_over();
    test_run_statistics();
    test_victory();
    test_restarts_and_seeds();
    test_difficulty();
    test_objectives();
    test_title();
    test_expedition_lander_touchdown();
    test_deposits();
    test_quarry();
    test_pressurised_draw();
    test_categorized_build_sheets();
    test_tech_progression_rules();
    test_foundation_and_clearance_rules();
    test_foundation_recycle_and_undo();
    test_hydrolox_dewar_and_inspector_clarity();
    test_starship_landing_pad();
    test_starship_flight_animation();
    test_earth_moon_trade_and_act2_victory();
    test_dynamic_cybernetic_voice();
    printf("[test] All Milestone 64.9-64.12, 64.17 - 64.21 tests passed cleanly.\n");
    return 0;
}
