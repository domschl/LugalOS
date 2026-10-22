#include "moon_grid.h"
#include <stdlib.h>

/* Deterministic Pseudo-Random Number Generator (Xorshift32) */
static inline uint32_t prng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline uint32_t prng_range(uint32_t *state, uint32_t min_v, uint32_t max_v) {
    if (min_v >= max_v) return min_v;
    return min_v + (prng_next(state) % (max_v - min_v + 1));
}

void moon_grid_generate(moon_grid_t *grid, uint32_t seed) {
    if (!grid) return;

    if (seed == 0) seed = 19690720; /* Apollo 11 touchdown seed */
    grid->seed = seed;
    uint32_t rng = seed;

    grid->lander_gx = 32;
    grid->lander_gy = 32;

    /* 1. Base Layer: Flat lunar regolith, elevation level 1 */
    for (int y = 0; y < MOON_MAP_SIZE_Y; y++) {
        for (int x = 0; x < MOON_MAP_SIZE_X; x++) {
            moon_tile_t *t = &grid->tiles[y][x];
            t->terrain = MOON_TERRAIN_REGOLITH_FLAT;
            t->elevation = 1;
            t->resource_val = (uint8_t)prng_range(&rng, 30, 60); /* Regolith mineral yield */
            t->flags = 0;
            t->structure_id = 0;
        }
    }

    /* 2. Impact Craters: 14 craters with rims and sunken floors */
    for (int i = 0; i < 14; i++) {
        int cx = (int)prng_range(&rng, 6, MOON_MAP_SIZE_X - 7);
        int cy = (int)prng_range(&rng, 6, MOON_MAP_SIZE_Y - 7);
        int radius = (int)prng_range(&rng, 2, 6);

        /* Avoid destroying the immediate landing zone */
        int dx_lander = cx - grid->lander_gx;
        int dy_lander = cy - grid->lander_gy;
        if (dx_lander * dx_lander + dy_lander * dy_lander < 49) {
            continue; /* Keep landing apron clear */
        }

        int r2 = radius * radius;
        int rim_r2 = (radius + 1) * (radius + 1);

        for (int y = cy - radius - 1; y <= cy + radius + 1; y++) {
            if (y < 0 || y >= MOON_MAP_SIZE_Y) continue;
            for (int x = cx - radius - 1; x <= cx + radius + 1; x++) {
                if (x < 0 || x >= MOON_MAP_SIZE_X) continue;

                int dx = x - cx;
                int dy = y - cy;
                int d2 = dx * dx + dy * dy;

                moon_tile_t *t = &grid->tiles[y][x];

                if (d2 <= (radius - 1) * (radius - 1)) {
                    /* Crater basin floor (sunken) */
                    t->elevation = 0;

                    /* Polar ice trap: craters in northern quadrant (y < 24) hold water ice */
                    if (y < 24 && radius >= 3 && d2 <= 2) {
                        t->terrain = MOON_TERRAIN_ICE_DEPOSIT;
                        t->resource_val = (uint8_t)prng_range(&rng, 80, 100);
                        t->flags |= MOON_TILE_FLAG_SHADOWED;
                    } else {
                        t->terrain = MOON_TERRAIN_CRATER_FLOOR;
                        t->resource_val = 15;
                    }
                } else if (d2 <= r2) {
                    /* Crater inner slope */
                    t->elevation = 0;
                    t->terrain = MOON_TERRAIN_CRATER_FLOOR;
                } else if (d2 <= rim_r2) {
                    /* Elevated crater rim ridge */
                    t->elevation = 2;
                    t->terrain = MOON_TERRAIN_CRATER_RIM;
                    t->resource_val = 20;
                }
            }
        }
    }

    /* 3. Dark Basalt Maria (Volcanic plains with high Titanium/Ilmenite) */
    for (int m = 0; m < 3; m++) {
        int mx = (int)prng_range(&rng, 10, MOON_MAP_SIZE_X - 11);
        int my = (int)prng_range(&rng, 10, MOON_MAP_SIZE_Y - 11);
        int m_rad = (int)prng_range(&rng, 4, 8);

        for (int y = my - m_rad; y <= my + m_rad; y++) {
            if (y < 0 || y >= MOON_MAP_SIZE_Y) continue;
            for (int x = mx - m_rad; x <= mx + m_rad; x++) {
                if (x < 0 || x >= MOON_MAP_SIZE_X) continue;

                int dx = x - mx;
                int dy = y - my;
                if (dx * dx + dy * dy <= m_rad * m_rad) {
                    moon_tile_t *t = &grid->tiles[y][x];
                    if (t->terrain == MOON_TERRAIN_REGOLITH_FLAT) {
                        t->terrain = MOON_TERRAIN_BASALT_MARE;
                        t->resource_val = (uint8_t)prng_range(&rng, 70, 95); /* Rich in Ilmenite */
                    }
                }
            }
        }
    }

    /* 4. Rough Regolith & Boulder fields */
    for (int y = 0; y < MOON_MAP_SIZE_Y; y++) {
        for (int x = 0; x < MOON_MAP_SIZE_X; x++) {
            moon_tile_t *t = &grid->tiles[y][x];
            if (t->terrain == MOON_TERRAIN_REGOLITH_FLAT) {
                if ((prng_next(&rng) % 100) < 12) {
                    t->terrain = MOON_TERRAIN_REGOLITH_ROUGH;
                    t->resource_val = (uint8_t)prng_range(&rng, 40, 75);
                }
            }
        }
    }

    /* 5. Guarantees for Expedition Landing Apron at (32, 32) */
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int gx = grid->lander_gx + dx;
            int gy = grid->lander_gy + dy;
            if (gx >= 0 && gx < MOON_MAP_SIZE_X && gy >= 0 && gy < MOON_MAP_SIZE_Y) {
                moon_tile_t *t = &grid->tiles[gy][gx];
                t->terrain = MOON_TERRAIN_REGOLITH_FLAT;
                t->elevation = 1;
                t->flags &= ~MOON_TILE_FLAG_OCCUPIED;
            }
        }
    }

    /* Landing tile itself is sintered and occupied by Lander */
    moon_tile_t *lander_tile = &grid->tiles[grid->lander_gy][grid->lander_gx];
    lander_tile->flags |= (MOON_TILE_FLAG_FOUNDATION | MOON_TILE_FLAG_OCCUPIED);
    lander_tile->structure_id = 1; /* Structure 1 = Expedition Lander */

    /* Ensure a reachable Polar Ice deposit exists near the landing site (approx 10 tiles away) */
    int ice_cx = grid->lander_gx - 6;
    int ice_cy = grid->lander_gy - 8;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int gx = ice_cx + dx;
            int gy = ice_cy + dy;
            if (gx >= 0 && gx < MOON_MAP_SIZE_X && gy >= 0 && gy < MOON_MAP_SIZE_Y) {
                moon_tile_t *t = &grid->tiles[gy][gx];
                int d2 = dx * dx + dy * dy;
                if (d2 <= 1) {
                    t->terrain = MOON_TERRAIN_ICE_DEPOSIT;
                    t->elevation = 0;
                    t->resource_val = 90;
                    t->flags |= MOON_TILE_FLAG_SHADOWED;
                } else if (d2 <= 4) {
                    t->terrain = MOON_TERRAIN_CRATER_FLOOR;
                    t->elevation = 0;
                } else {
                    t->terrain = MOON_TERRAIN_CRATER_RIM;
                    t->elevation = 2;
                }
            }
        }
    }
}

moon_tile_t* moon_grid_get_tile(moon_grid_t *grid, int gx, int gy) {
    if (!grid || gx < 0 || gx >= MOON_MAP_SIZE_X || gy < 0 || gy >= MOON_MAP_SIZE_Y) {
        return NULL;
    }
    return &grid->tiles[gy][gx];
}

const moon_tile_t* moon_grid_get_tile_const(const moon_grid_t *grid, int gx, int gy) {
    if (!grid || gx < 0 || gx >= MOON_MAP_SIZE_X || gy < 0 || gy >= MOON_MAP_SIZE_Y) {
        return NULL;
    }
    return &grid->tiles[gy][gx];
}

void moon_grid_to_screen(int32_t gx, int32_t gy, int32_t elevation,
                         int32_t cam_x, int32_t cam_y,
                         int32_t screen_w, int32_t screen_h,
                         int32_t *sx, int32_t *sy) {
    int32_t iso_x = (gx - gy) * MOON_TILE_HALF_W;
    int32_t iso_y = (gx + gy) * MOON_TILE_HALF_H - (elevation * MOON_ELEVATION_STEP_PX);

    *sx = iso_x - cam_x + (screen_w / 2);
    *sy = iso_y - cam_y + (screen_h / 2);
}

void moon_screen_to_grid(int32_t sx, int32_t sy,
                         int32_t cam_x, int32_t cam_y,
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

static void get_terrain_colors(moon_terrain_type_t type,
                               uint16_t *top_color, uint16_t *left_color,
                               uint16_t *right_color, uint16_t *border_color) {
    switch (type) {
        case MOON_TERRAIN_REGOLITH_FLAT:
            *top_color    = RGB565(88, 88, 92);
            *left_color   = RGB565(58, 58, 62);
            *right_color  = RGB565(72, 72, 76);
            *border_color = RGB565(115, 115, 120);
            break;

        case MOON_TERRAIN_REGOLITH_ROUGH:
            *top_color    = RGB565(74, 72, 78);
            *left_color   = RGB565(48, 46, 52);
            *right_color  = RGB565(60, 58, 64);
            *border_color = RGB565(98, 95, 102);
            break;

        case MOON_TERRAIN_BASALT_MARE:
            *top_color    = RGB565(40, 42, 46);
            *left_color   = RGB565(24, 25, 28);
            *right_color  = RGB565(32, 34, 38);
            *border_color = RGB565(58, 60, 66);
            break;

        case MOON_TERRAIN_CRATER_RIM:
            *top_color    = RGB565(135, 135, 140);
            *left_color   = RGB565(95, 95, 100);
            *right_color  = RGB565(115, 115, 120);
            *border_color = RGB565(165, 165, 170);
            break;

        case MOON_TERRAIN_CRATER_FLOOR:
            *top_color    = RGB565(22, 22, 26);
            *left_color   = RGB565(14, 14, 18);
            *right_color  = RGB565(18, 18, 22);
            *border_color = RGB565(38, 38, 44);
            break;

        case MOON_TERRAIN_ICE_DEPOSIT:
            *top_color    = RGB565(90, 165, 215);
            *left_color   = RGB565(55, 110, 150);
            *right_color  = RGB565(75, 135, 180);
            *border_color = RGB565(160, 220, 255);
            break;

        default:
            *top_color    = RGB565(80, 80, 80);
            *left_color   = RGB565(50, 50, 50);
            *right_color  = RGB565(65, 65, 65);
            *border_color = RGB565(100, 100, 100);
            break;
    }
}

void moon_grid_render(const moon_grid_t *grid, const moon_surface_t *surf,
                      int32_t cam_x, int32_t cam_y,
                      int32_t hover_gx, int32_t hover_gy, bool hover_valid) {
    if (!grid || !surf) return;

    int32_t view_left = 0;
    int32_t view_top = 34; /* Reserve top HUD */
    int32_t view_right = surf->width;
    int32_t view_bottom = surf->height;

    /* Render tiles diagonal wave: back to front order */
    for (int d = 0; d < MOON_MAP_SIZE_X + MOON_MAP_SIZE_Y - 1; d++) {
        for (int gx = 0; gx <= d; gx++) {
            int gy = d - gx;
            if (gx >= MOON_MAP_SIZE_X || gy >= MOON_MAP_SIZE_Y) continue;

            const moon_tile_t *t = &grid->tiles[gy][gx];

            /* Calculate base screen coordinate (elevation = 0) */
            int32_t base_sx, base_sy;
            moon_grid_to_screen(gx, gy, 0, cam_x, cam_y, surf->width, surf->height,
                                &base_sx, &base_sy);

            int height_px = t->elevation * MOON_ELEVATION_STEP_PX;

            /* Viewport culling */
            if (base_sx + MOON_TILE_HALF_W < view_left ||
                base_sx - MOON_TILE_HALF_W >= view_right ||
                base_sy + MOON_TILE_HALF_H < view_top ||
                base_sy - height_px - MOON_TILE_HALF_H >= view_bottom) {
                continue;
            }

            uint16_t top_col, left_col, right_col, border_col;
            get_terrain_colors((moon_terrain_type_t)t->terrain,
                               &top_col, &left_col, &right_col, &border_col);

            /* Checkered regolith subtle variation */
            if (t->terrain == MOON_TERRAIN_REGOLITH_FLAT && ((gx + gy) & 1)) {
                top_col = moon_color_dim(top_col, 235);
            }

            /* Highlight hovered tile */
            bool is_hover = (hover_valid && gx == hover_gx && gy == hover_gy);
            if (is_hover) {
                top_col = COLOR_LASER_CYAN;
                border_col = COLOR_LUNAR_WHITE;
            }

            /* Draw 3D elevated block if elevation > 0, else flat diamond */
            if (height_px > 0) {
                moon_blit_iso_block(surf, base_sx, base_sy,
                                    MOON_TILE_HALF_W, MOON_TILE_HALF_H, height_px,
                                    top_col, left_col, right_col, border_col);
            } else {
                moon_blit_iso_tile(surf, base_sx, base_sy,
                                   MOON_TILE_HALF_W, MOON_TILE_HALF_H,
                                   top_col, border_col);
            }

            /* Render Expedition Lander at its starting tile */
            if (gx == grid->lander_gx && gy == grid->lander_gy) {
                int lander_sy = base_sy - height_px;
                /* Gold Foil 3D Lander module sitting on top */
                moon_blit_iso_block(surf, lander_sy ? base_sx : base_sx, lander_sy,
                                    MOON_TILE_HALF_W - 4, MOON_TILE_HALF_H - 2, 22,
                                    COLOR_GOLD_FOIL,
                                    moon_color_dim(COLOR_GOLD_FOIL, 175),
                                    moon_color_dim(COLOR_GOLD_FOIL, 220),
                                    COLOR_LUNAR_WHITE);
            }
        }
    }
}

void moon_grid_render_minimap(const moon_grid_t *grid, const moon_surface_t *surf,
                             int map_x, int map_y, int map_w, int map_h,
                             int32_t cam_x, int32_t cam_y) {
    if (!grid || !surf || map_w < MOON_MAP_SIZE_X || map_h < MOON_MAP_SIZE_Y) return;

    /* Background & frame */
    moon_blit_fill_rect(surf, map_x - 2, map_y - 2, map_w + 4, map_h + 4, RGB565(12, 14, 18));
    moon_blit_rect_outline(surf, map_x - 2, map_y - 2, map_w + 4, map_h + 4, COLOR_GOLD_FOIL);

    /* Render terrain pixels (1:1 mapping: 64x64) */
    for (int gy = 0; gy < MOON_MAP_SIZE_Y; gy++) {
        for (int gx = 0; gx < MOON_MAP_SIZE_X; gx++) {
            const moon_tile_t *t = &grid->tiles[gy][gx];
            uint16_t c;

            if (gx == grid->lander_gx && gy == grid->lander_gy) {
                c = COLOR_GOLD_FOIL;
            } else {
                switch (t->terrain) {
                    case MOON_TERRAIN_ICE_DEPOSIT:
                        c = COLOR_LASER_CYAN;
                        break;
                    case MOON_TERRAIN_BASALT_MARE:
                        c = RGB565(30, 32, 36);
                        break;
                    case MOON_TERRAIN_CRATER_RIM:
                        c = COLOR_LUNAR_WHITE;
                        break;
                    case MOON_TERRAIN_CRATER_FLOOR:
                        c = RGB565(15, 15, 18);
                        break;
                    case MOON_TERRAIN_REGOLITH_ROUGH:
                        c = RGB565(60, 58, 62);
                        break;
                    default:
                        c = COLOR_REGOLITH_MID;
                        break;
                }
            }
            moon_blit_pixel(surf, map_x + gx, map_y + gy, c);
        }
    }

    /* Camera viewport indicator box on minimap */
    /* Map center is tile (32, 32). In isometric coordinates: */
    /* Estimate visible tiles from cam_x and cam_y */
    int center_gx = 0, center_gy = 0;
    moon_screen_to_grid(surf->width / 2, surf->height / 2,
                        cam_x, cam_y, surf->width, surf->height,
                        &center_gx, &center_gy);

    int view_tile_span = (surf->width / MOON_TILE_W);
    int cam_box_x = map_x + center_gx - (view_tile_span / 2);
    int cam_box_y = map_y + center_gy - (view_tile_span / 4);
    int cam_box_w = view_tile_span;
    int cam_box_h = view_tile_span / 2;

    moon_blit_rect_outline(surf, cam_box_x, cam_box_y, cam_box_w, cam_box_h, COLOR_ENERGY_YELLOW);
}
