#ifndef MOON_STRUCTURES_H
#define MOON_STRUCTURES_H

#include <stdbool.h>
#include <stdint.h>
#include "moon_config.h"
#include "moon_blitter.h"
#include "moon_grid.h"

/* Structure Types */
typedef enum {
    MOON_STRUCT_NONE = 0,
    MOON_STRUCT_LANDER,         /* Initial Expedition Lander & command base */
    MOON_STRUCT_SOLAR_ARRAY,    /* Photovoltaic field (25 kW day, 0 kW night) */
    MOON_STRUCT_BATTERY_BANK,   /* Regenerative fuel cell / storage (200 kWh) */
    MOON_STRUCT_HABITAT_DOME,   /* Pressurized 2x2 geodesic living module */
    MOON_STRUCT_REGOLITH_PLANT, /* Sintered block & mineral processor */
    MOON_STRUCT_O2_EXTRACTOR,   /* Polar ice electrolysis & ilmenite reduction */
    MOON_STRUCT_POWER_CONDUIT,  /* Surface power cable linking buildings */
    MOON_STRUCT_COUNT
} moon_structure_type_t;

/* Structure Definition Metadata (Static Catalog) */
typedef struct {
    uint8_t     type;
    const char *name;
    const char *short_name;
    uint8_t     footprint_w;    /* Width in tiles */
    uint8_t     footprint_h;    /* Height in tiles */
    uint16_t    cost_regolith;  /* Sintered blocks or raw minerals */
    int16_t     power_kw;       /* + produced (day), - consumed */
    uint16_t    power_storage;  /* Battery capacity (kWh) */
    int16_t     rate_o2;        /* + produced, - consumed (kg/day) */
    int16_t     rate_water;     /* + produced, - consumed (L/day) */
    uint16_t    primary_color;
    uint16_t    accent_color;
    uint8_t     height_px;      /* 3D isometric height extrusion */
    const char *desc;
} moon_structure_def_t;

/* Placed Structure Instance in World */
#define MOON_MAX_STRUCTURES 128

typedef struct {
    uint8_t  id;                /* 1..MOON_MAX_STRUCTURES (0 = empty) */
    uint8_t  type;              /* moon_structure_type_t */
    uint8_t  gx, gy;            /* Top-left grid anchor */
    uint8_t  footprint_w;
    uint8_t  footprint_h;
    bool     active;
    bool     powered;
    uint8_t  health;            /* 0..100% */
    uint16_t work;              /* Progress toward the next unit drawn from the ground */
    bool     depleted;          /* An Extractor or Sinterer with nothing left to draw beside it */
} moon_structure_t;

/* Structure Manager */
typedef struct {
    moon_structure_t list[MOON_MAX_STRUCTURES];
    uint8_t          count;
    uint8_t          selected_build_type; /* 0 = inspection mode, 1..COUNT = build tool */
    uint32_t         revision;            /* Bumped whenever the set or active state of structures changes */
    uint32_t         solved_revision;     /* Revision the power grid was last solved for */
} moon_structure_mgr_t;

/* Catalog Access */
const moon_structure_def_t* moon_structure_get_def(moon_structure_type_t type);

/* Manager Lifecycle */
void moon_structure_mgr_init(moon_structure_mgr_t *mgr);

/* Mark the structure set as changed so the power grid is re-solved on the next tick */
static inline void moon_structure_mark_dirty(moon_structure_mgr_t *mgr) { if (mgr) mgr->revision++; }

/* The ground a structure draws on, from the eight tiles around it: an Extractor takes
 * ice or mare (a deposit), a Sinterer quarries the regolith (any other terrain). The
 * stock of a tile is its resource_val. Other structure types draw on nothing. */
moon_tile_t* moon_structure_ore_tile(moon_grid_t *grid, moon_structure_type_t type, int gx, int gy);
int moon_structure_ore_left(const moon_grid_t *grid, moon_structure_type_t type, int gx, int gy);

/* Placement Queries & Actions */
bool moon_structure_can_place(const moon_grid_t *grid,
                              const moon_structure_mgr_t *mgr,
                              moon_structure_type_t type,
                              int gx, int gy,
                              const char **out_reason);

moon_structure_t* moon_structure_place(moon_grid_t *grid,
                                       moon_structure_mgr_t *mgr,
                                       moon_structure_type_t type,
                                       int gx, int gy);

void moon_structure_demolish(moon_grid_t *grid,
                             moon_structure_mgr_t *mgr,
                             int gx, int gy);

/* Forward declaration of resources */
struct moon_resources_s;

/* Rendering: terrain and sprites in one back-to-front pass, then the overlays
 * (badges, battery gauge, conduit junctions) above everything. They are separate
 * calls so that the caller can tint the world in between. */
void moon_world_render(const moon_structure_mgr_t *mgr,
                       const moon_grid_t *grid,
                       const moon_surface_t *surf,
                       int32_t cam_x, int32_t cam_y,
                       int32_t hover_gx, int32_t hover_gy, bool hover_valid);

void moon_world_render_overlays(const moon_structure_mgr_t *mgr,
                                const moon_grid_t *grid,
                                const moon_surface_t *surf,
                                const struct moon_resources_s *res,
                                int32_t cam_x, int32_t cam_y);

/* Front-most structure whose visible sprite pixels contain the screen point */
bool moon_structure_pick(const moon_structure_mgr_t *mgr, const moon_grid_t *grid,
                         int32_t px, int32_t py, int32_t cam_x, int32_t cam_y,
                         int32_t screen_w, int32_t screen_h, int *out_id);

/* Ghost preview of `type` at a tile (footprint outline, translucent sprite, reason label) */
void moon_structure_render_ghost(const moon_structure_mgr_t *mgr,
                                 const moon_grid_t *grid,
                                 const moon_surface_t *surf,
                                 int32_t cam_x, int32_t cam_y,
                                 moon_structure_type_t type,
                                 int hover_gx, int hover_gy);

#endif /* MOON_STRUCTURES_H */
