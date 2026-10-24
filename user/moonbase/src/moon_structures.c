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
        *sx += MOON_TILE_HALF_W;
        *sy += MOON_TILE_HALF_H / 2;
    }
}

static void sprite_rect(uint8_t type, int32_t sx, int32_t sy, moon_rect_t *r) {
    const moon_surface_t *spr = sprite_of(type);
    int w = spr ? spr->width : 0;
    int h = spr ? spr->height : 0;
    int base = (type == MOON_STRUCT_HABITAT_DOME) ? MOON_TILE_H : MOON_TILE_HALF_H;
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
    uint16_t cable_col = powered ? COLOR_ENERGY_YELLOW : RGB565(80, 85, 95);
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

static bool structure_visible(const moon_surface_t *surf, int32_t sx, int32_t sy) {
    return !(sx + MOON_TILE_W < 0 || sx - MOON_TILE_W >= surf->width ||
             sy + MOON_TILE_H < 30 || sy - 2 * MOON_TILE_H - 80 >= surf->height);
}

static void draw_structure_body(const moon_grid_t *grid, const moon_surface_t *surf,
                                const moon_structure_t *s, int32_t cam_x, int32_t cam_y) {
    int32_t sx, sy;
    structure_anchor(grid, s->type, s->gx, s->gy, cam_x, cam_y,
                     surf->width, surf->height, &sx, &sy);
    if (!structure_visible(surf, sx, sy)) return;

    if (s->type == MOON_STRUCT_POWER_CONDUIT) {
        draw_conduit_cables(surf, grid, s->gx, s->gy, sx, sy, s->powered);
        return;
    }
    const moon_surface_t *spr = sprite_of(s->type);
    if (!spr) return;
    moon_rect_t r;
    sprite_rect(s->type, sx, sy, &r);
    moon_blit_sprite(surf, r.x0, r.y0, spr, MOON_KEY_COLOR);
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
    if (!structure_visible(surf, sx, sy)) return;

    if (s->type == MOON_STRUCT_POWER_CONDUIT) {
        draw_conduit_junction(surf, sx, sy, s->powered);
        return;
    }
    if (s->type == MOON_STRUCT_BATTERY_BANK) {
        draw_battery_led(surf, sx, sy, res, s->active, s->powered);
    }

    const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)s->type);
    if (!def) return;
    int badge_y = (s->type == MOON_STRUCT_HABITAT_DOME) ? sy - 34 : sy - def->height_px - 8;

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

void moon_world_render(const moon_structure_mgr_t *mgr,
                       const moon_grid_t *grid,
                       const moon_surface_t *surf,
                       int32_t cam_x, int32_t cam_y,
                       int32_t hover_gx, int32_t hover_gy, bool hover_valid) {
    if (!mgr || !grid || !surf) return;

    /* Back to front, one diagonal at a time: a tile, then the structure whose
     * front tile it is. Sprites reach upward over diagonals already drawn. */
    for (int d = 0; d < MOON_MAP_SIZE_X + MOON_MAP_SIZE_Y - 1; d++) {
        for (int gx = 0; gx <= d; gx++) {
            int gy = d - gx;
            if (gx >= MOON_MAP_SIZE_X || gy >= MOON_MAP_SIZE_Y) continue;

            moon_grid_render_tile(grid, surf, gx, gy, cam_x, cam_y,
                                  hover_gx, hover_gy, hover_valid);

            uint8_t id = grid->tiles[gy][gx].structure_id;
            if (id == 0 || id >= MOON_MAX_STRUCTURES) continue;
            const moon_structure_t *s = &mgr->list[id];
            if (s->id != id || depth_key(s) != d) continue;
            draw_structure_body(grid, surf, s, cam_x, cam_y);
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
