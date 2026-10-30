#include "moon_structures.h"
#include "moon_sprites.h"
#include "moon_resources.h"
#include <string.h>

/* Static Catalog of Structures */
static const moon_structure_def_t s_catalog[MOON_STRUCT_COUNT] = {
    [MOON_STRUCT_NONE] = { 0 },

    [MOON_STRUCT_LANDER] = {
        .type           = MOON_STRUCT_LANDER,
        .name           = "Expedition Lander",
        .short_name     = "LANDER",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 0,
        .power_kw       = 10,       /* Baseline RTG generator */
        .power_storage  = 50,       /* Emergency battery reserve */
        .rate_o2        = -1,
        .rate_water     = -2,
        .primary_color  = COLOR_GOLD_FOIL,
        .accent_color   = COLOR_LUNAR_WHITE,
        .height_px      = 24,
        .desc           = "Initial descent stage with RTG generator & emergency life support."
    },

    [MOON_STRUCT_SOLAR_ARRAY] = {
        .type           = MOON_STRUCT_SOLAR_ARRAY,
        .name           = "Photovoltaic Array",
        .short_name     = "SOLAR",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 10,
        .power_kw       = 30,       /* Peak daytime generation */
        .power_storage  = 0,
        .rate_o2        = 0,
        .rate_water     = 0,
        .primary_color  = COLOR_SOLAR_BLUE,
        .accent_color   = COLOR_LUNAR_WHITE,
        .height_px      = 14,
        .desc           = "Dual-axis tracking solar wings generating 30 kW in sunlight."
    },

    [MOON_STRUCT_BATTERY_BANK] = {
        .type           = MOON_STRUCT_BATTERY_BANK,
        .name           = "Fuel Cell Battery",
        .short_name     = "BATTERY",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 20,
        .power_kw       = -1,
        .power_storage  = 250,      /* Stores 250 kWh for lunar night */
        .rate_o2        = 0,
        .rate_water     = 0,
        .primary_color  = RGB565(50, 70, 95),
        .accent_color   = COLOR_STATUS_GREEN,
        .height_px      = 16,
        .desc           = "High-capacity regenerative cell storing daytime solar energy."
    },

    [MOON_STRUCT_HABITAT_DOME] = {
        .type           = MOON_STRUCT_HABITAT_DOME,
        .name           = "Pressurized Habitat",
        .short_name     = "HABITAT",
        .footprint_w    = 2,
        .footprint_h    = 2,
        .cost_regolith  = 50,
        .power_kw       = -15,
        .power_storage  = 0,
        .rate_o2        = -5,
        .rate_water     = -10,
        .primary_color  = RGB565(215, 220, 230),
        .accent_color   = COLOR_LASER_CYAN,
        .height_px      = 28,
        .desc           = "2x2 geodesic shielded dome for crew quarters & hydroponics."
    },

    [MOON_STRUCT_REGOLITH_PLANT] = {
        .type           = MOON_STRUCT_REGOLITH_PLANT,
        .name           = "Regolith Sinterer",
        .short_name     = "SINTERER",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 15,
        .power_kw       = -20,
        .power_storage  = 0,
        .rate_o2        = 0,
        .rate_water     = 0,
        .primary_color  = RGB565(95, 85, 78),
        .accent_color   = COLOR_ENERGY_YELLOW,
        .height_px      = 18,
        .desc           = "Microwave furnace baking loose dust into structural pavers."
    },

    [MOON_STRUCT_O2_EXTRACTOR] = {
        .type           = MOON_STRUCT_O2_EXTRACTOR,
        .name           = "Oxygen Extractor",
        .short_name     = "OXYGEN",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 25,
        .power_kw       = -25,
        .power_storage  = 0,
        .rate_o2        = 20,       /* +20 kg of O2 per minute */
        .rate_water     = 30,       /* +30 L of water per minute */
        .primary_color  = RGB565(40, 115, 155),
        .accent_color   = COLOR_LASER_CYAN,
        .height_px      = 20,
        .desc           = "Electrolyzes ice & reduces ilmenite for closed-loop oxygen and water."
    },

    [MOON_STRUCT_HYDROLOX_PLANT] = {
        .type           = MOON_STRUCT_HYDROLOX_PLANT,
        .name           = "Hydrolox Plant",
        .short_name     = "HYDROLOX",
        .footprint_w    = 2,
        .footprint_h    = 2,
        .cost_regolith  = 80,
        .power_kw       = -60,      /* -60 kW high-power cracking */
        .power_storage  = 0,
        .rate_o2        = 0,
        .rate_water     = 0,
        .primary_color  = RGB565(50, 140, 200),
        .accent_color   = COLOR_LASER_CYAN,
        .height_px      = 28,
        .desc           = "Cracks water into LH2 and LOX cryogenic rocket propellants."
    },

    [MOON_STRUCT_CRYO_DEWAR] = {
        .type           = MOON_STRUCT_CRYO_DEWAR,
        .name           = "Cryo Dewar",
        .short_name     = "DEWAR",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 35,
        .power_kw       = -5,       /* 5 kW active cryocooling */
        .power_storage  = 0,
        .rate_o2        = 0,
        .rate_water     = 0,
        .primary_color  = RGB565(180, 190, 210),
        .accent_color   = COLOR_ENERGY_YELLOW,
        .height_px      = 20,
        .desc           = "Vacuum-insulated sphere storing 250kg LH2 and 2,000kg LOX."
    },

    [MOON_STRUCT_POWER_CONDUIT] = {
        .type           = MOON_STRUCT_POWER_CONDUIT,
        .name           = "Power Conduit",
        .short_name     = "CABLE",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 2,
        .power_kw       = 0,
        .power_storage  = 0,
        .rate_o2        = 0,
        .rate_water     = 0,
        .primary_color  = COLOR_ENERGY_YELLOW,
        .accent_color   = COLOR_REGOLITH_DARK,
        .height_px      = 2,
        .desc           = "Superconducting surface line distributing power across modules."
    },

    [MOON_STRUCT_FOUNDATION_PAD] = {
        .type           = MOON_STRUCT_FOUNDATION_PAD,
        .name           = "Foundation Pad",
        .short_name     = "PAVER",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 5,
        .power_kw       = 0,
        .power_storage  = 0,
        .rate_o2        = 0,
        .rate_water     = 0,
        .primary_color  = COLOR_GOLD_FOIL,
        .accent_color   = COLOR_REGOLITH_DARK,
        .height_px      = 1,
        .desc           = "Sintered interlocking pavers providing load-bearing foundation."
    },

    [MOON_STRUCT_LANDING_PAD] = {
        .type           = MOON_STRUCT_LANDING_PAD,
        .name           = "Starship Landing Pad",
        .short_name     = "STARPAD",
        .footprint_w    = 4,
        .footprint_h    = 4,
        .cost_regolith  = 120,      /* 120 kg sintered regolith spaceport apron */
        .power_kw       = -10,      /* 10 kW telemetry, beacon guidance & cryo umbilical */
        .power_storage  = 0,
        .rate_o2        = 0,
        .rate_water     = 0,
        .primary_color  = RGB565(180, 160, 60),
        .accent_color   = COLOR_GOLD_FOIL,
        .height_px      = 36,
        .desc           = "4x4 sintered regolith spaceport apron with blast berms & cryo umbilical."
    }
};

const moon_structure_def_t* moon_structure_get_def(moon_structure_type_t type) {
    if (type >= MOON_STRUCT_COUNT) return NULL;
    return &s_catalog[type];
}

void moon_structure_mgr_init(moon_structure_mgr_t *mgr) {
    if (!mgr) return;
    memset(mgr, 0, sizeof(moon_structure_mgr_t));
    mgr->selected_build_type = MOON_STRUCT_NONE;
    mgr->revision = 1;
    mgr->solved_revision = 0;
}

/* Does this terrain count as a deposit (drawn by Extractors) rather than regolith? */
static bool is_deposit(uint8_t terrain) {
    return terrain == MOON_TERRAIN_ICE_DEPOSIT || terrain == MOON_TERRAIN_BASALT_MARE;
}

moon_tile_t* moon_structure_ore_tile(moon_grid_t *grid, moon_structure_type_t type, int gx, int gy) {
    if (type != MOON_STRUCT_O2_EXTRACTOR && type != MOON_STRUCT_REGOLITH_PLANT) return NULL;
    bool want_deposit = (type == MOON_STRUCT_O2_EXTRACTOR);
    moon_tile_t *best = NULL;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            moon_tile_t *t = moon_grid_get_tile(grid, gx + dx, gy + dy);
            if (!t || (dx == 0 && dy == 0)) continue;
            if (is_deposit(t->terrain) != want_deposit || t->resource_val == 0) continue;
            if (!best || t->resource_val > best->resource_val) best = t;
        }
    }
    return best;
}

int moon_structure_ore_left(const moon_grid_t *grid, moon_structure_type_t type, int gx, int gy) {
    if (type != MOON_STRUCT_O2_EXTRACTOR && type != MOON_STRUCT_REGOLITH_PLANT) return 0;
    bool want_deposit = (type == MOON_STRUCT_O2_EXTRACTOR);
    int sum = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const moon_tile_t *t = moon_grid_get_tile_const(grid, gx + dx, gy + dy);
            if (!t || (dx == 0 && dy == 0)) continue;
            if (is_deposit(t->terrain) == want_deposit) sum += t->resource_val;
        }
    }
    return sum;
}

bool moon_structure_can_place(const moon_grid_t *grid,
                              const moon_structure_mgr_t *mgr,
                              moon_structure_type_t type,
                              int gx, int gy,
                              const char **out_reason) {
    const moon_structure_def_t *def = moon_structure_get_def(type);
    if (!def || def->type == MOON_STRUCT_NONE) {
        if (out_reason) *out_reason = "Invalid structure type";
        return false;
    }

    if (!grid) {
        if (out_reason) *out_reason = "No grid loaded";
        return false;
    }

    /* 1. Map Boundary Check */
    if (gx < 0 || gy < 0 ||
        gx + def->footprint_w > MOON_MAP_SIZE_X ||
        gy + def->footprint_h > MOON_MAP_SIZE_Y) {
        if (out_reason) *out_reason = "Exceeds map boundary";
        return false;
    }

    /* Helper: check if a structure of `t` exists in mgr */
    if (mgr) {
        if (type == MOON_STRUCT_FOUNDATION_PAD) {
            bool has_sinterer = false;
            for (uint8_t i = 1; i < MOON_MAX_STRUCTURES; i++) {
                if (mgr->list[i].id == i && mgr->list[i].type == MOON_STRUCT_REGOLITH_PLANT) {
                    has_sinterer = true;
                    break;
                }
            }
            if (!has_sinterer) {
                if (out_reason) *out_reason = "Requires Regolith Sinterer";
                return false;
            }
        } else if (type == MOON_STRUCT_HYDROLOX_PLANT || type == MOON_STRUCT_CRYO_DEWAR) {
            bool has_hab = false;
            for (uint8_t i = 1; i < MOON_MAX_STRUCTURES; i++) {
                if (mgr->list[i].id == i && mgr->list[i].type == MOON_STRUCT_HABITAT_DOME) {
                    has_hab = true;
                    break;
                }
            }
            if (!has_hab) {
                if (out_reason) *out_reason = "Tech locked: Requires Habitat Dome";
                return false;
            }
        } else if (type == MOON_STRUCT_LANDING_PAD) {
            bool has_hab = false;
            bool has_cryo = false;
            for (uint8_t i = 1; i < MOON_MAX_STRUCTURES; i++) {
                if (mgr->list[i].id == i) {
                    if (mgr->list[i].type == MOON_STRUCT_HABITAT_DOME) has_hab = true;
                    if (mgr->list[i].type == MOON_STRUCT_HYDROLOX_PLANT || mgr->list[i].type == MOON_STRUCT_CRYO_DEWAR) has_cryo = true;
                }
            }
            if (!has_hab || !has_cryo) {
                if (out_reason) *out_reason = "Tech locked: Requires Habitat & Cryogenics";
                return false;
            }
        }
    }

    /* 2. Tile Footprint & Collision Check */
    for (int dy = 0; dy < def->footprint_h; dy++) {
        for (int dx = 0; dx < def->footprint_w; dx++) {
            const moon_tile_t *t = moon_grid_get_tile_const(grid, gx + dx, gy + dy);
            if (!t) {
                if (out_reason) *out_reason = "Invalid tile";
                return false;
            }

            if (t->flags & MOON_TILE_FLAG_OCCUPIED) {
                if (out_reason) *out_reason = "Site already occupied";
                return false;
            }

            if (type == MOON_STRUCT_FOUNDATION_PAD) {
                if (t->flags & MOON_TILE_FLAG_FOUNDATION) {
                    if (out_reason) *out_reason = "Foundation already paved";
                    return false;
                }
                if (t->terrain != MOON_TERRAIN_REGOLITH_FLAT) {
                    if (t->terrain == MOON_TERRAIN_CRATER_RIM) {
                        if (out_reason) *out_reason = "Terrain too steep (Crater Rim)";
                    } else if (t->terrain == MOON_TERRAIN_REGOLITH_ROUGH) {
                        if (out_reason) *out_reason = "Boulder field (Requires clearing)";
                    } else {
                        if (out_reason) *out_reason = "Requires flat regolith";
                    }
                    return false;
                }
            } else {
                if (t->terrain == MOON_TERRAIN_CRATER_RIM) {
                    if (out_reason) *out_reason = "Terrain too steep (Crater Rim)";
                    return false;
                }

                if (t->terrain == MOON_TERRAIN_REGOLITH_ROUGH) {
                    if (out_reason) *out_reason = "Boulder field (Requires clearing)";
                    return false;
                }
            }
        }
    }

    /* 3. Sintered Foundation Requirement for Hydrolox Plant & Starship Landing Pad */
    if (type == MOON_STRUCT_HYDROLOX_PLANT || type == MOON_STRUCT_LANDING_PAD) {
        for (int dy = 0; dy < def->footprint_h; dy++) {
            for (int dx = 0; dx < def->footprint_w; dx++) {
                const moon_tile_t *t = moon_grid_get_tile_const(grid, gx + dx, gy + dy);
                if (!t || !(t->flags & MOON_TILE_FLAG_FOUNDATION)) {
                    if (out_reason) {
                        *out_reason = (type == MOON_STRUCT_LANDING_PAD)
                                      ? "Requires 4x4 sintered foundation"
                                      : "Requires sintered foundation";
                    }
                    return false;
                }
            }
        }
    }

    /* 4. Minimum Safety Clearance Rules */
    if (mgr) {
        if (type == MOON_STRUCT_HYDROLOX_PLANT) {
            /* 1-tile clearance around 2x2 footprint: [gx-1 .. gx+2] x [gy-1 .. gy+2] */
            for (int cy = gy - 1; cy <= gy + def->footprint_h; cy++) {
                for (int cx = gx - 1; cx <= gx + def->footprint_w; cx++) {
                    const moon_tile_t *ct = moon_grid_get_tile_const(grid, cx, cy);
                    if (!ct || ct->structure_id == 0 || ct->structure_id >= MOON_MAX_STRUCTURES) continue;
                    uint8_t stype = mgr->list[ct->structure_id].type;
                    if (stype == MOON_STRUCT_HABITAT_DOME || stype == MOON_STRUCT_LANDER) {
                        if (out_reason) *out_reason = "Safety: clearance from habitat required";
                        return false;
                    }
                }
            }
        } else if (type == MOON_STRUCT_HABITAT_DOME) {
            /* 1-tile clearance around 2x2 footprint: [gx-1 .. gx+2] x [gy-1 .. gy+2] */
            for (int cy = gy - 1; cy <= gy + def->footprint_h; cy++) {
                for (int cx = gx - 1; cx <= gx + def->footprint_w; cx++) {
                    const moon_tile_t *ct = moon_grid_get_tile_const(grid, cx, cy);
                    if (!ct || ct->structure_id == 0 || ct->structure_id >= MOON_MAX_STRUCTURES) continue;
                    uint8_t stype = mgr->list[ct->structure_id].type;
                    if (stype == MOON_STRUCT_HYDROLOX_PLANT) {
                        if (out_reason) *out_reason = "Safety: clearance from Hydrolox plant required";
                        return false;
                    } else if (stype == MOON_STRUCT_LANDING_PAD) {
                        if (out_reason) *out_reason = "Safety: clearance from landing pad required";
                        return false;
                    }
                }
            }
        } else if (type == MOON_STRUCT_LANDING_PAD) {
            /* 1-tile clearance around 4x4 footprint: [gx-1 .. gx+4] x [gy-1 .. gy+4] */
            for (int cy = gy - 1; cy <= gy + def->footprint_h; cy++) {
                for (int cx = gx - 1; cx <= gx + def->footprint_w; cx++) {
                    const moon_tile_t *ct = moon_grid_get_tile_const(grid, cx, cy);
                    if (!ct || ct->structure_id == 0 || ct->structure_id >= MOON_MAX_STRUCTURES) continue;
                    uint8_t stype = mgr->list[ct->structure_id].type;
                    if (stype == MOON_STRUCT_HABITAT_DOME || stype == MOON_STRUCT_LANDER) {
                        if (out_reason) *out_reason = "Safety: clearance from habitat required";
                        return false;
                    }
                }
            }
        }
    }

    /* 5. Specialized Placement Rules */
    if (type == MOON_STRUCT_O2_EXTRACTOR && moon_structure_ore_left(grid, type, gx, gy) == 0) {
        /* Must be beside Polar Ice or Basalt Mare that still has something to draw */
        if (out_reason) *out_reason = "Needs ice or mare with ore left";
        return false;
    }
    if (type == MOON_STRUCT_REGOLITH_PLANT && moon_structure_ore_left(grid, type, gx, gy) == 0) {
        if (out_reason) *out_reason = "No regolith left to quarry here";
        return false;
    }

    if (out_reason) *out_reason = "Placement Valid";
    return true;
}

moon_structure_t* moon_structure_place(moon_grid_t *grid,
                                       moon_structure_mgr_t *mgr,
                                       moon_structure_type_t type,
                                       int gx, int gy) {
    if (!grid || !mgr) return NULL;
    if (!moon_structure_can_place(grid, mgr, type, gx, gy, NULL)) return NULL;

    const moon_structure_def_t *def = moon_structure_get_def(type);
    if (!def) return NULL;

    if (type == MOON_STRUCT_FOUNDATION_PAD) {
        moon_tile_t *t = moon_grid_get_tile(grid, gx, gy);
        if (t) {
            t->flags |= MOON_TILE_FLAG_FOUNDATION;
        }
        mgr->revision++;
        /* single task: the moonbase session holder */
        static moon_structure_t s_foundation_dummy;
        s_foundation_dummy.id = 0;
        s_foundation_dummy.type = MOON_STRUCT_FOUNDATION_PAD;
        s_foundation_dummy.gx = (uint8_t)gx;
        s_foundation_dummy.gy = (uint8_t)gy;
        s_foundation_dummy.footprint_w = 1;
        s_foundation_dummy.footprint_h = 1;
        s_foundation_dummy.active = true;
        s_foundation_dummy.powered = true;
        s_foundation_dummy.health = 100;
        return &s_foundation_dummy;
    }

    /* Find empty structure slot (1..MOON_MAX_STRUCTURES-1) */
    uint8_t slot_id = 0;
    for (uint8_t i = 1; i < MOON_MAX_STRUCTURES; i++) {
        if (mgr->list[i].id == 0) {
            slot_id = i;
            break;
        }
    }
    if (slot_id == 0) return NULL; /* Structure table full */

    moon_structure_t *s = &mgr->list[slot_id];
    s->id          = slot_id;
    s->type        = type;
    s->gx          = (uint8_t)gx;
    s->gy          = (uint8_t)gy;
    s->footprint_w = def->footprint_w;
    s->footprint_h = def->footprint_h;
    s->active      = true;
    s->powered     = true;
    s->health      = 100;

    /* Mark tiles in grid */
    for (int dy = 0; dy < def->footprint_h; dy++) {
        for (int dx = 0; dx < def->footprint_w; dx++) {
            moon_tile_t *t = moon_grid_get_tile(grid, gx + dx, gy + dy);
            if (t) {
                t->flags |= MOON_TILE_FLAG_OCCUPIED;
                t->structure_id = slot_id;
            }
        }
    }

    mgr->count++;
    mgr->revision++;
    return s;
}

void moon_structure_demolish(moon_grid_t *grid,
                             moon_structure_mgr_t *mgr,
                             int gx, int gy) {
    if (!grid || !mgr) return;
    moon_tile_t *t = moon_grid_get_tile(grid, gx, gy);
    if (!t) return;

    if (t->structure_id == 0) {
        if ((t->flags & MOON_TILE_FLAG_FOUNDATION) &&
            (gx != grid->lander_gx || gy != grid->lander_gy)) {
            t->flags &= ~MOON_TILE_FLAG_FOUNDATION;
            mgr->revision++;
        }
        return;
    }

    uint8_t id = t->structure_id;
    if (id >= MOON_MAX_STRUCTURES) return;

    moon_structure_t *s = &mgr->list[id];
    if (s->type == MOON_STRUCT_LANDER) {
        return; /* Cannot demolish Expedition Lander */
    }

    /* Clear grid tiles */
    for (int dy = 0; dy < s->footprint_h; dy++) {
        for (int dx = 0; dx < s->footprint_w; dx++) {
            moon_tile_t *cur = moon_grid_get_tile(grid, s->gx + dx, s->gy + dy);
            if (cur && cur->structure_id == id) {
                cur->flags &= ~MOON_TILE_FLAG_OCCUPIED;
                cur->structure_id = 0;
            }
        }
    }

    memset(s, 0, sizeof(moon_structure_t));
    if (mgr->count > 0) mgr->count--;
    mgr->revision++;
}

/* Sprite catalog: NULL for structures drawn procedurally (conduits). Sprites are
 * anchored by the bottom of the footprint diamond; the dome spans 2x2 tiles. */
static const moon_surface_t *sprite_of(uint8_t type) {
    switch (type) {
        case MOON_STRUCT_LANDER:         return &g_surf_lander;
        case MOON_STRUCT_SOLAR_ARRAY:    return &g_surf_solar_array;
        case MOON_STRUCT_BATTERY_BANK:   return &g_surf_battery_bank;
        case MOON_STRUCT_HABITAT_DOME:   return &g_surf_habitat_dome;
        case MOON_STRUCT_REGOLITH_PLANT: return &g_surf_sinterer;
        case MOON_STRUCT_O2_EXTRACTOR:   return &g_surf_o2_extractor;
        case MOON_STRUCT_HYDROLOX_PLANT: return &g_surf_hydrolox_plant;
        case MOON_STRUCT_CRYO_DEWAR:     return &g_surf_cryo_dewar;
        case MOON_STRUCT_LANDING_PAD:    return &g_surf_landing_pad;
        default:                         return NULL;
    }
}

/* Screen point the sprite is anchored to: the origin tile's centre, moved to the
 * middle of a multi-tile footprint. */
static void structure_anchor(const moon_grid_t *grid, uint8_t type, int gx, int gy,
                             int32_t cam_x, int32_t cam_y, int sw, int sh,
                             int32_t *sx, int32_t *sy) {
    const moon_tile_t *t = moon_grid_get_tile_const(grid, gx, gy);
    moon_grid_to_screen(gx, gy, t ? t->elevation : 0, cam_x, cam_y, sw, sh, sx, sy);
    if (type == MOON_STRUCT_HABITAT_DOME) {
        /* centered */;
    }
}

static void sprite_rect(uint8_t type, int32_t sx, int32_t sy, moon_rect_t *r) {
    const moon_surface_t *spr = sprite_of(type);
    int w = spr ? spr->width : 0;
    int h = spr ? spr->height : 0;
    int base = (type == MOON_STRUCT_LANDING_PAD) ? 112 :
               (type == MOON_STRUCT_HABITAT_DOME || type == MOON_STRUCT_HYDROLOX_PLANT) ? 48 : MOON_TILE_HALF_H;
    r->x0 = sx - w / 2;
    r->y0 = sy + base - h;
    r->x1 = r->x0 + w;
    r->y1 = r->y0 + h;
}

/* Depth key: the diagonal of the footprint's front tile */
static int depth_key(const moon_structure_t *s) {
    return (s->gx + s->footprint_w - 1) + (s->gy + s->footprint_h - 1);
}

static void draw_conduit_cables(const moon_surface_t *surf, const moon_grid_t *grid,
                                int gx, int gy, int32_t sx, int32_t sy, bool powered) {
    uint16_t cable_col = moon_blit_tint_color(powered ? COLOR_ENERGY_YELLOW : RGB565(80, 85, 95));
    static const int dir[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };

    for (int i = 0; i < 4; i++) {
        const moon_tile_t *n = moon_grid_get_tile_const(grid, gx + dir[i][0], gy + dir[i][1]);
        if (!n || n->structure_id == 0) continue;
        /* One screen step to the neighbour's tile centre in iso space */
        int dx = (dir[i][0] - dir[i][1]) * MOON_TILE_HALF_W;
        int dy = (dir[i][0] + dir[i][1]) * MOON_TILE_HALF_H;
        moon_blit_line(surf, sx, sy, sx + dx, sy + dy, cable_col);
    }
}

static bool structure_visible(const moon_surface_t *surf, uint8_t type, int32_t sx, int32_t sy, int32_t view_top) {
    if (type == MOON_STRUCT_POWER_CONDUIT) {
        return !(sx + MOON_TILE_W < 0 || sx - MOON_TILE_W >= surf->width ||
                 sy + MOON_TILE_HALF_H < view_top || sy - MOON_TILE_HALF_H >= surf->height);
    }
    moon_rect_t r;
    sprite_rect(type, sx, sy, &r);
    return !(r.x1 <= 0 || r.x0 >= surf->width || r.y1 <= view_top || r.y0 >= surf->height);
}

static void draw_lander_ground_shadow(const moon_surface_t *surf, int32_t sx, int32_t sy, int32_t alt) {
    /* Target beacon optical alignment marks on the ground */
    uint16_t mark_col = (alt < 80) ? COLOR_GOLD_FOIL : RGB565(80, 85, 95);
    moon_blit_pixel(surf, sx - 16, sy, mark_col);
    moon_blit_pixel(surf, sx + 16, sy, mark_col);
    moon_blit_pixel(surf, sx, sy - 8, mark_col);
    moon_blit_pixel(surf, sx, sy + 8, mark_col);

    /* Dynamic ground shadow: darker and crisper as altitude decreases */
    int rx = 18 - (alt > 160 ? 8 : (alt / 20));
    int ry = rx / 2;
    if (ry < 2) ry = 2;

    for (int dy = -ry; dy <= ry; dy++) {
        int w = rx - (dy < 0 ? -dy : dy) * 2;
        if (w <= 0) continue;
        for (int dx = -w; dx <= w; dx++) {
            int px = sx + dx;
            int py = sy + 4 + dy;
            if (px >= 0 && px < surf->width && py >= 0 && py < surf->height) {
                uint16_t c = surf->pixels[py * surf->stride + px];
                /* 50% dark shadow tint */
                uint16_t r = (c >> 11) & 0x1F;
                uint16_t g = (c >> 5) & 0x3F;
                uint16_t b = c & 0x1F;
                surf->pixels[py * surf->stride + px] = ((r >> 1) << 11) | ((g >> 1) << 5) | (b >> 1);
            }
        }
    }
}

static void draw_structure_body(const moon_grid_t *grid, const moon_surface_t *surf,
                                const moon_structure_t *s, int32_t cam_x, int32_t cam_y,
                                int32_t view_top, int32_t lander_y_offset) {
    int32_t sx, sy;
    structure_anchor(grid, s->type, s->gx, s->gy, cam_x, cam_y,
                     surf->width, surf->height, &sx, &sy);
    if (!structure_visible(surf, s->type, sx, sy, view_top)) return;

    if (s->type == MOON_STRUCT_POWER_CONDUIT) {
        draw_conduit_cables(surf, grid, s->gx, s->gy, sx, sy, s->powered);
        return;
    }

    if (s->type == MOON_STRUCT_LANDER && lander_y_offset > 0) {
        /* While in descent flight, the lander sprite and plume are rendered
         * as an aerial vessel above the world pass, but we draw the landing zone
         * target marker and shadow on the ground */
        draw_lander_ground_shadow(surf, sx, sy, lander_y_offset);
        return;
    }

    const moon_surface_t *spr = sprite_of(s->type);
    if (!spr) return;
    moon_rect_t r;
    sprite_rect(s->type, sx, sy, &r);
    moon_blit_sprite(surf, r.x0, r.y0, spr, MOON_KEY_COLOR);
}

static void draw_meter_bar(const moon_surface_t *surf, int32_t sx, int32_t sy,
                            int pips, int max_pips, uint16_t active_col) {
    int bar_w = max_pips * 4;
    int bar_x = sx - (bar_w / 2);
    int bar_y = sy;
    int bar_h = 4;

    /* Dark bezel */
    moon_blit_fill_rect(surf, bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, RGB565(15, 18, 22));
    moon_blit_rect_outline(surf, bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, RGB565(40, 45, 55));

    for (int p = 0; p < max_pips; p++) {
        uint16_t col = (p < pips) ? active_col : RGB565(30, 32, 38);
        moon_blit_fill_rect(surf, bar_x + p * 4, bar_y, 3, bar_h, col);
    }
}

static void draw_battery_led(const moon_surface_t *surf, int32_t sx, int32_t sy,
                             const struct moon_resources_s *res, bool active, bool powered) {
    int bar_x = sx - 10;
    int bar_y = sy - 14;
    int bar_w = 20;
    int bar_h = 4;

    /* Dark background bezel */
    moon_blit_fill_rect(surf, bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, RGB565(15, 18, 22));
    moon_blit_rect_outline(surf, bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, RGB565(40, 45, 55));

    if (active && powered && res && res->battery_capacity_kwh > 0) {
        int capacity = res->battery_capacity_kwh;
        int stored = fixed_to_int(res->battery_stored_kwh);
        int pips = (stored * 5 + (capacity / 10)) / capacity;
        if (pips > 5) pips = 5;
        if (pips < 0) pips = 0;

        uint16_t led_col = (pips >= 3) ? COLOR_STATUS_GREEN :
                           ((pips >= 2) ? COLOR_ENERGY_YELLOW : COLOR_ALERT_RED);

        for (int p = 0; p < 5; p++) {
            moon_blit_fill_rect(surf, bar_x + p * 4, bar_y, 3, bar_h,
                                p < pips ? led_col : RGB565(30, 32, 38));
        }
    } else {
        /* Unpowered / standby: dark unlit pips */
        for (int p = 0; p < 5; p++) {
            moon_blit_fill_rect(surf, bar_x + p * 4, bar_y, 3, bar_h, RGB565(25, 25, 30));
        }
    }
}

static void draw_conduit_junction(const moon_surface_t *surf, int32_t sx, int32_t sy, bool powered) {
    moon_blit_fill_rect(surf, sx - 4, sy - 4, 9, 9, RGB565(25, 28, 35));
    moon_blit_rect_outline(surf, sx - 4, sy - 4, 9, 9, COLOR_GOLD_FOIL);
    moon_blit_pixel(surf, sx, sy, powered ? COLOR_STATUS_GREEN : COLOR_ALERT_RED);
}

/* Everything that must stay readable above every sprite: status badges, the
 * battery gauge and conduit junction boxes. */
static void draw_structure_overlay(const moon_grid_t *grid, const moon_surface_t *surf,
                                   const struct moon_resources_s *res,
                                   const moon_structure_t *s,
                                   int32_t cam_x, int32_t cam_y) {
    int32_t sx, sy;
    structure_anchor(grid, s->type, s->gx, s->gy, cam_x, cam_y,
                     surf->width, surf->height, &sx, &sy);
    if (!structure_visible(surf, s->type, sx, sy, 0)) return;

    if (s->type == MOON_STRUCT_POWER_CONDUIT) {
        draw_conduit_junction(surf, sx, sy, s->powered);
        return;
    }
    if (s->type == MOON_STRUCT_BATTERY_BANK) {
        draw_battery_led(surf, sx, sy, res, s->active, s->powered);
    } else if (s->type == MOON_STRUCT_HYDROLOX_PLANT) {
        /* Status bar above plant: cyan if cracking water, yellow if storage full or no dewar, red if out of water / unpowered */
        if (s->active && s->powered && res && !res->power_brownout) {
            int water_l = fixed_to_int(res->water_stored_l);
            int lh2_cap = res->lh2_capacity_kg;
            int lox_cap = res->lox_capacity_kg;
            int lh2_stored = fixed_to_int(res->lh2_stored_kg);
            int lox_stored = fixed_to_int(res->lox_stored_kg);
            bool has_dewar = (lh2_cap > 0 || lox_cap > 0);
            bool space_avail = has_dewar && (lh2_stored < lh2_cap || lox_stored < lox_cap);
            bool cracking = (water_l > 20) && space_avail;

            if (cracking) {
                draw_meter_bar(surf, sx, sy - 36, 5, 5, COLOR_LASER_CYAN);
            } else if (!has_dewar || !space_avail) {
                draw_meter_bar(surf, sx, sy - 36, 5, 5, COLOR_ENERGY_YELLOW);
            } else {
                draw_meter_bar(surf, sx, sy - 36, 1, 5, COLOR_ALERT_RED);
            }
        } else {
            draw_meter_bar(surf, sx, sy - 36, 0, 5, RGB565(30, 32, 38));
        }
    } else if (s->type == MOON_STRUCT_CRYO_DEWAR) {
        /* Fuel fill level bar: stored propellant vs capacity */
        if (s->active && res) {
            int total_cap = res->lh2_capacity_kg + res->lox_capacity_kg;
            int total_stored = fixed_to_int(res->lh2_stored_kg) + fixed_to_int(res->lox_stored_kg);
            int pips = (total_cap > 0) ? (total_stored * 5 + (total_cap / 10)) / total_cap : 0;
            if (pips > 5) pips = 5;
            if (pips < 0) pips = 0;
            uint16_t col = s->powered ? RGB565(120, 210, 255) : COLOR_ALERT_RED;
            draw_meter_bar(surf, sx, sy - 20, pips, 5, col);
        } else {
            draw_meter_bar(surf, sx, sy - 20, 0, 5, RGB565(30, 32, 38));
        }
    } else if (s->type == MOON_STRUCT_LANDING_PAD) {
        /* Starship Landing Pad guidance transponders & beacons indicator:
         * Cyan when active, powered, and grid stable; yellow if unpowered; red if brownout */
        if (s->active && s->powered && res && !res->power_brownout) {
            draw_meter_bar(surf, sx, sy - 116, 5, 5, COLOR_LASER_CYAN);
        } else if (s->active && !s->powered) {
            draw_meter_bar(surf, sx, sy - 116, 2, 5, COLOR_ENERGY_YELLOW);
        } else {
            draw_meter_bar(surf, sx, sy - 116, 0, 5, RGB565(30, 32, 38));
        }
    }

    const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)s->type);
    if (!def) return;
    int badge_y = (s->type == MOON_STRUCT_LANDING_PAD) ? sy - 128 :
                  (s->type == MOON_STRUCT_HABITAT_DOME || s->type == MOON_STRUCT_HYDROLOX_PLANT) ? sy - 38 :
                  sy - def->height_px - 8;

    if (!s->active) {
        /* Standby / Deactivated badge: yellow framed pill */
        int bw = 24;
        moon_blit_fill_rect(surf, sx - (bw / 2), badge_y, bw, 10, RGB565(30, 26, 10));
        moon_blit_rect_outline(surf, sx - (bw / 2), badge_y, bw, 10, COLOR_ENERGY_YELLOW);
        moon_blit_text(surf, sx - (bw / 2) + 3, badge_y + 1, "OFF", COLOR_ENERGY_YELLOW, 0, true);
    } else if (s->depleted) {
        /* Nothing left to draw: orange pill */
        int bw = 34;
        moon_blit_fill_rect(surf, sx - (bw / 2), badge_y, bw, 10, RGB565(40, 22, 8));
        moon_blit_rect_outline(surf, sx - (bw / 2), badge_y, bw, 10, RGB565(230, 130, 30));
        moon_blit_text(surf, sx - (bw / 2) + 3, badge_y + 1, "DEPL", RGB565(230, 130, 30), 0, true);
    } else if (!s->powered) {
        /* No power / brownout failure badge: red framed pill */
        int bw = 30;
        moon_blit_fill_rect(surf, sx - (bw / 2), badge_y, bw, 10, RGB565(35, 10, 10));
        moon_blit_rect_outline(surf, sx - (bw / 2), badge_y, bw, 10, COLOR_ALERT_RED);
        moon_blit_text(surf, sx - (bw / 2) + 3, badge_y + 1, "!PWR", COLOR_ALERT_RED, 0, true);
    }
}

static inline int div_floor(int a, int b) {
    int q = a / b;
    int r = a % b;
    if (r != 0 && ((a < 0) ^ (b < 0))) q--;
    return q;
}

void moon_world_render(const moon_structure_mgr_t *mgr,
                       const moon_grid_t *grid,
                       const moon_surface_t *surf,
                       int32_t cam_x, int32_t cam_y, int32_t view_top,
                       int32_t hover_gx, int32_t hover_gy, bool hover_valid) {
    if (!mgr || !grid || !surf) return;

    /* Visible diagonal range: d = gx + gy.
     * base_sy = d * MOON_TILE_HALF_H - cam_y + (surf->height / 2).
     * Tiles on diagonal d have their bottom at base_sy + MOON_TILE_HALF_H.
     * Margin of 2 diagonals for elevation and safety. */
    int d_min = div_floor(view_top + cam_y - (surf->height / 2) - MOON_TILE_HALF_H, MOON_TILE_HALF_H) - 2;
    if (d_min < 0) d_min = 0;

    /* Sprites (up to 80px), overlays, and raised cliffs (up to 32px) reach upward from base_sy.
     * A tile / structure on diagonal d can reach into the screen while base_sy - 128 < surf->height.
     * Margin of 3 diagonals at the bottom. */
    int d_max = div_floor(surf->height + 128 + cam_y - (surf->height / 2), MOON_TILE_HALF_H) + 3;
    if (d_max >= MOON_MAP_SIZE_X + MOON_MAP_SIZE_Y - 1)
        d_max = MOON_MAP_SIZE_X + MOON_MAP_SIZE_Y - 2;

    /* Back to front, one diagonal at a time: a tile, then the structure whose
     * front tile it is. Sprites reach upward over diagonals already drawn. */
    for (int d = d_min; d <= d_max; d++) {
        int map_gx_min = d - (MOON_MAP_SIZE_Y - 1);
        if (map_gx_min < 0) map_gx_min = 0;
        int map_gx_max = d;
        if (map_gx_max >= MOON_MAP_SIZE_X) map_gx_max = MOON_MAP_SIZE_X - 1;

        /* Screen horizontal bounds:
         * base_sx = (2*gx - d) * MOON_TILE_HALF_W - cam_x + (surf->width / 2).
         * Allow 128 px margin on left and right for sprites and tile half-widths: */
        int left_target = -128 + cam_x - (surf->width / 2);
        int scr_gx_min = div_floor(d + div_floor(left_target, MOON_TILE_HALF_W), 2) - 2;

        int right_target = surf->width + 128 + cam_x - (surf->width / 2);
        int scr_gx_max = div_floor(d + div_floor(right_target + MOON_TILE_HALF_W - 1, MOON_TILE_HALF_W), 2) + 2;

        int gx_start = map_gx_min > scr_gx_min ? map_gx_min : scr_gx_min;
        int gx_end = map_gx_max < scr_gx_max ? map_gx_max : scr_gx_max;

        for (int gx = gx_start; gx <= gx_end; gx++) {
            int gy = d - gx;

            moon_grid_render_tile(grid, surf, gx, gy, cam_x, cam_y, view_top,
                                  hover_gx, hover_gy, hover_valid);

            uint8_t id = grid->tiles[gy][gx].structure_id;
            if (id == 0 || id >= MOON_MAX_STRUCTURES) continue;
            const moon_structure_t *s = &mgr->list[id];
            if (s->id != id || depth_key(s) != d) continue;
            draw_structure_body(grid, surf, s, cam_x, cam_y, view_top, mgr->lander_y_offset);
        }
    }
}

void moon_world_render_overlays(const moon_structure_mgr_t *mgr,
                                const moon_grid_t *grid,
                                const moon_surface_t *surf,
                                const struct moon_resources_s *res,
                                int32_t cam_x, int32_t cam_y) {
    if (!mgr || !grid || !surf) return;
    for (int i = 1; i < MOON_MAX_STRUCTURES; i++) {
        if (mgr->list[i].id != 0) {
            draw_structure_overlay(grid, surf, res, &mgr->list[i], cam_x, cam_y);
        }
    }
}

bool moon_structure_pick(const moon_structure_mgr_t *mgr, const moon_grid_t *grid,
                         int32_t px, int32_t py, int32_t cam_x, int32_t cam_y,
                         int32_t screen_w, int32_t screen_h, int *out_id) {
    if (!mgr || !grid) return false;
    int best = 0, best_key = -1;

    for (int i = 1; i < MOON_MAX_STRUCTURES; i++) {
        const moon_structure_t *s = &mgr->list[i];
        if (s->id == 0 || s->type == MOON_STRUCT_POWER_CONDUIT) continue;
        int32_t sx, sy;
        structure_anchor(grid, s->type, s->gx, s->gy, cam_x, cam_y, screen_w, screen_h, &sx, &sy);
        moon_rect_t r;
        sprite_rect(s->type, sx, sy, &r);
        if (px < r.x0 || px >= r.x1 || py < r.y0 || py >= r.y1) continue;
        /* Transparent pixels of the sprite do not count as a hit */
        const moon_surface_t *spr = sprite_of(s->type);
        if (spr && spr->pixels[(py - r.y0) * spr->stride + (px - r.x0)] == MOON_KEY_COLOR) continue;
        int key = depth_key(s);
        if (key > best_key) { best_key = key; best = i; }
    }
    if (best == 0) return false;
    if (out_id) *out_id = best;
    return true;
}

void moon_structure_render_ghost(const moon_structure_mgr_t *mgr,
                                 const moon_grid_t *grid,
                                 const moon_surface_t *surf,
                                 int32_t cam_x, int32_t cam_y,
                                 moon_structure_type_t type,
                                 int hover_gx, int hover_gy) {
    if (!mgr || !grid || !surf) return;
    if (type == MOON_STRUCT_NONE) return;

    const moon_structure_def_t *def = moon_structure_get_def(type);
    if (!def) return;

    const char *reason = NULL;
    bool valid = moon_structure_can_place(grid, mgr, type, hover_gx, hover_gy, &reason);
    uint16_t ghost_color = valid ? COLOR_STATUS_GREEN : COLOR_ALERT_RED;

    /* Footprint outline, one diamond per tile */
    for (int dy = 0; dy < def->footprint_h; dy++) {
        for (int dx = 0; dx < def->footprint_w; dx++) {
            const moon_tile_t *t = moon_grid_get_tile_const(grid, hover_gx + dx, hover_gy + dy);
            int32_t sx, sy;
            moon_grid_to_screen(hover_gx + dx, hover_gy + dy, t ? t->elevation : 0,
                                cam_x, cam_y, surf->width, surf->height, &sx, &sy);
            moon_blit_line(surf, sx, sy - MOON_TILE_HALF_H, sx + MOON_TILE_HALF_W, sy, ghost_color);
            moon_blit_line(surf, sx + MOON_TILE_HALF_W, sy, sx, sy + MOON_TILE_HALF_H, ghost_color);
            moon_blit_line(surf, sx, sy + MOON_TILE_HALF_H, sx - MOON_TILE_HALF_W, sy, ghost_color);
            moon_blit_line(surf, sx - MOON_TILE_HALF_W, sy, sx, sy - MOON_TILE_HALF_H, ghost_color);
        }
    }

    /* Translucent sprite of what will be built */
    const moon_surface_t *spr = sprite_of(def->type);
    if (spr) {
        int32_t sx, sy;
        structure_anchor(grid, def->type, hover_gx, hover_gy, cam_x, cam_y,
                         surf->width, surf->height, &sx, &sy);
        moon_rect_t r;
        sprite_rect(def->type, sx, sy, &r);
        moon_blit_sprite_blend(surf, r.x0, r.y0, spr, MOON_KEY_COLOR, ghost_color, 140);
    }

    /* Print structure placement reason badge above mouse */
    int32_t label_sx, label_sy;
    moon_grid_to_screen(hover_gx, hover_gy, 2, cam_x, cam_y, surf->width, surf->height, &label_sx, &label_sy);

    int text_x = label_sx - 80;
    int text_y = label_sy - 30;
    if (text_x < 10) text_x = 10;
    if (text_y < 40) text_y = 40;

    /* Extractors and Sinterers also say how much ground they would have to work with */
    int ore = moon_structure_ore_left(grid, type, hover_gx, hover_gy);
    bool show_ore = valid && (type == MOON_STRUCT_O2_EXTRACTOR || type == MOON_STRUCT_REGOLITH_PLANT);
    char text[64];
    int len = 0;
    const char *parts[3] = { "[", def->short_name, "] " };
    for (int p = 0; p < 3; p++) for (const char *c = parts[p]; *c && len < 40; c++) text[len++] = *c;
    for (const char *c = reason ? reason : ""; *c && len < 52; c++) text[len++] = *c;
    if (show_ore) {
        const char *tag = "  ore ";
        for (const char *c = tag; *c; c++) text[len++] = *c;
        char digits[8]; int nd = 0;
        for (int v = ore; v > 0 || nd == 0; v /= 10) digits[nd++] = (char)('0' + v % 10);
        while (nd > 0) text[len++] = digits[--nd];
    }
    text[len] = '\0';
    int box_w = len * MOON_FONT_W + 16;
    moon_blit_fill_rect(surf, text_x - 4, text_y - 2, box_w, 20, RGB565(14, 16, 20));
    moon_blit_rect_outline(surf, text_x - 4, text_y - 2, box_w, 20, ghost_color);
    moon_blit_text(surf, text_x + 4, text_y + 2, text, ghost_color, 0, true);
}

void moon_lander_render(const moon_grid_t *grid,
                        const moon_surface_t *surf,
                        int32_t cam_x, int32_t cam_y,
                        int32_t alt,
                        uint32_t elapsed_ms) {
    if (!grid || !surf || alt <= 0) return;

    /* Lander tile is (32, 32) */
    const moon_tile_t *t = moon_grid_get_tile_const(grid, 32, 32);
    int32_t sx, sy;
    moon_grid_to_screen(32, 32, t ? t->elevation : 0, cam_x, cam_y,
                        surf->width, surf->height, &sx, &sy);

    /* Culling check */
    if (sx + 40 < 0 || sx - 40 >= surf->width ||
        sy + 20 < 0 || (sy - 48 - alt) >= surf->height) return;

    /* 1. Low-altitude radial dust skirts on the regolith */
    if (alt <= 45) {
        uint32_t tick = elapsed_ms >> 4;
        static const int8_t cos_table[16] = { 16, 15, 11, 6, 0, -6, -11, -15, -16, -15, -11, -6, 0, 6, 11, 15 };
        static const int8_t sin_table[16] = { 0, 6, 11, 15, 16, 15, 11, 6, 0, -6, -11, -15, -16, -15, -11, -6 };
        for (int p = 0; p < 16; p++) {
            uint32_t h = (p * 2654435761u + tick * 1103515245u);
            int phase = (h & 0xF); /* 0..15 */
            int angle_bin = (p * 4) + (phase & 3);
            int dist = 8 + (phase * 2) + ((45 - alt) / 2);
            int dx = (cos_table[angle_bin & 15] * dist) / 16;
            int dy = (sin_table[angle_bin & 15] * dist) / 32; /* 2:1 isometric aspect ratio */
            int px = sx + dx;
            int py = sy + 4 + dy;
            if (px >= 0 && px < surf->width && py >= 0 && py < surf->height) {
                uint16_t dust_col = (phase < 8) ? COLOR_LUNAR_WHITE : COLOR_REGOLITH_LIGHT;
                moon_blit_pixel(surf, px, py, dust_col);
            }
        }
    }

    /* 2. Hypergolic Descent Rocket Plume & Core */
    int nozzle_y = sy + 4 - alt;
    int flame_h = (alt > 35) ? (24 + ((int)(elapsed_ms / 35) & 7)) : alt;
    if (flame_h > 0) {
        for (int dy = 0; dy <= flame_h; dy++) {
            int py = nozzle_y + dy;
            if (py < 0 || py >= surf->height) continue;
            int w = (dy < flame_h / 3) ? (2 + dy / 2) :
                    ((flame_h - dy) * 4 / (flame_h ? flame_h : 1));
            if (w < 1) w = 1;
            if (w > 5) w = 5;

            for (int dx = -w; dx <= w; dx++) {
                int px = sx + dx;
                if (px < 0 || px >= surf->width) continue;
                int adx = dx < 0 ? -dx : dx;
                uint16_t col;
                if (adx == 0 && dy < flame_h * 3 / 4) {
                    col = (dy < flame_h / 2) ? COLOR_LUNAR_WHITE : COLOR_ENERGY_YELLOW;
                } else if (adx <= 1 && dy < flame_h - 2) {
                    col = COLOR_ENERGY_YELLOW;
                } else if (adx <= 2) {
                    col = COLOR_GOLD_FOIL;
                } else {
                    col = RGB565(230, 90, 20);
                }
                moon_blit_pixel(surf, px, py, col);
            }
        }

        /* Ground deflection plume flare when alt <= 25 */
        if (alt <= 25) {
            int flare_w = (25 - alt) / 2 + 6;
            int fpy = sy + 4;
            for (int fdx = -flare_w; fdx <= flare_w; fdx++) {
                int px = sx + fdx;
                if (px >= 0 && px < surf->width && fpy >= 0 && fpy < surf->height) {
                    int afdx = fdx < 0 ? -fdx : fdx;
                    uint16_t fcol = (afdx < 4) ? COLOR_ENERGY_YELLOW :
                                    (afdx < flare_w - 2) ? COLOR_GOLD_FOIL : RGB565(220, 80, 20);
                    moon_blit_pixel(surf, px, fpy, fcol);
                }
            }
        }
    }

    /* 3. Cold-Gas RCS Reaction Control Thruster Puffs */
    if (((elapsed_ms / 60) & 3) == 1) {
        moon_blit_pixel(surf, sx - 20, nozzle_y - 12, COLOR_LASER_CYAN);
        moon_blit_pixel(surf, sx - 21, nozzle_y - 12, COLOR_LUNAR_WHITE);
    } else if (((elapsed_ms / 60) & 3) == 2) {
        moon_blit_pixel(surf, sx + 20, nozzle_y - 12, COLOR_LASER_CYAN);
        moon_blit_pixel(surf, sx + 21, nozzle_y - 12, COLOR_LUNAR_WHITE);
    }

    /* 4. Lander Sprite */
    int spr_x = sx - 32;
    int spr_y = (sy - 48) - alt;
    moon_blit_sprite(surf, spr_x, spr_y, &g_surf_lander, MOON_KEY_COLOR);
}
