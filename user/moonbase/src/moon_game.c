#include "moon_game.h"
#include "moon_config.h"
#include "moon_fixed.h"
#include "moon_blitter.h"

/* Coordinate transform: Grid (gx, gy) -> Screen (sx, sy) relative to camera */
static void grid_to_screen(int32_t gx, int32_t gy, int32_t cam_x, int32_t cam_y,
                           int32_t screen_w, int32_t screen_h,
                           int32_t *sx, int32_t *sy) {
    /* Isometric 2:1 projection */
    int32_t iso_x = (gx - gy) * MOON_TILE_HALF_W;
    int32_t iso_y = (gx + gy) * MOON_TILE_HALF_H;

    *sx = iso_x - cam_x + (screen_w / 2);
    *sy = iso_y - cam_y + (screen_h / 2);
}

/* Coordinate transform: Screen (sx, sy) -> Grid (gx, gy) */
static void screen_to_grid(int32_t sx, int32_t sy, int32_t cam_x, int32_t cam_y,
                           int32_t screen_w, int32_t screen_h,
                           int32_t *gx, int32_t *gy) {
    int32_t iso_x = sx + cam_x - (screen_w / 2);
    int32_t iso_y = sy + cam_y - (screen_h / 2);

    /* Inverse 2:1 isometric formula using pure integer math */
    int64_t num_x = (int64_t)iso_x * FIXED_ONE / MOON_TILE_HALF_W;
    int64_t num_y = (int64_t)iso_y * FIXED_ONE / MOON_TILE_HALF_H;

    int64_t calc_gx = (num_y + num_x) / 2;
    int64_t calc_gy = (num_y - num_x) / 2;

    *gx = (int32_t)(calc_gx >> FIXED_SHIFT);
    *gy = (int32_t)(calc_gy >> FIXED_SHIFT);
}

bool moon_game_init(moon_game_t *game, int width, int height) {
    if (!game) return false;

    game->state = MOON_STATE_PLAYING;
    game->running = true;
    game->screen_w = width;
    game->screen_h = height;

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
                    break;
                case MOON_KEY_DOWN:
                case MOON_KEY_S:
                    game->cam_y += 24;
                    break;
                case MOON_KEY_LEFT:
                case MOON_KEY_A:
                    game->cam_x -= 24;
                    break;
                case MOON_KEY_RIGHT:
                case MOON_KEY_D:
                    game->cam_x += 24;
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
            }

            /* Update hover tile */
            screen_to_grid(ev->x, ev->y, game->cam_x, game->cam_y,
                           game->screen_w, game->screen_h,
                           &game->hover_tile_x, &game->hover_tile_y);

            game->hover_valid = (game->hover_tile_x >= 0 && game->hover_tile_x < MOON_MAP_SIZE_X &&
                                 game->hover_tile_y >= 0 && game->hover_tile_y < MOON_MAP_SIZE_Y);
            break;

        case MOON_EVENT_SCROLL:
            /* Panning with scroll wheel */
            game->cam_x -= (int32_t)ev->wheel_x * 32;
            game->cam_y -= (int32_t)ev->wheel_y * 32;
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

    /* 2. Render visible isometric terrain tiles */
    int32_t view_left = 0;
    int32_t view_top = 36; /* Top HUD bar reserve */
    int32_t view_right = width;
    int32_t view_bottom = height;

    /* Draw tiles in back-to-front order (diagonal wave) */
    for (int d = 0; d < MOON_MAP_SIZE_X + MOON_MAP_SIZE_Y - 1; d++) {
        for (int gx = 0; gx <= d; gx++) {
            int gy = d - gx;
            if (gx >= MOON_MAP_SIZE_X || gy >= MOON_MAP_SIZE_Y) continue;

            int32_t sx, sy;
            grid_to_screen(gx, gy, game->cam_x, game->cam_y, width, height, &sx, &sy);

            /* Viewport culling */
            if (sx + MOON_TILE_HALF_W < view_left || sx - MOON_TILE_HALF_W >= view_right ||
                sy + MOON_TILE_HALF_H < view_top  || sy - MOON_TILE_HALF_H >= view_bottom) {
                continue;
            }

            /* Procedural lunar landscape shades */
            uint16_t fill_color = ((gx + gy) & 1) ? COLOR_REGOLITH_MID : COLOR_REGOLITH_DARK;

            /* Checkered crater pattern simulation */
            if (((gx ^ gy) % 7) == 0) {
                fill_color = COLOR_CRATER_SHADOW;
            }

            /* Highlight hovered tile */
            bool is_hover = (game->hover_valid && gx == game->hover_tile_x && gy == game->hover_tile_y);
            if (is_hover) {
                fill_color = COLOR_LASER_CYAN;
            }

            /* Render Lander at tile (32, 32) as an extruded 3D isometric structure */
            if (gx == 32 && gy == 32) {
                moon_blit_iso_block(&surf, sx, sy,
                                    MOON_TILE_HALF_W, MOON_TILE_HALF_H, 20,
                                    COLOR_GOLD_FOIL,
                                    moon_color_dim(COLOR_GOLD_FOIL, 180),
                                    moon_color_dim(COLOR_GOLD_FOIL, 220),
                                    COLOR_LUNAR_WHITE);
            } else {
                /* Standard terrain tile */
                moon_blit_iso_tile(&surf, sx, sy,
                                   MOON_TILE_HALF_W, MOON_TILE_HALF_H,
                                   fill_color, COLOR_REGOLITH_LIGHT);
            }
        }
    }

    /* 3. Render Top HUD Bar (Title, Resources, Coordinates) */
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

    /* Right HUD Info (Hover Tile Coordinates) */
    if (game->hover_valid) {
        moon_blit_fill_rect(&surf, width - 180, 4, 170, 24, RGB565(26, 30, 36));
        moon_blit_rect_outline(&surf, width - 180, 4, 170, 24, COLOR_LASER_CYAN);
        moon_blit_text_fmt(&surf, width - 170, 8, COLOR_LASER_CYAN, 0, true,
                           "TILE: [%02d, %02d]", game->hover_tile_x, game->hover_tile_y);
    }
}
