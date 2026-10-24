#ifndef MOON_GRID_H
#define MOON_GRID_H

#include <stdbool.h>
#include <stdint.h>
#include "moon_config.h"
#include "moon_blitter.h"

/* Lunar Terrain Types */
typedef enum {
    MOON_TERRAIN_REGOLITH_FLAT = 0,  /* Smooth, dusty lunar soil (ideal for building) */
    MOON_TERRAIN_REGOLITH_ROUGH,     /* Jagged rocks & boulders (high mineral yield) */
    MOON_TERRAIN_BASALT_MARE,        /* Dark volcanic basalt (rich in titanium & ilmenite) */
    MOON_TERRAIN_CRATER_RIM,         /* High elevated crater ridge */
    MOON_TERRAIN_CRATER_FLOOR,       /* Deep sunken crater floor (cold, shadowed) */
    MOON_TERRAIN_ICE_DEPOSIT,        /* Permanently shadowed polar ice pocket (source of H2O) */
    MOON_TERRAIN_COUNT
} moon_terrain_type_t;

/* Tile Flags */
#define MOON_TILE_FLAG_OCCUPIED     (1u << 0)  /* A structure occupies this tile */
#define MOON_TILE_FLAG_FOUNDATION   (1u << 1)  /* Sintered regolith pad */
#define MOON_TILE_FLAG_REVEALED     (1u << 2)  /* Explored / visible */
#define MOON_TILE_FLAG_SHADOWED     (1u << 3)  /* In permanent or crater shadow */

/* Elevation parameters */
#define MOON_ELEVATION_MAX          4
#define MOON_ELEVATION_STEP_PX      8   /* Pixels raised per elevation level */

/* Single Grid Tile Data */
typedef struct {
    uint8_t terrain;        /* moon_terrain_type_t */
    uint8_t elevation;      /* 0..MOON_ELEVATION_MAX */
    uint8_t resource_val;   /* Concentration of primary resource (0..100) */
    uint8_t flags;          /* Bitmask of MOON_TILE_FLAG_* */
    uint8_t structure_id;   /* ID of occupying building (0 = none) */
} moon_tile_t;

/* World Map Grid (64x64 tiles) */
typedef struct {
    moon_tile_t tiles[MOON_MAP_SIZE_Y][MOON_MAP_SIZE_X];
    uint32_t    seed;
    int32_t     lander_gx;  /* Default expedition landing site X */
    int32_t     lander_gy;  /* Default expedition landing site Y */
    bool        textured_terrain; /* true = textured, false = flat-shaded retro */
} moon_grid_t;

/* Procedural Generation */
void moon_grid_generate(moon_grid_t *grid, uint32_t seed);

/* Tile Access */
moon_tile_t* moon_grid_get_tile(moon_grid_t *grid, int gx, int gy);
const moon_tile_t* moon_grid_get_tile_const(const moon_grid_t *grid, int gx, int gy);

/* Coordinate Transformations */
void moon_grid_to_screen(int32_t gx, int32_t gy, int32_t elevation,
                         int32_t cam_x, int32_t cam_y,
                         int32_t screen_w, int32_t screen_h,
                         int32_t *sx, int32_t *sy);

void moon_screen_to_grid(int32_t sx, int32_t sy,
                         int32_t cam_x, int32_t cam_y,
                         int32_t screen_w, int32_t screen_h,
                         int32_t *gx, int32_t *gy);

/* Elevation-aware picking: tile under a screen point (true if inside the map) */
bool moon_grid_pick(const moon_grid_t *grid, int32_t sx, int32_t sy,
                    int32_t cam_x, int32_t cam_y,
                    int32_t screen_w, int32_t screen_h,
                    int32_t *gx, int32_t *gy);

/* Terrain Rendering */
void moon_grid_render_tile(const moon_grid_t *grid, const moon_surface_t *surf,
                           int gx, int gy, int32_t cam_x, int32_t cam_y,
                           int32_t view_top,   /* tiles entirely above this row are skipped (the HUD covers them) */
                           int32_t hover_gx, int32_t hover_gy, bool hover_valid);

void moon_grid_render(const moon_grid_t *grid, const moon_surface_t *surf,
                      int32_t cam_x, int32_t cam_y,
                      int32_t hover_gx, int32_t hover_gy, bool hover_valid);

/* Minimap Overview */
void moon_grid_render_minimap(const moon_grid_t *grid, const moon_surface_t *surf,
                             int map_x, int map_y, int map_w, int map_h,
                             int32_t cam_x, int32_t cam_y);

#endif /* MOON_GRID_H */
