/* user/ceres/include/ceres_terrain.h — Streaming Cavern Heightfield & Ring Buffer */
#ifndef CERES_TERRAIN_H
#define CERES_TERRAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "ceres_config.h"
#include "ceres_blitter.h"
#include "ceres_fixed.h"
#include "ceres_textures.h"

#define CERES_RING_COLS       2048
#define CERES_RING_MASK       (CERES_RING_COLS - 1)

#define CERES_HUD_HEIGHT      32
#define CERES_BOTTOM_MARGIN   8

/* Column Flags */
#define CERES_COL_FLAT        0x01  /* Flat terrain segment suitable for ground structures */
#define CERES_COL_CAVERN      0x02  /* Enclosed cavern ceiling active */
#define CERES_COL_CRATER      0x04  /* Crater basin depression */
#define CERES_COL_STRUCTURE   0x08  /* Ground structure present */

typedef struct {
    uint16_t ceil_y;   /* Absolute screen Y coordinate of ceiling edge (32 in open sky) */
    uint16_t floor_y;  /* Absolute screen Y coordinate of floor edge (180..352) */
    uint8_t  flags;    /* CERES_COL_* */
    uint8_t  strata;   /* Sedimentary strata pattern variation */
} ceres_column_t;

/* Multi-layer Parallax Starfield */
#define CERES_PARALLAX_STARS  64

typedef struct {
    int16_t  x;
    int16_t  y;
    uint8_t  speed_fp; /* Speed in 4.4 fixed point (e.g. 0x08 = 0.5 px, 0x10 = 1.0 px) */
    uint16_t color;
} ceres_parallax_star_t;

/* Streaming Terrain Context */
typedef struct {
    ceres_column_t        ring[CERES_RING_COLS];
    uint32_t                 generated_col_end;  /* World column index of next column to generate */
    fixed32_t                camera_x_fp;        /* Camera position in 16.16 fixed point */
    uint32_t                 camera_col;         /* Integer camera column (camera_x_fp >> 16) */
    int                      sector;             /* Current sector index (1..6) */
    uint32_t                 seed;
    int                      playfield_h;        /* Virtual playfield height in pixels */

    /* Generator State Machine */
    int                      cur_floor_y;
    int                      target_floor_y;
    int                      cur_ceil_y;
    int                      target_ceil_y;
    int                      segment_len;
    int                      segment_step;
    uint8_t                  segment_type;

    /* Parallax Starfield */
    ceres_parallax_star_t stars[CERES_PARALLAX_STARS];

    /* Seamless Geological Texture Buffer */
    uint16_t                 tex_rock[TEX_ROCK_PIXELS];
} ceres_terrain_t;

/* Lifecycle & Streaming API */
void ceres_terrain_init(ceres_terrain_t *t, int sector, uint32_t seed, int playfield_h);
void ceres_terrain_advance(ceres_terrain_t *t, fixed32_t delta_x_fp);
void ceres_terrain_render(const ceres_terrain_t *t, const ceres_surface_t *surf, int view_w, int view_h);

/* Terrain Geometry Sampling */
bool ceres_terrain_sample(const ceres_terrain_t *t, int screen_x, int *out_ceil_y, int *out_floor_y);
bool ceres_terrain_check_collision(const ceres_terrain_t *t, int x, int y, int w, int h);

#endif /* CERES_TERRAIN_H */
