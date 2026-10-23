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
        .rate_o2        = 20,       /* +20 kg/day breathable O2 */
        .rate_water     = 30,       /* +30 L/day purified water */
        .primary_color  = RGB565(40, 115, 155),
        .accent_color   = COLOR_LASER_CYAN,
        .height_px      = 20,
        .desc           = "Electrolyzes ice & reduces ilmenite for closed-loop oxygen and water."
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
    /* Blit realistic 64x64 Lander sprite anchored at diamond bottom */
    int dst_x = sx - (SPRITE_LANDER_W / 2);
    int dst_y = sy + MOON_TILE_HALF_H - SPRITE_LANDER_H;
    moon_blit_sprite(surf, dst_x, dst_y, &g_surf_lander, MOON_KEY_COLOR);
}

/* Individual Structure Visual Renderers */
static void render_solar_array(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Blit realistic 64x48 Solar Array sprite */
    int dst_x = sx - (SPRITE_SOLAR_ARRAY_W / 2);
    int dst_y = sy + MOON_TILE_HALF_H - SPRITE_SOLAR_ARRAY_H;
    moon_blit_sprite(surf, dst_x, dst_y, &g_surf_solar_array, MOON_KEY_COLOR);
}

static void render_battery_bank(const moon_surface_t *surf, int32_t sx, int32_t sy,
                                const struct moon_resources_s *res, bool active, bool powered) {
    /* Blit realistic 64x48 Battery Bank sprite */
    int dst_x = sx - (SPRITE_BATTERY_BANK_W / 2);
    int dst_y = sy + MOON_TILE_HALF_H - SPRITE_BATTERY_BANK_H;
    moon_blit_sprite(surf, dst_x, dst_y, &g_surf_battery_bank, MOON_KEY_COLOR);

    /* Dynamic LED charge level bar */
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
            int px = bar_x + p * 4;
            if (p < pips) {
                moon_blit_fill_rect(surf, px, bar_y, 3, bar_h, led_col);
            } else {
                moon_blit_fill_rect(surf, px, bar_y, 3, bar_h, RGB565(30, 32, 38));
            }
        }
    } else {
        /* Unpowered / standby: dark unlit pips */
        for (int p = 0; p < 5; p++) {
            moon_blit_fill_rect(surf, bar_x + p * 4, bar_y, 3, bar_h, RGB565(25, 25, 30));
        }
    }
}

static void render_habitat_dome(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Blit realistic 96x80 2x2 Habitat Dome sprite centered on 2x2 footprint */
    int dst_x = sx - (SPRITE_HABITAT_DOME_W / 2);
    int dst_y = sy + MOON_TILE_H - SPRITE_HABITAT_DOME_H;
    moon_blit_sprite(surf, dst_x, dst_y, &g_surf_habitat_dome, MOON_KEY_COLOR);
}

static void render_regolith_sinterer(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Blit realistic 64x48 Regolith Sinterer sprite */
    int dst_x = sx - (SPRITE_SINTERER_W / 2);
    int dst_y = sy + MOON_TILE_HALF_H - SPRITE_SINTERER_H;
    moon_blit_sprite(surf, dst_x, dst_y, &g_surf_sinterer, MOON_KEY_COLOR);
}

static void render_o2_extractor(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    /* Blit realistic 64x48 Oxygen Extractor sprite */
    int dst_x = sx - (SPRITE_O2_EXTRACTOR_W / 2);
    int dst_y = sy + MOON_TILE_HALF_H - SPRITE_O2_EXTRACTOR_H;
    moon_blit_sprite(surf, dst_x, dst_y, &g_surf_o2_extractor, MOON_KEY_COLOR);
}

static void render_power_conduit(const moon_surface_t *surf, const moon_grid_t *grid,
                                 int gx, int gy, int32_t sx, int32_t sy, bool powered) {
    /* Directional cable connections to adjacent structures */
    const moon_tile_t *e = moon_grid_get_tile_const(grid, gx + 1, gy);
    const moon_tile_t *w = moon_grid_get_tile_const(grid, gx - 1, gy);
    const moon_tile_t *s = moon_grid_get_tile_const(grid, gx, gy + 1);
    const moon_tile_t *n = moon_grid_get_tile_const(grid, gx, gy - 1);

    uint16_t cable_col = powered ? COLOR_ENERGY_YELLOW : RGB565(80, 85, 95);

    /* +gx = Down-Right */
    if (e && e->structure_id != 0) {
        moon_blit_line(surf, sx, sy, sx + 16, sy + 8, cable_col);
    }
    /* -gx = Up-Left */
    if (w && w->structure_id != 0) {
        moon_blit_line(surf, sx, sy, sx - 16, sy - 8, cable_col);
    }
    /* +gy = Down-Left */
    if (s && s->structure_id != 0) {
        moon_blit_line(surf, sx, sy, sx - 16, sy + 8, cable_col);
    }
    /* -gy = Up-Right */
    if (n && n->structure_id != 0) {
        moon_blit_line(surf, sx, sy, sx + 16, sy - 8, cable_col);
    }

    /* Central insulated junction box */
    moon_blit_fill_rect(surf, sx - 4, sy - 4, 9, 9, RGB565(25, 28, 35));
    moon_blit_rect_outline(surf, sx - 4, sy - 4, 9, 9, COLOR_GOLD_FOIL);
    moon_blit_pixel(surf, sx, sy, powered ? COLOR_STATUS_GREEN : COLOR_ALERT_RED);
}

void moon_structure_render_all(const moon_structure_mgr_t *mgr,
                               const moon_grid_t *grid,
                               const moon_surface_t *surf,
                               const struct moon_resources_s *res,
                               int32_t cam_x, int32_t cam_y) {
    if (!mgr || !grid || !surf) return;

    for (int i = 1; i < MOON_MAX_STRUCTURES; i++) {
        const moon_structure_t *s = &mgr->list[i];
        if (s->id == 0) continue; /* NEVER skip inactive buildings */

        const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)s->type);
        if (!def) continue;

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
                render_battery_bank(surf, sx, sy, res, s->active, s->powered);
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
                render_power_conduit(surf, grid, s->gx, s->gy, sx, sy, s->powered);
                break;
            default:
                break;
        }

        /* Floating operational status icon if disabled or unpowered */
        if (s->type != MOON_STRUCT_POWER_CONDUIT) {
            int badge_y = sy - def->height_px - 8;
            if (s->type == MOON_STRUCT_HABITAT_DOME) {
                badge_y = sy - 34;
            }

            if (!s->active) {
                /* Standby / Deactivated badge: yellow framed pill */
                int bw = 24;
                int bh = 10;
                moon_blit_fill_rect(surf, sx - (bw / 2), badge_y, bw, bh, RGB565(30, 26, 10));
                moon_blit_rect_outline(surf, sx - (bw / 2), badge_y, bw, bh, COLOR_ENERGY_YELLOW);
                moon_blit_text(surf, sx - (bw / 2) + 3, badge_y + 1, "OFF", COLOR_ENERGY_YELLOW, 0, true);
            } else if (!s->powered) {
                /* No power / brownout failure badge: red framed pill */
                int bw = 30;
                int bh = 10;
                moon_blit_fill_rect(surf, sx - (bw / 2), badge_y, bw, bh, RGB565(35, 10, 10));
                moon_blit_rect_outline(surf, sx - (bw / 2), badge_y, bw, bh, COLOR_ALERT_RED);
                moon_blit_text(surf, sx - (bw / 2) + 3, badge_y + 1, "!PWR", COLOR_ALERT_RED, 0, true);
            }
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
