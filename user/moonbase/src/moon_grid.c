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
    grid->textured_terrain = true;
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
                t->resource_val = (uint8_t)prng_range(&rng, 30, 60); /* ordinary ground, not leftover mare or crater richness */
                t->flags &= ~MOON_TILE_FLAG_OCCUPIED;
            }
        }
    }

    /* Landing tile itself is sintered foundation pad ready for Lander */
    moon_tile_t *lander_tile = &grid->tiles[grid->lander_gy][grid->lander_gx];
    lander_tile->flags |= MOON_TILE_FLAG_FOUNDATION;
    lander_tile->structure_id = 0;

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

    /* A basalt mare patch just outside the apron, in a direction the seed picks: an
     * Extractor works beside mare as well as beside ice, so no map starts with the
     * first source of water and oxygen a long cable run away. */
    static const int8_t dirs[4][2] = { { 5, 4 }, { 4, -5 }, { -5, 4 }, { -4, -5 } };
    const int8_t *dir = dirs[prng_next(&rng) & 3];
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int gx = grid->lander_gx + dir[0] + dx;
            int gy = grid->lander_gy + dir[1] + dy;
            if (gx >= 0 && gx < MOON_MAP_SIZE_X && gy >= 0 && gy < MOON_MAP_SIZE_Y) {
                moon_tile_t *t = &grid->tiles[gy][gx];
                t->terrain = MOON_TERRAIN_BASALT_MARE;
                t->elevation = 1;
                t->resource_val = (uint8_t)prng_range(&rng, 70, 95);
                t->flags &= (uint8_t)~MOON_TILE_FLAG_SHADOWED;
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

    /* moon_grid_to_screen() yields the tile centre, so round to the nearest tile */
    *gx = (int32_t)((calc_gx + FIXED_ONE / 2) >> FIXED_SHIFT);
    *gy = (int32_t)((calc_gy + FIXED_ONE / 2) >> FIXED_SHIFT);
}

bool moon_grid_pick(const moon_grid_t *grid, int32_t sx, int32_t sy,
                    int32_t cam_x, int32_t cam_y,
                    int32_t screen_w, int32_t screen_h,
                    int32_t *gx, int32_t *gy) {
    /* Raised tiles are drawn shifted up by elevation * STEP, so test the highest
     * levels first against the screen point lowered by that amount. */
    if (grid) {
        for (int e = MOON_ELEVATION_MAX; e >= 1; e--) {
            int32_t cx, cy;
            moon_screen_to_grid(sx, sy + e * MOON_ELEVATION_STEP_PX, cam_x, cam_y,
                                screen_w, screen_h, &cx, &cy);
            const moon_tile_t *t = moon_grid_get_tile_const(grid, cx, cy);
            if (t && t->elevation == e) {
                *gx = cx; *gy = cy;
                return true;
            }
        }
    }
    moon_screen_to_grid(sx, sy, cam_x, cam_y, screen_w, screen_h, gx, gy);
    return *gx >= 0 && *gx < MOON_MAP_SIZE_X && *gy >= 0 && *gy < MOON_MAP_SIZE_Y;
}

void moon_grid_render_tile(const moon_grid_t *grid, const moon_surface_t *surf,
                           int gx, int gy, int32_t cam_x, int32_t cam_y,
                           int32_t view_top,
                           int32_t hover_gx, int32_t hover_gy, bool hover_valid) {
    if (!grid || !surf) return;
    if (gx < 0 || gx >= MOON_MAP_SIZE_X || gy < 0 || gy >= MOON_MAP_SIZE_Y) return;

    const moon_tile_t *t = &grid->tiles[gy][gx];

    /* Calculate base screen coordinate (elevation = 0) */
    int32_t base_sx, base_sy;
    moon_grid_to_screen(gx, gy, 0, cam_x, cam_y, surf->width, surf->height,
                        &base_sx, &base_sy);

    int height_px = t->elevation * MOON_ELEVATION_STEP_PX;

    /* Viewport culling */
    if (base_sx + MOON_TILE_HALF_W < 0 ||
        base_sx - MOON_TILE_HALF_W >= surf->width ||
        base_sy + MOON_TILE_HALF_H < view_top ||
        base_sy - height_px - MOON_TILE_HALF_H >= surf->height) {
        return;
    }

    /* Highlight hovered tile */
    if (hover_valid && gx == hover_gx && gy == hover_gy) {
        if (height_px > 0) {
            moon_blit_iso_block(surf, base_sx, base_sy,
                                MOON_TILE_HALF_W, MOON_TILE_HALF_H, height_px,
                                COLOR_LASER_CYAN,
                                moon_color_dim(COLOR_LASER_CYAN, 180),
                                moon_color_dim(COLOR_LASER_CYAN, 220),
                                COLOR_LUNAR_WHITE);
        } else {
            moon_blit_iso_tile(surf, base_sx, base_sy,
                               MOON_TILE_HALF_W, MOON_TILE_HALF_H,
                               COLOR_LASER_CYAN, COLOR_LUNAR_WHITE);
        }
        return;
    }

    uint16_t border_col = (t->flags & MOON_TILE_FLAG_FOUNDATION) ? COLOR_GOLD_FOIL : 0;
    uint16_t cliff_rim = (border_col != 0) ? border_col : RGB565(48, 48, 54);

    if (grid->textured_terrain) {
        /* Seamless textured rendering with geological cliff stratification */
        if (height_px > 0) {
            moon_blit_iso_block_textured(surf, base_sx, base_sy,
                                         MOON_TILE_HALF_W, MOON_TILE_HALF_H, height_px,
                                         gx, gy, t->terrain, t->flags, cliff_rim);
        } else {
            moon_blit_iso_tile_textured(surf, base_sx, base_sy,
                                        MOON_TILE_HALF_W, MOON_TILE_HALF_H,
                                        gx, gy, t->terrain, t->flags, border_col);
        }
    } else {
        /* High-performance flat-shaded retro terrain rendering */
        uint16_t top_col = (t->flags & MOON_TILE_FLAG_FOUNDATION)
                               ? RGB565(110, 110, 116) /* paver grey, as textured */
                               : moon_terrain_mean_color(t->terrain);
        if (height_px > 0) {
            moon_blit_iso_block(surf, base_sx, base_sy,
                                MOON_TILE_HALF_W, MOON_TILE_HALF_H, height_px,
                                top_col, moon_cliff_mean_color(t->terrain, 1),
                                moon_cliff_mean_color(t->terrain, 2), cliff_rim);
        } else {
            moon_blit_iso_tile(surf, base_sx, base_sy,
                               MOON_TILE_HALF_W, MOON_TILE_HALF_H,
                               top_col, border_col);
        }
    }
}

void moon_grid_render(const moon_grid_t *grid, const moon_surface_t *surf,
                      int32_t cam_x, int32_t cam_y,
                      int32_t hover_gx, int32_t hover_gy, bool hover_valid) {
    /* Diagonal wave: back to front */
    for (int d = 0; d < MOON_MAP_SIZE_X + MOON_MAP_SIZE_Y - 1; d++) {
        for (int gx = 0; gx <= d; gx++) {
            moon_grid_render_tile(grid, surf, gx, d - gx, cam_x, cam_y, 0,
                                  hover_gx, hover_gy, hover_valid);
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
            if (t->structure_id != 0) c = COLOR_STATUS_GREEN;
            moon_blit_pixel(surf, map_x + gx, map_y + gy, c);
        }
    }

    /* Viewport outline: the visible screen area (between the HUD bar and the
     * ribbon) is a diamond in tile space, one corner per screen corner. */
    const int32_t top = 34, bottom = surf->height - 40;
    const int32_t sxs[4] = { 0, surf->width, surf->width, 0 };
    const int32_t sys[4] = { top, top, bottom, bottom };
    int px[4], py[4];
    for (int i = 0; i < 4; i++) {
        int32_t gx, gy;
        moon_screen_to_grid(sxs[i], sys[i], cam_x, cam_y, surf->width, surf->height, &gx, &gy);
        if (gx < 0) gx = 0;
        if (gx > map_w - 1) gx = map_w - 1;
        if (gy < 0) gy = 0;
        if (gy > map_h - 1) gy = map_h - 1;
        px[i] = map_x + gx;
        py[i] = map_y + gy;
    }
    for (int i = 0; i < 4; i++) {
        moon_blit_line(surf, px[i], py[i], px[(i + 1) % 4], py[(i + 1) % 4], COLOR_ENERGY_YELLOW);
    }
}
