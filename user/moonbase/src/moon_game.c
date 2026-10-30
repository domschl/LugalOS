#include "moon_game.h"
#include "moon_config.h"
#include "moon_fixed.h"
#include "moon_blitter.h"
#include "moon_grid.h"
#include "moon_structures.h"
#include "moon_textures.h"
#include <string.h>

#define MOON_MAX_FRAME_DELTA_MS 100
#define DRAG_LIMIT_MOUSE_PX     6     /* Movement that turns a press into a drag */
#define DRAG_LIMIT_TOUCH_PX     12    /* About 2 mm on both touch boards */
#define SCROLL_PX_PER_S         480
#define KEY_STEP_PX             24

static const char *get_terrain_name(moon_terrain_type_t t) {
    switch (t) {
        case MOON_TERRAIN_REGOLITH_FLAT:  return "REGOLITH";
        case MOON_TERRAIN_REGOLITH_ROUGH: return "BOULDERS";
        case MOON_TERRAIN_BASALT_MARE:    return "BASALT MARE";
        case MOON_TERRAIN_CRATER_RIM:     return "CRATER RIM";
        case MOON_TERRAIN_CRATER_FLOOR:   return "CRATER FLOOR";
        case MOON_TERRAIN_ICE_DEPOSIT:    return "POLAR ICE";
        default:                          return "UNKNOWN";
    }
}

/* Lander tile; the camera starts on it and Space returns to it */
#define HOME_GX 32
#define HOME_GY 32

static void camera_home(moon_game_t *game) {
    game->cam_x = 0;
    game->cam_y = HOME_GY * MOON_TILE_H;
}

/* Fresh base. Keeps what belongs to the platform and the player's display choice. */
static void game_reset(moon_game_t *game, int width, int height) {
    bool textured = (game->state != MOON_STATE_INIT) ? game->grid.textured_terrain : true;
    /* A fresh seed per run: the requested one for the first, then one derived from
     * the previous seed and how long that run lasted */
    uint32_t seed = game->keep_seed ? game->seed :
                    game->seed_request ? game->seed_request :
                    game->seed ? game->seed * 1664525u + 1013904223u + game->sim_ticks : 19690720u;
    bool hide_objectives = game->objectives_hidden;
    uint32_t (*clock_us)(void) = game->clock_us;
    bool show_perf = game->show_perf;
    int difficulty = game->difficulty_request ? game->difficulty_request - 1
                   : (game->state != MOON_STATE_INIT) ? game->difficulty : MOON_DIFFICULTY_NORMAL;
    memset(game, 0, sizeof(*game));
    game->seed = seed;
    game->difficulty = (uint8_t)difficulty;
    game->objectives_hidden = hide_objectives;
    game->clock_us = clock_us;
    game->show_perf = show_perf;

    moon_textures_init();

    game->state = MOON_STATE_PLAYING;
    game->running = true;
    game->screen_w = width;
    game->screen_h = height;
    game->speed = 1;
    moon_ui_layout(&game->ui, width, height);

    /* Generate procedural lunar terrain */
    moon_grid_generate(&game->grid, game->seed);
    game->grid.textured_terrain = textured;

    /* Initialize structure manager & place expedition landing site */
    moon_structure_mgr_init(&game->structs);
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_LANDER, 32, 32);

    /* Initialize resource & life support simulation */
    moon_resources_init(&game->resources, game->difficulty);
    moon_resources_solve_power_grid(&game->grid, &game->structs);
    moon_starship_init(&game->starship);

    camera_home(game);
    game->cam_start_x = game->cam_x;
    game->cam_start_y = game->cam_y;
    game->selected_tile_x = HOME_GX;
    game->selected_tile_y = HOME_GY;
    game->dirty = MOON_DIRTY_WORLD;
    game->press_slot = -1;
    game->preview_slot = -1;
    game->last_slot = -1;
}

bool moon_game_init(moon_game_t *game, int width, int height) {
    if (!game) return false;
    bool title = game->show_title;
    game_reset(game, width, height);
    if (title) {
        game->menu_page = MOON_MENU_TITLE;
        game->menu_sel = game->difficulty;
    }
    return true;
}

void moon_game_shutdown(moon_game_t *game) {
    if (!game) return;
    game->running = false;
}


/* The view centre may reach any tile of the map (independent of screen size). */
void moon_game_clamp_camera(moon_game_t *game) {
    const int32_t min_x = -(MOON_MAP_SIZE_Y - 1) * MOON_TILE_HALF_W;
    const int32_t max_x =  (MOON_MAP_SIZE_X - 1) * MOON_TILE_HALF_W;
    const int32_t max_y =  (MOON_MAP_SIZE_X + MOON_MAP_SIZE_Y - 2) * MOON_TILE_HALF_H;
    if (game->cam_x < min_x) game->cam_x = min_x;
    if (game->cam_x > max_x) game->cam_x = max_x;
    if (game->cam_y < 0)     game->cam_y = 0;
    if (game->cam_y > max_y) game->cam_y = max_y;
}
#define clamp_camera moon_game_clamp_camera

/* Screen regions, used to route pointer input (and to keep drags that start on UI off the map) */
enum { UI_MAP = 0, UI_HUD, UI_RIBBON, UI_MINIMAP, UI_INSPECTOR, UI_UNDO, UI_MENU };

static bool undo_available(const moon_game_t *game) {
    return game->undo_kind != MOON_UNDO_NONE && game->undo_ms > 0;
}

static int ui_region(const moon_game_t *game, int px, int py) {
    const moon_ui_layout_t *ui = &game->ui;
    if (game->menu_page != MOON_MENU_CLOSED) return UI_MENU;
    if (undo_available(game) && moon_ui_hit(&ui->undo_btn, px, py)) return UI_UNDO;
    if (moon_ui_hit(&ui->ribbon_bar, px, py)) return UI_RIBBON;
    if (moon_ui_hit(&ui->minimap_zone, px, py)) return UI_MINIMAP;
    if (game->panel_open && moon_ui_hit(&ui->inspector, px, py)) return UI_INSPECTOR;
    if (py < ui->hud.h) return UI_HUD;
    return UI_MAP;
}

/* Ribbon slots: 0 standby, 1..6 build, 7 recycle; then category, pause, speed, menu */
enum { SLOT_STANDBY = 0, SLOT_RECYCLE = 7, SLOT_CATEGORY = 8, SLOT_PAUSE = 9, SLOT_SPEED = 10, SLOT_MENU = 11 };

static int ribbon_slot_at(const moon_game_t *game, int px, int py) {
    const moon_ui_layout_t *ui = &game->ui;
    for (int i = 0; i < MOON_UI_RIBBON_SLOTS; i++) {
        if (moon_ui_hit(&ui->ribbon[i], px, py)) return i;
    }
    if (moon_ui_hit(&ui->cat_btn, px, py)) return SLOT_CATEGORY;
    if (moon_ui_hit(&ui->pause_btn, px, py)) return SLOT_PAUSE;
    if (moon_ui_hit(&ui->speed_btn, px, py)) return SLOT_SPEED;
    if (moon_ui_hit(&ui->menu_btn, px, py)) return SLOT_MENU;
    return -1;
}

static const moon_structure_t *find_structure(const moon_game_t *game, uint8_t id) {
    if (id == 0 || id >= MOON_MAX_STRUCTURES) return NULL;
    return (game->structs.list[id].id == id) ? &game->structs.list[id] : NULL;
}

static const moon_structure_t *selected_structure(const moon_game_t *game) {
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
    return t ? find_structure(game, t->structure_id) : NULL;
}

static void notify(moon_game_t *game, const char *msg, uint32_t ms) {
    game->notify_msg = msg;
    game->notify_timer_ms = ms;
    game->dirty = MOON_DIRTY_WORLD;
}

static void update_hover(moon_game_t *game, int px, int py) {
    /* A building's sprite hides the tile behind it, so the front-most sprite
     * under the pointer wins over the ground tile (except while placing, where
     * the ground tile behind a building is a valid target). */
    int id;
    if (game->structs.selected_build_type == MOON_STRUCT_NONE &&
        moon_structure_pick(&game->structs, &game->grid, px, py, game->cam_x, game->cam_y,
                            game->screen_w, game->screen_h, &id)) {
        game->hover_tile_x = game->structs.list[id].gx;
        game->hover_tile_y = game->structs.list[id].gy;
        game->hover_valid = true;
        return;
    }
    game->hover_valid = moon_grid_pick(&game->grid, px, py, game->cam_x, game->cam_y,
                                       game->screen_w, game->screen_h,
                                       &game->hover_tile_x, &game->hover_tile_y);
}

/* ---- Selection and view ---- */

static void view_glide_to(moon_game_t *game, int gx, int gy) {
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, gx, gy);
    game->glide_x = (gx - gy) * MOON_TILE_HALF_W;
    game->glide_y = (gx + gy) * MOON_TILE_HALF_H - (t ? t->elevation : 0) * MOON_ELEVATION_STEP_PX;
    game->glide = true;
}

static void select_tile(moon_game_t *game, int gx, int gy) {
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    if (gx >= MOON_MAP_SIZE_X) gx = MOON_MAP_SIZE_X - 1;
    if (gy >= MOON_MAP_SIZE_Y) gy = MOON_MAP_SIZE_Y - 1;
    game->selected_tile_x = gx;
    game->selected_tile_y = gy;
}

/* Screen position of the selected tile for a given camera */
static void selection_on_screen(const moon_game_t *game, int32_t cam_x, int32_t cam_y, int32_t *sx, int32_t *sy) {
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
    moon_grid_to_screen(game->selected_tile_x, game->selected_tile_y, t ? t->elevation : 0,
                        cam_x, cam_y, game->screen_w, game->screen_h, sx, sy);
}

/* The view stays still while the selection moves inside the middle of the map
 * area; once it leaves that zone the view glides to centre it, so a run of key
 * presses scrolls rarely rather than on every step. */
static void view_follow_selection(moon_game_t *game) {
    int32_t sx, sy;
    if (game->glide) selection_on_screen(game, game->glide_x, game->glide_y, &sx, &sy);
    else selection_on_screen(game, game->cam_x, game->cam_y, &sx, &sy);

    const int top = game->ui.hud.h, bottom = game->ui.ribbon_bar.y;
    const int mx = game->screen_w / 5, my = (bottom - top) / 5;
    if (sx < mx || sx > game->screen_w - mx || sy < top + my || sy > bottom - my) {
        view_glide_to(game, game->selected_tile_x, game->selected_tile_y);
    }
}

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

/* The arrow keys are the map's four edge-neighbour moves, i.e. the diagonals on
 * screen: the view rotated by 45 degrees. Up is up-left, Right up-right, Down
 * down-right and Left down-left, so each key always moves to the adjacent tile
 * it points toward and the opposite key undoes it. */
static void move_selection(moon_game_t *game, int dir, int steps) {
    static const int8_t d[4][2] = {
        [DIR_UP] = { -1, 0 }, [DIR_DOWN] = { 1, 0 }, [DIR_LEFT] = { 0, 1 }, [DIR_RIGHT] = { 0, -1 }
    };
    select_tile(game, game->selected_tile_x + d[dir][0] * steps, game->selected_tile_y + d[dir][1] * steps);
    view_follow_selection(game);
}

static void select_next_structure(moon_game_t *game) {
    const moon_structure_t *cur = selected_structure(game);
    int start = cur ? cur->id : 0;
    for (int n = 1; n <= MOON_MAX_STRUCTURES; n++) {
        int i = (start + n) % MOON_MAX_STRUCTURES;
        if (i != 0 && game->structs.list[i].id == i) {
            select_tile(game, game->structs.list[i].gx, game->structs.list[i].gy);
            game->panel_open = true;
            view_follow_selection(game);
            return;
        }
    }
}

/* Space: bring the view to the selection; when it is already there, to the lander */
static void view_toggle_home(moon_game_t *game) {
    int32_t sx, sy;
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
    moon_grid_to_screen(game->selected_tile_x, game->selected_tile_y, t ? t->elevation : 0,
                        game->cam_x, game->cam_y, game->screen_w, game->screen_h, &sx, &sy);
    bool centred = (sx - game->screen_w / 2) * (sx - game->screen_w / 2) +
                   (sy - game->screen_h / 2) * (sy - game->screen_h / 2) < 40 * 40;
    if (centred) view_glide_to(game, HOME_GX, HOME_GY);
    else view_glide_to(game, game->selected_tile_x, game->selected_tile_y);
}

static void pan_by(moon_game_t *game, int dx, int dy) {
    game->glide = false;
    game->cam_x += dx;
    game->cam_y += dy;
    clamp_camera(game);
}

/* ---- Actions on the selected tile ---- */

static void undo_record(moon_game_t *game, int kind, int type, int gx, int gy, int refund_kg) {
    game->undo_kind = (uint8_t)kind;
    game->undo_type = (uint8_t)type;
    game->undo_x = (uint8_t)gx;
    game->undo_y = (uint8_t)gy;
    game->undo_refund_kg = refund_kg;
    game->undo_ms = MOON_UNDO_WINDOW_MS;
}

static void add_regolith(moon_game_t *game, int kg) {
    game->resources.regolith_stored_kg += fixed_from_int(kg);
    fixed32_t cap = fixed_from_int(game->resources.regolith_capacity_kg);
    if (game->resources.regolith_stored_kg > cap) game->resources.regolith_stored_kg = cap;
}

/* Recycle the structure on tile (gx, gy) for a 50% refund; the Lander is permanent. */
static bool recycle_at(moon_game_t *game, int gx, int gy) {
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, gx, gy);
    const moon_structure_t *s = t ? find_structure(game, t->structure_id) : NULL;
    if (!s) {
        if (t && (t->flags & MOON_TILE_FLAG_FOUNDATION) &&
            (gx != game->grid.lander_gx || gy != game->grid.lander_gy)) {
            const moon_structure_def_t *def = moon_structure_get_def(MOON_STRUCT_FOUNDATION_PAD);
            int refund = def ? def->cost_regolith / 2 : 2;
            add_regolith(game, refund);
            moon_structure_demolish(&game->grid, &game->structs, gx, gy);
            moon_resources_solve_power_grid(&game->grid, &game->structs);
            undo_record(game, MOON_UNDO_RECYCLE, MOON_STRUCT_FOUNDATION_PAD, gx, gy, refund);
            return true;
        }
        notify(game, "NOTHING TO RECYCLE", 1500);
        return false;
    }
    if (s->type == MOON_STRUCT_LANDER) { notify(game, "THE LANDER CANNOT BE RECYCLED", 2000); return false; }

    int type = s->type, ox = s->gx, oy = s->gy;
    const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)type);
    fixed32_t before = game->resources.regolith_stored_kg;
    if (def) add_regolith(game, def->cost_regolith / 2);
    int refund = fixed_to_int(game->resources.regolith_stored_kg - before);
    moon_structure_demolish(&game->grid, &game->structs, gx, gy);
    moon_resources_solve_power_grid(&game->grid, &game->structs);
    undo_record(game, MOON_UNDO_RECYCLE, type, ox, oy, refund);
    return true;
}

/* Can build button `slot` (1..6) act on the selected tile? */
static bool build_possible(const moon_game_t *game, moon_structure_type_t type, const char **reason) {
    const moon_structure_def_t *def = moon_structure_get_def(type);
    if (!def) return false;
    if (!moon_structure_can_place(&game->grid, &game->structs, type,
                                  game->selected_tile_x, game->selected_tile_y, reason)) {
        return false;
    }
    if (game->resources.regolith_stored_kg < fixed_from_int(def->cost_regolith)) {
        if (reason) *reason = "INSUFFICIENT REGOLITH!";
        return false;
    }
    return true;
}

static bool build_at_selection(moon_game_t *game, moon_structure_type_t type) {
    const char *reason = NULL;
    if (!build_possible(game, type, &reason)) {
        if (reason) notify(game, reason, 2000);
        return false;
    }
    const moon_structure_def_t *def = moon_structure_get_def(type);
    moon_structure_t *placed = moon_structure_place(&game->grid, &game->structs, type,
                                                    game->selected_tile_x, game->selected_tile_y);
    if (!placed) return false;
    game->resources.regolith_stored_kg -= fixed_from_int(def->cost_regolith);
    moon_resources_solve_power_grid(&game->grid, &game->structs);
    undo_record(game, MOON_UNDO_BUILD, type, game->selected_tile_x, game->selected_tile_y, def->cost_regolith);
    return true;
}

static void toggle_standby_selected(moon_game_t *game) {
    const moon_structure_t *s = selected_structure(game);
    if (!s) { notify(game, "NO BUILDING SELECTED", 1500); return; }
    if (s->type == MOON_STRUCT_LANDER) { notify(game, "THE LANDER CANNOT STAND BY", 2000); return; }
    game->structs.list[s->id].active = !s->active;
    moon_structure_mark_dirty(&game->structs);
    moon_resources_solve_power_grid(&game->grid, &game->structs);
}

static void undo_last(moon_game_t *game) {
    if (!undo_available(game)) return;
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, game->undo_x, game->undo_y);
    const moon_structure_t *s = t ? find_structure(game, t->structure_id) : NULL;

    if (game->undo_kind == MOON_UNDO_BUILD) {
        if (game->undo_type == MOON_STRUCT_FOUNDATION_PAD) {
            if (!t || !(t->flags & MOON_TILE_FLAG_FOUNDATION) || t->structure_id != 0) {
                notify(game, "CANNOT UNDO", 1500); game->undo_kind = MOON_UNDO_NONE; return;
            }
            moon_structure_demolish(&game->grid, &game->structs, game->undo_x, game->undo_y);
            add_regolith(game, game->undo_refund_kg);
        } else {
            if (!s || s->type != game->undo_type) { notify(game, "CANNOT UNDO", 1500); game->undo_kind = MOON_UNDO_NONE; return; }
            moon_structure_demolish(&game->grid, &game->structs, game->undo_x, game->undo_y);
            add_regolith(game, game->undo_refund_kg);
        }
    } else {
        fixed32_t give_back = fixed_from_int(game->undo_refund_kg);
        if (game->resources.regolith_stored_kg < give_back) {
            notify(game, "CANNOT UNDO", 1500);
            game->undo_kind = MOON_UNDO_NONE;
            return;
        }
        if (game->undo_type == MOON_STRUCT_FOUNDATION_PAD) {
            if (!moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_FOUNDATION_PAD, game->undo_x, game->undo_y)) {
                notify(game, "CANNOT UNDO", 1500);
                game->undo_kind = MOON_UNDO_NONE;
                return;
            }
            game->resources.regolith_stored_kg -= give_back;
        } else {
            if (s || !moon_structure_place(&game->grid, &game->structs, (moon_structure_type_t)game->undo_type,
                                          game->undo_x, game->undo_y)) {
                notify(game, "CANNOT UNDO", 1500);
                game->undo_kind = MOON_UNDO_NONE;
                return;
            }
            game->resources.regolith_stored_kg -= give_back;
        }
    }
    moon_resources_solve_power_grid(&game->grid, &game->structs);
    select_tile(game, game->undo_x, game->undo_y);
    game->undo_kind = MOON_UNDO_NONE;
}

static void toggle_pause(moon_game_t *game) {
    game->paused = !game->paused;
    game->state = game->paused ? MOON_STATE_PAUSED : MOON_STATE_PLAYING;
}

static void set_speed(moon_game_t *game, int speed) {
    if (speed < 1) speed = 1;
    if (speed > 4) speed = 4;
    game->speed = (uint8_t)speed;
}

static void toggle_texture(moon_game_t *game) {
    game->grid.textured_terrain = !game->grid.textured_terrain;
    notify(game, game->grid.textured_terrain ? "TERRAIN: TEXTURED (HIGH QUALITY)"
                                             : "TERRAIN: FLAT SHADED (FAST RETRO)", 2500);
}

/* ---- Brush: a build type or recycle that stays armed for repeated use ---- */

/* Category 0: Power & Energy Storage
 * Category 1: Life Support & Cryogenics
 * Category 2: Spaceport & Logistics */
static const moon_structure_type_t s_build_buttons[MOON_CAT_COUNT][6] = {
    [MOON_CAT_POWER] = {
        MOON_STRUCT_SOLAR_ARRAY,
        MOON_STRUCT_BATTERY_BANK,
        MOON_STRUCT_POWER_CONDUIT,
        MOON_STRUCT_NONE,
        MOON_STRUCT_NONE,
        MOON_STRUCT_NONE
    },
    [MOON_CAT_LIFECRYO] = {
        MOON_STRUCT_O2_EXTRACTOR,
        MOON_STRUCT_HABITAT_DOME,
        MOON_STRUCT_HYDROLOX_PLANT,
        MOON_STRUCT_CRYO_DEWAR,
        MOON_STRUCT_POWER_CONDUIT,
        MOON_STRUCT_NONE
    },
    [MOON_CAT_LOGISTICS] = {
        MOON_STRUCT_REGOLITH_PLANT,
        MOON_STRUCT_FOUNDATION_PAD,
        MOON_STRUCT_LANDING_PAD,
        MOON_STRUCT_POWER_CONDUIT,
        MOON_STRUCT_NONE,
        MOON_STRUCT_NONE
    }
};

static inline moon_structure_type_t get_build_button_type(const moon_game_t *game, int idx) {
    uint8_t cat = game->build_category >= MOON_CAT_COUNT ? 0 : game->build_category;
    return s_build_buttons[cat][idx];
}

/* Slot of the armed brush (1..7) or -1 */
static int brush_slot(const moon_game_t *game) {
    if (game->demolish_tool_active) return SLOT_RECYCLE;
    for (int i = 0; i < 6; i++) {
        moon_structure_type_t t = get_build_button_type(game, i);
        if (t != MOON_STRUCT_NONE && game->structs.selected_build_type == t) return i + 1;
    }
    return -1;
}

static void brush_set(moon_game_t *game, int slot) {
    game->structs.selected_build_type = MOON_STRUCT_NONE;
    game->demolish_tool_active = false;
    if (slot == SLOT_RECYCLE) game->demolish_tool_active = true;
    else if (slot >= 1 && slot <= 6) {
        moon_structure_type_t t = get_build_button_type(game, slot - 1);
        if (t != MOON_STRUCT_NONE) game->structs.selected_build_type = t;
    }
}

static void brush_apply(moon_game_t *game) {
    int slot = brush_slot(game);
    if (slot == SLOT_RECYCLE) recycle_at(game, game->selected_tile_x, game->selected_tile_y);
    else if (slot >= 1) {
        moon_structure_type_t t = get_build_button_type(game, slot - 1);
        if (t != MOON_STRUCT_NONE) build_at_selection(game, t);
    }
}

static void toggle_build_category(moon_game_t *game) {
    game->build_category = (game->build_category + 1) % MOON_CAT_COUNT;
    game->dirty = MOON_DIRTY_WORLD;
    const char *msgs[MOON_CAT_COUNT] = {
        "BUILD SHEET: POWER & STORAGE",
        "BUILD SHEET: LIFE SUPPORT & CRYO",
        "BUILD SHEET: SPACEPORT & LOGISTICS"
    };
    notify(game, msgs[game->build_category], 2500);
}

/* A build / recycle / standby button or key. One press acts on the selected tile;
 * pressing the same one again with nothing in between arms it as a brush, which
 * then acts on every tile selected until it is cancelled. */
static void press_slot(moon_game_t *game, int slot) {
    if (slot == SLOT_STANDBY) {
        game->last_slot = -1;
        toggle_standby_selected(game);
        return;
    }
    if (slot >= 1 && slot <= 6) {
        if (get_build_button_type(game, slot - 1) == MOON_STRUCT_NONE) return;
    }
    int armed = brush_slot(game);
    bool again = (game->last_slot == slot);
    game->last_slot = slot;

    if (armed >= 0) {
        brush_set(game, armed == slot ? -1 : slot);   /* switch or end the brush */
        if (armed != slot) game->last_slot = -1;
        return;
    }
    if (again) {
        brush_set(game, slot);
        game->last_slot = -1;
        notify(game, "BRUSH ARMED - ESC ENDS", 2000);
        return;
    }
    if (slot == SLOT_RECYCLE) recycle_at(game, game->selected_tile_x, game->selected_tile_y);
    else build_at_selection(game, get_build_button_type(game, slot - 1));
}

/* ---- Menu ---- */

/* Rows of the menu panel a page uses: the end-of-game pages keep their summary in
 * the rows above the buttons */
static int menu_item_count(const moon_game_t *game) {
    switch (game->menu_page) {
    case MOON_MENU_MAIN:         return MOON_UI_MENU_ITEMS;
    case MOON_MENU_VICTORY:
    case MOON_MENU_VICTORY_ACT2: return 3;
    case MOON_MENU_TITLE:        return 3;   /* the three levels */
    case MOON_MENU_NEW_GAME:     return 4;   /* three levels and Back */
    default:                     return 2;
    }
}

static int menu_first_row(const moon_game_t *game) {
    switch (game->menu_page) {
    case MOON_MENU_GAME_OVER:    return 2;
    case MOON_MENU_VICTORY:
    case MOON_MENU_VICTORY_ACT2: return 1;
    case MOON_MENU_TITLE:        return 1;
    default:                     return 0;
    }
}

static const moon_ui_rect_t *menu_item_rect(const moon_game_t *game, int i) {
    return &game->ui.menu_item[menu_first_row(game) + i];
}

static void menu_open(moon_game_t *game) {
    game->menu_page = MOON_MENU_MAIN;
    game->menu_sel = 0;
    game->pointer_down = false;
}

static void open_new_game(moon_game_t *game, int from_page) {
    game->menu_return = (uint8_t)from_page;
    game->menu_page = MOON_MENU_NEW_GAME;
    game->menu_sel = game->difficulty;
}

static void menu_activate(moon_game_t *game, int item) {
    switch (game->menu_page) {
    case MOON_MENU_MAIN:
        if (item == 0) game->menu_page = MOON_MENU_CLOSED;
        else if (item == 1) toggle_texture(game);
        else if (item == 2) { open_new_game(game, MOON_MENU_MAIN); }
        else if (item == 3) { game->menu_page = MOON_MENU_CONFIRM_QUIT; game->menu_sel = 1; }
        break;
    case MOON_MENU_CONFIRM_QUIT:
        if (item == 0) game->running = false;
        else { game->menu_page = MOON_MENU_MAIN; game->menu_sel = 3; }
        break;
    case MOON_MENU_NEW_GAME:
        if (item < MOON_DIFFICULTY_COUNT) {
            game->difficulty_request = (uint8_t)(item + 1);
            game_reset(game, game->screen_w, game->screen_h);
            game->landing_active = true;
            game->landing_elapsed_ms = 0;
            game->landing_y_offset = 260;
        } else {
            game->menu_page = game->menu_return;
            game->menu_sel = (game->menu_return == MOON_MENU_MAIN) ? 2 : 0;
        }
        break;
    case MOON_MENU_TITLE:
        /* Same map, the chosen level: the seed was picked at launch */
        game->difficulty_request = (uint8_t)(item + 1);
        game->keep_seed = true;
        game_reset(game, game->screen_w, game->screen_h);
        game->landing_active = true;
        game->landing_elapsed_ms = 0;
        game->landing_y_offset = 260;
        break;
    case MOON_MENU_GAME_OVER:
        if (item == 0) open_new_game(game, MOON_MENU_GAME_OVER);
        else game->running = false;
        break;
    case MOON_MENU_VICTORY:
        if (item == 0) {
            game->victory_claimed = true;
            game->menu_page = MOON_MENU_CLOSED;
            game->state = game->paused ? MOON_STATE_PAUSED : MOON_STATE_PLAYING;
        } else if (item == 1) open_new_game(game, MOON_MENU_VICTORY);
        else game->running = false;
        break;
    case MOON_MENU_VICTORY_ACT2:
        if (item == 0) {
            game->act2_victory = true;
            game->act2_victory_claimed = true;
            game->menu_page = MOON_MENU_CLOSED;
            game->state = game->paused ? MOON_STATE_PAUSED : MOON_STATE_PLAYING;
        } else if (item == 1) open_new_game(game, MOON_MENU_VICTORY_ACT2);
        else game->running = false;
        break;
    default:
        break;
    }
}

/* Same on every device: back out of the innermost thing (menu page, brush, inspector).
 * Returns false when there was nothing to back out of. */
static bool cancel_action(moon_game_t *game) {
    if (game->menu_page == MOON_MENU_GAME_OVER || game->menu_page == MOON_MENU_TITLE) return true; /* the only ways out are the buttons */
    if (game->menu_page == MOON_MENU_VICTORY || game->menu_page == MOON_MENU_VICTORY_ACT2) {
        menu_activate(game, 0);                                   /* ESC continues playing */
        return true;
    }
    if (game->menu_page == MOON_MENU_CONFIRM_QUIT) {
        game->menu_page = MOON_MENU_MAIN;
        return true;
    }
    if (game->menu_page == MOON_MENU_NEW_GAME) {
        menu_activate(game, MOON_DIFFICULTY_COUNT);               /* Back */
        return true;
    }
    if (game->menu_page == MOON_MENU_MAIN) {
        game->menu_page = MOON_MENU_CLOSED;
        return true;
    }
    if (brush_slot(game) >= 0) {
        brush_set(game, -1);
        return true;
    }
    if (game->panel_open) {
        game->panel_open = false;
        return true;
    }
    return false;
}

/* ESC / Q: cancel, and with nothing to cancel open the menu (never quits directly) */
static void escape_action(moon_game_t *game) {
    if (!cancel_action(game)) menu_open(game);
}

static int32_t drag_limit_sq(const moon_game_t *game) {
    int32_t px = game->touch_input ? DRAG_LIMIT_TOUCH_PX : DRAG_LIMIT_MOUSE_PX;
    return px * px;
}

static void handle_key_down(moon_game_t *game, const moon_event_t *ev) {
    uint32_t key = ev->key;

    /* Menu keys */
    if (game->menu_page != MOON_MENU_CLOSED) {
        int n = menu_item_count(game);
        bool confirm = game->menu_page == MOON_MENU_CONFIRM_QUIT;
        bool newgame = game->menu_page == MOON_MENU_NEW_GAME;
        bool ending = game->menu_page == MOON_MENU_GAME_OVER ||
                      game->menu_page == MOON_MENU_VICTORY ||
                      game->menu_page == MOON_MENU_VICTORY_ACT2;
        if (ending) {
            /* Restart and Quit are immediate here: the run is over */
            int restart = (game->menu_page == MOON_MENU_VICTORY || game->menu_page == MOON_MENU_VICTORY_ACT2) ? 1 : 0;
            if (key == MOON_KEY_R) { menu_activate(game, restart); return; }
            if (key == MOON_KEY_Q) { menu_activate(game, restart + 1); return; }
            if (key == MOON_KEY_C && (game->menu_page == MOON_MENU_VICTORY || game->menu_page == MOON_MENU_VICTORY_ACT2)) {
                menu_activate(game, 0);
                return;
            }
        }
        if ((newgame || game->menu_page == MOON_MENU_TITLE) && key >= MOON_KEY_1 && key <= MOON_KEY_3) {
            menu_activate(game, (int)(key - MOON_KEY_1));
            return;
        }
        if (game->menu_page == MOON_MENU_TITLE && key == MOON_KEY_Q) {
            game->running = false;
            return;
        }
        switch (key) {
        case MOON_KEY_ESCAPE: case MOON_KEY_BACKSPACE:
            cancel_action(game);
            break;
        case MOON_KEY_N:
            if (confirm || newgame) cancel_action(game);
            break;
        case MOON_KEY_Y:
            if (confirm) menu_activate(game, 0);
            break;
        case MOON_KEY_T:
            if (game->menu_page == MOON_MENU_MAIN) menu_activate(game, 1);
            break;
        case MOON_KEY_R:
            if (game->menu_page == MOON_MENU_MAIN) menu_activate(game, 2);
            break;
        case MOON_KEY_Q:
            if (game->menu_page == MOON_MENU_MAIN) menu_activate(game, 3);
            break;
        case MOON_KEY_UP: case MOON_KEY_W:
            game->menu_sel = (game->menu_sel + n - 1) % n;
            break;
        case MOON_KEY_DOWN: case MOON_KEY_S: case MOON_KEY_TAB:
            game->menu_sel = (game->menu_sel + 1) % n;
            break;
        case MOON_KEY_RETURN: case MOON_KEY_SPACE:
            menu_activate(game, game->menu_sel);
            break;
        default:
            break;
        }
        return;
    }

    game->touch_input = false;
    bool slot_key = (key >= MOON_KEY_1 && key <= MOON_KEY_6) || key == MOON_KEY_R || key == MOON_KEY_E;
    if (!slot_key) game->last_slot = -1;
    int steps = (ev->mods & MOON_MOD_SHIFT) ? 8 : 1;

    switch (key) {
    case MOON_KEY_ESCAPE:
    case MOON_KEY_Q:
        escape_action(game);
        break;
    case MOON_KEY_BACKSPACE:
    case MOON_KEY_0:
        cancel_action(game);
        break;

    case MOON_KEY_1: case MOON_KEY_2: case MOON_KEY_3:
    case MOON_KEY_4: case MOON_KEY_5: case MOON_KEY_6:
        press_slot(game, (int)(key - MOON_KEY_0));
        break;
    case MOON_KEY_R: press_slot(game, SLOT_RECYCLE); break;
    case MOON_KEY_E: press_slot(game, SLOT_STANDBY); break;
    case MOON_KEY_Z: undo_last(game); break;

    case MOON_KEY_T: toggle_texture(game); break;
    case MOON_KEY_B: toggle_build_category(game); break;
    case MOON_KEY_L: {
        int32_t pad_gx = 0, pad_gy = 0;
        bool has_pad = false;
        const moon_tile_t *st = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
        if (st && st->structure_id != 0 && st->structure_id < MOON_MAX_STRUCTURES &&
            game->structs.list[st->structure_id].type == MOON_STRUCT_LANDING_PAD) {
            pad_gx = game->structs.list[st->structure_id].gx;
            pad_gy = game->structs.list[st->structure_id].gy;
            has_pad = true;
        } else {
            has_pad = moon_starship_find_pad(&game->structs, &pad_gx, &pad_gy);
        }

        if (has_pad) {
            if (game->starship.state == MOON_STARSHIP_ORBIT || game->starship.state == MOON_STARSHIP_DEPARTED) {
                if (moon_starship_call(&game->starship, pad_gx, pad_gy)) {
                    notify(game, "STARSHIP DISPATCHED: APPROACHING SPACEPORT", 3000);
                    game->dirty = MOON_DIRTY_WORLD;
                }
            } else if (game->starship.state == MOON_STARSHIP_SERVICING) {
                if (game->starship.lh2_loaded_kg >= game->starship.lh2_target_kg &&
                    game->starship.lox_loaded_kg >= game->starship.lox_target_kg) {
                    if (moon_starship_launch(&game->starship)) {
                        notify(game, "STARSHIP RETURN LAUNCH AUTHORIZED", 3000);
                        game->dirty = MOON_DIRTY_WORLD;
                    }
                } else {
                    notify(game, "REFUELING INCOMPLETE - PROPELLANT REQUIRED", 3000);
                }
            }
        } else {
            notify(game, "NO ACTIVE STARSHIP LANDING PAD AVAILABLE", 3000);
        }
        break;
    }
    case MOON_KEY_P: toggle_pause(game); break;
    case MOON_KEY_O: game->objectives_hidden = !game->objectives_hidden; break;
    case MOON_KEY_F: game->show_perf = !game->show_perf; break;
    case MOON_KEY_MINUS: set_speed(game, game->speed / 2); break;
    case MOON_KEY_PLUS:  set_speed(game, game->speed * 2); break;

    /* Arrows and WASD move the selected tile; the view follows near the border */
    case MOON_KEY_UP:    case MOON_KEY_W: move_selection(game, DIR_UP, steps); break;
    case MOON_KEY_DOWN:  case MOON_KEY_S: move_selection(game, DIR_DOWN, steps); break;
    case MOON_KEY_LEFT:  case MOON_KEY_A: move_selection(game, DIR_LEFT, steps); break;
    case MOON_KEY_RIGHT: case MOON_KEY_D: move_selection(game, DIR_RIGHT, steps); break;

    case MOON_KEY_TAB: select_next_structure(game); break;
    case MOON_KEY_RETURN:
        if (brush_slot(game) >= 0) brush_apply(game);
        else game->panel_open = !game->panel_open;
        break;
    case MOON_KEY_SPACE:
        view_toggle_home(game);
        break;
    default:
        break;
    }
}

/* ---- Pointer clicks per region ---- */

static void click_menu(moon_game_t *game, int px, int py) {
    for (int i = 0; i < menu_item_count(game); i++) {
        if (moon_ui_hit(menu_item_rect(game, i), px, py)) {
            menu_activate(game, i);
            return;
        }
    }
}

static void click_ribbon(moon_game_t *game, int slot) {
    if (slot >= 0 && slot < MOON_UI_RIBBON_SLOTS) {
        press_slot(game, slot);
        return;
    }
    game->last_slot = -1;
    if (slot == SLOT_CATEGORY) toggle_build_category(game);
    else if (slot == SLOT_PAUSE) toggle_pause(game);
    else if (slot == SLOT_SPEED) set_speed(game, game->speed >= 4 ? 1 : game->speed * 2);
    else if (slot == SLOT_MENU) menu_open(game);
}

static void click_minimap(moon_game_t *game, int px, int py) {
    int tx = px - game->ui.minimap.x;
    int ty = py - game->ui.minimap.y;
    game->glide = false;
    game->cam_x = (tx - ty) * MOON_TILE_HALF_W;
    game->cam_y = (tx + ty) * MOON_TILE_HALF_H;
    clamp_camera(game);
}

static void click_inspector(moon_game_t *game, int px, int py) {
    const moon_ui_layout_t *ui = &game->ui;
    if (moon_ui_hit(&ui->insp_close, px, py)) {
        game->panel_open = false;
        return;
    }
    const moon_structure_t *s = selected_structure(game);
    if (!s || s->type == MOON_STRUCT_LANDER) {
        if (!s) {
            const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
            if (t && (t->flags & MOON_TILE_FLAG_FOUNDATION) &&
                (game->selected_tile_x != game->grid.lander_gx || game->selected_tile_y != game->grid.lander_gy)) {
                if (moon_ui_hit(&ui->insp_recycle, px, py)) {
                    recycle_at(game, game->selected_tile_x, game->selected_tile_y);
                    game->panel_open = false;
                }
            }
        }
        return;
    }
    if (moon_ui_hit(&ui->insp_recycle, px, py)) {
        recycle_at(game, game->selected_tile_x, game->selected_tile_y);
        game->panel_open = false;
    } else if (moon_ui_hit(&ui->insp_standby, px, py)) {
        toggle_standby_selected(game);
    }
}

/* A click or tap on the map selects the tile; with a brush armed it also acts there */
static void click_map(moon_game_t *game) {
    if (!game->hover_valid) return;
    select_tile(game, game->hover_tile_x, game->hover_tile_y);
    if (brush_slot(game) >= 0) {
        brush_apply(game);
    } else {
        const moon_structure_t *s = selected_structure(game);
        if (s) game->panel_open = true;
    }
}

static void handle_event_inner(moon_game_t *game, const moon_event_t *ev) {
    if (!game || !ev) return;

    if (game->landing_active) {
        /* Pressing space, enter, or tapping skips the landing sequence */
        if (ev->type == MOON_EVENT_KEY_DOWN) {
            if (ev->key == MOON_KEY_SPACE || ev->key == MOON_KEY_RETURN) {
                if (game->landing_elapsed_ms >= 350) {
                    game->landing_active = false;
                    game->landing_y_offset = 0;
                    game->dirty = MOON_DIRTY_WORLD;
                    notify(game, "TOUCHDOWN CONFIRMED: EXPEDITION 1 SAFELY LANDED AT SOUTH POLE", 4000);
                }
                return;
            }
        } else if (ev->type == MOON_EVENT_POINTER_DOWN) {
            if (game->landing_elapsed_ms >= 350) {
                game->landing_active = false;
                game->landing_y_offset = 0;
                game->dirty = MOON_DIRTY_WORLD;
                notify(game, "TOUCHDOWN CONFIRMED: EXPEDITION 1 SAFELY LANDED AT SOUTH POLE", 4000);
            }
            return;
        } else if (ev->type == MOON_EVENT_POINTER_UP || ev->type == MOON_EVENT_POINTER_MOVE) {
            /* Absorb touch move/up events during the landing sequence */
            return;
        }
    }

    if (game->starship.state == MOON_STARSHIP_APPROACH ||
        game->starship.state == MOON_STARSHIP_RETRO ||
        game->starship.state == MOON_STARSHIP_GEAR_EXTEND) {
        if (ev->type == MOON_EVENT_KEY_DOWN && (ev->key == MOON_KEY_SPACE || ev->key == MOON_KEY_RETURN)) {
            game->starship.state = MOON_STARSHIP_SERVICING;
            game->starship.state_timer_ms = 0;
            game->starship.alt_px = 0;
            game->starship.umbilical_pct = 100;
            moon_starship_deliver_cargo(&game->starship, &game->resources);
            notify(game, "CARGO UNLOADED: +4 CREW, +100kg PARTS, +30kW REACTOR", 4000);
            game->dirty = MOON_DIRTY_WORLD;
            return;
        }
    } else if (game->starship.state == MOON_STARSHIP_ASCENT) {
        if (ev->type == MOON_EVENT_KEY_DOWN && (ev->key == MOON_KEY_SPACE || ev->key == MOON_KEY_RETURN)) {
            game->starship.state = MOON_STARSHIP_DEPARTED;
            game->starship.state_timer_ms = 0;
            game->starship.alt_px = 450;
            if (!game->starship.window_expired &&
                game->starship.lh2_loaded_kg >= game->starship.lh2_target_kg &&
                game->starship.lox_loaded_kg >= game->starship.lox_target_kg) {
                game->starship.commercial_mission_success = true;
            }
            game->dirty = MOON_DIRTY_WORLD;
            return;
        }
    }

    switch (ev->type) {
        case MOON_EVENT_QUIT:
            game->running = false;
            break;

        case MOON_EVENT_KEY_DOWN:
            handle_key_down(game, ev);
            break;

        case MOON_EVENT_POINTER_LEAVE:
            if (!game->touch_input) game->hover_valid = false;
            game->preview_slot = -1;
            break;

        case MOON_EVENT_POINTER_DOWN:
            game->touch_input = ev->touch != 0;
            game->pointer_down = true;
            game->was_dragging = false;
            game->pointer_x = ev->x;
            game->pointer_y = ev->y;
            game->pointer_button = ev->button;
            game->press_region = ui_region(game, ev->x, ev->y);
            game->drag_start_x = ev->x;
            game->drag_start_y = ev->y;
            game->cam_start_x = game->cam_x;
            game->cam_start_y = game->cam_y;
            game->press_slot = (game->press_region == UI_RIBBON) ? (int8_t)ribbon_slot_at(game, ev->x, ev->y) : -1;
            /* Holding a build button shows what it would build on the selected tile */
            game->preview_slot = (game->press_slot >= 1 && game->press_slot <= 6) ? game->press_slot : -1;
            if (game->press_region == UI_MAP) update_hover(game, ev->x, ev->y);
            else game->hover_valid = false;
            break;

        case MOON_EVENT_POINTER_UP: {
            bool had_press = game->pointer_down;
            if (had_press) {
                int32_t dx = ev->x - game->drag_start_x;
                int32_t dy = ev->y - game->drag_start_y;
                if (dx * dx + dy * dy > drag_limit_sq(game)) game->was_dragging = true;
            }
            game->pointer_down = false;
            game->touch_input = ev->touch != 0;
            game->preview_slot = -1;

            int region = ui_region(game, ev->x, ev->y);
            if (region == UI_MAP) update_hover(game, ev->x, ev->y);

            /* Only a press and release in the same region is a click */
            if (had_press && region == game->press_region) {
                if (ev->button == 3) {
                    cancel_action(game);
                } else if (ev->button == 1 && !game->was_dragging) {
                    if (region != UI_RIBBON) game->last_slot = -1;
                    switch (region) {
                    case UI_MENU:      click_menu(game, ev->x, ev->y); break;
                    case UI_UNDO:      undo_last(game); break;
                    case UI_RIBBON: {
                        int slot = ribbon_slot_at(game, ev->x, ev->y);
                        if (slot >= 0 && slot == game->press_slot) click_ribbon(game, slot);
                        break;
                    }
                    case UI_MINIMAP:   click_minimap(game, ev->x, ev->y); break;
                    case UI_INSPECTOR: click_inspector(game, ev->x, ev->y); break;
                    case UI_HUD:       break;
                    default:           click_map(game); break;
                    }
                }
            }
            game->press_slot = -1;
            if (game->touch_input || region != UI_MAP) game->hover_valid = false;
            break;
        }

        case MOON_EVENT_POINTER_MOVE: {
            game->pointer_x = ev->x;
            game->pointer_y = ev->y;
            game->touch_input = ev->touch != 0;
            int region = ui_region(game, ev->x, ev->y);
            if (game->pointer_down) {
                int32_t dx = ev->x - game->drag_start_x;
                int32_t dy = ev->y - game->drag_start_y;
                if (dx * dx + dy * dy > drag_limit_sq(game)) game->was_dragging = true;
                /* Only a drag that started on the map (left, middle or touch) pans it */
                if (game->press_region == UI_MAP && game->pointer_button != 3 && game->was_dragging) {
                    game->glide = false;
                    game->cam_x = game->cam_start_x - dx;
                    game->cam_y = game->cam_start_y - dy;
                    clamp_camera(game);
                }
                /* Sliding off a held build button cancels its preview */
                if (game->press_region == UI_RIBBON) {
                    int slot = ribbon_slot_at(game, ev->x, ev->y);
                    game->preview_slot = (slot == game->press_slot && slot >= 1 && slot <= 6) ? (int8_t)slot : -1;
                }
            } else if (!game->touch_input) {
                int slot = (region == UI_RIBBON) ? ribbon_slot_at(game, ev->x, ev->y) : -1;
                game->preview_slot = (slot >= 1 && slot <= 6) ? (int8_t)slot : -1;
            }
            if (game->touch_input) {
                /* A finger on the map previews under itself; once lifted nothing stays lit */
                if (game->pointer_down && game->press_region == UI_MAP && !game->was_dragging) {
                    update_hover(game, ev->x, ev->y);
                } else {
                    game->hover_valid = false;
                }
            } else if (region == UI_MAP) {
                update_hover(game, ev->x, ev->y);
            } else {
                game->hover_valid = false;
            }
            break;
        }

        case MOON_EVENT_SCROLL:
            /* The wheel pans the map (two-finger scroll on a trackpad) */
            pan_by(game, -(int32_t)ev->wheel_x * 32, -(int32_t)ev->wheel_y * 32);
            break;

        default:
            break;
    }
}

void moon_game_handle_event(moon_game_t *game, const moon_event_t *ev) {
    if (!game || !ev) return;
    handle_event_inner(game, ev);
    /* A moving pointer only matters when it changes what is lit or previewed, or drags the map */
    if (ev->type != MOON_EVENT_POINTER_MOVE || game->pointer_down ||
        game->hover_valid != game->shown_hover_valid ||
        (game->hover_valid && (game->hover_tile_x != game->shown_hover_x || game->hover_tile_y != game->shown_hover_y)) ||
        game->preview_slot != game->shown_preview) {
        game->dirty = MOON_DIRTY_WORLD;
    }
}

/* ---- Objectives: the tutorial path ---- */

static int count_built(const moon_game_t *game, int type, bool powered_only) {
    int n = 0;
    for (int i = 1; i < MOON_MAX_STRUCTURES; i++) {
        const moon_structure_t *s = &game->structs.list[i];
        if (s->id && s->type == type && (!powered_only || (s->active && s->powered))) n++;
    }
    return n;
}

static bool objective_done(const moon_game_t *game, int i) {
    switch (i) {
    case 0: return count_built(game, MOON_STRUCT_O2_EXTRACTOR, true) >= 1;
    case 1: return count_built(game, MOON_STRUCT_SOLAR_ARRAY, false) >= 3;
    case 2: return count_built(game, MOON_STRUCT_REGOLITH_PLANT, true) >= 1;
    case 3: return count_built(game, MOON_STRUCT_BATTERY_BANK, false) >= 2;
    case 4: return count_built(game, MOON_STRUCT_HABITAT_DOME, true) >= 1;
    case 5: return game->resources.population >= MOON_ACT1_CREW;
    default: return game->victory_claimed || game->state == MOON_STATE_VICTORY;
    }
}

/* Moves on when the current objective is met (also past several at once) */
static void objectives_update(moon_game_t *game) {
    bool advanced = false;
    while (game->objective < MOON_OBJECTIVES && objective_done(game, game->objective)) {
        game->objective++;
        advanced = true;
    }
    if (advanced) {
        notify(game, game->objective >= MOON_OBJECTIVES ? "ALL OBJECTIVES COMPLETE" : "OBJECTIVE COMPLETE", 3000);
    }
}

/* A digest of what the world pass draws that a sim tick can change: the tint step, each
 * structure's state, the battery gauge, the warnings behind the banners, the objective line,
 * the inspector and tile readout. When it has not changed only the top bar is stale. */
static uint32_t world_signature(const moon_game_t *game) {
    const moon_resources_t *r = &game->resources;
    uint32_t h = 2166136261u;
#define MIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    int sun = fixed_to_int(r->solar_intensity * 256);
    MIX(sun < 0 ? 0 : sun >= 256 ? 16 : (sun >> 4));                  /* 16 tint steps */
    int pips = 0;
    if (r->battery_capacity_kwh > 0) {
        pips = (fixed_to_int(r->battery_stored_kwh) * 5 + r->battery_capacity_kwh / 10) / r->battery_capacity_kwh;
        pips = pips < 0 ? 0 : pips > 5 ? 5 : pips;
    }
    MIX(pips);
    for (int i = 1; i < MOON_MAX_STRUCTURES; i++) {
        const moon_structure_t *s = &game->structs.list[i];
        if (s->id) MIX((i << 8) | (s->type << 3) | (s->powered << 2) | (s->active << 1) | s->depleted);
    }
    MIX((r->population == 0) | (r->warning_crew_lost << 1) | (r->warning_cold << 2) | (r->power_brownout << 3) |
        (r->warning_power << 4) | (r->warning_o2 << 5) | ((r->o2_stored_kg <= 0) << 6) |
        (r->warning_water << 7) | ((r->water_stored_l <= 0) << 8));
    MIX(game->objective);
    MIX((game->objective == 1 ? count_built(game, MOON_STRUCT_SOLAR_ARRAY, false) :
         game->objective == 3 ? count_built(game, MOON_STRUCT_BATTERY_BANK, false) :
         game->objective == 5 ? r->population : game->objective == 6 ? (int)r->lunar_day : 0));
    const moon_tile_t *st = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
    MIX(st ? st->resource_val : 0);                                  /* the tile readout and "Ore left" */
    const moon_structure_t *sel = selected_structure(game);
    if (sel) {
        MIX(moon_structure_ore_left(&game->grid, (moon_structure_type_t)sel->type, sel->gx, sel->gy));
        MIX(sel->health);
    }
    if (game->hover_valid) {
        const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid, game->hover_tile_x, game->hover_tile_y);
        MIX(ht ? ht->resource_val : 0);
    }
#undef MIX
    return h;
}

void moon_game_update(moon_game_t *game, uint32_t delta_ms) {
    if (!game) return;

    /* A stalled frame must not trigger a burst of sim ticks */
    if (delta_ms > MOON_MAX_FRAME_DELTA_MS) delta_ms = MOON_MAX_FRAME_DELTA_MS;

    game->frame_count++;

    if (game->notify_timer_ms > 0) {
        if (delta_ms >= game->notify_timer_ms) { game->notify_timer_ms = 0; game->dirty = MOON_DIRTY_WORLD; }
        else game->notify_timer_ms -= delta_ms;
    }
    if (game->undo_ms > 0) {
        game->undo_ms = (delta_ms >= game->undo_ms) ? 0 : game->undo_ms - delta_ms;
        if (game->undo_ms == 0) game->undo_kind = MOON_UNDO_NONE;
        uint8_t secs = (uint8_t)((game->undo_ms + 999) / 1000);       /* the button counts seconds down */
        if (secs != game->undo_secs_shown) { game->undo_secs_shown = secs; game->dirty = MOON_DIRTY_WORLD; }
    }

    /* The view eases toward a keyboard-selected tile (and works while paused) */
    if (game->glide) {
        int32_t dx = game->glide_x - game->cam_x, dy = game->glide_y - game->cam_y;
        int32_t step_x = (delta_ms >= 100) ? dx : dx * (int32_t)delta_ms / 100;
        int32_t step_y = (delta_ms >= 100) ? dy : dy * (int32_t)delta_ms / 100;
        if (step_x == 0 && dx != 0) step_x = dx > 0 ? 1 : -1;
        if (step_y == 0 && dy != 0) step_y = dy > 0 ? 1 : -1;
        game->cam_x += step_x;
        game->cam_y += step_y;
        clamp_camera(game);
        if (game->cam_x == game->glide_x && game->cam_y == game->glide_y) game->glide = false;
        game->dirty = MOON_DIRTY_WORLD;
    }

    /* Expedition Lander Touchdown Animation */
    if (game->landing_active) {
        game->landing_elapsed_ms += delta_ms;
        const uint32_t total_ms = 2600;
        if (game->landing_elapsed_ms >= total_ms) {
            game->landing_active = false;
            game->landing_y_offset = 0;
            notify(game, "TOUCHDOWN CONFIRMED: EXPEDITION 1 SAFELY LANDED AT SOUTH POLE", 4000);
        } else {
            /* Smooth deceleration (ease-out quadratic): y = 260 * (rem / total)^2 */
            int64_t rem = (int64_t)(total_ms - game->landing_elapsed_ms);
            game->landing_y_offset = (int32_t)((260 * rem * rem) / ((int64_t)total_ms * total_ms));
        }
        game->dirty = MOON_DIRTY_WORLD;
    }
    game->structs.lander_y_offset = game->landing_y_offset;

    /* Animated Starship Flight & Spaceport Refueling Sequence (Milestone 64.19) */
    moon_starship_update(&game->starship, &game->structs, &game->resources, delta_ms);
    if (game->starship.state != MOON_STARSHIP_ORBIT) {
        game->dirty = MOON_DIRTY_WORLD;
    }

    if (game->paused || game->menu_page != MOON_MENU_CLOSED) return;

    game->last_tick_ms += delta_ms * game->speed;
    bool ticked = false;
    while (game->last_tick_ms >= MOON_SIM_TICK_INTERVAL_MS) {
        game->last_tick_ms -= MOON_SIM_TICK_INTERVAL_MS;
        game->sim_ticks++;
        moon_resources_tick(&game->resources, &game->grid, &game->structs);
        ticked = true;
    }
    moon_game_check_end(game);
    objectives_update(game);

    if (ticked) {
        /* Numbers moved. The world pass only needs redoing if something it shows moved too. */
        uint32_t sig = world_signature(game);
        if (sig != game->world_sig || game->state == MOON_STATE_GAME_OVER || game->state == MOON_STATE_VICTORY ||
            game->menu_page != MOON_MENU_CLOSED) {
            game->world_sig = sig;
            game->dirty = MOON_DIRTY_WORLD;
        } else if (game->dirty == MOON_DIRTY_NONE) {
            game->dirty = MOON_DIRTY_HUD;
        }
    }
}

/* The run ends when the last colonist dies, or is won when Act I is complete. Either
 * way an overlay takes over (the sim is frozen while a menu page is open). */
bool moon_game_ghost_target(const moon_game_t *game, int *gx, int *gy) {
    bool brush = game->structs.selected_build_type != MOON_STRUCT_NONE || game->demolish_tool_active;
    bool preview = game->preview_slot >= 1 && game->preview_slot <= 6;
    if (!brush && !preview) return false;
    if (brush && game->hover_valid) {
        *gx = game->hover_tile_x;
        *gy = game->hover_tile_y;
        return true;
    }
    if (brush && game->touch_input) return false;
    *gx = game->selected_tile_x;
    *gy = game->selected_tile_y;
    return true;
}

void moon_game_check_end(moon_game_t *game) {
    if (game->state != MOON_STATE_PLAYING && game->state != MOON_STATE_PAUSED) return;

    bool lost = game->resources.population == 0;
    bool won_act1 = !lost && !game->victory_claimed && moon_resources_act1_complete(&game->resources);
    bool won_act2 = !lost && !game->act2_victory && game->starship.commercial_mission_success;
    if (!lost && !won_act1 && !won_act2) return;

    if (lost) {
        game->state = MOON_STATE_GAME_OVER;
        game->menu_page = MOON_MENU_GAME_OVER;
    } else if (won_act2) {
        game->starship.commercial_mission_success = false;
        game->state = MOON_STATE_VICTORY;
        game->menu_page = MOON_MENU_VICTORY_ACT2;
    } else {
        game->state = MOON_STATE_VICTORY;
        game->menu_page = MOON_MENU_VICTORY;
    }
    game->menu_sel = 0;
    game->panel_open = false;
    game->pointer_down = false;
    brush_set(game, -1);
}

static void render_inspector_panel(const moon_game_t *game, const moon_surface_t *surf) {
    if (!game->panel_open) return;

    const moon_ui_layout_t *ui = &game->ui;
    int ip_x = ui->inspector.x, ip_y = ui->inspector.y, ip_w = ui->inspector.w, ip_h = ui->inspector.h;

    /* Panel frame */
    moon_blit_fill_rect(surf, ip_x, ip_y, ip_w, ip_h, RGB565(14, 18, 24));
    moon_blit_rect_outline(surf, ip_x, ip_y, ip_w, ip_h, COLOR_LASER_CYAN);
    moon_blit_line(surf, ip_x, ip_y + 40, ip_x + ip_w - 1, ip_y + 40, COLOR_GOLD_FOIL);

    /* Close button [X] */
    const moon_ui_rect_t *cb = &ui->insp_close;
    moon_blit_fill_rect(surf, cb->x, cb->y, cb->w, cb->h, RGB565(35, 40, 50));
    moon_blit_rect_outline(surf, cb->x, cb->y, cb->w, cb->h, COLOR_ALERT_RED);
    moon_blit_text(surf, cb->x + cb->w / 2 - 4, cb->y + cb->h / 2 - 8, "X", COLOR_ALERT_RED, 0, true);

    moon_blit_text(surf, ip_x + 8, ip_y + 22, "Enter/Bksp: close", RGB565(110, 115, 125), 0, true);

    const moon_tile_t *tile = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
    if (!tile) return;

    if (tile->structure_id != 0) {
        const moon_structure_t *s = NULL;
        for (int i = 0; i < MOON_MAX_STRUCTURES; i++) {
            if (game->structs.list[i].id == tile->structure_id) {
                s = &game->structs.list[i];
                break;
            }
        }
        if (!s) return;

        const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)s->type);
        if (!def) return;

        /* Title */
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 6, COLOR_GOLD_FOIL, 0, true, "%s", def->name);

        int y0 = ip_y + 44;
        int y1 = ip_y + 60;
        int y2 = ip_y + 76;
        int y3 = ip_y + 92;
        int y4 = ip_y + 108;
        int y5 = ip_y + 124;
        int y6 = ip_y + 140;

        switch (s->type) {
        case MOON_STRUCT_HYDROLOX_PLANT: {
            int water_l = fixed_to_int(game->resources.water_stored_l);
            int lh2_cap = game->resources.lh2_capacity_kg;
            int lox_cap = game->resources.lox_capacity_kg;
            int lh2_val = fixed_to_int(game->resources.lh2_stored_kg);
            int lox_val = fixed_to_int(game->resources.lox_stored_kg);
            bool has_dewar = (lh2_cap > 0 || lox_cap > 0);
            bool space_avail = has_dewar && (lh2_val < lh2_cap || lox_val < lox_cap);
            bool brownout = game->resources.power_brownout;

            const char *st_str = "CRACKING WATER";
            uint16_t st_col = COLOR_STATUS_GREEN;
            if (!s->active) {
                st_str = "STANDBY";
                st_col = RGB565(140, 140, 150);
            } else if (!s->powered || brownout) {
                st_str = "NO POWER";
                st_col = COLOR_ALERT_RED;
            } else if (!has_dewar) {
                st_str = "NEEDS CRYO DEWAR";
                st_col = RGB565(240, 160, 40);
            } else if (water_l <= 20) {
                st_str = "LOW WATER (>20L)";
                st_col = RGB565(240, 160, 40);
            } else if (!space_avail) {
                st_str = "DEWARS FULL";
                st_col = COLOR_ENERGY_YELLOW;
            }

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            moon_blit_text_fmt(surf, ip_x + 8, y1, COLOR_ENERGY_YELLOW, 0, true, "POWER: -60 kW (Cracking)");
            moon_blit_text(surf, ip_x + 8, y2, "INPUT: Water 18 L/min", COLOR_LASER_CYAN, 0, true);
            moon_blit_text(surf, ip_x + 8, y3, "OUTPUT: +2 LH2 /min", RGB565(120, 210, 255), 0, true);
            moon_blit_text(surf, ip_x + 8, y4, "OUTPUT: +16 LOX /min", COLOR_LASER_CYAN, 0, true);
            if (has_dewar) {
                moon_blit_text_fmt(surf, ip_x + 8, y5, COLOR_REGOLITH_LIGHT, 0, true,
                                   "DEWAR: %d/%d H2 %d/%d O2", lh2_val, lh2_cap, lox_val, lox_cap);
            } else {
                moon_blit_text(surf, ip_x + 8, y5, "DEWAR: None! Build 1x1", COLOR_ALERT_RED, 0, true);
            }
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_CRYO_DEWAR: {
            int lh2_cap = 250;
            int lox_cap = 2000;
            int lh2_val = fixed_to_int(game->resources.lh2_stored_kg);
            int lox_val = fixed_to_int(game->resources.lox_stored_kg);
            bool brownout = game->resources.power_brownout;

            const char *st_str = "CRYOPRESERVED";
            uint16_t st_col = COLOR_STATUS_GREEN;
            if (!s->active) {
                st_str = "STANDBY";
                st_col = RGB565(140, 140, 150);
            } else if (!s->powered || brownout) {
                st_str = "NO CHILL / UNPWR";
                st_col = COLOR_ALERT_RED;
            }

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            moon_blit_text_fmt(surf, ip_x + 8, y1, COLOR_ENERGY_YELLOW, 0, true, "POWER: -5 kW (Active Chill)");
            moon_blit_text(surf, ip_x + 8, y2, "CAP: 250 H2, 2,000 O2 kg", RGB565(120, 210, 255), 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y3, COLOR_LUNAR_WHITE, 0, true, "STORED LH2: %d / %d kg", lh2_val, lh2_cap);
            moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_LASER_CYAN, 0, true, "STORED LOX: %d / %d kg", lox_val, lox_cap);
            moon_blit_text(surf, ip_x + 8, y5, "Rocket Propellant Store", COLOR_GOLD_FOIL, 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_REGOLITH_PLANT: {
            int left = moon_structure_ore_left(&game->grid, MOON_STRUCT_REGOLITH_PLANT, s->gx, s->gy);
            int rate = game->resources.rules.sinterer_kg_per_min;
            int stock_val = fixed_to_int(game->resources.regolith_stored_kg);
            int stock_cap = game->resources.regolith_capacity_kg;
            bool brownout = game->resources.power_brownout;
            bool full = (stock_val >= stock_cap);

            const char *st_str = "BAKING PAVERS";
            uint16_t st_col = COLOR_STATUS_GREEN;
            if (!s->active) {
                st_str = "STANDBY";
                st_col = RGB565(140, 140, 150);
            } else if (s->depleted || left == 0) {
                st_str = "QUARRY DEPLETED";
                st_col = RGB565(230, 130, 30);
            } else if (!s->powered || brownout) {
                st_str = "NO POWER";
                st_col = COLOR_ALERT_RED;
            } else if (full) {
                st_str = "SILO FULL (IDLE)";
                st_col = COLOR_ENERGY_YELLOW;
            }

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            if (!s->active || s->depleted || left == 0 || !s->powered || brownout || full) {
                moon_blit_text(surf, ip_x + 8, y1, "POWER: Idle (0 kW Demand)", COLOR_ENERGY_YELLOW, 0, true);
            } else {
                moon_blit_text_fmt(surf, ip_x + 8, y1, COLOR_ENERGY_YELLOW, 0, true, "POWER: -20 kW (Furnace)");
            }
            moon_blit_text_fmt(surf, ip_x + 8, y2, COLOR_REGOLITH_LIGHT, 0, true, "OUTPUT: +%d kg/min Pavers", rate);
            moon_blit_text_fmt(surf, ip_x + 8, y3, left ? COLOR_LASER_CYAN : RGB565(230, 130, 30), 0, true,
                               "QUARRY ORE: %d units left", left);
            moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_LUNAR_WHITE, 0, true,
                               "BASE STOCK: %d / %d kg", stock_val, stock_cap);
            moon_blit_text(surf, ip_x + 8, y5, full ? "Silo Full: Quarrying paused" : "Quarries 8 adj tiles", COLOR_GOLD_FOIL, 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_O2_EXTRACTOR: {
            int left = moon_structure_ore_left(&game->grid, MOON_STRUCT_O2_EXTRACTOR, s->gx, s->gy);
            bool brownout = game->resources.power_brownout;

            const char *st_str = "EXTRACTING";
            uint16_t st_col = COLOR_STATUS_GREEN;
            if (!s->active) {
                st_str = "STANDBY";
                st_col = RGB565(140, 140, 150);
            } else if (s->depleted || left == 0) {
                st_str = "DEPLETED";
                st_col = RGB565(230, 130, 30);
            } else if (!s->powered || brownout) {
                st_str = "NO POWER";
                st_col = COLOR_ALERT_RED;
            }

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            moon_blit_text_fmt(surf, ip_x + 8, y1, COLOR_ENERGY_YELLOW, 0, true, "POWER: -25 kW (Demand)");
            moon_blit_text(surf, ip_x + 8, y2, "OUTPUT: +20 O2, +30 H2O", COLOR_LASER_CYAN, 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y3, left ? COLOR_LASER_CYAN : RGB565(230, 130, 30), 0, true,
                               "GROUND DEPOSIT: %d", left);
            moon_blit_text(surf, ip_x + 8, y4, "Draws Ice Cold Traps", COLOR_LUNAR_WHITE, 0, true);
            moon_blit_text(surf, ip_x + 8, y5, "Footprint: 1x1 tile", RGB565(160, 165, 175), 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_HABITAT_DOME: {
            int pop = game->resources.population;
            int max_pop = game->resources.max_population;

            const char *st_str = s->active ? "PRESSURIZED" : "STANDBY";
            uint16_t st_col = s->active ? COLOR_STATUS_GREEN : RGB565(140, 140, 150);

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            moon_blit_text(surf, ip_x + 8, y1, "POWER: Passive (0 kW)", COLOR_REGOLITH_LIGHT, 0, true);
            moon_blit_text(surf, ip_x + 8, y2, "HOUSING: +8 Crew Berths", COLOR_GOLD_FOIL, 0, true);
            moon_blit_text(surf, ip_x + 8, y3, "UPKEEP: -3 O2, -5 H2O /m", COLOR_LASER_CYAN, 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_LUNAR_WHITE, 0, true, "BASE CREW: %d / %d", pop, max_pop);
            moon_blit_text(surf, ip_x + 8, y5, "Footprint: 2x2 tiles", RGB565(160, 165, 175), 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_LANDER: {
            int pop = game->resources.population;
            int max_pop = game->resources.max_population;

            moon_blit_text(surf, ip_x + 8, y0, "STATUS: [COMMAND ANCHOR]", COLOR_GOLD_FOIL, 0, true);
            moon_blit_text(surf, ip_x + 8, y1, "POWER: +10 kW (RTG)", COLOR_ENERGY_YELLOW, 0, true);
            moon_blit_text(surf, ip_x + 8, y2, "CREW BERTH: 4 Colonists", COLOR_GOLD_FOIL, 0, true);
            moon_blit_text(surf, ip_x + 8, y3, "UPKEEP: -1 O2, -2 H2O /m", COLOR_LASER_CYAN, 0, true);
            moon_blit_text(surf, ip_x + 8, y4, "BASE LOAD: -5 kW Comms", COLOR_ENERGY_YELLOW, 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y5, COLOR_LUNAR_WHITE, 0, true, "COLONY CREW: %d / %d", pop, max_pop);
            moon_blit_text(surf, ip_x + 8, y6, "EXPEDITION CORE MODULE", COLOR_GOLD_FOIL, 0, true);
            break;
        }

        case MOON_STRUCT_SOLAR_ARRAY: {
            int32_t solar_kw = fixed_to_int((int64_t)def->power_kw * game->resources.solar_intensity);
            int32_t pct = fixed_to_int(game->resources.solar_intensity * 100);

            const char *st_str = "GENERATING";
            uint16_t st_col = COLOR_STATUS_GREEN;
            if (!s->active) {
                st_str = "STANDBY";
                st_col = RGB565(140, 140, 150);
            } else if (solar_kw <= 0) {
                st_str = "LUNAR NIGHT";
                st_col = COLOR_ALERT_RED;
            } else if (solar_kw < def->power_kw / 2) {
                st_str = "LOW SUN";
                st_col = RGB565(240, 160, 40);
            }

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            moon_blit_text_fmt(surf, ip_x + 8, y1, COLOR_ENERGY_YELLOW, 0, true, "POWER: +%d kW (Max 15)", solar_kw);
            moon_blit_text_fmt(surf, ip_x + 8, y2, COLOR_LASER_CYAN, 0, true, "SUN INTENSITY: %d%%", pct);
            moon_blit_text_fmt(surf, ip_x + 8, y3, COLOR_ENERGY_YELLOW, 0, true,
                               "GRID SUPPLY: %d kW", game->resources.power_produced_kw);
            moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_ENERGY_YELLOW, 0, true,
                               "GRID DEMAND: %d kW", game->resources.power_consumed_kw);
            moon_blit_text(surf, ip_x + 8, y5, "Footprint: 1x1 tile", RGB565(160, 165, 175), 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_BATTERY_BANK: {
            int stored_kwh = fixed_to_int(game->resources.battery_stored_kwh);
            int cap_kwh = game->resources.battery_capacity_kwh;
            int net_kw = game->resources.power_net_kw;

            const char *st_str = "BALANCED";
            uint16_t st_col = COLOR_LASER_CYAN;
            if (!s->active) {
                st_str = "STANDBY";
                st_col = RGB565(140, 140, 150);
            } else if (net_kw > 0) {
                st_str = "CHARGING";
                st_col = COLOR_STATUS_GREEN;
            } else if (net_kw < 0) {
                st_str = "DISCHARGING";
                st_col = RGB565(240, 160, 40);
            }

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            moon_blit_text_fmt(surf, ip_x + 8, y1, COLOR_ENERGY_YELLOW, 0, true, "CAPACITY: 120 kWh");
            moon_blit_text_fmt(surf, ip_x + 8, y2, COLOR_ENERGY_YELLOW, 0, true,
                               "STORED: %d / %d kWh", stored_kwh, cap_kwh);
            moon_blit_text_fmt(surf, ip_x + 8, y3, net_kw >= 0 ? COLOR_STATUS_GREEN : COLOR_ALERT_RED, 0, true,
                               "GRID FLOW: %+d kW", net_kw);
            moon_blit_text(surf, ip_x + 8, y4, "Solid-State Storage", COLOR_LUNAR_WHITE, 0, true);
            moon_blit_text(surf, ip_x + 8, y5, "Footprint: 1x1 tile", RGB565(160, 165, 175), 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_POWER_CONDUIT: {
            const char *st_str = s->powered ? "ENERGIZED" : "NO GRID";
            uint16_t st_col = s->powered ? COLOR_STATUS_GREEN : COLOR_ALERT_RED;

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            moon_blit_text(surf, ip_x + 8, y1, "POWER: Superconducting", COLOR_ENERGY_YELLOW, 0, true);
            moon_blit_text(surf, ip_x + 8, y2, "Lossless Power Line", COLOR_LUNAR_WHITE, 0, true);
            moon_blit_text(surf, ip_x + 8, y3, "Distributes Energy", COLOR_REGOLITH_LIGHT, 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_LASER_CYAN, 0, true,
                               "Coords: [%02d, %02d]", s->gx, s->gy);
            moon_blit_text(surf, ip_x + 8, y5, "Footprint: 1x1 tile", RGB565(160, 165, 175), 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_FOUNDATION_PAD: {
            moon_blit_text(surf, ip_x + 8, y0, "STATUS: [FOUNDATION PAD]", COLOR_GOLD_FOIL, 0, true);
            moon_blit_text(surf, ip_x + 8, y1, "Sintered Regolith Base", COLOR_LUNAR_WHITE, 0, true);
            moon_blit_text(surf, ip_x + 8, y2, "Supports 2x2 Habitat", COLOR_ENERGY_YELLOW, 0, true);
            moon_blit_text(surf, ip_x + 8, y3, "Supports 2x2 Hydrolox", COLOR_LASER_CYAN, 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_LASER_CYAN, 0, true,
                               "Coords: [%02d, %02d]", s->gx, s->gy);
            moon_blit_text(surf, ip_x + 8, y5, "Footprint: 1x1 tile", RGB565(160, 165, 175), 0, true);
            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        case MOON_STRUCT_LANDING_PAD: {
            bool brownout = game->resources.power_brownout;
            const char *st_str = "SPACEPORT READY";
            uint16_t st_col = COLOR_STATUS_GREEN;
            if (!s->active) {
                st_str = "STANDBY";
                st_col = RGB565(140, 140, 150);
            } else if (!s->powered || brownout) {
                st_str = "UNPOWERED / NO GUID";
                st_col = COLOR_ALERT_RED;
            }

            moon_blit_text_fmt(surf, ip_x + 8, y0, st_col, 0, true, "STATUS: [%s]", st_str);
            moon_blit_text(surf, ip_x + 8, y1, "POWER: -10 kW (Guidance)", COLOR_ENERGY_YELLOW, 0, true);
            moon_blit_text(surf, ip_x + 8, y2, "APRON: 4x4 Sintered Basalt", COLOR_GOLD_FOIL, 0, true);
            if (game->starship.manifest.delivered) {
                moon_blit_text(surf, ip_x + 8, y3, "CARGO: Specialists & Reactor Active", COLOR_STATUS_GREEN, 0, true);
            } else {
                moon_blit_text(surf, ip_x + 8, y3, "BLAST DEFENSE: Berm Prot", COLOR_LUNAR_WHITE, 0, true);
            }

            const moon_starship_t *ship = &game->starship;
            switch (ship->state) {
                case MOON_STARSHIP_ORBIT:
                    moon_blit_text(surf, ip_x + 8, y4, "STARSHIP: In Cislunar Orbit", COLOR_ENERGY_YELLOW, 0, true);
                    moon_blit_text(surf, ip_x + 8, y5, "FLIGHT: Press [L] to Call", COLOR_STATUS_GREEN, 0, true);
                    break;
                case MOON_STARSHIP_APPROACH:
                    moon_blit_text(surf, ip_x + 8, y4, "STARSHIP: Orbital Approach", COLOR_LASER_CYAN, 0, true);
                    moon_blit_text_fmt(surf, ip_x + 8, y5, COLOR_LASER_CYAN, 0, true, "TELEMETRY: Alt %dm (RCS)", ship->alt_px);
                    break;
                case MOON_STARSHIP_RETRO:
                    moon_blit_text(surf, ip_x + 8, y4, "STARSHIP: Retro-Propulsion", COLOR_ALERT_RED, 0, true);
                    moon_blit_text_fmt(surf, ip_x + 8, y5, COLOR_LASER_CYAN, 0, true, "HYDROLOX BURN: Alt %dm", ship->alt_px);
                    break;
                case MOON_STARSHIP_GEAR_EXTEND:
                    moon_blit_text(surf, ip_x + 8, y4, "STARSHIP: Final Flare", COLOR_ENERGY_YELLOW, 0, true);
                    moon_blit_text(surf, ip_x + 8, y5, "LANDING GEAR: Deployed", COLOR_STATUS_GREEN, 0, true);
                    break;
                case MOON_STARSHIP_TOUCHDOWN:
                    moon_blit_text(surf, ip_x + 8, y4, "STARSHIP: Touchdown on Apron", COLOR_STATUS_GREEN, 0, true);
                    moon_blit_text(surf, ip_x + 8, y5, "ENGINES: Cutoff / Dust Skirt", COLOR_LUNAR_WHITE, 0, true);
                    break;
                case MOON_STARSHIP_SERVICING: {
                    int lh2_cur = fixed_to_int(ship->lh2_loaded_kg);
                    int lh2_tgt = fixed_to_int(ship->lh2_target_kg);
                    int lox_cur = fixed_to_int(ship->lox_loaded_kg);
                    int lox_tgt = fixed_to_int(ship->lox_target_kg);
                    int tot_cur = lh2_cur + lox_cur;
                    int tot_tgt = lh2_tgt + lox_tgt;
                    int pct = (tot_tgt > 0) ? (tot_cur * 100) / tot_tgt : 0;
                    int rem_s = (int)(ship->turnaround_timer_ms / 1000);
                    uint16_t timer_col = (rem_s < 30) ? COLOR_ALERT_RED : COLOR_ENERGY_YELLOW;

                    if (pct >= 100) {
                        moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_STATUS_GREEN, 0, true, "REFUELING: 100%% (WINDOW: %ds)", rem_s);
                        moon_blit_text(surf, ip_x + 8, y5, "RETURN: Press [L] to Launch", COLOR_STATUS_GREEN, 0, true);
                    } else if (ship->umbilical_pct < 100) {
                        moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_LASER_CYAN, 0, true, "UMBILICAL: Docking (%d%%)", ship->umbilical_pct);
                        moon_blit_text_fmt(surf, ip_x + 8, y5, timer_col, 0, true, "WINDOW: %ds | Coupling Mast", rem_s);
                    } else {
                        moon_blit_text_fmt(surf, ip_x + 8, y4, timer_col, 0, true, "WINDOW: %ds | %d%% Loaded", rem_s, pct);
                        moon_blit_text_fmt(surf, ip_x + 8, y5, COLOR_GOLD_FOIL, 0, true, "H2: %d/%dkg O2: %d/%dkg",
                                           lh2_cur, lh2_tgt, lox_cur, lox_tgt);
                    }
                    break;
                }
                case MOON_STARSHIP_PRELAUNCH:
                    moon_blit_text(surf, ip_x + 8, y4, "STARSHIP: Pre-Ignition Warmup", COLOR_ENERGY_YELLOW, 0, true);
                    moon_blit_text(surf, ip_x + 8, y5, "UMBILICAL: Disconnected / Safe", COLOR_STATUS_GREEN, 0, true);
                    break;
                case MOON_STARSHIP_ASCENT:
                    moon_blit_text(surf, ip_x + 8, y4, "STARSHIP: Vertical Ascent Burn", COLOR_ALERT_RED, 0, true);
                    moon_blit_text_fmt(surf, ip_x + 8, y5, COLOR_LASER_CYAN, 0, true, "RAPTOR ASCENT: Alt %dm", ship->alt_px);
                    break;
                case MOON_STARSHIP_DEPARTED:
                    moon_blit_text(surf, ip_x + 8, y4, "STARSHIP: Orbit Achieved", COLOR_STATUS_GREEN, 0, true);
                    moon_blit_text(surf, ip_x + 8, y5, "TRANSIT: Return to Earth", COLOR_ENERGY_YELLOW, 0, true);
                    break;
            }

            moon_blit_text_fmt(surf, ip_x + 8, y6, COLOR_STATUS_GREEN, 0, true,
                               "Integrity: %d%%", s->health);
            break;
        }

        default: {
            const char *status_str = !s->active ? "STANDBY" : s->depleted ? "DEPLETED" : (s->powered ? "ONLINE" : "NO POWER");
            uint16_t status_col = !s->active ? RGB565(140, 140, 150) : s->depleted ? RGB565(230, 130, 30) : (s->powered ? COLOR_STATUS_GREEN : COLOR_ALERT_RED);
            moon_blit_text_fmt(surf, ip_x + 8, y0, status_col, 0, true, "STATUS: [%s]", status_str);
            if (def->power_kw > 0) {
                moon_blit_text_fmt(surf, ip_x + 8, y1, COLOR_ENERGY_YELLOW, 0, true, "POWER: +%d kW (Gen)", def->power_kw);
            } else if (def->power_kw < 0) {
                moon_blit_text_fmt(surf, ip_x + 8, y1, COLOR_ENERGY_YELLOW, 0, true, "POWER: %d kW (Demand)", def->power_kw);
            } else {
                moon_blit_text(surf, ip_x + 8, y1, "POWER: Passive", COLOR_REGOLITH_LIGHT, 0, true);
            }
            moon_blit_text_fmt(surf, ip_x + 8, y2, COLOR_LASER_CYAN, 0, true, "Coords: [%02d, %02d]", s->gx, s->gy);
            moon_blit_text_fmt(surf, ip_x + 8, y3, RGB565(160, 165, 175), 0, true, "Footprint: %dx%d tiles", s->footprint_w, s->footprint_h);
            moon_blit_text_fmt(surf, ip_x + 8, y4, COLOR_STATUS_GREEN, 0, true, "Integrity: %d%%", s->health);
            break;
        }
        }

        
        if (s->type != MOON_STRUCT_LANDER) {
            const moon_ui_rect_t *rb = &ui->insp_recycle;
            moon_blit_fill_rect(surf, rb->x, rb->y, rb->w, rb->h, RGB565(45, 20, 20));
            moon_blit_rect_outline(surf, rb->x, rb->y, rb->w, rb->h, COLOR_ALERT_RED);
            moon_blit_text(surf, rb->x + 6, rb->y + rb->h / 2 - 8, ui->narrow ? "R:RECYCLE" : "R:RECYCLE 50%", COLOR_LUNAR_WHITE, 0, true);

            const moon_ui_rect_t *ab = &ui->insp_standby;
            uint16_t act_bg = s->active ? RGB565(20, 45, 30) : RGB565(45, 40, 20);
            uint16_t act_border = s->active ? COLOR_STATUS_GREEN : COLOR_ENERGY_YELLOW;
            moon_blit_fill_rect(surf, ab->x, ab->y, ab->w, ab->h, act_bg);
            moon_blit_rect_outline(surf, ab->x, ab->y, ab->w, ab->h, act_border);
            moon_blit_text(surf, ab->x + 6, ab->y + ab->h / 2 - 8, s->active ? "E:STANDBY" : "E:ACTIVATE", COLOR_LUNAR_WHITE, 0, true);
        } else {
            moon_blit_text(surf, ip_x + 8, ip_y + ip_h - 34, "EXPEDITION COMMAND ANCHOR", COLOR_GOLD_FOIL, 0, true);
        }
    } else {
        /* Terrain Inspector */
        const char *tname = get_terrain_name((moon_terrain_type_t)tile->terrain);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 6, COLOR_GOLD_FOIL, 0, true, "TERRAIN: %s", tname);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 50, COLOR_LASER_CYAN, 0, true,
                           "Coordinates: [%02d, %02d]", game->selected_tile_x, game->selected_tile_y);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 70, COLOR_REGOLITH_LIGHT, 0, true,
                           "Elevation Level: %d", tile->elevation);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 90, COLOR_GOLD_FOIL, 0, true,
                           "Mineral Richness: %d%%", tile->resource_val);

        if (tile->flags & MOON_TILE_FLAG_FOUNDATION) {
            moon_blit_text(surf, ip_x + 8, ip_y + 118, "Sintered foundation pad.", COLOR_GOLD_FOIL, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 134, "Load-bearing base ready.", COLOR_LUNAR_WHITE, 0, true);
            if (game->selected_tile_x != game->grid.lander_gx || game->selected_tile_y != game->grid.lander_gy) {
                const moon_ui_rect_t *rb = &ui->insp_recycle;
                moon_blit_fill_rect(surf, rb->x, rb->y, rb->w, rb->h, RGB565(45, 20, 20));
                moon_blit_rect_outline(surf, rb->x, rb->y, rb->w, rb->h, COLOR_ALERT_RED);
                moon_blit_text(surf, rb->x + 6, rb->y + rb->h / 2 - 8, ui->narrow ? "R:RECYCLE" : "R:RECYCLE 50%", COLOR_LUNAR_WHITE, 0, true);
            }
        } else if (tile->terrain == MOON_TERRAIN_ICE_DEPOSIT) {
            moon_blit_text(surf, ip_x + 8, ip_y + 118, "Water ice cold trap.", COLOR_LASER_CYAN, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 134, "Ideal for O2 Extractor.", COLOR_LUNAR_WHITE, 0, true);
        } else if (tile->terrain == MOON_TERRAIN_BASALT_MARE) {
            moon_blit_text(surf, ip_x + 8, ip_y + 118, "Volcanic titanium mare.", COLOR_REGOLITH_LIGHT, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 134, "Suitable for O2 / Sinterer.", COLOR_LUNAR_WHITE, 0, true);
        } else if (tile->terrain == MOON_TERRAIN_CRATER_RIM) {
            moon_blit_text(surf, ip_x + 8, ip_y + 118, "Elevated crater rim ridge.", COLOR_ALERT_RED, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 134, "Terrain too steep to build.", RGB565(160, 160, 160), 0, true);
        } else {
            moon_blit_text(surf, ip_x + 8, ip_y + 118, "Flat regolith plain.", COLOR_REGOLITH_LIGHT, 0, true);
            moon_blit_text(surf, ip_x + 8, ip_y + 134, "Prime foundation site.", COLOR_LUNAR_WHITE, 0, true);
        }
    }
}

static void draw_button(const moon_surface_t *surf, const moon_ui_rect_t *r, const char *label,
                        uint16_t bg, uint16_t border, uint16_t text) {
    moon_blit_fill_rect(surf, r->x, r->y, r->w, r->h, bg);
    moon_blit_rect_outline(surf, r->x, r->y, r->w, r->h, border);
    int len = 0;
    while (label[len]) len++;
    int tx = r->x + (r->w - len * MOON_FONT_W) / 2;
    if (tx < r->x + 2) tx = r->x + 2;
    moon_blit_text(surf, tx, r->y + (r->h - MOON_FONT_H) / 2, label, text, 0, true);
}

static void render_build_ribbon(const moon_game_t *game, const moon_surface_t *surf) {
    const moon_ui_layout_t *ui = &game->ui;
    const bool narrow = ui->narrow;

    /* Ribbon Background */
    moon_blit_fill_rect(surf, 0, ui->ribbon_bar.y, ui->ribbon_bar.w, ui->ribbon_bar.h, RGB565(10, 12, 16));
    moon_blit_line(surf, 0, ui->ribbon_bar.y, ui->ribbon_bar.w - 1, ui->ribbon_bar.y, COLOR_GOLD_FOIL);

    static const char *labels[MOON_CAT_COUNT][8] = {
        [MOON_CAT_POWER] = {
            "E:STANDBY",
            "1:SOLAR 10t", "2:BATT  20t", "3:CABLE  2t",
            "-", "-", "-",
            "R:RECYCLE"
        },
        [MOON_CAT_LIFECRYO] = {
            "E:STANDBY",
            "1:OXY   25t", "2:HAB   50t", "3:HYDRO 80t",
            "4:DEWAR 35t", "5:CABLE  2t", "-",
            "R:RECYCLE"
        },
        [MOON_CAT_LOGISTICS] = {
            "E:STANDBY",
            "1:SINT  15t", "2:PAVER  5t", "3:PAD  120t",
            "4:CABLE  2t", "-", "-",
            "R:RECYCLE"
        }
    };

    static const char *labels_narrow[MOON_CAT_COUNT][8] = {
        [MOON_CAT_POWER] = {
            "E:STBY",
            "1:SOL", "2:BAT", "3:WIRE",
            "-", "-", "-",
            "R:RECY"
        },
        [MOON_CAT_LIFECRYO] = {
            "E:STBY",
            "1:OXY", "2:HAB", "3:HYD",
            "4:DEW", "5:WIRE", "-",
            "R:RECY"
        },
        [MOON_CAT_LOGISTICS] = {
            "E:STBY",
            "1:SINT", "2:PAVR", "3:PAD",
            "4:WIRE", "-", "-",
            "R:RECY"
        }
    };

    const moon_structure_t *sel = selected_structure(game);
    int armed = brush_slot(game);
    uint8_t cat = game->build_category >= MOON_CAT_COUNT ? 0 : game->build_category;
    for (int i = 0; i < MOON_UI_RIBBON_SLOTS; i++) {
        /* Enabled when the button could act on the selected tile right now */
        bool enabled;
        if (i >= 1 && i <= 6) {
            moon_structure_type_t t = s_build_buttons[cat][i - 1];
            enabled = (t != MOON_STRUCT_NONE) && build_possible(game, t, NULL);
        } else {
            enabled = (sel && sel->type != MOON_STRUCT_LANDER);
            if (i == SLOT_RECYCLE && !enabled) {
                const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
                if (t && (t->flags & MOON_TILE_FLAG_FOUNDATION) &&
                    (game->selected_tile_x != game->grid.lander_gx || game->selected_tile_y != game->grid.lander_gy)) {
                    enabled = true;
                }
            }
        }
        bool selected = (i == armed);

        uint16_t bg = selected ? (i == SLOT_RECYCLE ? RGB565(55, 20, 20) : RGB565(30, 45, 60)) : RGB565(20, 24, 30);
        uint16_t border = selected ? (i == SLOT_RECYCLE ? COLOR_ALERT_RED : COLOR_LASER_CYAN)
                                   : (enabled ? RGB565(60, 65, 75) : RGB565(35, 38, 45));
        uint16_t text_col = selected ? COLOR_LUNAR_WHITE : (enabled ? RGB565(170, 175, 185) : RGB565(80, 85, 95));
        const char *label = narrow ? labels_narrow[cat][i] : labels[cat][i];
        if (i == SLOT_STANDBY && sel && !sel->active) label = narrow ? "E:ACT" : "E:ACTIVATE";
        draw_button(surf, &ui->ribbon[i], label, bg, border, text_col);
    }

    /* Category, pause, speed and menu */
    static const char *cat_labels[MOON_CAT_COUNT] = { "B:POWER", "B:LIFE/CRYO", "B:LOGISTICS" };
    static const char *cat_labels_narrow[MOON_CAT_COUNT] = { "B:POWR", "B:LIFE", "B:LOGI" };
    const char *cat_label = narrow ? cat_labels_narrow[cat] : cat_labels[cat];
    uint16_t cat_bg = (cat == MOON_CAT_POWER) ? RGB565(35, 30, 15) :
                      (cat == MOON_CAT_LIFECRYO) ? RGB565(20, 40, 55) : RGB565(35, 30, 20);
    uint16_t cat_border = (cat == MOON_CAT_POWER) ? COLOR_ENERGY_YELLOW :
                          (cat == MOON_CAT_LIFECRYO) ? COLOR_LASER_CYAN : COLOR_GOLD_FOIL;
    draw_button(surf, &ui->cat_btn, cat_label, cat_bg, cat_border, COLOR_LUNAR_WHITE);

    const char *spd = game->speed == 4 ? "+/-:4x" : game->speed == 2 ? "+/-:2x" : "+/-:1x";
    draw_button(surf, &ui->pause_btn, game->paused ? "P:RUN" : "P:PAUSE",
                game->paused ? RGB565(55, 45, 15) : RGB565(20, 24, 30),
                game->paused ? COLOR_ENERGY_YELLOW : RGB565(60, 65, 75), COLOR_LUNAR_WHITE);
    draw_button(surf, &ui->speed_btn, spd,
                game->speed > 1 ? RGB565(20, 40, 55) : RGB565(20, 24, 30),
                game->speed > 1 ? COLOR_LASER_CYAN : RGB565(60, 65, 75), COLOR_LUNAR_WHITE);
    draw_button(surf, &ui->menu_btn, "ESC:MENU", RGB565(20, 24, 30), RGB565(60, 65, 75), COLOR_LUNAR_WHITE);
}

/* 24 h dial: noon at the top, dawn left (06:00), dusk right (18:00), the night below */
static void render_clock(const moon_game_t *game, const moon_surface_t *surf) {
    const moon_ui_layout_t *ui = &game->ui;
    const int cx = ui->clock_cx, cy = ui->clock_cy, r = ui->clock_r;
    moon_clock_t clk = moon_resources_clock(&game->resources);

    for (int deg = 0; deg < 360; deg += 6) {
        int32_t s = fixed_sin_deg(deg), c = fixed_cos_deg(deg);
        int x = cx + (int)(((int64_t)r * s) >> FIXED_SHIFT);
        int y = cy - (int)(((int64_t)r * c) >> FIXED_SHIFT);
        bool day_arc = (deg < 90 || deg >= 270);
        moon_blit_fill_rect(surf, x - 1, y - 1, 2, 2, day_arc ? COLOR_ENERGY_YELLOW : RGB565(60, 90, 170));
    }
    int hand = ((clk.minutes - 720) * 360 / 1440 + 360) % 360;
    int32_t hs = fixed_sin_deg(hand), hc = fixed_cos_deg(hand);
    int hx = cx + (int)(((int64_t)(r - 3) * hs) >> FIXED_SHIFT);
    int hy = cy - (int)(((int64_t)(r - 3) * hc) >> FIXED_SHIFT);
    moon_blit_line(surf, cx, cy, hx, hy, COLOR_LUNAR_WHITE);
    moon_blit_fill_rect(surf, hx - 1, hy - 1, 3, 3, game->resources.is_night ? COLOR_LASER_CYAN : COLOR_GOLD_FOIL);
}

static void render_undo_button(const moon_game_t *game, const moon_surface_t *surf) {
    if (!undo_available(game)) return;
    unsigned secs = (game->undo_ms + 999) / 1000; /* 1..10 */
    char label[] = "Z:UNDO   s";
    label[7] = (secs >= 10) ? '1' : ' ';
    label[8] = (char)('0' + secs % 10);
    draw_button(surf, &game->ui.undo_btn, label, RGB565(35, 30, 15), COLOR_ENERGY_YELLOW, COLOR_LUNAR_WHITE);
}

static void render_objective(const moon_game_t *game, const moon_surface_t *surf) {
    if (game->objectives_hidden) return;
    const bool narrow = game->ui.narrow;
    const moon_resources_t *res = &game->resources;
    char text[72];
    int n = game->objective;
    int have = 0;
    const char *fmt;
    switch (n) {
    case 0: fmt = narrow ? "Extractor (LIFE 1) on ice" : "Build Oxygen Extractor (LIFE 1) beside ice/mare"; break;
    case 1: have = count_built(game, MOON_STRUCT_SOLAR_ARRAY, false);
            fmt = narrow ? "Solar Arrays (1): %d of 3" : "Power the base: Solar Arrays (1), %d of 3"; break;
    case 2: fmt = narrow ? "Sinterer (LOGI 1) for ore" : "Build Regolith Sinterer (LOGISTICS 1) for regolith"; break;
    case 3: have = count_built(game, MOON_STRUCT_BATTERY_BANK, false);
            fmt = narrow ? "Batteries (2): %d of 2" : "Store the night: a second Battery Bank (2), %d of 2"; break;
    case 4: fmt = narrow ? "Habitat (LIFE 2) for crew" : "Build a Habitat Dome (LIFE 2) for more crew"; break;
    case 5: have = res->population;
            fmt = narrow ? "Grow the crew: %d of 8" : "Grow the crew to 8 colonists: %d now"; break;
    case 6: have = (int)res->lunar_day;
            fmt = narrow ? "Act I: day %d of 6" : "Act I: reach day 6 with water and oxygen to spare (day %d)"; break;
    default:
        if (!game->act2_victory) {
            if (count_built(game, MOON_STRUCT_HYDROLOX_PLANT, true) == 0) {
                fmt = narrow ? "Act II: Build Hydrolox (LIFE 3)" : "Act II: Pave 2x2 foundations & build Hydrolox Plant (LIFE 3)";
            } else if (count_built(game, MOON_STRUCT_CRYO_DEWAR, true) == 0) {
                fmt = narrow ? "Act II: Build Cryo Dewar (LIFE 4)" : "Act II: Build Cryo Dewar storage for H2/LOX (LIFE 4)";
            } else if (count_built(game, MOON_STRUCT_LANDING_PAD, true) == 0) {
                fmt = narrow ? "Act II: Pave 4x4 Pad (LOGI 3)" : "Act II: Pave 4x4 foundation apron & build Landing Pad (LOGISTICS 3)";
            } else if (game->starship.state == MOON_STARSHIP_ORBIT) {
                fmt = narrow ? "Act II: Press [L] to Call Ship" : "Act II: Press [L] or inspect Landing Pad to call Starship";
            } else if (game->starship.state == MOON_STARSHIP_SERVICING) {
                fmt = narrow ? "Act II: Refuel Ship & Launch [L]" : "Act II: Refuel Starship (250kg H2, 2000kg O2) & Launch [L]";
            } else {
                fmt = narrow ? "Act II: Starship in flight" : "Act II: Starship transit to Earth in progress";
            }
        } else {
            fmt = narrow ? "Act II Complete! (O hides)" : "Act II Complete: Spaceport operational! (O hides this)";
        }
        break;
    }
    int len = 0;
    const char *f = fmt;
    char pre[8] = { 0 };
    if (n < MOON_OBJECTIVES) { pre[0] = (char)('1' + n); pre[1] = '/'; pre[2] = '7'; pre[3] = ' '; }
    while (pre[len]) { text[len] = pre[len]; len++; }
    /* a single %d at most */
    for (; *f && len < (int)sizeof(text) - 12; f++) {
        if (f[0] == '%' && f[1] == 'd') {
            int v = have, digits = 1;
            for (int t = v; t >= 10; t /= 10) digits++;
            for (int d = digits - 1; d >= 0; d--) { text[len + d] = (char)('0' + v % 10); v /= 10; }
            len += digits;
            f++;
        } else {
            text[len++] = *f;
        }
    }
    text[len] = '\0';
    const moon_ui_rect_t *hb = &game->ui.hover_badge;
    int w = len * MOON_FONT_W + 16, y = hb->y - 24;
    bool done = (n >= MOON_OBJECTIVES && game->act2_victory);
    moon_blit_fill_rect(surf, hb->x, y, w, 20, RGB565(20, 26, 20));
    moon_blit_rect_outline(surf, hb->x, y, w, 20, done ? COLOR_STATUS_GREEN : COLOR_GOLD_FOIL);
    moon_blit_text(surf, hb->x + 8, y + 2, text, done ? COLOR_STATUS_GREEN : COLOR_LUNAR_WHITE, 0, true);
}

static const char *cause_name(int cause) {
    switch (cause) {
    case MOON_CAUSE_OXYGEN:   return "NO OXYGEN";
    case MOON_CAUSE_WATER:    return "NO WATER";
    case MOON_CAUSE_BLACKOUT: return "POWER BLACKOUT";
    default:                  return "UNKNOWN";
    }
}

static void render_menu(const moon_game_t *game, const moon_surface_t *surf) {
    if (game->menu_page == MOON_MENU_CLOSED) return;
    const moon_ui_layout_t *ui = &game->ui;
    const moon_ui_rect_t *p = &ui->menu_panel;
    const moon_resources_t *res = &game->resources;

    moon_blit_tint_rect(surf, 0, 0, surf->width, surf->height, 110, 110, 120);
    moon_blit_fill_rect(surf, p->x, p->y, p->w, p->h, RGB565(14, 18, 24));
    moon_blit_rect_outline(surf, p->x, p->y, p->w, p->h,
                           game->menu_page == MOON_MENU_GAME_OVER ? COLOR_ALERT_RED : COLOR_GOLD_FOIL);

    const char *items[MOON_UI_MENU_ITEMS];
    int n = menu_item_count(game);
    int tx = p->x + 16, ty = p->y + 10, row = p->y + 36;
    switch (game->menu_page) {
    case MOON_MENU_MAIN:
        moon_blit_text_fmt(surf, tx, ty, COLOR_GOLD_FOIL, 0, true, "%s  SEED %u", res->rules.name, (unsigned)game->seed);
        items[0] = "ESC:RESUME";
        items[1] = game->grid.textured_terrain ? "T:TERRAIN TEXTURED" : "T:TERRAIN FAST";
        items[2] = "R:NEW GAME";
        items[3] = "Q:QUIT";
        break;
    case MOON_MENU_NEW_GAME: {
        static const char *const labels[MOON_DIFFICULTY_COUNT] = {
            "1:CADET (EASY)", "2:ENGINEER (NORMAL)", "3:COMMANDER (HARD)"
        };
        static char text[MOON_DIFFICULTY_COUNT][24];   /* single task: the moonbase session holder */
        moon_blit_text(surf, tx, ty, game->menu_return == MOON_MENU_MAIN ? "NEW GAME? BASE IS LOST" : "NEW GAME",
                       COLOR_GOLD_FOIL, 0, true);
        for (int i = 0; i < MOON_DIFFICULTY_COUNT; i++) {
            int len = 0;
            while (labels[i][len]) { text[i][len] = labels[i][len]; len++; }
            if (i == game->difficulty) { text[i][len++] = ' '; text[i][len++] = '*'; }
            text[i][len] = '\0';
            items[i] = text[i];
        }
        items[3] = "N:BACK";
        break;
    }
    case MOON_MENU_TITLE:
        moon_blit_text(surf, tx, ty, "MOONBASE LAMBDA", COLOR_GOLD_FOIL, 0, true);
        moon_blit_text(surf, tx, row,      "South pole, day 1. 4 crew.", COLOR_LUNAR_WHITE, 0, true);
        moon_blit_text(surf, tx, row + 16, "Power, water, air, regolith.", COLOR_LUNAR_WHITE, 0, true);
        moon_blit_text(surf, tx, row + 32, "Pick a level.      Q:QUIT", COLOR_REGOLITH_LIGHT, 0, true);
        items[0] = "1:CADET (EASY)";
        items[1] = "2:ENGINEER (NORMAL)";
        items[2] = "3:COMMANDER (HARD)";
        break;
    case MOON_MENU_GAME_OVER:
        moon_blit_text_fmt(surf, tx, ty, COLOR_ALERT_RED, 0, true, "BASE LOST - %s", res->rules.name);
        moon_blit_text_fmt(surf, tx, row,       COLOR_LUNAR_WHITE, 0, true, "DAY REACHED  %d", (int)res->lunar_day);
        moon_blit_text_fmt(surf, tx, row + 18,  COLOR_LUNAR_WHITE, 0, true, "PEAK CREW    %d", (int)res->peak_population);
        moon_blit_text_fmt(surf, tx, row + 36,  COLOR_LUNAR_WHITE, 0, true, "BUILDINGS    %d", (int)game->structs.count);
        moon_blit_text_fmt(surf, tx, row + 54,  COLOR_ALERT_RED, 0, true, "CAUSE  %s", cause_name(res->death_cause));
        moon_blit_text_fmt(surf, tx, row + 72,  RGB565(110, 115, 125), 0, true, "SEED %u", (unsigned)game->seed);
        items[0] = "R:NEW GAME";
        items[1] = "Q:QUIT";
        break;
    case MOON_MENU_VICTORY:
        moon_blit_text(surf, tx, ty, "ACT I COMPLETE", COLOR_STATUS_GREEN, 0, true);
        moon_blit_text_fmt(surf, tx, row,      COLOR_LUNAR_WHITE, 0, true, "SELF-SUSTAINING: DAY %d", (int)res->lunar_day);
        moon_blit_text_fmt(surf, tx, row + 18, COLOR_LUNAR_WHITE, 0, true, "CREW %d  SEED %u", (int)res->population, (unsigned)game->seed);
        items[0] = "C:CONTINUE";
        items[1] = "R:NEW GAME";
        items[2] = "Q:QUIT";
        break;
    case MOON_MENU_VICTORY_ACT2:
        moon_blit_text(surf, tx, ty, "ACT II COMPLETE", COLOR_STATUS_GREEN, 0, true);
        moon_blit_text(surf, tx, row,      "SPACEPORT OPERATIONAL", COLOR_LASER_CYAN, 0, true);
        moon_blit_text_fmt(surf, tx, row + 18, COLOR_LUNAR_WHITE, 0, true, "HYDROLOX STARSHIP DISPATCHED: DAY %d", (int)res->lunar_day);
        moon_blit_text_fmt(surf, tx, row + 36, COLOR_LUNAR_WHITE, 0, true, "CREW %d  SEED %u  REACTOR +%dkW",
                           (int)res->population, (unsigned)game->seed, (int)res->reactor_power_kw);
        moon_blit_text(surf, tx, row + 54, "EARTH TRADE ROUTE ESTABLISHED", COLOR_GOLD_FOIL, 0, true);
        items[0] = "C:CONTINUE";
        items[1] = "R:NEW GAME";
        items[2] = "Q:QUIT";
        break;
    default:
        moon_blit_text(surf, tx, ty, "QUIT GAME?", COLOR_GOLD_FOIL, 0, true);
        items[0] = "Y:YES, QUIT";
        items[1] = "N:NO";
        break;
    }
    for (int i = 0; i < n; i++) {
        bool sel = (i == game->menu_sel);
        draw_button(surf, menu_item_rect(game, i), items[i],
                    sel ? RGB565(30, 45, 60) : RGB565(20, 24, 30),
                    sel ? COLOR_LASER_CYAN : RGB565(60, 65, 75), COLOR_LUNAR_WHITE);
    }
}

/* Outline and surveyor brackets around the selected tile, or around the whole
 * footprint when a structure stands there. */
static void render_selection_frame(const moon_game_t *game, const moon_surface_t *surf) {
    int gx0 = game->selected_tile_x, gy0 = game->selected_tile_y;
    int fw = 1, fh = 1;

    const moon_tile_t *st = moon_grid_get_tile_const(&game->grid, gx0, gy0);
    if (st && st->structure_id != 0 && st->structure_id < MOON_MAX_STRUCTURES) {
        const moon_structure_t *s = &game->structs.list[st->structure_id];
        if (s->id == st->structure_id) {
            gx0 = s->gx; gy0 = s->gy; fw = s->footprint_w; fh = s->footprint_h;
        }
    }

    /* Corners of the footprint diamond, each at its own tile's elevation */
    int32_t tx, ty, rx, ry, bx, by, lx, ly, sx, sy;
    #define TILE_AT(gx, gy, ox, oy) do { \
        const moon_tile_t *_t = moon_grid_get_tile_const(&game->grid, (gx), (gy)); \
        moon_grid_to_screen((gx), (gy), _t ? _t->elevation : 0, game->cam_x, game->cam_y, \
                            surf->width, surf->height, &sx, &sy); \
        ox = sx; oy = sy; } while (0)
    TILE_AT(gx0, gy0, tx, ty);
    TILE_AT(gx0 + fw - 1, gy0, rx, ry);
    TILE_AT(gx0 + fw - 1, gy0 + fh - 1, bx, by);
    TILE_AT(gx0, gy0 + fh - 1, lx, ly);
    #undef TILE_AT
    ty -= MOON_TILE_HALF_H;
    rx += MOON_TILE_HALF_W;
    by += MOON_TILE_HALF_H;
    lx -= MOON_TILE_HALF_W;

    moon_blit_line(surf, tx, ty, rx, ry, COLOR_LASER_CYAN);
    moon_blit_line(surf, rx, ry, bx, by, COLOR_LASER_CYAN);
    moon_blit_line(surf, bx, by, lx, ly, COLOR_LASER_CYAN);
    moon_blit_line(surf, lx, ly, tx, ty, COLOR_LASER_CYAN);

    /* High-tech NASA gold surveyor corner brackets */
    int L = 6;
    moon_blit_line(surf, tx, ty, tx + L * 2, ty + L, COLOR_GOLD_FOIL);
    moon_blit_line(surf, tx, ty, tx - L * 2, ty + L, COLOR_GOLD_FOIL);
    moon_blit_line(surf, bx, by, bx + L * 2, by - L, COLOR_GOLD_FOIL);
    moon_blit_line(surf, bx, by, bx - L * 2, by - L, COLOR_GOLD_FOIL);
    moon_blit_line(surf, lx, ly, lx + L * 2, ly - L, COLOR_GOLD_FOIL);
    moon_blit_line(surf, lx, ly, lx + L * 2, ly + L, COLOR_GOLD_FOIL);
    moon_blit_line(surf, rx, ry, rx - L * 2, ry - L, COLOR_GOLD_FOIL);
    moon_blit_line(surf, rx, ry, rx - L * 2, ry + L, COLOR_GOLD_FOIL);
}

static void render_hud_bar(const moon_game_t *game, const moon_surface_t *surf);
static void render_build_ribbon(const moon_game_t *game, const moon_surface_t *surf);

int moon_game_take_dirty(moon_game_t *game) {
    int d = game->show_perf ? MOON_DIRTY_WORLD : game->dirty;   /* the timing overlay wants every frame */
    game->dirty = MOON_DIRTY_NONE;
    return d;
}

void moon_game_render_hud(moon_game_t *game, uint16_t *fb, int width, int height, int stride) {
    if (!game || !fb) return;
    moon_surface_t surf = moon_surface_make(fb, width, height, stride);
    render_hud_bar(game, &surf);
    render_build_ribbon(game, &surf);
}

void moon_game_note_frame(moon_game_t *game, uint32_t present_us, uint32_t frame_us) {
    game->present_us = present_us;
    game->frame_ring[game->frames_noted % MOON_PERF_RING] = frame_us;
    game->frames_noted++;
}

void moon_game_frame_stats(const moon_game_t *game, uint32_t *median_us, uint32_t *p95_us) {
    uint32_t n = game->frames_noted < MOON_PERF_RING ? game->frames_noted : MOON_PERF_RING;
    *median_us = *p95_us = 0;
    if (n == 0) return;
    uint32_t v[MOON_PERF_RING];
    for (uint32_t i = 0; i < n; i++) v[i] = game->frame_ring[i];
    /* insertion sort: at most 256 values, run a few times a second at most */
    for (uint32_t i = 1; i < n; i++) {
        uint32_t x = v[i], j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
    *median_us = v[n / 2];
    *p95_us = v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
}

/* F: where the time of a frame goes (microseconds measured with the platform's clock) */
/* The top bar: day and clock, power, resources, crew. Opaque, so it can be redrawn alone. */
static void render_hud_bar(const moon_game_t *game, const moon_surface_t *surf) {
    const moon_resources_t *res = &game->resources;
    uint16_t pwr_col = res->power_brownout ? COLOR_ALERT_RED : COLOR_ENERGY_YELLOW;
    int batt_kwh = fixed_to_int(res->battery_stored_kwh);
    int rgl_kg = fixed_to_int(res->regolith_stored_kg);
    int o2_kg = fixed_to_int(res->o2_stored_kg);
    uint16_t o2_col = res->warning_o2 ? COLOR_ALERT_RED : COLOR_LASER_CYAN;
    int water_l = fixed_to_int(res->water_stored_l);
    uint16_t water_col = res->warning_water ? COLOR_ALERT_RED : COLOR_SOLAR_BLUE;
    uint16_t ls_col = (res->life_support_pct < 50) ? COLOR_ALERT_RED :
                      ((res->life_support_pct < 80) ? COLOR_ENERGY_YELLOW : COLOR_STATUS_GREEN);

    moon_clock_t clk = moon_resources_clock(res);
    uint16_t clk_col = res->is_night ? COLOR_LASER_CYAN : COLOR_GOLD_FOIL;
    int hud_text_x = game->ui.clock_cx + game->ui.clock_r + 6;

    /* One table for both layouts: x, colour, format and four values per readout.
     * The wide layout shows the clock's next event and a colour marker per item. */
    const bool narrow = (surf->width < 800);
    const int hud_h = game->ui.hud.h - 1;
    moon_blit_fill_rect(surf, 0, 0, surf->width, hud_h, RGB565(14, 16, 20));
    moon_blit_line(surf, 0, hud_h, surf->width - 1, hud_h, COLOR_GOLD_FOIL);
    const int ty = narrow ? 4 : 8;

    moon_blit_text_fmt(surf, hud_text_x, ty, clk_col, 0, true, narrow ? "D%02d %02d:%02d" : "D%02d %02d:%02d %s %d:%02d",
                       res->lunar_day, clk.minutes / 60, clk.minutes % 60,
                       clk.next_is_dusk ? "dusk" : "dawn", clk.next_in_min / 60, clk.next_in_min % 60);

    struct { int16_t xw, xn; uint16_t col; const char *fw, *fn; int v[4]; } hud[] = {
        { 200,  98, pwr_col, "PWR %d/%dkW BATT %d/%dkWh", "P:%d/%d",
          { res->power_produced_kw, res->power_consumed_kw, batt_kwh, res->battery_capacity_kwh } },
        { 0,   168, pwr_col, NULL, "B:%d", { batt_kwh } },
        { 440, 216, COLOR_REGOLITH_LIGHT, "RGL %dkg", "R:%d", { rgl_kg } },
        { 535, 272, o2_col, "O2 %dkg", "O2:%d", { o2_kg } },
        { 615, 320, water_col, "H2O %dL", "W:%d", { water_l } },
        { 695, 368, ls_col, "CREW %d/%d (LS:%d%%)", "CRW:%d/%d (%d%%)",
          { res->population, res->max_population, res->life_support_pct } },
        { 855, 492, RGB565(120, 210, 255), "FUEL %d/%dkg", "F:%d/%d",
          { fixed_to_int(res->lh2_stored_kg) + fixed_to_int(res->lox_stored_kg),
            res->lh2_capacity_kg + res->lox_capacity_kg } },
    };
    for (unsigned i = 0; i < sizeof(hud) / sizeof(hud[0]); i++) {
        const char *fmt = narrow ? hud[i].fn : hud[i].fw;
        if (!fmt) continue;
        int x = narrow ? hud[i].xn : hud[i].xw + 10;
        if (!narrow) moon_blit_fill_rect(surf, x - 10, 10, 6, 12, hud[i].col);
        moon_blit_text_fmt(surf, x, ty, hud[i].col, 0, true, fmt, hud[i].v[0], hud[i].v[1], hud[i].v[2], hud[i].v[3]);
    }

    render_clock(game, surf);
}

/* True when every pixel of the view is inside the map's tiles: then drawing them all makes
 * clearing first pointless. The corners are tested a couple of tiles inside the map, which
 * also covers raised tiles shifting up. */
static bool world_covers_view(const moon_game_t *game) {
    const int w = game->screen_w, h = game->screen_h, top = game->ui.hud.h;
    const int xs[4] = { 0, w - 1, 0, w - 1 }, ys[4] = { top, top, h - 1, h - 1 };
    for (int i = 0; i < 4; i++) {
        int32_t gx, gy;
        moon_screen_to_grid(xs[i], ys[i], game->cam_x, game->cam_y, w, h, &gx, &gy);
        if (gx < 3 || gy < 3 || gx > MOON_MAP_SIZE_X - 4 || gy > MOON_MAP_SIZE_Y - 4) return false;
    }
    return true;
}

static void render_perf(const moon_game_t *game, const moon_surface_t *surf) {
    uint32_t med, p95;
    moon_game_frame_stats(game, &med, &p95);
    const moon_phase_us_t *p = &game->phase;
    int bx = (surf->width - 360) / 2, by = game->ui.hud.h + 4;
    moon_blit_fill_rect(surf, bx, by, 360, 38, RGB565(10, 14, 10));
    moon_blit_rect_outline(surf, bx, by, 360, 38, COLOR_STATUS_GREEN);
    moon_blit_text_fmt(surf, bx + 6, by + 3, COLOR_STATUS_GREEN, 0, true, "med %u.%u ms  p95 %u.%u ms  %u fps",
                       (unsigned)(med / 1000), (unsigned)(med % 1000 / 100), (unsigned)(p95 / 1000), (unsigned)(p95 % 1000 / 100),
                       (unsigned)(med ? 1000000u / med : 0));
    moon_blit_text_fmt(surf, bx + 6, by + 19, COLOR_REGOLITH_LIGHT, 0, true, "w%u o%u m%u u%u p%u us",
                       (unsigned)p->world, (unsigned)p->overlay, (unsigned)p->minimap, (unsigned)p->ui,
                       (unsigned)game->present_us);
}

void moon_game_render(moon_game_t *game, uint16_t *fb, int width, int height, int stride) {
    if (!game || !fb) return;

    moon_surface_t surf = moon_surface_make(fb, width, height, stride);
    uint32_t t_mark = game->clock_us ? game->clock_us() : 0;
#define PHASE_END(field) do { if (game->clock_us) { uint32_t t_now = game->clock_us(); game->phase.field = t_now - t_mark; t_mark = t_now; } } while (0)

    /* 1. Clear background to lunar deep space, unless the tiles cover the whole view anyway
     * (a pass over the frame is expensive on PSRAM) */
    if (!world_covers_view(game)) moon_blit_clear(&surf, COLOR_SPACE_BLACK);

    /* 2. Terrain and structures in one back-to-front pass, overlays on top. The day/night
     * tint (dimmer and bluer at night) is folded into the drawing; badges and the UI stay bright. */
    {
        int sun = fixed_to_int(game->resources.solar_intensity * 256); /* 0..256 */
        if (sun < 0) sun = 0;
        if (sun < 256) {
            moon_blit_set_tint((uint16_t)(116 + sun * 140 / 256), (uint16_t)(128 + sun * 128 / 256),
                               (uint16_t)(150 + sun * 106 / 256));
        }
    }
    game->structs.lander_y_offset = game->landing_y_offset;
    moon_world_render(&game->structs, &game->grid, &surf,
                      game->cam_x, game->cam_y, game->ui.hud.h,
                      game->hover_tile_x, game->hover_tile_y, game->hover_valid);
    moon_blit_set_tint(256, 256, 256);
    PHASE_END(world);

    moon_world_render_overlays(&game->structs, &game->grid, &surf, &game->resources,
                               game->cam_x, game->cam_y);

    /* Render Expedition Lander descent touchdown sequence */
    if (game->landing_y_offset > 0) {
        moon_lander_render(&game->grid, &surf, game->cam_x, game->cam_y,
                           game->landing_y_offset, game->landing_elapsed_ms);
    }

    /* Render Starship rocket, hydrolox plume & shock diamonds, dust skirts, and umbilical */
    moon_starship_render(&game->starship, &game->grid, &surf, game->cam_x, game->cam_y);

    /* 3. Selection frame around the selected structure's footprint (or tile) */
    render_selection_frame(game, &surf);

    /* 4. Ghost of what the armed brush or the held / hovered build button would build,
     * and the recycle brush's target marker. A brush follows the pointer; a button
     * preview shows on the selected tile. */
    int ghost_x, ghost_y;
    if (moon_game_ghost_target(game, &ghost_x, &ghost_y)) {
        moon_structure_type_t gtype = game->structs.selected_build_type;
        if (gtype == MOON_STRUCT_NONE && game->preview_slot >= 1 && game->preview_slot <= 6) gtype = get_build_button_type(game, game->preview_slot - 1);
        if (gtype != MOON_STRUCT_NONE) {
            moon_structure_render_ghost(&game->structs, &game->grid, &surf, game->cam_x, game->cam_y,
                                        gtype, ghost_x, ghost_y);
        } else if (game->demolish_tool_active) {
            const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid, ghost_x, ghost_y);
            int32_t hsx, hsy;
            moon_grid_to_screen(ghost_x, ghost_y, ht ? ht->elevation : 0, game->cam_x, game->cam_y, width, height, &hsx, &hsy);
            moon_blit_iso_tile(&surf, hsx, hsy, MOON_TILE_HALF_W, MOON_TILE_HALF_H, moon_color_dim(COLOR_ALERT_RED, 120), COLOR_ALERT_RED);
            moon_blit_line(&surf, hsx - 10, hsy - 5, hsx + 10, hsy + 5, COLOR_LUNAR_WHITE);
            moon_blit_line(&surf, hsx - 10, hsy + 5, hsx + 10, hsy - 5, COLOR_LUNAR_WHITE);
        }
    }

    PHASE_END(overlay);

    /* 5. Render Bottom-Right Minimap Radar */
    moon_grid_render_minimap(&game->grid, &surf, game->ui.minimap.x, game->ui.minimap.y,
                             game->ui.minimap.w, game->ui.minimap.h,
                             game->cam_x, game->cam_y);
    moon_blit_fill_rect(&surf, game->ui.minimap.x + game->selected_tile_x - 1,
                        game->ui.minimap.y + game->selected_tile_y - 1, 3, 3, COLOR_LUNAR_WHITE);
    PHASE_END(minimap);

    /* 6. Render Bottom Build Ribbon */
    render_build_ribbon(game, &surf);
    render_undo_button(game, &surf);

    /* 7. Top HUD bar */
    render_hud_bar(game, &surf);
    const moon_resources_t *res = &game->resources;

    /* Tile readout: under the mouse, else the selected tile */
    {
        bool hov = game->hover_valid;
        int bx = hov ? game->hover_tile_x : game->selected_tile_x;
        int by = hov ? game->hover_tile_y : game->selected_tile_y;
        const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid, bx, by);
        const char *tname = ht ? get_terrain_name((moon_terrain_type_t)ht->terrain) : "VOID";
        const moon_ui_rect_t *hb = &game->ui.hover_badge;
        int badge_w = (22 + (int)strlen(tname)) * MOON_FONT_W + 8; /* "[xx,yy] NAME E:n R:nnn%" */
        uint16_t col = hov ? COLOR_LASER_CYAN : COLOR_GOLD_FOIL;
        moon_blit_fill_rect(&surf, hb->x, hb->y, badge_w, hb->h, RGB565(26, 30, 36));
        moon_blit_rect_outline(&surf, hb->x, hb->y, badge_w, hb->h, col);
        moon_blit_text_fmt(&surf, hb->x + 4, hb->y + 2, col, 0, true,
                           "[%02d,%02d] %s E:%d R:%d%%", bx, by, tname,
                           ht ? ht->elevation : 0, ht ? ht->resource_val : 0);
    }

    if (game->menu_page == MOON_MENU_CLOSED) render_objective(game, &surf);

    /* 8. Active Life Support / Crisis Warning Ticker Banner */
    int banner_y = (width < 800) ? 26 : 38;
    struct { const char *msg; uint16_t col; } banners[5];
    int nb = 0;
    if (res->population == 0) {
        banners[nb].msg = "[MISSION FAILED] ALL CREW LOST: COLONY ABANDONED"; banners[nb++].col = COLOR_ALERT_RED;
    } else if (res->warning_crew_lost) {
        banners[nb].msg = "[CASUALTY] CREW MEMBER PERISHED - ZERO LIFE SUPPORT"; banners[nb++].col = COLOR_ALERT_RED;
    }
    if (res->warning_cold) {
        banners[nb].msg = "[CRITICAL] CRYOGENIC FREEZING: HEATING LOST"; banners[nb++].col = COLOR_ALERT_RED;
    }
    if (res->power_brownout) {
        banners[nb].msg = "[ALERT] POWER BLACKOUT: BATTERIES DEPLETED"; banners[nb++].col = RGB565(180, 50, 20);
    } else if (res->warning_power) {
        banners[nb].msg = "[WARNING] BATTERY LOW"; banners[nb++].col = RGB565(150, 100, 20);
    }
    if (res->warning_o2) {
        bool empty = res->o2_stored_kg <= 0;
        banners[nb].msg = empty ? "[CRITICAL] O2 STARVATION: RESERVES EMPTY" : "[WARNING] O2 RESERVES LOW";
        banners[nb++].col = empty ? COLOR_ALERT_RED : RGB565(150, 100, 20);
    }
    if (res->warning_water && nb < 5) {
        bool empty = res->water_stored_l <= 0;
        banners[nb].msg = empty ? "[CRITICAL] WATER RESERVES EMPTY" : "[WARNING] WATER RESERVES LOW";
        banners[nb++].col = empty ? COLOR_ALERT_RED : RGB565(150, 100, 20);
    }
    for (int i = 0; i < nb; i++) {
        int len = 0;
        while (banners[i].msg[len]) len++;
        int by = banner_y + i * 22;
        moon_blit_fill_rect(&surf, 8, by, len * 8 + 12, 20, banners[i].col);
        moon_blit_text(&surf, 14, by + 2, banners[i].msg, COLOR_LUNAR_WHITE, 0, true);
    }

    /* 9. Live Notification Banner (Terrain toggle, etc.) */
    if (game->notify_timer_ms > 0 && game->notify_msg) {
        int len = 0;
        const char *p = game->notify_msg;
        while (*p++) len++;
        int nw = len * 8 + 16;
        int nx = (width - nw) / 2;
        int ny = (width < 800) ? 28 : 38;
        moon_blit_fill_rect(&surf, nx, ny, nw, 22, RGB565(20, 25, 35));
        moon_blit_rect_outline(&surf, nx, ny, nw, 22, COLOR_GOLD_FOIL);
        moon_blit_text(&surf, nx + 8, ny + 3, game->notify_msg, COLOR_GOLD_FOIL, 0, true);
    }

    /* 10. Interactive Building & Terrain Inspector Panel */
    render_inspector_panel(game, &surf);

    /* 11. Pause marker and the modal menu */
    if (game->paused && game->menu_page == MOON_MENU_CLOSED) {
        const char *msg = "|| PAUSED  (P to resume)";
        int len = 0;
        while (msg[len]) len++;
        int pw = len * MOON_FONT_W + 20;
        int px = (width - pw) / 2;
        int py = game->ui.hud.h + 40;
        moon_blit_fill_rect(&surf, px, py, pw, 26, RGB565(40, 32, 10));
        moon_blit_rect_outline(&surf, px, py, pw, 26, COLOR_ENERGY_YELLOW);
        moon_blit_text(&surf, px + 10, py + 5, msg, COLOR_ENERGY_YELLOW, 0, true);
    }
    render_menu(game, &surf);
    PHASE_END(ui);
    game->shown_hover_valid = game->hover_valid;
    game->shown_hover_x = game->hover_tile_x;
    game->shown_hover_y = game->hover_tile_y;
    game->shown_preview = game->preview_slot;
    if (game->show_perf) render_perf(game, &surf);
#undef PHASE_END
}
