#ifndef MOON_STARSHIP_H
#define MOON_STARSHIP_H

#include <stdint.h>
#include <stdbool.h>
#include "moon_fixed.h"
#include "moon_blitter.h"
#include "moon_grid.h"
#include "moon_resources.h"
#include "moon_structures.h"

/* Starship Flight & Spaceport Refueling State Machine (Milestone 64.19) */
typedef enum {
    MOON_STARSHIP_ORBIT = 0,    /* In orbital standby / awaiting landing call */
    MOON_STARSHIP_APPROACH,     /* High-altitude orbital entry & RCS orientation (descent from 360 px) */
    MOON_STARSHIP_RETRO,        /* Main retro-propulsion ease-out burn, cyan hydrolox plume & shock diamonds */
    MOON_STARSHIP_GEAR_EXTEND,  /* Low altitude final flare: landing gear struts deploy, pad deflection plume */
    MOON_STARSHIP_TOUCHDOWN,    /* Engine cut-off, touchdown dust skirts expand along 4x4 sintered apron */
    MOON_STARSHIP_SERVICING,    /* Parked on pad: articulated gantry umbilical arm connected, cryogenic LH2/LOX transfer */
    MOON_STARSHIP_PRELAUNCH,    /* Refueling complete! Umbilical retracts, warning strobes, pre-ignition sequence */
    MOON_STARSHIP_ASCENT,       /* Vertical ascent burn: Raptor ignition, full plume, liftoff dust skirt, acceleration to orbit */
    MOON_STARSHIP_DEPARTED      /* Cleared lunar orbit, transit burn to Earth */
} moon_starship_state_t;

#define MOON_STARSHIP_MAX_DUST 24

typedef struct {
    int32_t x;          /* Offset in pixels from pad center (sx, sy + 48) */
    int32_t y;
    int32_t vx;         /* Velocity per tick */
    int32_t vy;
    uint8_t life;       /* Remaining life ticks */
    uint8_t max_life;
} moon_dust_particle_t;

typedef struct {
    moon_starship_state_t state;
    uint32_t state_timer_ms;    /* Elapsed milliseconds in current state */
    int32_t  alt_px;            /* Current altitude in pixels above pad center (0 at touchdown) */
    int32_t  pad_gx;            /* Target 4x4 landing pad origin tile */
    int32_t  pad_gy;
    bool     has_pad;           /* True if an active target pad is assigned */

    /* Servicing & Cryogenic Fuel Transfer */
    fixed32_t lh2_loaded_kg;    /* Liquid hydrogen loaded into Starship tanks */
    fixed32_t lox_loaded_kg;    /* Liquid oxygen loaded into Starship tanks */
    fixed32_t lh2_target_kg;    /* Refueling target for return burn (250 kg) */
    fixed32_t lox_target_kg;    /* Refueling target for return burn (2000 kg) */
    uint8_t   umbilical_pct;    /* 0..100: Extension percentage of cryogenic gantry arm */

    /* Procedural Plume & Particulate FX */
    moon_dust_particle_t dust[MOON_STARSHIP_MAX_DUST];
    uint8_t  dust_count;
    uint32_t anim_frame;        /* Frame counter for supersonic shock diamond oscillation */
} moon_starship_t;

/* Initialize the Starship simulation state */
void moon_starship_init(moon_starship_t *ship);

/* Find the first active, powered Starship Landing Pad on the grid */
bool moon_starship_find_pad(const moon_structure_mgr_t *mgr, int32_t *out_gx, int32_t *out_gy);

/* Dispatch or call a Starship flight to the given landing pad */
bool moon_starship_call(moon_starship_t *ship, int32_t pad_gx, int32_t pad_gy);

/* Command a fully serviced Starship to begin its vertical ascent return burn */
bool moon_starship_launch(moon_starship_t *ship);

/* Advance the Starship flight sequence, propellant transfer, and particle physics */
void moon_starship_update(moon_starship_t *ship,
                          const moon_structure_mgr_t *mgr,
                          moon_resources_t *res,
                          uint32_t delta_ms);

/* Render the Starship rocket, hydrolox exhaust plume, shock diamonds, umbilical arm,
 * dust skirts, and cryogenic loading meter */
void moon_starship_render(const moon_starship_t *ship,
                          const moon_grid_t *grid,
                          const moon_surface_t *surf,
                          int32_t cam_x, int32_t cam_y);

#endif /* MOON_STARSHIP_H */
