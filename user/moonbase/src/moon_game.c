#include "moon_game.h"
#include "moon_config.h"
#include "moon_fixed.h"
#include "moon_blitter.h"
#include "moon_grid.h"

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

    /* Center camera on initial expedition landing site (tile 32, 32) */
    game->cam_x = 0;
    game->cam_y = 32 * MOON_TILE_H;

    game->pointer_x = 0;
    game->pointer_y = 0;
    game->pointer_down = false;
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

static void clamp_camera(moon_game_t *game) {
    if (game->cam_x < -1200) game->cam_x = -1200;
    if (game->cam_x > 1200)  game->cam_x = 1200;
    if (game->cam_y < 100)   game->cam_y = 100;
    if (game->cam_y > 2000)  game->cam_y = 2000;
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
                    game->running = false;
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
            game->pointer_x = ev->x;
            game->pointer_y = ev->y;
            game->drag_start_x = ev->x;
            game->drag_start_y = ev->y;
            game->cam_start_x = game->cam_x;
            game->cam_start_y = game->cam_y;
            break;

        case MOON_EVENT_POINTER_UP:
            game->pointer_down = false;
            break;

        case MOON_EVENT_POINTER_MOVE:
            game->pointer_x = ev->x;
            game->pointer_y = ev->y;
            if (game->pointer_down) {
                /* Drag-to-pan camera */
                int32_t dx = ev->x - game->drag_start_x;
                int32_t dy = ev->y - game->drag_start_y;
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
            /* Panning with scroll wheel */
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

    while (game->last_tick_ms >= MOON_SIM_TICK_MS) {
        game->last_tick_ms -= MOON_SIM_TICK_MS;
        game->sim_ticks++;
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

    /* 3. Render Bottom-Right Minimap */
    int mm_size = 64;
    int mm_x = width - mm_size - 10;
    int mm_y = height - mm_size - 10;
    moon_grid_render_minimap(&game->grid, &surf, mm_x, mm_y, mm_size, mm_size,
                             game->cam_x, game->cam_y);

    /* Minimap label */
    moon_blit_text(&surf, mm_x - 30, mm_y - 18, "RADAR", COLOR_GOLD_FOIL, 0, true);

    /* 4. Render Top HUD Bar (Title, Resources, Coordinates) */
    moon_blit_fill_rect(&surf, 0, 0, width, 32, RGB565(14, 16, 20));
    moon_blit_line(&surf, 0, 32, width - 1, 32, COLOR_GOLD_FOIL);

    /* Game Banner */
    moon_blit_text(&surf, 12, 8, "MOONBASE LAMBDA", COLOR_GOLD_FOIL, 0, true);

    /* Resource Indicators with numeric values */
    /* PWR (Yellow) */
    moon_blit_fill_rect(&surf, 160, 10, 8, 12, COLOR_ENERGY_YELLOW);
    moon_blit_text(&surf, 172, 8, "PWR 100%", COLOR_ENERGY_YELLOW, 0, true);

    /* RGL (Regolith, Gray) */
    moon_blit_fill_rect(&surf, 255, 10, 8, 12, COLOR_REGOLITH_LIGHT);
    moon_blit_text(&surf, 267, 8, "RGL 50t", COLOR_REGOLITH_LIGHT, 0, true);

    /* O2 (Oxygen, Cyan) */
    moon_blit_fill_rect(&surf, 345, 10, 8, 12, COLOR_LASER_CYAN);
    moon_blit_text(&surf, 357, 8, "O2 99%", COLOR_LASER_CYAN, 0, true);

    /* H2O (Water, Blue) */
    moon_blit_fill_rect(&surf, 435, 10, 8, 12, COLOR_SOLAR_BLUE);
    moon_blit_text(&surf, 447, 8, "H2O 1.2kL", COLOR_SOLAR_BLUE, 0, true);

    /* FUEL (Red) */
    moon_blit_fill_rect(&surf, 540, 10, 8, 12, COLOR_ALERT_RED);
    moon_blit_text(&surf, 552, 8, "FUEL 400kg", COLOR_ALERT_RED, 0, true);

    /* Right HUD Info (Hover Tile Coordinates & Resource Inspector) */
    if (game->hover_valid) {
        const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid,
                                                         game->hover_tile_x,
                                                         game->hover_tile_y);
        const char *tname = ht ? get_terrain_name((moon_terrain_type_t)ht->terrain) : "VOID";
        int elev = ht ? ht->elevation : 0;
        int rval = ht ? ht->resource_val : 0;

        int badge_w = 270;
        int badge_x = width - badge_w - 10;
        moon_blit_fill_rect(&surf, badge_x, 4, badge_w, 24, RGB565(26, 30, 36));
        moon_blit_rect_outline(&surf, badge_x, 4, badge_w, 24, COLOR_LASER_CYAN);

        moon_blit_text_fmt(&surf, badge_x + 8, 8, COLOR_LASER_CYAN, 0, true,
                           "[%02d,%02d] %s E:%d R:%d%%",
                           game->hover_tile_x, game->hover_tile_y,
                           tname, elev, rval);
    }
}
