#include "moon_game.h"
#include "moon_config.h"
#include "moon_fixed.h"
#include "moon_blitter.h"
#include "moon_grid.h"
#include "moon_structures.h"

static const char *get_terrain_name(moon_terrain_type_t t) {
    switch (t) {
        case MOON_TERRAIN_REGOLITH_FLAT:  return "REGOLITH";
        case MOON_TERRAIN_REGOLITH_ROUGH: return "BOULDERS";
        case MOON_TERRAIN_BASALT_MARE:    return "BASALT MARE";
        case MOON_TERRAIN_CRATER_RIM:     return "CRATER RIM";
        case MOON_TERRAIN_CRATER_FLOOR:   return "CRATER FLOOR";
        case MOON_TERRAIN_ICE_DEPOSIT:    return "POLAR ICE";
        default:                          return "UNKNOWN";
    }
}

bool moon_game_init(moon_game_t *game, int width, int height) {
    if (!game) return false;

    game->state = MOON_STATE_PLAYING;
    game->running = true;
    game->screen_w = width;
    game->screen_h = height;

    /* Generate procedural lunar terrain */
    moon_grid_generate(&game->grid, 19690720);

    /* Initialize structure manager & place expedition lander at touchdown site */
    moon_structure_mgr_init(&game->structs);
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_LANDER, 32, 32);

    /* Initialize resource & life support simulation */
    moon_resources_init(&game->resources);
    moon_resources_solve_power_grid(&game->grid, &game->structs);

    /* Center camera on initial expedition landing site (tile 32, 32) */
    game->cam_x = 0;
    game->cam_y = 32 * MOON_TILE_H;

    game->pointer_x = 0;
    game->pointer_y = 0;
    game->pointer_down = false;
    game->was_dragging = false;
    game->drag_start_x = 0;
    game->drag_start_y = 0;
    game->cam_start_x = game->cam_x;
    game->cam_start_y = game->cam_y;

    game->hover_tile_x = 0;
    game->hover_tile_y = 0;
    game->hover_valid = false;

    game->frame_count = 0;
    game->sim_ticks = 0;
    game->last_tick_ms = 0;

    return true;
}

void moon_game_shutdown(moon_game_t *game) {
    if (!game) return;
    game->running = false;
}

/* Mapping from the 6 Ribbon buttons / keys to placeable structure types */
static const moon_structure_type_t s_build_buttons[6] = {
    MOON_STRUCT_SOLAR_ARRAY,    /* Button 1 / Key 1 */
    MOON_STRUCT_BATTERY_BANK,   /* Button 2 / Key 2 */
    MOON_STRUCT_HABITAT_DOME,   /* Button 3 / Key 3 */
    MOON_STRUCT_REGOLITH_PLANT, /* Button 4 / Key 4 */
    MOON_STRUCT_O2_EXTRACTOR,   /* Button 5 / Key 5 */
    MOON_STRUCT_POWER_CONDUIT   /* Button 6 / Key 6 */
};

static void clamp_camera(moon_game_t *game) {
    if (game->cam_x < -1200) game->cam_x = -1200;
    if (game->cam_x > 1200)  game->cam_x = 1200;
    if (game->cam_y < 100)   game->cam_y = 100;
    if (game->cam_y > 2000)  game->cam_y = 2000;
}

static void try_place_selected_structure(moon_game_t *game) {
    if (game->structs.selected_build_type == MOON_STRUCT_NONE || !game->hover_valid) return;

    const moon_structure_def_t *def = moon_structure_get_def(game->structs.selected_build_type);
    if (!def) return;

    fixed32_t cost = fixed_from_int(def->cost_regolith);
    if (game->resources.regolith_stored_kg < cost) {
        /* Insufficient materials */
        return;
    }

    moon_structure_t *placed = moon_structure_place(&game->grid, &game->structs,
                                                    game->structs.selected_build_type,
                                                    game->hover_tile_x, game->hover_tile_y);
    if (placed) {
        game->resources.regolith_stored_kg -= cost;
        moon_resources_solve_power_grid(&game->grid, &game->structs);
    }
}

void moon_game_handle_event(moon_game_t *game, const moon_event_t *ev) {
    if (!game || !ev) return;

    switch (ev->type) {
        case MOON_EVENT_QUIT:
            game->running = false;
            break;

        case MOON_EVENT_KEY_DOWN:
            switch (ev->key) {
                case MOON_KEY_ESCAPE:
                    if (game->structs.selected_build_type != MOON_STRUCT_NONE) {
                        game->structs.selected_build_type = MOON_STRUCT_NONE;
                    } else {
                        game->running = false;
                    }
                    break;

                case MOON_KEY_1:
                    game->structs.selected_build_type = MOON_STRUCT_SOLAR_ARRAY;
                    break;
                case MOON_KEY_2:
                    game->structs.selected_build_type = MOON_STRUCT_BATTERY_BANK;
                    break;
                case MOON_KEY_3:
                    game->structs.selected_build_type = MOON_STRUCT_HABITAT_DOME;
                    break;
                case MOON_KEY_4:
                    game->structs.selected_build_type = MOON_STRUCT_REGOLITH_PLANT;
                    break;
                case MOON_KEY_5:
                    game->structs.selected_build_type = MOON_STRUCT_O2_EXTRACTOR;
                    break;
                case MOON_KEY_6:
                    game->structs.selected_build_type = MOON_STRUCT_POWER_CONDUIT;
                    break;

                case MOON_KEY_Q:
                    game->structs.selected_build_type = MOON_STRUCT_NONE;
                    break;

                case MOON_KEY_R:
                    /* Demolish structure at hover tile */
                    if (game->hover_valid) {
                        moon_structure_demolish(&game->grid, &game->structs,
                                                game->hover_tile_x, game->hover_tile_y);
                        moon_resources_solve_power_grid(&game->grid, &game->structs);
                    }
                    break;

                case MOON_KEY_UP:
                case MOON_KEY_W:
                    game->cam_y -= 24;
                    clamp_camera(game);
                    break;
                case MOON_KEY_DOWN:
                case MOON_KEY_S:
                    game->cam_y += 24;
                    clamp_camera(game);
                    break;
                case MOON_KEY_LEFT:
                case MOON_KEY_A:
                    game->cam_x -= 24;
                    clamp_camera(game);
                    break;
                case MOON_KEY_RIGHT:
                case MOON_KEY_D:
                    game->cam_x += 24;
                    clamp_camera(game);
                    break;
                case MOON_KEY_SPACE:
                    /* Recenter camera on expedition lander */
                    game->cam_x = 0;
                    game->cam_y = 32 * MOON_TILE_H;
                    break;
                default:
                    break;
            }
            break;

        case MOON_EVENT_POINTER_DOWN:
            game->pointer_down = true;
            game->was_dragging = false;
            game->pointer_x = ev->x;
            game->pointer_y = ev->y;
            game->drag_start_x = ev->x;
            game->drag_start_y = ev->y;
            game->cam_start_x = game->cam_x;
            game->cam_start_y = game->cam_y;
            break;

        case MOON_EVENT_POINTER_UP:
            if (ev->button == 3) {
                /* Right click cancels build mode */
                game->structs.selected_build_type = MOON_STRUCT_NONE;
            } else if (ev->button == 1 && !game->was_dragging) {
                /* Check if clicked on Bottom Build Ribbon */
                if (ev->y >= game->screen_h - 40) {
                    int btn_w = 95;
                    int start_x = 10;
                    for (int i = 0; i < 6; i++) {
                        int bx = start_x + i * (btn_w + 6);
                        if (ev->x >= bx && ev->x <= bx + btn_w) {
                            moon_structure_type_t target_type = s_build_buttons[i];
                            if (game->structs.selected_build_type == target_type) {
                                game->structs.selected_build_type = MOON_STRUCT_NONE;
                            } else {
                                game->structs.selected_build_type = target_type;
                            }
                            break;
                        }
                    }
                } else {
                    /* Click on map viewport to place structure */
                    try_place_selected_structure(game);
                }
            }
            game->pointer_down = false;
            break;

        case MOON_EVENT_POINTER_MOVE:
            game->pointer_x = ev->x;
            game->pointer_y = ev->y;
            if (game->pointer_down) {
                int32_t dx = ev->x - game->drag_start_x;
                int32_t dy = ev->y - game->drag_start_y;
                if (dx * dx + dy * dy > 16) {
                    game->was_dragging = true;
                }
                game->cam_x = game->cam_start_x - dx;
                game->cam_y = game->cam_start_y - dy;
                clamp_camera(game);
            }

            /* Update hover tile */
            moon_screen_to_grid(ev->x, ev->y, game->cam_x, game->cam_y,
                                game->screen_w, game->screen_h,
                                &game->hover_tile_x, &game->hover_tile_y);

            game->hover_valid = (game->hover_tile_x >= 0 && game->hover_tile_x < MOON_MAP_SIZE_X &&
                                 game->hover_tile_y >= 0 && game->hover_tile_y < MOON_MAP_SIZE_Y);
            break;

        case MOON_EVENT_SCROLL:
            game->cam_x -= (int32_t)ev->wheel_x * 32;
            game->cam_y -= (int32_t)ev->wheel_y * 32;
            clamp_camera(game);
            break;

        default:
            break;
    }
}

void moon_game_update(moon_game_t *game, uint32_t delta_ms) {
    if (!game) return;

    game->frame_count++;
    game->last_tick_ms += delta_ms;

    while (game->last_tick_ms >= MOON_SIM_TICK_INTERVAL_MS) {
        game->last_tick_ms -= MOON_SIM_TICK_INTERVAL_MS;
        game->sim_ticks++;
        moon_resources_tick(&game->resources, &game->grid, &game->structs);
    }
}

static void render_build_ribbon(const moon_game_t *game, const moon_surface_t *surf) {
    int ribbon_y = game->screen_h - 38;
    int btn_w = 95;
    int btn_h = 32;
    int start_x = 10;

    /* Ribbon Background */
    moon_blit_fill_rect(surf, 0, ribbon_y - 2, game->screen_w, 40, RGB565(10, 12, 16));
    moon_blit_line(surf, 0, ribbon_y - 2, game->screen_w - 1, ribbon_y - 2, COLOR_GOLD_FOIL);

    static const char *labels[6] = {
        "1:SOLAR 10t", "2:BATT  20t", "3:HAB   50t",
        "4:SINT  15t", "5:OXY   25t", "6:CABLE  2t"
    };

    for (int i = 0; i < 6; i++) {
        int bx = start_x + i * (btn_w + 6);
        bool selected = (game->structs.selected_build_type == s_build_buttons[i]);
        const moon_structure_def_t *def = moon_structure_get_def(s_build_buttons[i]);
        bool can_afford = def && (game->resources.regolith_stored_kg >= fixed_from_int(def->cost_regolith));

        uint16_t bg = selected ? RGB565(30, 45, 60) : RGB565(20, 24, 30);
        uint16_t border = selected ? COLOR_LASER_CYAN : (can_afford ? RGB565(60, 65, 75) : RGB565(35, 38, 45));
        uint16_t text_col = selected ? COLOR_LUNAR_WHITE : (can_afford ? RGB565(170, 175, 185) : RGB565(80, 85, 95));

        moon_blit_fill_rect(surf, bx, ribbon_y + 2, btn_w, btn_h, bg);
        moon_blit_rect_outline(surf, bx, ribbon_y + 2, btn_w, btn_h, border);
        moon_blit_text(surf, bx + 6, ribbon_y + 10, labels[i], text_col, 0, true);
    }
}

void moon_game_render(moon_game_t *game, uint16_t *fb, int width, int height, int stride) {
    if (!game || !fb) return;

    moon_surface_t surf = moon_surface_make(fb, width, height, stride);

    /* 1. Clear background to lunar deep space */
    moon_blit_clear(&surf, COLOR_SPACE_BLACK);

    /* 2. Render visible isometric terrain & elevated relief */
    moon_grid_render(&game->grid, &surf,
                     game->cam_x, game->cam_y,
                     game->hover_tile_x, game->hover_tile_y,
                     game->hover_valid);

    /* 3. Render all placed structures */
    moon_structure_render_all(&game->structs, &game->grid, &surf,
                              game->cam_x, game->cam_y);

    /* 4. Render structure placement ghost if build tool is active */
    if (game->structs.selected_build_type != MOON_STRUCT_NONE && game->hover_valid) {
        moon_structure_render_ghost(&game->structs, &game->grid, &surf,
                                    game->cam_x, game->cam_y,
                                    game->hover_tile_x, game->hover_tile_y);
    }

    /* 5. Render Bottom-Right Minimap Radar */
    int mm_size = 64;
    int mm_x = width - mm_size - 10;
    int mm_y = height - mm_size - 44; /* Above build ribbon */
    moon_grid_render_minimap(&game->grid, &surf, mm_x, mm_y, mm_size, mm_size,
                             game->cam_x, game->cam_y);
    moon_blit_text(&surf, mm_x - 30, mm_y - 18, "RADAR", COLOR_GOLD_FOIL, 0, true);

    /* 6. Render Bottom Build Ribbon */
    render_build_ribbon(game, &surf);

    /* 7. Render Top HUD Bar (Title, Day/Night Clock, Power, O2, H2O, Materials, Crew) */
    moon_blit_fill_rect(&surf, 0, 0, width, 32, RGB565(14, 16, 20));
    moon_blit_line(&surf, 0, 32, width - 1, 32, COLOR_GOLD_FOIL);

    /* Lunar Day / Solar Cycle Clock */
    const moon_resources_t *res = &game->resources;
    int sun_pct = fixed_to_int(res->solar_intensity * 100);
    if (!res->is_night) {
        moon_blit_text_fmt(&surf, 8, 8, COLOR_GOLD_FOIL, 0, true,
                           "D%02d SUN:%d%%", res->lunar_day, sun_pct);
    } else {
        moon_blit_text_fmt(&surf, 8, 8, COLOR_LASER_CYAN, 0, true,
                           "D%02d NIGHT", res->lunar_day);
    }

    /* Power Indicator & Battery Reserve */
    uint16_t pwr_col = res->power_brownout ? COLOR_ALERT_RED : (res->warning_power ? COLOR_ENERGY_YELLOW : COLOR_ENERGY_YELLOW);
    int batt_kwh = fixed_to_int(res->battery_stored_kwh);
    moon_blit_fill_rect(&surf, 130, 10, 6, 12, pwr_col);
    moon_blit_text_fmt(&surf, 140, 8, pwr_col, 0, true,
                       "PWR %d/%dkW BATT %d/%dkWh",
                       res->power_produced_kw, res->power_consumed_kw,
                       batt_kwh, res->battery_capacity_kwh);

    /* Regolith / Paver Material Stock */
    int rgl_kg = fixed_to_int(res->regolith_stored_kg);
    moon_blit_fill_rect(&surf, 380, 10, 6, 12, COLOR_REGOLITH_LIGHT);
    moon_blit_text_fmt(&surf, 390, 8, COLOR_REGOLITH_LIGHT, 0, true,
                       "RGL %dkg", rgl_kg);

    /* O2 (Oxygen) */
    int o2_kg = fixed_to_int(res->o2_stored_kg);
    uint16_t o2_col = res->warning_o2 ? COLOR_ALERT_RED : COLOR_LASER_CYAN;
    moon_blit_fill_rect(&surf, 480, 10, 6, 12, o2_col);
    moon_blit_text_fmt(&surf, 490, 8, o2_col, 0, true,
                       "O2 %dkg", o2_kg);

    /* H2O (Water) */
    int water_l = fixed_to_int(res->water_stored_l);
    uint16_t water_col = res->warning_water ? COLOR_ALERT_RED : COLOR_SOLAR_BLUE;
    moon_blit_fill_rect(&surf, 570, 10, 6, 12, water_col);
    moon_blit_text_fmt(&surf, 580, 8, water_col, 0, true,
                       "H2O %dL", water_l);

    /* Crew & Life Support Status */
    uint16_t ls_col = (res->life_support_pct < 50) ? COLOR_ALERT_RED :
                      ((res->life_support_pct < 80) ? COLOR_ENERGY_YELLOW : COLOR_STATUS_GREEN);
    moon_blit_fill_rect(&surf, 660, 10, 6, 12, ls_col);
    moon_blit_text_fmt(&surf, 670, 8, ls_col, 0, true,
                       "CREW %d/%d (LS:%d%%)",
                       res->population, res->max_population, res->life_support_pct);

    /* Right HUD Info (Hover Tile Coordinates & Resource Inspector) */
    if (game->hover_valid) {
        const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid,
                                                         game->hover_tile_x,
                                                         game->hover_tile_y);
        const char *tname = ht ? get_terrain_name((moon_terrain_type_t)ht->terrain) : "VOID";
        int elev = ht ? ht->elevation : 0;
        int rval = ht ? ht->resource_val : 0;

        int badge_w = 210;
        int badge_x = width - badge_w - 8;
        moon_blit_fill_rect(&surf, badge_x, 4, badge_w, 24, RGB565(26, 30, 36));
        moon_blit_rect_outline(&surf, badge_x, 4, badge_w, 24, COLOR_LASER_CYAN);

        moon_blit_text_fmt(&surf, badge_x + 6, 8, COLOR_LASER_CYAN, 0, true,
                           "[%02d,%02d] %s E:%d R:%d%%",
                           game->hover_tile_x, game->hover_tile_y,
                           tname, elev, rval);
    }

    /* 8. Active Life Support / Crisis Warning Ticker Banner */
    if (res->warning_cold) {
        moon_blit_fill_rect(&surf, 8, 38, 380, 20, COLOR_ALERT_RED);
        moon_blit_text(&surf, 14, 40, "[CRITICAL] CRYOGENIC FREEZING: HEATING LOST", COLOR_LUNAR_WHITE, 0, true);
    } else if (res->warning_power) {
        moon_blit_fill_rect(&surf, 8, 38, 350, 20, RGB565(180, 50, 20));
        moon_blit_text(&surf, 14, 40, "[ALERT] POWER BLACKOUT: BATTERIES DEPLETED", COLOR_LUNAR_WHITE, 0, true);
    } else if (res->warning_o2) {
        moon_blit_fill_rect(&surf, 8, 38, 320, 20, COLOR_ALERT_RED);
        moon_blit_text(&surf, 14, 40, "[CRITICAL] O2 STARVATION: RESERVES EMPTY", COLOR_LUNAR_WHITE, 0, true);
    }
}
