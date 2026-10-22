#include "moon_structures.h"
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
        .name           = "Volatiles Extractor",
        .short_name     = "O2 PLANT",
        .footprint_w    = 1,
        .footprint_h    = 1,
        .cost_regolith  = 25,
        .power_kw       = -25,
        .power_storage  = 0,
        .rate_o2        = 20,       /* +20 kg/day breathable O2 */
        .rate_water     = 30,       /* +30 L/day purified water */
        .primary_color  = RGB565(40, 115, 155),
        .accent_color   = COLOR_LASER_CYAN,
        .height_px      = 20,
        .desc           = "Electrolyzes ice & reduces ilmenite for closed-loop O2 and water."
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
}

bool moon_structure_can_place(const moon_grid_t *grid,
                              const moon_structure_mgr_t *mgr,
                              moon_structure_type_t type,
                              int gx, int gy,
                              const char **out_reason) {
    (void)mgr;
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

    /* 3. Specialized Placement Rules */
    if (type == MOON_STRUCT_O2_EXTRACTOR) {
        /* Must be adjacent to or on Polar Ice or Basalt Mare */
        bool near_deposit = false;
        for (int dy = -1; dy <= def->footprint_h; dy++) {
            for (int dx = -1; dx <= def->footprint_w; dx++) {
                const moon_tile_t *t = moon_grid_get_tile_const(grid, gx + dx, gy + dy);
                if (t && (t->terrain == MOON_TERRAIN_ICE_DEPOSIT || t->terrain == MOON_TERRAIN_BASALT_MARE)) {
                    near_deposit = true;
                    break;
                }
            }
            if (near_deposit) break;
        }

        if (!near_deposit) {
            if (out_reason) *out_reason = "Must be placed near Ice or Basalt Mare";
            return false;
        }
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
    return s;
}

void moon_structure_demolish(moon_grid_t *grid,
                             moon_structure_mgr_t *mgr,
                             int gx, int gy) {
    if (!grid || !mgr) return;
    moon_tile_t *t = moon_grid_get_tile(grid, gx, gy);
    if (!t || t->structure_id == 0) return;

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
}

static void render_lander(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Gold Foil 3D Lander module sitting on top */
    moon_blit_iso_block(surf, sx, sy,
                        MOON_TILE_HALF_W - 4, MOON_TILE_HALF_H - 2, 22,
                        COLOR_GOLD_FOIL,
                        moon_color_dim(COLOR_GOLD_FOIL, 175),
                        moon_color_dim(COLOR_GOLD_FOIL, 220),
                        COLOR_LUNAR_WHITE);

    /* Pressurized crew cabin cylinder & antenna */
    moon_blit_circle_filled(surf, sx, sy - 24, 6, COLOR_LUNAR_WHITE);
    moon_blit_circle(surf, sx, sy - 24, 6, COLOR_GOLD_FOIL);
    moon_blit_line(surf, sx, sy - 30, sx, sy - 38, COLOR_LUNAR_WHITE);
    moon_blit_pixel(surf, sx, sy - 39, COLOR_ALERT_RED); /* beacon LED */
}

/* Individual Structure Visual Renderers */
static void render_solar_array(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Central support mast */
    moon_blit_fill_rect(surf, sx - 2, sy - 14, 4, 14, COLOR_LUNAR_WHITE);

    /* Angled photovoltaic wing diamond */
    int panel_w = MOON_TILE_HALF_W - 4;
    int panel_h = MOON_TILE_HALF_H - 4;
    int panel_sy = sy - 14;

    moon_blit_iso_tile(surf, sx, panel_sy, panel_w, panel_h,
                       COLOR_SOLAR_BLUE, COLOR_LUNAR_WHITE);

    /* Photovoltaic cell busbars (grid lines) */
    moon_blit_line(surf, sx, panel_sy - panel_h + 2, sx, panel_sy + panel_h - 2, COLOR_LUNAR_WHITE);
    moon_blit_line(surf, sx - panel_w + 4, panel_sy, sx + panel_w - 4, panel_sy, COLOR_LUNAR_WHITE);
}

static void render_battery_bank(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Reinforced battery block */
    moon_blit_iso_block(surf, sx, sy, MOON_TILE_HALF_W - 6, MOON_TILE_HALF_H - 4, 16,
                        RGB565(60, 75, 95), RGB565(40, 50, 65), RGB565(50, 62, 80),
                        COLOR_LUNAR_WHITE);

    /* Glowing charge indicator status LED strip */
    moon_blit_line(surf, sx - 8, sy - 10, sx + 8, sy - 2, COLOR_STATUS_GREEN);
}

static void render_habitat_dome(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* 2x2 Geodesic Dome: centered between 2x2 tiles */
    int dome_r = MOON_TILE_HALF_W - 4;
    int dome_cy = sy - 12;

    /* Base pressurized foundation ring */
    moon_blit_iso_tile(surf, sx, sy, MOON_TILE_HALF_W * 2 - 8, MOON_TILE_HALF_H * 2 - 4,
                       RGB565(180, 185, 195), COLOR_LUNAR_WHITE);

    /* Rounded geodesic dome sphere */
    moon_blit_circle_filled(surf, sx, dome_cy, dome_r, RGB565(210, 215, 225));
    moon_blit_circle(surf, sx, dome_cy, dome_r, COLOR_LUNAR_WHITE);

    /* Translucent hexagonal observation window facets */
    moon_blit_fill_rect(surf, sx - 8, dome_cy - 4, 16, 8, COLOR_LASER_CYAN);
    moon_blit_rect_outline(surf, sx - 8, dome_cy - 4, 16, 8, COLOR_LUNAR_WHITE);
}

static void render_regolith_sinterer(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Main furnace bunker */
    moon_blit_iso_block(surf, sx, sy, MOON_TILE_HALF_W - 6, MOON_TILE_HALF_H - 4, 18,
                        RGB565(95, 85, 75), RGB565(65, 55, 48), RGB565(80, 70, 62),
                        COLOR_ENERGY_YELLOW);

    /* Microwave chimney / hopper */
    moon_blit_fill_rect(surf, sx - 4, sy - 26, 8, 10, RGB565(120, 105, 95));
    moon_blit_rect_outline(surf, sx - 4, sy - 26, 8, 10, COLOR_GOLD_FOIL);
}

static void render_o2_extractor(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Base plant */
    moon_blit_iso_block(surf, sx, sy, MOON_TILE_HALF_W - 6, MOON_TILE_HALF_H - 4, 14,
                        RGB565(45, 100, 135), RGB565(30, 70, 95), RGB565(38, 85, 115),
                        COLOR_LASER_CYAN);

    /* Dual spherical cryogenic storage tanks */
    moon_blit_circle_filled(surf, sx - 8, sy - 16, 7, COLOR_LUNAR_WHITE);
    moon_blit_circle(surf, sx - 8, sy - 16, 7, COLOR_LASER_CYAN);

    moon_blit_circle_filled(surf, sx + 8, sy - 16, 7, COLOR_LUNAR_WHITE);
    moon_blit_circle(surf, sx + 8, sy - 16, 7, COLOR_LASER_CYAN);
}

static void render_power_conduit(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Sleek surface cable running through tile center */
    moon_blit_fill_rect(surf, sx - 4, sy - 2, 8, 4, COLOR_ENERGY_YELLOW);
    moon_blit_line(surf, sx - 8, sy, sx + 8, sy, COLOR_GOLD_FOIL);
}

void moon_structure_render_all(const moon_structure_mgr_t *mgr,
                               const moon_grid_t *grid,
                               const moon_surface_t *surf,
                               int32_t cam_x, int32_t cam_y) {
    if (!mgr || !grid || !surf) return;

    for (int i = 1; i < MOON_MAX_STRUCTURES; i++) {
        const moon_structure_t *s = &mgr->list[i];
        if (s->id == 0 || !s->active) continue;

        const moon_tile_t *t = moon_grid_get_tile_const(grid, s->gx, s->gy);
        int elev = t ? t->elevation : 0;

        int32_t sx, sy;
        moon_grid_to_screen(s->gx, s->gy, elev, cam_x, cam_y,
                            surf->width, surf->height, &sx, &sy);

        /* Offset 2x2 habitat dome center */
        if (s->type == MOON_STRUCT_HABITAT_DOME) {
            sx += MOON_TILE_HALF_W;
            sy += MOON_TILE_HALF_H / 2;
        }

        /* Viewport culling */
        if (sx + MOON_TILE_W < 0 || sx - MOON_TILE_W >= surf->width ||
            sy + MOON_TILE_H < 30 || sy - MOON_TILE_H - 30 >= surf->height) {
            continue;
        }

        switch (s->type) {
            case MOON_STRUCT_LANDER:
                render_lander(surf, sx, sy);
                break;
            case MOON_STRUCT_SOLAR_ARRAY:
                render_solar_array(surf, sx, sy);
                break;
            case MOON_STRUCT_BATTERY_BANK:
                render_battery_bank(surf, sx, sy);
                break;
            case MOON_STRUCT_HABITAT_DOME:
                render_habitat_dome(surf, sx, sy);
                break;
            case MOON_STRUCT_REGOLITH_PLANT:
                render_regolith_sinterer(surf, sx, sy);
                break;
            case MOON_STRUCT_O2_EXTRACTOR:
                render_o2_extractor(surf, sx, sy);
                break;
            case MOON_STRUCT_POWER_CONDUIT:
                render_power_conduit(surf, sx, sy);
                break;
            default:
                break;
        }
    }
}

void moon_structure_render_ghost(const moon_structure_mgr_t *mgr,
                                 const moon_grid_t *grid,
                                 const moon_surface_t *surf,
                                 int32_t cam_x, int32_t cam_y,
                                 int hover_gx, int hover_gy) {
    if (!mgr || !grid || !surf) return;
    if (mgr->selected_build_type == MOON_STRUCT_NONE) return;

    const moon_structure_def_t *def = moon_structure_get_def(mgr->selected_build_type);
    if (!def) return;

    const char *reason = NULL;
    bool valid = moon_structure_can_place(grid, mgr, mgr->selected_build_type,
                                          hover_gx, hover_gy, &reason);

    uint16_t ghost_color = valid ? COLOR_STATUS_GREEN : COLOR_ALERT_RED;

    /* Render ghost footprint diamonds */
    for (int dy = 0; dy < def->footprint_h; dy++) {
        for (int dx = 0; dx < def->footprint_w; dx++) {
            int gx = hover_gx + dx;
            int gy = hover_gy + dy;
            const moon_tile_t *t = moon_grid_get_tile_const(grid, gx, gy);
            int elev = t ? t->elevation : 0;

            int32_t sx, sy;
            moon_grid_to_screen(gx, gy, elev, cam_x, cam_y, surf->width, surf->height, &sx, &sy);

            moon_blit_iso_tile(surf, sx, sy, MOON_TILE_HALF_W, MOON_TILE_HALF_H,
                               moon_color_dim(ghost_color, 120), ghost_color);
        }
    }

    /* Print structure placement reason badge above mouse */
    int32_t label_sx, label_sy;
    moon_grid_to_screen(hover_gx, hover_gy, 2, cam_x, cam_y, surf->width, surf->height, &label_sx, &label_sy);

    int text_x = label_sx - 80;
    int text_y = label_sy - 30;
    if (text_x < 10) text_x = 10;
    if (text_y < 40) text_y = 40;

    moon_blit_fill_rect(surf, text_x - 4, text_y - 2, 220, 20, RGB565(14, 16, 20));
    moon_blit_rect_outline(surf, text_x - 4, text_y - 2, 220, 20, ghost_color);
    moon_blit_text_fmt(surf, text_x + 4, text_y + 2, ghost_color, 0, true,
                       "[%s] %s", def->short_name, reason ? reason : "");
}
