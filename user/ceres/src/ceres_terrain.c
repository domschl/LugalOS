/* user/ceres/src/ceres_terrain.c — Streaming Cavern Heightfield & Ring Buffer */

#include "ceres_terrain.h"
#include "ceres_textures.h"
#include <string.h>

#define MIN_CORRIDOR_CLEARANCE  110   /* Minimum vertical gap (pixels) in tight caverns */
#define DEFAULT_FLOOR_Y         320
#define DEFAULT_CEIL_Y          CERES_HUD_HEIGHT

/* LCG Pseudo-random generator for procedural terrain (deterministic) */
static inline uint32_t terrain_rand(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static inline int rand_range(uint32_t *state, int min_v, int max_v) {
    if (min_v >= max_v) return min_v;
    uint32_t r = terrain_rand(state);
    return min_v + (int)(r % (uint32_t)(max_v - min_v + 1));
}

static void init_stars(ceres_terrain_t *t) {
    uint32_t r = t->seed ? t->seed : 0x19810425;
    for (int i = 0; i < CERES_PARALLAX_STARS; i++) {
        t->stars[i].x = (int16_t)(terrain_rand(&r) % CERES_VIRTUAL_W);
        t->stars[i].y = (int16_t)(CERES_HUD_HEIGHT + 4 + (terrain_rand(&r) % (CERES_VIRTUAL_H - CERES_HUD_HEIGHT - 32)));

        if (i < 24) {
            /* Layer 1: Distant dim stars (0.5 px/frame) */
            t->stars[i].speed_fp = 0x08;
            t->stars[i].color = COLOR_STAR_DIM;
        } else if (i < 48) {
            /* Layer 2: Mid bright stars (1.0 px/frame) */
            t->stars[i].speed_fp = 0x10;
            t->stars[i].color = COLOR_STAR_WHITE;
        } else {
            /* Layer 3: Cosmic blue/cyan dust (1.5 px/frame) */
            t->stars[i].speed_fp = 0x18;
            t->stars[i].color = COLOR_STAR_BLUE;
        }
    }
}

/* Pick next terrain feature segment */
static void pick_next_segment(ceres_terrain_t *t) {
    t->cur_floor_y = t->target_floor_y;
    t->cur_ceil_y = t->target_ceil_y;
    t->segment_step = 0;

    int r_type = rand_range(&t->seed, 0, 100);

    if (t->sector == 1) {
        /* Sector 1: Mountain Approach (Ceiling retracted to HUD) */
        t->target_ceil_y = CERES_HUD_HEIGHT;

        if (r_type < 40) {
            /* Flat Plain / Plateau */
            t->segment_type = CERES_COL_FLAT;
            t->segment_len = rand_range(&t->seed, 50, 80);
            t->target_floor_y = rand_range(&t->seed, 312, 330);
        } else if (r_type < 70) {
            /* Mountain Ridge with Summit Mesa */
            t->segment_type = CERES_COL_FLAT;
            t->segment_len = rand_range(&t->seed, 50, 75);
            t->target_floor_y = rand_range(&t->seed, 210, 255);
        } else {
            /* Crater Basin with Flat Floor */
            t->segment_type = CERES_COL_CRATER | CERES_COL_FLAT;
            t->segment_len = rand_range(&t->seed, 48, 72);
            t->target_floor_y = rand_range(&t->seed, 335, 348);
        }
    } else if (t->sector == 2) {
        /* Sector 2: Crystal Cavern (Active ceiling and floor stalactites) */
        t->segment_type = CERES_COL_CAVERN | CERES_COL_FLAT;
        t->segment_len = rand_range(&t->seed, 48, 70);

        if (r_type < 35) {
            /* Flat cavern floor section */
            t->target_floor_y = rand_range(&t->seed, 290, 320);
            t->target_ceil_y = rand_range(&t->seed, 60, 90);
        } else if (r_type < 70) {
            /* Narrowing Chasm / Squeeze */
            t->target_floor_y = rand_range(&t->seed, 250, 280);
            t->target_ceil_y = rand_range(&t->seed, 90, 120);
        } else {
            /* Wide cavern chamber */
            t->target_floor_y = rand_range(&t->seed, 320, 344);
            t->target_ceil_y = rand_range(&t->seed, 48, 75);
        }
    } else if (t->sector == 3) {
        /* Sector 3: Asteroid Trench (Jagged, steep undulating trench) */
        t->segment_type = CERES_COL_CAVERN;
        t->segment_len = rand_range(&t->seed, 36, 54);
        if (r_type < 40) {
            t->target_floor_y = rand_range(&t->seed, 275, 305);
            t->target_ceil_y = rand_range(&t->seed, 75, 105);
        } else {
            t->target_floor_y = rand_range(&t->seed, 295, 330);
            t->target_ceil_y = rand_range(&t->seed, 55, 85);
        }
    } else if (t->sector == 4) {
        /* Sector 4: Fortified Base (Level platforms for generators and laser barriers) */
        t->segment_type = CERES_COL_CAVERN | CERES_COL_FLAT;
        t->segment_len = rand_range(&t->seed, 60, 85);
        if (r_type < 60) {
            /* Base interior hangar corridor */
            t->target_floor_y = rand_range(&t->seed, 305, 325);
            t->target_ceil_y = rand_range(&t->seed, 55, 75);
        } else {
            /* Elevated conduit platform */
            t->target_floor_y = rand_range(&t->seed, 260, 285);
            t->target_ceil_y = rand_range(&t->seed, 80, 100);
        }
    } else if (t->sector == 5) {
        /* Sector 5: The Labyrinth (Winding, steep climbs and tight clearance) */
        t->segment_type = CERES_COL_CAVERN;
        t->segment_len = rand_range(&t->seed, 40, 60);
        if (r_type < 45) {
            /* Steep ascent shaft */
            t->target_floor_y = rand_range(&t->seed, 230, 260);
            t->target_ceil_y = rand_range(&t->seed, 48, 70);
        } else if (r_type < 80) {
            /* Steep descent shaft */
            t->target_floor_y = rand_range(&t->seed, 315, 342);
            t->target_ceil_y = rand_range(&t->seed, 120, 145);
        } else {
            /* Tight undulating corridor */
            t->target_floor_y = rand_range(&t->seed, 270, 295);
            t->target_ceil_y = rand_range(&t->seed, 85, 110);
        }
    } else {
        /* Sector 6: Reactor Stronghold (Corridor leading into final reactor hall) */
        t->segment_type = CERES_COL_CAVERN | CERES_COL_FLAT;
        t->segment_len = rand_range(&t->seed, 55, 80);
        if (r_type < 70) {
            /* Reinforced reactor facility corridor */
            t->target_floor_y = rand_range(&t->seed, 300, 325);
            t->target_ceil_y = rand_range(&t->seed, 55, 80);
        } else {
            /* Grand reactor containment hall */
            t->target_floor_y = 330;
            t->target_ceil_y = 50;
        }
    }

    /* Enforce minimum corridor clearance */
    if (t->target_floor_y - t->target_ceil_y < MIN_CORRIDOR_CLEARANCE) {
        t->target_floor_y = t->target_ceil_y + MIN_CORRIDOR_CLEARANCE;
        if (t->target_floor_y > CERES_VIRTUAL_H - 12) {
            t->target_floor_y = CERES_VIRTUAL_H - 12;
            t->target_ceil_y = t->target_floor_y - MIN_CORRIDOR_CLEARANCE;
        }
    }
}

/* Generate a single world column */
static void generate_next_column(ceres_terrain_t *t) {
    if (t->segment_step >= t->segment_len) {
        pick_next_segment(t);
    }

    uint32_t col_idx = t->generated_col_end;
    uint32_t ring_idx = col_idx & CERES_RING_MASK;
    ceres_column_t *col = &t->ring[ring_idx];

    if (t->segment_type & CERES_COL_FLAT) {
        /* Flat Plateau/Floor: Transition in first 16 columns, then hold strictly level */
        int trans_len = (t->segment_len > 32) ? 16 : (t->segment_len / 2);
        if (t->segment_step < trans_len) {
            uint8_t angle = (uint8_t)((t->segment_step * 128) / trans_len);
            fixed32_t c = fixed_cos(angle);
            fixed32_t weight = (FIXED_ONE - c) / 2;
            int floor_diff = t->target_floor_y - t->cur_floor_y;
            col->floor_y = (uint16_t)(t->cur_floor_y + fixed_to_int(floor_diff * weight));
            col->flags = t->segment_type & ~CERES_COL_FLAT; /* still transitioning */
        } else {
            col->floor_y = (uint16_t)t->target_floor_y;
            col->flags = t->segment_type | CERES_COL_FLAT;  /* 100% strictly level plateau */
        }

        /* Ceiling interpolation for caverns */
        if (t->cur_ceil_y != t->target_ceil_y) {
            int ceil_diff = t->target_ceil_y - t->cur_ceil_y;
            uint8_t c_angle = (uint8_t)((t->segment_step * 128) / trans_len);
            fixed32_t c_weight = (t->segment_step < trans_len)
                                     ? ((FIXED_ONE - fixed_cos(c_angle)) / 2)
                                     : FIXED_ONE;
            col->ceil_y = (uint16_t)(t->cur_ceil_y + fixed_to_int(ceil_diff * c_weight));
        } else {
            col->ceil_y = (uint16_t)t->target_ceil_y;
        }
    } else {
        /* Cosine S-curve interpolation between cur and target */
        uint8_t angle = (uint8_t)((t->segment_step * 128) / t->segment_len);
        fixed32_t c = fixed_cos(angle);
        fixed32_t weight = (FIXED_ONE - c) / 2;

        int floor_diff = t->target_floor_y - t->cur_floor_y;
        int ceil_diff = t->target_ceil_y - t->cur_ceil_y;

        col->floor_y = (uint16_t)(t->cur_floor_y + fixed_to_int(floor_diff * weight));
        col->ceil_y  = (uint16_t)(t->cur_ceil_y  + fixed_to_int(ceil_diff  * weight));
        col->flags   = t->segment_type;
    }
    col->strata  = (uint8_t)((col_idx >> 2) & 0x07);
    col->strata  = (uint8_t)((col_idx >> 2) & 0x07);

    t->segment_step++;
    t->generated_col_end++;
}

void ceres_terrain_init(ceres_terrain_t *t, int sector, uint32_t seed) {
    if (!t) return;
    memset(t, 0, sizeof(*t));

    /* Decompress seamless geological texture into caller-owned buffer */
    ceres_textures_unpack(t->tex_rock);

    t->sector = (sector > 0) ? sector : 1;
    t->seed = seed ? seed : 0x19810425;
    t->camera_x_fp = 0;
    t->camera_col = 0;

    t->cur_floor_y = DEFAULT_FLOOR_Y;
    t->target_floor_y = DEFAULT_FLOOR_Y;
    t->cur_ceil_y = DEFAULT_CEIL_Y;
    t->target_ceil_y = DEFAULT_CEIL_Y;
    t->segment_len = 32;
    t->segment_step = 0;
    t->segment_type = CERES_COL_FLAT;

    /* Pre-fill ring buffer with initial columns */
    for (uint32_t i = 0; i < CERES_RING_COLS; i++) {
        generate_next_column(t);
    }

    init_stars(t);
}

void ceres_terrain_advance(ceres_terrain_t *t, fixed32_t delta_x_fp) {
    if (!t) return;

    t->camera_x_fp += delta_x_fp;
    t->camera_col = (uint32_t)fixed_to_int(t->camera_x_fp);

    /* Generate new columns at the right edge ahead of the viewport */
    uint32_t target_col = t->camera_col + CERES_VIRTUAL_W + 64;
    while (t->generated_col_end < target_col) {
        generate_next_column(t);
    }

    /* Update parallax star positions */
    int dx_int = fixed_to_int(delta_x_fp);
    if (dx_int < 1) dx_int = 1;

    for (int i = 0; i < CERES_PARALLAX_STARS; i++) {
        int shift = (dx_int * (int)t->stars[i].speed_fp) >> 4;
        if (shift < 1 && (t->camera_col & 1)) shift = 1;
        t->stars[i].x -= (int16_t)shift;
        if (t->stars[i].x < 0) {
            t->stars[i].x += (int16_t)CERES_VIRTUAL_W;
        }
    }
}

void ceres_terrain_render(const ceres_terrain_t *t, const ceres_surface_t *surf, int view_w, int view_h) {
    if (!t || !surf || !surf->pixels) return;

    int render_w = (view_w < surf->width) ? view_w : surf->width;
    int render_h = (view_h < surf->height) ? view_h : surf->height;
    int stride = surf->stride;

    if (render_w <= 0 || render_h <= CERES_HUD_HEIGHT) return;
    if (render_w > CERES_PRESET_TAB5_W) render_w = CERES_PRESET_TAB5_W;

    uint16_t ceils[CERES_PRESET_TAB5_W];
    uint16_t floors[CERES_PRESET_TAB5_W];

    /* 1. Fast column bounds pre-sampling (contiguous linear access to ring buffer) */
    for (int x = 0; x < render_w; x++) {
        uint32_t world_col = t->camera_col + (uint32_t)x;
        const ceres_column_t *col = &t->ring[world_col & CERES_RING_MASK];

        int cy = (int)col->ceil_y;
        int fy = (int)col->floor_y;
        if (cy < CERES_HUD_HEIGHT) cy = CERES_HUD_HEIGHT;
        if (fy > render_h) fy = render_h;
        if (fy < cy) fy = cy;

        ceils[x] = (uint16_t)cy;
        floors[x] = (uint16_t)fy;
    }

    /* 2. Cache-Friendly Row-Major Rasterization (consecutive 16-bit stores prevent PSRAM thrashing) */
    uint32_t tu_base = t->camera_col & (TEX_ROCK_W - 1);

    for (int y = CERES_HUD_HEIGHT; y < render_h; y++) {
        uint16_t *line = surf->pixels + (size_t)y * stride;
        uint32_t tv = (uint32_t)y & (TEX_ROCK_H - 1);
        const uint16_t *tex_row = &t->tex_rock[tv * TEX_ROCK_W];

        for (int x = 0; x < render_w; x++) {
            int cy = (int)ceils[x];
            int fy = (int)floors[x];

            if (y < cy) {
                if (y == cy - 1) {
                    line[x] = COLOR_ROCK_EDGE;
                } else if (y == cy - 2) {
                    line[x] = COLOR_ROCK_LIGHT;
                } else {
                    line[x] = tex_row[(tu_base + (uint32_t)x) & (TEX_ROCK_W - 1)];
                }
            } else if (y < fy) {
                line[x] = COLOR_SPACE_BLACK;
            } else {
                if (y == fy) {
                    line[x] = COLOR_ROCK_EDGE;
                } else if (y == fy + 1) {
                    line[x] = COLOR_ROCK_LIGHT;
                } else {
                    line[x] = tex_row[(tu_base + (uint32_t)x) & (TEX_ROCK_W - 1)];
                }
            }
        }
    }

    /* 3. Parallax Starfield Overlay (Occluded by Rock Geometry) */
    for (int i = 0; i < CERES_PARALLAX_STARS; i++) {
        int sx = t->stars[i].x;
        int sy = t->stars[i].y;
        if (sx < 0 || sx >= render_w || sy < CERES_HUD_HEIGHT || sy >= render_h) continue;

        /* Only render star if inside the open flight space */
        if (sy >= (int)ceils[sx] && sy < (int)floors[sx]) {
            surf->pixels[(size_t)sy * stride + sx] = t->stars[i].color;
        }
    }
}

bool ceres_terrain_sample(const ceres_terrain_t *t, int screen_x, int *out_ceil_y, int *out_floor_y) {
    if (!t || screen_x < 0 || screen_x >= CERES_VIRTUAL_W) return false;

    uint32_t world_col = t->camera_col + (uint32_t)screen_x;
    const ceres_column_t *col = &t->ring[world_col & CERES_RING_MASK];

    if (out_ceil_y)  *out_ceil_y = (int)col->ceil_y;
    if (out_floor_y) *out_floor_y = (int)col->floor_y;
    return true;
}

bool ceres_terrain_check_collision(const ceres_terrain_t *t, int x, int y, int w, int h) {
    if (!t) return false;

    int x0 = x;
    int x1 = x + w;
    if (x0 < 0) x0 = 0;
    if (x1 > CERES_VIRTUAL_W) x1 = CERES_VIRTUAL_W;

    for (int sx = x0; sx < x1; sx++) {
        uint32_t world_col = t->camera_col + (uint32_t)sx;
        const ceres_column_t *col = &t->ring[world_col & CERES_RING_MASK];

        /* Check collision with ceiling rock */
        if (y < (int)col->ceil_y) return true;

        /* Check collision with floor rock */
        if (y + h > (int)col->floor_y) return true;
    }
    return false;
}
