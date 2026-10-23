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

    /* Initialize structure manager & place expedition landing site */
    moon_structure_mgr_init(&game->structs);
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_LANDER, 32, 32);
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_POWER_CONDUIT, 33, 32);
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_SOLAR_ARRAY, 34, 32);
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_BATTERY_BANK, 33, 33);

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
                    if (game->structs.selected_build_type != MOON_STRUCT_NONE || game->demolish_tool_active) {
                        game->structs.selected_build_type = MOON_STRUCT_NONE;
                        game->demolish_tool_active = false;
                    } else if (game->has_selection) {
                        game->has_selection = false;
                    } else {
                        game->running = false;
                    }
                    break;

                case MOON_KEY_0:
                case MOON_KEY_Q:
                    game->structs.selected_build_type = MOON_STRUCT_NONE;
                    game->demolish_tool_active = false;
                    break;

                case MOON_KEY_1:
                    game->structs.selected_build_type = MOON_STRUCT_SOLAR_ARRAY;
                    game->demolish_tool_active = false;
                    break;
                case MOON_KEY_2:
                    game->structs.selected_build_type = MOON_STRUCT_BATTERY_BANK;
                    game->demolish_tool_active = false;
                    break;
                case MOON_KEY_3:
                    game->structs.selected_build_type = MOON_STRUCT_HABITAT_DOME;
                    game->demolish_tool_active = false;
                    break;
                case MOON_KEY_4:
                    game->structs.selected_build_type = MOON_STRUCT_REGOLITH_PLANT;
                    game->demolish_tool_active = false;
                    break;
                case MOON_KEY_5:
                    game->structs.selected_build_type = MOON_STRUCT_O2_EXTRACTOR;
                    game->demolish_tool_active = false;
                    break;
                case MOON_KEY_6:
                    game->structs.selected_build_type = MOON_STRUCT_POWER_CONDUIT;
                    game->demolish_tool_active = false;
                    break;

                case MOON_KEY_R:
                    game->demolish_tool_active = !game->demolish_tool_active;
                    game->structs.selected_build_type = MOON_STRUCT_NONE;
                    break;

                case MOON_KEY_T:
                    game->grid.textured_terrain = !game->grid.textured_terrain;
                    game->notify_msg = game->grid.textured_terrain
                        ? "TERRAIN: TEXTURED (HIGH QUALITY)"
                        : "TERRAIN: FLAT SHADED (FAST RETRO)";
                    game->notify_timer_ms = 2500;
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
                /* Right click cancels build / demolish tool */
                game->structs.selected_build_type = MOON_STRUCT_NONE;
                game->demolish_tool_active = false;
            } else if (ev->button == 1 && !game->was_dragging) {
                int mm_size = 64;
                int mm_x = game->screen_w - mm_size - 10;
                int mm_y = game->screen_h - mm_size - 44;

                /* 0. Check if clicked on Terrain Texture Mode Toggle button */
                if (ev->x >= mm_x && ev->x < mm_x + mm_size && ev->y >= mm_y - 20 && ev->y < mm_y - 2) {
                    game->grid.textured_terrain = !game->grid.textured_terrain;
                    game->notify_msg = game->grid.textured_terrain
                        ? "TERRAIN: TEXTURED (HIGH QUALITY)"
                        : "TERRAIN: FLAT SHADED (FAST RETRO)";
                    game->notify_timer_ms = 2500;
                    game->pointer_down = false;
                    break;
                }

                /* 1. Check if clicked on Minimap Radar for Instant Navigation */
                if (ev->x >= mm_x && ev->x < mm_x + mm_size && ev->y >= mm_y && ev->y < mm_y + mm_size) {
                    int target_gx = ev->x - mm_x;
                    int target_gy = ev->y - mm_y;
                    game->cam_x = (target_gx - target_gy) * MOON_TILE_HALF_W;
                    game->cam_y = (target_gx + target_gy) * MOON_TILE_HALF_H;
                    clamp_camera(game);
                    game->pointer_down = false;
                    break;
                }

                /* 2. Check if clicked on Bottom Build & Tools Ribbon */
                if (ev->y >= game->screen_h - 40) {
                    int btn_w = (game->screen_w < 800) ? 70 : 95;
                    int btn_gap = (game->screen_w < 800) ? 4 : 6;
                    int start_x = (game->screen_w < 800) ? 4 : 10;
                    for (int i = 0; i < 8; i++) {
                        int bx = start_x + i * (btn_w + btn_gap);
                        if (ev->x >= bx && ev->x <= bx + btn_w) {
                            if (i == 0) {
                                /* 0: Inspect Mode */
                                game->structs.selected_build_type = MOON_STRUCT_NONE;
                                game->demolish_tool_active = false;
                            } else if (i >= 1 && i <= 6) {
                                /* 1..6: Placeable Structure Tools */
                                moon_structure_type_t target_type = s_build_buttons[i - 1];
                                if (game->structs.selected_build_type == target_type) {
                                    game->structs.selected_build_type = MOON_STRUCT_NONE;
                                } else {
                                    game->structs.selected_build_type = target_type;
                                    game->demolish_tool_active = false;
                                }
                            } else if (i == 7) {
                                /* R: Demolish / Recycle Tool */
                                game->demolish_tool_active = !game->demolish_tool_active;
                                game->structs.selected_build_type = MOON_STRUCT_NONE;
                            }
                            break;
                        }
                    }
                } else if (game->has_selection &&
                           ev->x >= game->screen_w - ((game->screen_w < 800) ? 230 : 280) &&
                           ev->x <= game->screen_w - 10 &&
                           ev->y >= 40 && ev->y <= 220) {
                    /* 3. Check if clicked inside Inspector Drawer */
                    int ip_w = (game->screen_w < 800) ? 220 : 260;
                    int ip_x = game->screen_w - ip_w - 10;
                    int ip_y = 40;

                    /* Clicked [X] close button */
                    if (ev->x >= ip_x + 260 - 22 && ev->y <= ip_y + 22) {
                        game->has_selection = false;
                    } else if (ev->y >= ip_y + 138 && ev->y <= ip_y + 164) {
                        const moon_tile_t *tile = moon_grid_get_tile_const(&game->grid,
                                                                           game->selected_tile_x,
                                                                           game->selected_tile_y);
                        if (tile && tile->structure_id != 0 && tile->structure_id != 1) {
                            /* Clicked [RECYCLE] */
                            if (ev->x >= ip_x + 8 && ev->x <= ip_x + 123) {
                                for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
                                    if (game->structs.list[i].id == tile->structure_id) {
                                        const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)game->structs.list[i].type);
                                        if (def) {
                                            game->resources.regolith_stored_kg += fixed_from_int(def->cost_regolith / 2);
                                        }
                                        break;
                                    }
                                }
                                moon_structure_demolish(&game->grid, &game->structs,
                                                        game->selected_tile_x, game->selected_tile_y);
                                moon_resources_solve_power_grid(&game->grid, &game->structs);
                                game->has_selection = false;
                            }
                            /* Clicked [STANDBY / ACTIVATE] */
                            else if (ev->x >= ip_x + 132 && ev->x <= ip_x + 247) {
                                for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
                                    if (game->structs.list[i].id == tile->structure_id) {
                                        game->structs.list[i].active = !game->structs.list[i].active;
                                        moon_resources_solve_power_grid(&game->grid, &game->structs);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    /* 4. Clicked on Map Viewport */
                    if (game->demolish_tool_active) {
                        if (game->hover_valid) {
                            const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid,
                                                                             game->hover_tile_x,
                                                                             game->hover_tile_y);
                            if (ht && ht->structure_id != 0 && ht->structure_id != 1) {
                                for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
                                    if (game->structs.list[i].id == ht->structure_id) {
                                        const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)game->structs.list[i].type);
                                        if (def) {
                                            game->resources.regolith_stored_kg += fixed_from_int(def->cost_regolith / 2);
                                        }
                                        break;
                                    }
                                }
                                moon_structure_demolish(&game->grid, &game->structs,
                                                        game->hover_tile_x, game->hover_tile_y);
                                moon_resources_solve_power_grid(&game->grid, &game->structs);
                            }
                        }
                    } else if (game->structs.selected_build_type != MOON_STRUCT_NONE) {
                        try_place_selected_structure(game);
                    } else {
                        /* Inspection mode: select clicked tile */
                        if (game->hover_valid) {
                            game->selected_tile_x = game->hover_tile_x;
                            game->selected_tile_y = game->hover_tile_y;
                            game->has_selection = true;
                        } else {
                            game->has_selection = false;
                        }
                    }
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

    if (game->notify_timer_ms > 0) {
        if (delta_ms >= game->notify_timer_ms) game->notify_timer_ms = 0;
        else game->notify_timer_ms -= delta_ms;
    }

    while (game->last_tick_ms >= MOON_SIM_TICK_INTERVAL_MS) {
        game->last_tick_ms -= MOON_SIM_TICK_INTERVAL_MS;
        game->sim_ticks++;
        moon_resources_tick(&game->resources, &game->grid, &game->structs);
    }
}

static void render_inspector_panel(const moon_game_t *game, const moon_surface_t *surf) {
    if (!game->has_selection) return;

    int ip_w = (game->screen_w < 800) ? 220 : 260;
    int ip_h = 180;
    int ip_x = game->screen_w - ip_w - 10;
    int ip_y = 40;

    /* Panel frame */
    moon_blit_fill_rect(surf, ip_x, ip_y, ip_w, ip_h, RGB565(14, 18, 24));
    moon_blit_rect_outline(surf, ip_x, ip_y, ip_w, ip_h, COLOR_LASER_CYAN);
    moon_blit_line(surf, ip_x, ip_y + 26, ip_x + ip_w - 1, ip_y + 26, COLOR_GOLD_FOIL);

    /* Close button [X] */
    moon_blit_fill_rect(surf, ip_x + ip_w - 22, ip_y + 4, 18, 18, RGB565(35, 40, 50));
    moon_blit_rect_outline(surf, ip_x + ip_w - 22, ip_y + 4, 18, 18, COLOR_ALERT_RED);
    moon_blit_text(surf, ip_x + ip_w - 17, ip_y + 5, "X", COLOR_ALERT_RED, 0, true);

    const moon_tile_t *tile = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
    if (!tile) return;

    if (tile->structure_id != 0) {
        const moon_structure_t *s = NULL;
        for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
            if (game->structs.list[i].id == tile->structure_id) {
                s = &game->structs.list[i];
                break;
            }
        }
        if (!s) return;

        const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)s->type);
        if (!def) return;

        /* Title */
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 6, COLOR_GOLD_FOIL, 0, true, "%s", def->name);

        /* Status */
        const char *status_str = !s->active ? "STANDBY" : (s->powered ? "ONLINE" : "NO POWER");
        uint16_t status_col = !s->active ? RGB565(140, 140, 150) : (s->powered ? COLOR_STATUS_GREEN : COLOR_ALERT_RED);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 34, status_col, 0, true, "STATUS: [%s]", status_str);

        /* Power metrics */
        if (def->power_kw > 0) {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 52, COLOR_ENERGY_YELLOW, 0, true,
                               "POWER: +%d kW (Gen)", def->power_kw);
        } else if (def->power_kw < 0) {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 52, COLOR_ENERGY_YELLOW, 0, true,
                               "POWER: %d kW (Demand)", def->power_kw);
        } else {
            moon_blit_text(surf, ip_x + 8, ip_y + 52, "POWER: Passive", COLOR_REGOLITH_LIGHT, 0, true);
        }

        /* Storage & Life Support */
        if (def->power_storage > 0) {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 70, COLOR_ENERGY_YELLOW, 0, true,
                               "STORAGE: %d kWh capacity", def->power_storage);
        } else if (def->rate_o2 != 0 || def->rate_water != 0) {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 70, COLOR_LASER_CYAN, 0, true,
                               "LIFE: %+d kg O2 / %+d L H2O", def->rate_o2, def->rate_water);
        } else {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 70, COLOR_REGOLITH_LIGHT, 0, true,
                               "COORDS: [%02d, %02d]", s->gx, s->gy);
        }

        /* Integrity */
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 88, COLOR_STATUS_GREEN, 0, true,
                           "STRUCTURAL INTEGRITY: %d%%", s->health);

        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 108, RGB565(160, 165, 175), 0, true,
                           "Footprint: %dx%d tiles", s->footprint_w, s->footprint_h);

        /* Buttons if not primary lander */
        if (tile->structure_id != 1) {
            /* Recycle button */
            moon_blit_fill_rect(surf, ip_x + 8, ip_y + 138, 115, 26, RGB565(45, 20, 20));
            moon_blit_rect_outline(surf, ip_x + 8, ip_y + 138, 115, 26, COLOR_ALERT_RED);
            moon_blit_text(surf, ip_x + 14, ip_y + 144, "RECYCLE 50%", COLOR_LUNAR_WHITE, 0, true);

            /* Toggle standby button */
            uint16_t act_bg = s->active ? RGB565(20, 45, 30) : RGB565(45, 40, 20);
            uint16_t act_border = s->active ? COLOR_STATUS_GREEN : COLOR_ENERGY_YELLOW;
            const char *act_label = s->active ? "STANDBY" : "ACTIVATE";
            moon_blit_fill_rect(surf, ip_x + 132, ip_y + 138, 115, 26, act_bg);
            moon_blit_rect_outline(surf, ip_x + 132, ip_y + 138, 115, 26, act_border);
            moon_blit_text(surf, ip_x + 148, ip_y + 144, act_label, COLOR_LUNAR_WHITE, 0, true);
        } else {
            moon_blit_text(surf, ip_x + 8, ip_y + 144, "EXPEDITION COMMAND ANCHOR", COLOR_GOLD_FOIL, 0, true);
        }
    } else {
        /* Terrain Inspector */
        const char *tname = get_terrain_name((moon_terrain_type_t)tile->terrain);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 6, COLOR_GOLD_FOIL, 0, true, "TERRAIN: %s", tname);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 36, COLOR_LASER_CYAN, 0, true,
                           "Coordinates: [%02d, %02d]", game->selected_tile_x, game->selected_tile_y);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 56, COLOR_REGOLITH_LIGHT, 0, true,
                           "Elevation Level: %d", tile->elevation);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 76, COLOR_GOLD_FOIL, 0, true,
                           "Mineral Richness: %d%%", tile->resource_val);

        if (tile->terrain == MOON_TERRAIN_ICE_DEPOSIT) {
            moon_blit_text(surf, ip_x + 8, ip_y + 104, "Water ice cold trap.", COLOR_LASER_CYAN, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 120, "Ideal for O2 Extractor.", COLOR_LUNAR_WHITE, 0, true);
        } else if (tile->terrain == MOON_TERRAIN_BASALT_MARE) {
            moon_blit_text(surf, ip_x + 8, ip_y + 104, "Volcanic titanium mare.", COLOR_REGOLITH_LIGHT, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 120, "Suitable for O2 / Sinterer.", COLOR_LUNAR_WHITE, 0, true);
        } else if (tile->terrain == MOON_TERRAIN_CRATER_RIM) {
            moon_blit_text(surf, ip_x + 8, ip_y + 104, "Elevated crater rim ridge.", COLOR_ALERT_RED, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 120, "Terrain too steep to build.", RGB565(160, 160, 160), 0, true);
        } else {
            moon_blit_text(surf, ip_x + 8, ip_y + 104, "Flat regolith plain.", COLOR_REGOLITH_LIGHT, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 120, "Prime foundation site.", COLOR_LUNAR_WHITE, 0, true);
        }
    }
}

static void render_build_ribbon(const moon_game_t *game, const moon_surface_t *surf) {
    int ribbon_y = game->screen_h - 38;
    bool narrow = (game->screen_w < 800);
    int btn_w = narrow ? 70 : 95;
    int btn_h = 32;
    int btn_gap = narrow ? 4 : 6;
    int start_x = narrow ? 4 : 10;

    /* Ribbon Background */
    moon_blit_fill_rect(surf, 0, ribbon_y - 2, game->screen_w, 40, RGB565(10, 12, 16));
    moon_blit_line(surf, 0, ribbon_y - 2, game->screen_w - 1, ribbon_y - 2, COLOR_GOLD_FOIL);

    static const char *labels[8] = {
        "0:INSPECT",
        "1:SOLAR 10t", "2:BATT  20t", "3:HAB   50t",
        "4:SINT  15t", "5:OXY   25t", "6:CABLE  2t",
        "R:RECYCLE"
    };

    static const char *labels_narrow[8] = {
        "0:INSP",
        "1:SOL", "2:BAT", "3:HAB",
        "4:SINT", "5:OXY", "6:WIRE",
        "R:DEM"
    };

    for (int i = 0; i < 8; i++) {
        int bx = start_x + i * (btn_w + btn_gap);
        bool selected = false;
        bool can_afford = true;

        if (i == 0) {
            selected = (game->structs.selected_build_type == MOON_STRUCT_NONE && !game->demolish_tool_active);
        } else if (i >= 1 && i <= 6) {
            selected = (game->structs.selected_build_type == s_build_buttons[i - 1]);
            const moon_structure_def_t *def = moon_structure_get_def(s_build_buttons[i - 1]);
            can_afford = def && (game->resources.regolith_stored_kg >= fixed_from_int(def->cost_regolith));
        } else if (i == 7) {
            selected = game->demolish_tool_active;
        }

        uint16_t bg = selected ? (i == 7 ? RGB565(55, 20, 20) : RGB565(30, 45, 60)) : RGB565(20, 24, 30);
        uint16_t border = selected ? (i == 7 ? COLOR_ALERT_RED : COLOR_LASER_CYAN) : (can_afford ? RGB565(60, 65, 75) : RGB565(35, 38, 45));
        uint16_t text_col = selected ? COLOR_LUNAR_WHITE : (can_afford ? RGB565(170, 175, 185) : RGB565(80, 85, 95));

        moon_blit_fill_rect(surf, bx, ribbon_y + 2, btn_w, btn_h, bg);
        moon_blit_rect_outline(surf, bx, ribbon_y + 2, btn_w, btn_h, border);
        const char *lbl = narrow ? labels_narrow[i] : labels[i];
        int tx = narrow ? (bx + 4) : (bx + 6);
        moon_blit_text(surf, tx, ribbon_y + 10, lbl, text_col, 0, true);
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

    /* 2.5 Selection Highlight Framing (Unobtrusive outline & corner brackets, NO black fill!) */
    if (game->has_selection) {
        const moon_tile_t *st = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
        int elev = st ? st->elevation : 0;
        int32_t ssx, ssy;
        moon_grid_to_screen(game->selected_tile_x, game->selected_tile_y, elev, game->cam_x, game->cam_y, width, height, &ssx, &ssy);

        int hw = MOON_TILE_HALF_W;
        int hh = MOON_TILE_HALF_H;

        /* If selected tile has a 2x2 habitat dome, frame full 2x2 footprint */
        if (st && st->structure_id != 0) {
            for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
                if (game->structs.list[i].id == st->structure_id && game->structs.list[i].type == MOON_STRUCT_HABITAT_DOME) {
                    hw = MOON_TILE_W;
                    hh = MOON_TILE_H;
                    ssx += MOON_TILE_HALF_W;
                    ssy += MOON_TILE_HALF_H / 2;
                    break;
                }
            }
        }

        /* Perimeter diamond outline */
        moon_blit_line(&surf, ssx, ssy - hh, ssx + hw, ssy, COLOR_LASER_CYAN);
        moon_blit_line(&surf, ssx + hw, ssy, ssx, ssy + hh, COLOR_LASER_CYAN);
        moon_blit_line(&surf, ssx, ssy + hh, ssx - hw, ssy, COLOR_LASER_CYAN);
        moon_blit_line(&surf, ssx - hw, ssy, ssx, ssy - hh, COLOR_LASER_CYAN);

        /* High-tech NASA gold surveyor corner brackets */
        int L = 6;
        moon_blit_line(&surf, ssx, ssy - hh, ssx + L * 2, ssy - hh + L, COLOR_GOLD_FOIL);
        moon_blit_line(&surf, ssx, ssy - hh, ssx - L * 2, ssy - hh + L, COLOR_GOLD_FOIL);
        moon_blit_line(&surf, ssx, ssy + hh, ssx + L * 2, ssy + hh - L, COLOR_GOLD_FOIL);
        moon_blit_line(&surf, ssx, ssy + hh, ssx - L * 2, ssy + hh - L, COLOR_GOLD_FOIL);
        moon_blit_line(&surf, ssx - hw, ssy, ssx - hw + L * 2, ssy - L, COLOR_GOLD_FOIL);
        moon_blit_line(&surf, ssx - hw, ssy, ssx - hw + L * 2, ssy + L, COLOR_GOLD_FOIL);
        moon_blit_line(&surf, ssx + hw, ssy, ssx + hw - L * 2, ssy - L, COLOR_GOLD_FOIL);
        moon_blit_line(&surf, ssx + hw, ssy, ssx + hw - L * 2, ssy + L, COLOR_GOLD_FOIL);
    }

    /* 3. Render all placed structures with live resources & status icons */
    moon_structure_render_all(&game->structs, &game->grid, &surf, &game->resources,
                              game->cam_x, game->cam_y);

    /* 4. Render structure placement ghost if build tool is active */
    if (game->structs.selected_build_type != MOON_STRUCT_NONE && game->hover_valid) {
        moon_structure_render_ghost(&game->structs, &game->grid, &surf,
                                    game->cam_x, game->cam_y,
                                    game->hover_tile_x, game->hover_tile_y);
    } else if (game->demolish_tool_active && game->hover_valid) {
        /* Render bulldozer / demolish target indicator */
        const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid, game->hover_tile_x, game->hover_tile_y);
        int elev = ht ? ht->elevation : 0;
        int32_t hsx, hsy;
        moon_grid_to_screen(game->hover_tile_x, game->hover_tile_y, elev, game->cam_x, game->cam_y, width, height, &hsx, &hsy);
        moon_blit_iso_tile(&surf, hsx, hsy, MOON_TILE_HALF_W, MOON_TILE_HALF_H, moon_color_dim(COLOR_ALERT_RED, 120), COLOR_ALERT_RED);
        moon_blit_line(&surf, hsx - 10, hsy - 5, hsx + 10, hsy + 5, COLOR_LUNAR_WHITE);
        moon_blit_line(&surf, hsx - 10, hsy + 5, hsx + 10, hsy - 5, COLOR_LUNAR_WHITE);
    }

    /* 5. Render Bottom-Right Minimap Radar */
    int mm_size = 64;
    int mm_x = width - mm_size - 10;
    int mm_y = height - mm_size - 44; /* Above build ribbon */
    moon_grid_render_minimap(&game->grid, &surf, mm_x, mm_y, mm_size, mm_size,
                             game->cam_x, game->cam_y);

    /* Render Terrain Texture Mode Toggle Button above Radar */
    uint16_t tex_border = game->grid.textured_terrain ? COLOR_LASER_CYAN : COLOR_REGOLITH_LIGHT;
    uint16_t tex_bg = game->grid.textured_terrain ? RGB565(20, 35, 45) : RGB565(35, 30, 20);
    const char *tex_lbl = game->grid.textured_terrain ? "TEX:ON" : "TEX:FAST";
    moon_blit_fill_rect(&surf, mm_x, mm_y - 20, mm_size, 16, tex_bg);
    moon_blit_rect_outline(&surf, mm_x, mm_y - 20, mm_size, 16, tex_border);
    moon_blit_text(&surf, mm_x + 2, mm_y - 20, tex_lbl, COLOR_LUNAR_WHITE, 0, true);

    /* 6. Render Bottom Build Ribbon */
    render_build_ribbon(game, &surf);

    /* 7. Render Top HUD Bar (Title, Day/Night Clock, Power, O2, H2O, Materials, Crew) */
    const moon_resources_t *res = &game->resources;
    int sun_pct = fixed_to_int(res->solar_intensity * 100);
    uint16_t pwr_col = res->power_brownout ? COLOR_ALERT_RED : COLOR_ENERGY_YELLOW;
    int batt_kwh = fixed_to_int(res->battery_stored_kwh);
    int rgl_kg = fixed_to_int(res->regolith_stored_kg);
    int o2_kg = fixed_to_int(res->o2_stored_kg);
    uint16_t o2_col = res->warning_o2 ? COLOR_ALERT_RED : COLOR_LASER_CYAN;
    int water_l = fixed_to_int(res->water_stored_l);
    uint16_t water_col = res->warning_water ? COLOR_ALERT_RED : COLOR_SOLAR_BLUE;
    uint16_t ls_col = (res->life_support_pct < 50) ? COLOR_ALERT_RED :
                      ((res->life_support_pct < 80) ? COLOR_ENERGY_YELLOW : COLOR_STATUS_GREEN);

    if (width < 800) {
        /* Compact HUD layout for 640x360 HiDPI */
        moon_blit_fill_rect(&surf, 0, 0, width, 24, RGB565(14, 16, 20));
        moon_blit_line(&surf, 0, 24, width - 1, 24, COLOR_GOLD_FOIL);

        if (!res->is_night) {
            moon_blit_text_fmt(&surf, 4, 4, COLOR_GOLD_FOIL, 0, true, "D%02d %d%%", res->lunar_day, sun_pct);
        } else {
            moon_blit_text_fmt(&surf, 4, 4, COLOR_LASER_CYAN, 0, true, "D%02d NGT", res->lunar_day);
        }

        moon_blit_text_fmt(&surf, 70, 4, pwr_col, 0, true, "P:%d/%d", res->power_produced_kw, res->power_consumed_kw);
        moon_blit_text_fmt(&surf, 165, 4, pwr_col, 0, true, "B:%d", batt_kwh);
        moon_blit_text_fmt(&surf, 240, 4, COLOR_REGOLITH_LIGHT, 0, true, "R:%d", rgl_kg);
        moon_blit_text_fmt(&surf, 320, 4, o2_col, 0, true, "O2:%d", o2_kg);
        moon_blit_text_fmt(&surf, 395, 4, water_col, 0, true, "W:%d", water_l);
        moon_blit_text_fmt(&surf, 465, 4, ls_col, 0, true, "CRW:%d/%d (%d%%)", res->population, res->max_population, res->life_support_pct);
    } else {
        /* Standard HUD layout for 1024x600 and 1280x720 */
        moon_blit_fill_rect(&surf, 0, 0, width, 32, RGB565(14, 16, 20));
        moon_blit_line(&surf, 0, 32, width - 1, 32, COLOR_GOLD_FOIL);

        if (!res->is_night) {
            moon_blit_text_fmt(&surf, 8, 8, COLOR_GOLD_FOIL, 0, true,
                               "D%02d SUN:%d%%", res->lunar_day, sun_pct);
        } else {
            moon_blit_text_fmt(&surf, 8, 8, COLOR_LASER_CYAN, 0, true,
                               "D%02d NIGHT", res->lunar_day);
        }

        moon_blit_fill_rect(&surf, 130, 10, 6, 12, pwr_col);
        moon_blit_text_fmt(&surf, 140, 8, pwr_col, 0, true,
                           "PWR %d/%dkW BATT %d/%dkWh",
                           res->power_produced_kw, res->power_consumed_kw,
                           batt_kwh, res->battery_capacity_kwh);

        moon_blit_fill_rect(&surf, 380, 10, 6, 12, COLOR_REGOLITH_LIGHT);
        moon_blit_text_fmt(&surf, 390, 8, COLOR_REGOLITH_LIGHT, 0, true,
                           "RGL %dkg", rgl_kg);

        moon_blit_fill_rect(&surf, 480, 10, 6, 12, o2_col);
        moon_blit_text_fmt(&surf, 490, 8, o2_col, 0, true,
                           "O2 %dkg", o2_kg);

        moon_blit_fill_rect(&surf, 570, 10, 6, 12, water_col);
        moon_blit_text_fmt(&surf, 580, 8, water_col, 0, true,
                           "H2O %dL", water_l);

        moon_blit_fill_rect(&surf, 660, 10, 6, 12, ls_col);
        moon_blit_text_fmt(&surf, 670, 8, ls_col, 0, true,
                           "CREW %d/%d (LS:%d%%)",
                           res->population, res->max_population, res->life_support_pct);
    }

    /* Right HUD Info (Hover Tile Coordinates & Resource Inspector) */
    if (game->hover_valid) {
        const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid,
                                                         game->hover_tile_x,
                                                         game->hover_tile_y);
        const char *tname = ht ? get_terrain_name((moon_terrain_type_t)ht->terrain) : "VOID";
        int elev = ht ? ht->elevation : 0;
        int rval = ht ? ht->resource_val : 0;

        int badge_w = (width < 800) ? 170 : 210;
        int badge_x = width - badge_w - 8;
        int badge_y = (width < 800) ? 26 : 4;
        moon_blit_fill_rect(&surf, badge_x, badge_y, badge_w, 20, RGB565(26, 30, 36));
        moon_blit_rect_outline(&surf, badge_x, badge_y, badge_w, 20, COLOR_LASER_CYAN);

        moon_blit_text_fmt(&surf, badge_x + 4, badge_y + 2, COLOR_LASER_CYAN, 0, true,
                           "[%02d,%02d] %s E:%d R:%d%%",
                           game->hover_tile_x, game->hover_tile_y,
                           tname, elev, rval);
    }

    /* 8. Active Life Support / Crisis Warning Ticker Banner */
    int banner_y = (width < 800) ? 26 : 38;
    if (res->population == 0) {
        moon_blit_fill_rect(&surf, 8, banner_y, 430, 20, COLOR_ALERT_RED);
        moon_blit_text(&surf, 14, banner_y + 2, "[MISSION FAILED] ALL CREW LOST: COLONY ABANDONED", COLOR_LUNAR_WHITE, 0, true);
    } else if (res->warning_crew_lost) {
        moon_blit_fill_rect(&surf, 8, banner_y, 430, 20, COLOR_ALERT_RED);
        moon_blit_text(&surf, 14, banner_y + 2, "[CASUALTY] CREW MEMBER PERISHED - ZERO LIFE SUPPORT", COLOR_LUNAR_WHITE, 0, true);
    } else if (res->warning_cold) {
        moon_blit_fill_rect(&surf, 8, banner_y, 380, 20, COLOR_ALERT_RED);
        moon_blit_text(&surf, 14, banner_y + 2, "[CRITICAL] CRYOGENIC FREEZING: HEATING LOST", COLOR_LUNAR_WHITE, 0, true);
    } else if (res->warning_power) {
        moon_blit_fill_rect(&surf, 8, banner_y, 350, 20, RGB565(180, 50, 20));
        moon_blit_text(&surf, 14, banner_y + 2, "[ALERT] POWER BLACKOUT: BATTERIES DEPLETED", COLOR_LUNAR_WHITE, 0, true);
    } else if (res->warning_o2) {
        moon_blit_fill_rect(&surf, 8, banner_y, 320, 20, COLOR_ALERT_RED);
        moon_blit_text(&surf, 14, banner_y + 2, "[CRITICAL] O2 STARVATION: RESERVES EMPTY", COLOR_LUNAR_WHITE, 0, true);
    }

    /* 9. Live Notification Banner (Terrain toggle, etc.) */
    if (game->notify_timer_ms > 0 && game->notify_msg) {
        int len = 0;
        const char *p = game->notify_msg;
        while (*p++) len++;
        int nw = len * 8 + 16;
        int nx = (width - nw) / 2;
        int ny = (width < 800) ? 28 : 38;
        moon_blit_fill_rect(&surf, nx, ny, nw, 22, RGB565(20, 25, 35));
        moon_blit_rect_outline(&surf, nx, ny, nw, 22, COLOR_GOLD_FOIL);
        moon_blit_text(&surf, nx + 8, ny + 3, game->notify_msg, COLOR_GOLD_FOIL, 0, true);
    }

    /* 10. Interactive Building & Terrain Inspector Panel */
    render_inspector_panel(game, &surf);
}
