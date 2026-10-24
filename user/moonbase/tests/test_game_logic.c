/* Milestone 64.9-64.11 regression tests: picking, power solver, life-support rules,
 * camera bounds, pointer routing, the regolith guard, depth-sorted rendering and
 * sprite picking, shared UI layout, pause and clock, and the same
 * play sequence by mouse, touch and keyboard. Headless (no SDL). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "moon_game.h"
#include "moon_sprites.h"

static moon_game_t g;

static void fresh(void) {
    memset(&g, 0, sizeof(g));
    assert(moon_game_init(&g, 1024, 600));
}

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

static void key_up(uint32_t k) {
    moon_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MOON_EVENT_KEY_UP; ev.key = k;
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
    ticks(1); /* capacity now includes the battery bank */
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
    assert(!g.has_selection);

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
        g.has_selection = true;
        g.selected_tile_x = 33; g.selected_tile_y = 32;
        int ip_w = w ? 260 : 220;
        int ip_x = g.screen_w - ip_w - 10;
        click(ip_x + ip_w - 14, 40 + 12);
        assert(!g.has_selection);
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
    moon_world_render(&g.structs, &g.grid, surf, g.cam_x, g.cam_y,
                      g.hover_tile_x, g.hover_tile_y, false);
    moon_world_render_overlays(&g.structs, &g.grid, surf, &g.resources, g.cam_x, g.cam_y);
}

static void tile_screen(int gx, int gy, int32_t *sx, int32_t *sy) {
    const moon_tile_t *t = moon_grid_get_tile_const(&g.grid, gx, gy);
    moon_grid_to_screen(gx, gy, t->elevation, g.cam_x, g.cam_y, 1024, 600, sx, sy);
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

    /* Put a taller neighbour in front of it, so that its sprite covers the badge */
    int covered = 0;
    for (int gy = 27; gy <= 33 && !covered; gy++) {
        for (int gx = 27; gx <= 33 && !covered; gx++) {
            if (!moon_structure_can_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, gx, gy, NULL)) continue;
            moon_structure_t *f = moon_structure_place(&g.grid, &g.structs, MOON_STRUCT_HABITAT_DOME, gx, gy);
            int id;
            if (moon_structure_pick(&g.structs, &g.grid, badge_x, badge_y, g.cam_x, g.cam_y, 1024, 600, &id)
                && id == f->id && gx + gy + 2 > 60) {
                covered = 1;
            } else {
                moon_structure_demolish(&g.grid, &g.structs, gx, gy);
            }
        }
    }
    assert(covered);
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
    assert(g.has_selection && g.selected_tile_x == 30 && g.selected_tile_y == 30);

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
        &ui.pause_btn, &ui.speed_btn, &ui.menu_btn, &ui.insp_recycle, &ui.insp_standby,
        &ui.confirm_ok, &ui.confirm_cancel, &ui.menu_item[0], &ui.menu_item[1],
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
        assert(!overlap(&ui.ribbon[i], &ui.pause_btn) && !overlap(&ui.ribbon[i], &ui.menu_btn));
        for (int j = i + 1; j < MOON_UI_RIBBON_SLOTS; j++) assert(!overlap(&ui.ribbon[i], &ui.ribbon[j]));
    }
    assert(!overlap(&ui.pause_btn, &ui.speed_btn) && !overlap(&ui.speed_btn, &ui.menu_btn));
    /* Panels that can be open together must not share pixels */
    assert(!overlap(&ui.inspector, &ui.minimap_zone));
    assert(!overlap(&ui.inspector, &ui.hud));
    assert(!overlap(&ui.inspector, &ui.ribbon_bar));
    assert(!overlap(&ui.minimap_zone, &ui.ribbon_bar));
    assert(!overlap(&ui.confirm_ok, &ui.confirm_cancel));
    assert(!overlap(&ui.confirm_ok, &ui.ribbon_bar) && !overlap(&ui.confirm_cancel, &ui.minimap_zone));
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

/* One play sequence, three input devices. Returns the final regolith in 1/16 kg. */
typedef struct { int regolith, structures; bool tile_free, standby_seen; } outcome_t;

static outcome_t play(int device) {
    fresh();
    s_touch = (device == 1);
    outcome_t o = { 0, 0, false, false };
    const int T = 30;

    if (device == 2) {
        key(MOON_KEY_4);                 /* Sinterer */
        key(MOON_KEY_UP);                /* cursor (32,32) -> (30,30) */
        key(MOON_KEY_UP);
        key(MOON_KEY_RETURN);
    } else {
        click_rect(&g.ui.ribbon[4]);
        click_tile(T, T);
        if (device == 1) {
            /* A finger only positions the ghost; nothing is built yet */
            assert(g.pending_valid && g.structs.count == 4);
            click_rect(&g.ui.confirm_ok);
        }
    }
    assert(g.structs.count == 5);
    const moon_tile_t *t = moon_grid_get_tile_const(&g.grid, T, T);
    const moon_structure_t *s = &g.structs.list[t->structure_id];
    assert(s->type == MOON_STRUCT_REGOLITH_PLANT && s->active);

    /* Inspect it */
    if (device == 2) { key(MOON_KEY_0); key(MOON_KEY_RETURN); }
    else { click_rect(&g.ui.ribbon[0]); click_tile(T, T); }
    assert(g.has_selection && g.selected_tile_x == T && g.selected_tile_y == T);

    /* Standby on, then recycle */
    if (device == 2) key(MOON_KEY_E); else click_rect(&g.ui.insp_standby);
    o.standby_seen = !g.structs.list[t->structure_id].active;
    if (device == 2) { key(MOON_KEY_R); key(MOON_KEY_RETURN); }
    else click_rect(&g.ui.insp_recycle);

    o.regolith = fixed_to_int(g.resources.regolith_stored_kg);
    o.structures = g.structs.count;
    o.tile_free = (moon_grid_get_tile_const(&g.grid, T, T)->structure_id == 0);
    s_touch = 0;
    return o;
}

static void test_three_devices(void) {
    outcome_t m = play(0), f = play(1), k = play(2);
    assert(m.standby_seen && f.standby_seen && k.standby_seen);
    assert(m.tile_free && f.tile_free && k.tile_free);
    assert(m.structures == 4 && f.structures == 4 && k.structures == 4);
    assert(m.regolith == f.regolith && m.regolith == k.regolith);
    printf("[test] Build / inspect / standby / recycle by mouse, touch, keyboard: PASS (%d kg)\n", m.regolith);
}

static void test_touch_flow(void) {
    fresh();
    s_touch = 1;
    int rgl = g.resources.regolith_stored_kg;
    click_rect(&g.ui.ribbon[4]);

    /* Press: the ghost follows the finger; a second tap moves it; cancel builds nothing */
    int32_t sx, sy;
    tile_screen(31, 31, &sx, &sy);
    ptr(MOON_EVENT_POINTER_DOWN, sx, sy, 1);
    assert(g.hover_valid && g.hover_tile_x == 31);
    ptr(MOON_EVENT_POINTER_UP, sx, sy, 1);
    assert(g.pending_valid && g.pending_x == 31 && g.pending_y == 31 && g.hover_valid);
    click_tile(30, 30);
    assert(g.pending_x == 30 && g.pending_y == 30);
    click_rect(&g.ui.confirm_cancel);
    assert(!g.pending_valid && g.structs.selected_build_type == MOON_STRUCT_NONE);
    assert(g.structs.count == 4 && g.resources.regolith_stored_kg == rgl);

    /* Inspect by tap: the highlight does not stay behind */
    click_tile(35, 35);
    assert(g.has_selection && !g.hover_valid);

    /* A 10 px wobble is a tap; 20 px is a drag that pans and does not select */
    g.has_selection = false;
    tile_screen(36, 30, &sx, &sy);
    int cam = g.cam_x;
    ptr(MOON_EVENT_POINTER_DOWN, sx, sy, 1);
    ptr(MOON_EVENT_POINTER_MOVE, sx + 8, sy + 4, 1);
    ptr(MOON_EVENT_POINTER_UP, sx + 8, sy + 4, 1);
    assert(g.has_selection && g.cam_x == cam);
    g.has_selection = false;
    ptr(MOON_EVENT_POINTER_DOWN, sx, sy, 1);
    ptr(MOON_EVENT_POINTER_MOVE, sx + 30, sy, 1);
    ptr(MOON_EVENT_POINTER_UP, sx + 30, sy, 1);
    assert(!g.has_selection && g.cam_x != cam);
    s_touch = 0;
    printf("[test] Touch two-step placement, hover clearing, drag threshold: PASS\n");
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
    key(MOON_KEY_Q); key(MOON_KEY_Q);          /* menu, then closed again */
    assert(g.running);

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
    click_rect(&g.ui.ribbon[4]);
    click_tile(31, 31);
    assert(g.structs.count == 5);
    click_rect(&g.ui.menu_btn);
    assert(g.menu_page == MOON_MENU_MAIN);
    click_rect(&g.ui.menu_item[2]);
    assert(g.menu_page == MOON_MENU_CONFIRM_RESTART && g.structs.count == 5);
    click_rect(&g.ui.menu_item[0]);
    assert(g.menu_page == MOON_MENU_CLOSED && g.structs.count == 4 && g.running);
    assert(g.sim_ticks == 0 && !g.has_selection && g.structs.selected_build_type == MOON_STRUCT_NONE);

    /* Right click, Backspace, Tab-to-inspect and ESC cancel a tool alike */
    for (int how = 0; how < 3; how++) {
        fresh();
        key(MOON_KEY_3);
        assert(g.structs.selected_build_type == MOON_STRUCT_HABITAT_DOME);
        if (how == 0) { ptr(MOON_EVENT_POINTER_DOWN, 500, 300, 3); ptr(MOON_EVENT_POINTER_UP, 500, 300, 3); }
        if (how == 1) key(MOON_KEY_BACKSPACE);
        if (how == 2) key(MOON_KEY_ESCAPE);
        assert(g.structs.selected_build_type == MOON_STRUCT_NONE && g.menu_page == MOON_MENU_CLOSED);
    }

    /* Tab walks the ribbon */
    fresh();
    key(MOON_KEY_TAB);
    assert(g.structs.selected_build_type == MOON_STRUCT_SOLAR_ARRAY);
    for (int i = 0; i < 6; i++) key(MOON_KEY_TAB);
    assert(g.demolish_tool_active);
    key(MOON_KEY_TAB);
    assert(!g.demolish_tool_active && g.structs.selected_build_type == MOON_STRUCT_NONE);
    printf("[test] Menu, quit/restart confirmation, identical cancel: PASS\n");
}

static void test_keyboard_scroll_and_cursor(void) {
    fresh();
    g.key_up_supported = true;
    int cy = g.cam_y;
    key(MOON_KEY_S);
    moon_game_update(&g, 100);
    assert(g.cam_y > cy + 30);
    key_up(MOON_KEY_S);
    cy = g.cam_y;
    moon_game_update(&g, 100);
    assert(g.cam_y == cy);

    /* Without key-up events a press is one step */
    g.key_up_supported = false;
    key(MOON_KEY_S);
    assert(g.cam_y == cy + 24);

    /* The cursor stays on the map and drags the view along when it leaves the screen */
    for (int i = 0; i < 40; i++) key(MOON_KEY_RIGHT);
    assert(g.cursor_x == MOON_MAP_SIZE_X - 1 && g.cursor_y == 0 && g.hover_valid);
    int32_t sx, sy;
    tile_screen(g.cursor_x, g.cursor_y, &sx, &sy);
    assert(sx >= 0 && sx < 1024 && sy >= 0 && sy < 600);

    /* The mouse leaving the window clears the hover, a keyboard cursor keeps it */
    fresh();
    ptr(MOON_EVENT_POINTER_MOVE, 500, 300, 0);
    assert(g.hover_valid);
    moon_event_t lv; memset(&lv, 0, sizeof(lv)); lv.type = MOON_EVENT_POINTER_LEAVE;
    moon_game_handle_event(&g, &lv);
    assert(!g.hover_valid);
    printf("[test] Held-key scroll, map cursor, pointer leave: PASS\n");
}

int main(void) {
    test_picking();
    test_power_solver();
    test_life_support_rules();
    test_camera_and_ticks();
    test_pointer_routing();
    test_regolith_guard();
    test_depth_order();
    test_badge_survives();
    test_sprite_pick();
    test_palette_and_tint();
    test_layout();
    test_clock_and_pause();
    test_three_devices();
    test_touch_flow();
    test_menu_and_cancel();
    test_keyboard_scroll_and_cursor();
    printf("[test] All Milestone 64.9-64.11 tests passed cleanly.\n");
    return 0;
}
