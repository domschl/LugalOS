#include "moon_game.h"
#include "moon_config.h"
#include "moon_fixed.h"

/* Simple integer clamp */
static inline int32_t clamp_i32(int32_t v, int32_t min_v, int32_t max_v) {
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

/* Fast flat-colored rectangle */
static void draw_rect(uint16_t *fb, int stride, int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    for (int cy = y; cy < y + h; cy++) {
        uint16_t *row = fb + cy * stride + x;
        for (int cx = 0; cx < w; cx++) {
            row[cx] = color;
        }
    }
}

/* Fast Bresenham line for RGB565 */
static void draw_line(uint16_t *fb, int stride, int x0, int y0, int x1, int y1, uint16_t color, int clip_w, int clip_h) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && x0 < clip_w && y0 >= 0 && y0 < clip_h) {
            fb[y0 * stride + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

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
    /* iso_x / (W/2) = gx - gy */
    /* iso_y / (H/2) = gx + gy */
    int64_t num_x = (int64_t)iso_x * FIXED_ONE / MOON_TILE_HALF_W;
    int64_t num_y = (int64_t)iso_y * FIXED_ONE / MOON_TILE_HALF_H;

    int64_t calc_gx = (num_y + num_x) / 2;
    int64_t calc_gy = (num_y - num_x) / 2;

    *gx = (int32_t)(calc_gx >> FIXED_SHIFT);
    *gy = (int32_t)(calc_gy >> FIXED_SHIFT);
}

/* Draw one isometric tile diamond */
static void draw_iso_tile(uint16_t *fb, int stride, int32_t sx, int32_t sy,
                          uint16_t fill_color, uint16_t border_color,
                          int clip_w, int clip_h) {
    /* Diamond vertices:
     * Top:    (sx, sy - HALF_H)
     * Right:  (sx + HALF_W, sy)
     * Bottom: (sx, sy + HALF_H)
     * Left:   (sx - HALF_W, sy)
     */
    int32_t top_x = sx, top_y = sy - MOON_TILE_HALF_H;
    int32_t right_x = sx + MOON_TILE_HALF_W, right_y = sy;
    int32_t bot_x = sx, bot_y = sy + MOON_TILE_HALF_H;
    int32_t left_x = sx - MOON_TILE_HALF_W, left_y = sy;

    /* Fill horizontal spans across diamond */
    for (int y = -MOON_TILE_HALF_H; y <= MOON_TILE_HALF_H; y++) {
        int py = sy + y;
        if (py < 0 || py >= clip_h) continue;

        int span = (y <= 0) ? (MOON_TILE_HALF_H + y) : (MOON_TILE_HALF_H - y);
        int half_span_x = span * 2; /* 2:1 slope */

        int lx = sx - half_span_x;
        int rx = sx + half_span_x;
        if (lx < 0) lx = 0;
        if (rx >= clip_w) rx = clip_w - 1;

        if (lx <= rx) {
            uint16_t *row = fb + py * stride;
            for (int x = lx; x <= rx; x++) {
                row[x] = fill_color;
            }
        }
    }

    /* Outline borders */
    draw_line(fb, stride, top_x, top_y, right_x, right_y, border_color, clip_w, clip_h);
    draw_line(fb, stride, right_x, right_y, bot_x, bot_y, border_color, clip_w, clip_h);
    draw_line(fb, stride, bot_x, bot_y, left_x, left_y, border_color, clip_w, clip_h);
    draw_line(fb, stride, left_x, left_y, top_x, top_y, border_color, clip_w, clip_h);
}

bool moon_game_init(moon_game_t *game, int width, int height) {
    if (!game) return false;

    game->state = MOON_STATE_PLAYING;
    game->running = true;
    game->screen_w = width;
    game->screen_h = height;

    /* Center camera on initial base site (tile 32, 32) */
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
                    game->cam_y -= 16;
                    break;
                case MOON_KEY_DOWN:
                case MOON_KEY_S:
                    game->cam_y += 16;
                    break;
                case MOON_KEY_LEFT:
                case MOON_KEY_A:
                    game->cam_x -= 16;
                    break;
                case MOON_KEY_RIGHT:
                case MOON_KEY_D:
                    game->cam_x += 16;
                    break;
                case MOON_KEY_SPACE:
                    /* Reset camera to center */
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
            game->cam_x -= (int32_t)ev->wheel_x * 24;
            game->cam_y -= (int32_t)ev->wheel_y * 24;
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
        /* Future: resource tick & simulation updates */
    }
}

void moon_game_render(moon_game_t *game, uint16_t *fb, int width, int height, int stride) {
    if (!game || !fb) return;

    /* 1. Clear background to lunar deep space */
    for (int y = 0; y < height; y++) {
        uint16_t *row = fb + y * stride;
        for (int x = 0; x < width; x++) {
            row[x] = COLOR_SPACE_BLACK;
        }
    }

    /* 2. Render visible isometric terrain tiles */
    int32_t view_left = 0;
    int32_t view_top = 32; /* Top HUD bar reserve */
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
            if (game->hover_valid && gx == game->hover_tile_x && gy == game->hover_tile_y) {
                fill_color = COLOR_LASER_CYAN;
            }

            /* Draw the tile */
            draw_iso_tile(fb, stride, sx, sy, fill_color, COLOR_REGOLITH_LIGHT, width, height);
        }
    }

    /* 3. Render Top HUD Bar (Title, Coordinates, Resources) */
    draw_rect(fb, stride, 0, 0, width, 28, RGB565(18, 20, 24));
    draw_line(fb, stride, 0, 28, width - 1, 28, COLOR_GOLD_FOIL, width, height);

    /* Indicator blocks representing resource status */
    /* Power (Yellow) */
    draw_rect(fb, stride, 16, 6, 16, 16, COLOR_ENERGY_YELLOW);
    /* Regolith (Gray) */
    draw_rect(fb, stride, 120, 6, 16, 16, COLOR_REGOLITH_LIGHT);
    /* Oxygen (Cyan) */
    draw_rect(fb, stride, 230, 6, 16, 16, COLOR_LASER_CYAN);
    /* Water (Blue) */
    draw_rect(fb, stride, 340, 6, 16, 16, COLOR_SOLAR_BLUE);
    /* Fuel (Red) */
    draw_rect(fb, stride, 450, 6, 16, 16, COLOR_ALERT_RED);

    /* Hover tile coordinate badge */
    if (game->hover_valid) {
        draw_rect(fb, stride, width - 180, 4, 160, 20, RGB565(30, 35, 42));
    }
}
