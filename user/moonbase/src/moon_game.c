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
    uint32_t seed = game->seed_request ? game->seed_request :
                    game->seed ? game->seed * 1664525u + 1013904223u + game->sim_ticks : 19690720u;
    memset(game, 0, sizeof(*game));
    game->seed = seed;

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
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_POWER_CONDUIT, 33, 32);
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_SOLAR_ARRAY, 34, 32);
    moon_structure_place(&game->grid, &game->structs, MOON_STRUCT_BATTERY_BANK, 33, 33);

    /* Initialize resource & life support simulation */
    moon_resources_init(&game->resources);
    moon_resources_solve_power_grid(&game->grid, &game->structs);

    camera_home(game);
    game->cam_start_x = game->cam_x;
    game->cam_start_y = game->cam_y;
    game->selected_tile_x = HOME_GX;
    game->selected_tile_y = HOME_GY;
    game->press_slot = -1;
    game->preview_slot = -1;
    game->last_slot = -1;
}

bool moon_game_init(moon_game_t *game, int width, int height) {
    if (!game) return false;
    game_reset(game, width, height);
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

/* Ribbon slots: 0 standby, 1..6 build, 7 recycle; then pause, speed, menu */
enum { SLOT_STANDBY = 0, SLOT_RECYCLE = 7, SLOT_PAUSE = 8, SLOT_SPEED = 9, SLOT_MENU = 10 };

static int ribbon_slot_at(const moon_game_t *game, int px, int py) {
    const moon_ui_layout_t *ui = &game->ui;
    for (int i = 0; i < MOON_UI_RIBBON_SLOTS; i++) {
        if (moon_ui_hit(&ui->ribbon[i], px, py)) return i;
    }
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
    if (!s) { notify(game, "NOTHING TO RECYCLE", 1500); return false; }
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
        if (!s || s->type != game->undo_type) { notify(game, "CANNOT UNDO", 1500); game->undo_kind = MOON_UNDO_NONE; return; }
        moon_structure_demolish(&game->grid, &game->structs, game->undo_x, game->undo_y);
        add_regolith(game, game->undo_refund_kg);
    } else {
        fixed32_t give_back = fixed_from_int(game->undo_refund_kg);
        if (s || game->resources.regolith_stored_kg < give_back ||
            !moon_structure_place(&game->grid, &game->structs, (moon_structure_type_t)game->undo_type,
                                  game->undo_x, game->undo_y)) {
            notify(game, "CANNOT UNDO", 1500);
            game->undo_kind = MOON_UNDO_NONE;
            return;
        }
        game->resources.regolith_stored_kg -= give_back;
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

static const moon_structure_type_t s_build_buttons[6] = {
    MOON_STRUCT_SOLAR_ARRAY, MOON_STRUCT_BATTERY_BANK, MOON_STRUCT_HABITAT_DOME,
    MOON_STRUCT_REGOLITH_PLANT, MOON_STRUCT_O2_EXTRACTOR, MOON_STRUCT_POWER_CONDUIT
};

/* Slot of the armed brush (1..7) or -1 */
static int brush_slot(const moon_game_t *game) {
    if (game->demolish_tool_active) return SLOT_RECYCLE;
    for (int i = 0; i < 6; i++) {
        if (game->structs.selected_build_type == s_build_buttons[i]) return i + 1;
    }
    return -1;
}

static void brush_set(moon_game_t *game, int slot) {
    game->structs.selected_build_type = MOON_STRUCT_NONE;
    game->demolish_tool_active = false;
    if (slot == SLOT_RECYCLE) game->demolish_tool_active = true;
    else if (slot >= 1 && slot <= 6) game->structs.selected_build_type = s_build_buttons[slot - 1];
}

static void brush_apply(moon_game_t *game) {
    int slot = brush_slot(game);
    if (slot == SLOT_RECYCLE) recycle_at(game, game->selected_tile_x, game->selected_tile_y);
    else if (slot >= 1) build_at_selection(game, game->structs.selected_build_type);
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
    else build_at_selection(game, s_build_buttons[slot - 1]);
}

/* ---- Menu ---- */

/* Rows of the menu panel a page uses: the end-of-game pages keep their summary in
 * the rows above the buttons */
static int menu_item_count(const moon_game_t *game) {
    switch (game->menu_page) {
    case MOON_MENU_MAIN:     return MOON_UI_MENU_ITEMS;
    case MOON_MENU_VICTORY:  return 3;
    default:                 return 2;
    }
}

static int menu_first_row(const moon_game_t *game) {
    switch (game->menu_page) {
    case MOON_MENU_GAME_OVER: return 2;
    case MOON_MENU_VICTORY:   return 1;
    default:                  return 0;
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

static void menu_activate(moon_game_t *game, int item) {
    switch (game->menu_page) {
    case MOON_MENU_MAIN:
        if (item == 0) game->menu_page = MOON_MENU_CLOSED;
        else if (item == 1) toggle_texture(game);
        else if (item == 2) { game->menu_page = MOON_MENU_CONFIRM_RESTART; game->menu_sel = 1; }
        else if (item == 3) { game->menu_page = MOON_MENU_CONFIRM_QUIT; game->menu_sel = 1; }
        break;
    case MOON_MENU_CONFIRM_QUIT:
        if (item == 0) game->running = false;
        else { game->menu_page = MOON_MENU_MAIN; game->menu_sel = 3; }
        break;
    case MOON_MENU_CONFIRM_RESTART:
        if (item == 0) game_reset(game, game->screen_w, game->screen_h);
        else { game->menu_page = MOON_MENU_MAIN; game->menu_sel = 2; }
        break;
    case MOON_MENU_GAME_OVER:
        if (item == 0) game_reset(game, game->screen_w, game->screen_h);
        else game->running = false;
        break;
    case MOON_MENU_VICTORY:
        if (item == 0) {
            game->victory_claimed = true;
            game->menu_page = MOON_MENU_CLOSED;
            game->state = game->paused ? MOON_STATE_PAUSED : MOON_STATE_PLAYING;
        } else if (item == 1) game_reset(game, game->screen_w, game->screen_h);
        else game->running = false;
        break;
    default:
        break;
    }
}

/* Same on every device: back out of the innermost thing (menu page, brush, inspector).
 * Returns false when there was nothing to back out of. */
static bool cancel_action(moon_game_t *game) {
    if (game->menu_page == MOON_MENU_GAME_OVER) return true;      /* the only ways out are the buttons */
    if (game->menu_page == MOON_MENU_VICTORY) {
        menu_activate(game, 0);                                   /* ESC continues playing */
        return true;
    }
    if (game->menu_page == MOON_MENU_CONFIRM_QUIT || game->menu_page == MOON_MENU_CONFIRM_RESTART) {
        game->menu_page = MOON_MENU_MAIN;
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
        bool confirm = game->menu_page == MOON_MENU_CONFIRM_QUIT || game->menu_page == MOON_MENU_CONFIRM_RESTART;
        bool ending = game->menu_page == MOON_MENU_GAME_OVER || game->menu_page == MOON_MENU_VICTORY;
        if (ending) {
            /* Restart and Quit are immediate here: the run is over */
            int restart = game->menu_page == MOON_MENU_VICTORY ? 1 : 0;
            if (key == MOON_KEY_R) { menu_activate(game, restart); return; }
            if (key == MOON_KEY_Q) { menu_activate(game, restart + 1); return; }
            if (key == MOON_KEY_C && game->menu_page == MOON_MENU_VICTORY) { menu_activate(game, 0); return; }
        }
        switch (key) {
        case MOON_KEY_ESCAPE: case MOON_KEY_BACKSPACE:
            cancel_action(game);
            break;
        case MOON_KEY_N:
            if (confirm) cancel_action(game);
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
    case MOON_KEY_P: toggle_pause(game); break;
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
    if (slot == SLOT_PAUSE) toggle_pause(game);
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
    if (!s || s->type == MOON_STRUCT_LANDER) return;
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

void moon_game_handle_event(moon_game_t *game, const moon_event_t *ev) {
    if (!game || !ev) return;

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

void moon_game_update(moon_game_t *game, uint32_t delta_ms) {
    if (!game) return;

    /* A stalled frame must not trigger a burst of sim ticks */
    if (delta_ms > MOON_MAX_FRAME_DELTA_MS) delta_ms = MOON_MAX_FRAME_DELTA_MS;

    game->frame_count++;

    if (game->notify_timer_ms > 0) {
        if (delta_ms >= game->notify_timer_ms) game->notify_timer_ms = 0;
        else game->notify_timer_ms -= delta_ms;
    }
    if (game->undo_ms > 0) {
        game->undo_ms = (delta_ms >= game->undo_ms) ? 0 : game->undo_ms - delta_ms;
        if (game->undo_ms == 0) game->undo_kind = MOON_UNDO_NONE;
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
    }

    if (game->paused || game->menu_page != MOON_MENU_CLOSED) return;

    game->last_tick_ms += delta_ms * game->speed;
    while (game->last_tick_ms >= MOON_SIM_TICK_INTERVAL_MS) {
        game->last_tick_ms -= MOON_SIM_TICK_INTERVAL_MS;
        game->sim_ticks++;
        moon_resources_tick(&game->resources, &game->grid, &game->structs);
    }
    moon_game_check_end(game);
}

/* The run ends when the last colonist dies, or is won when Act I is complete. Either
 * way an overlay takes over (the sim is frozen while a menu page is open). */
void moon_game_check_end(moon_game_t *game) {
    if (game->state != MOON_STATE_PLAYING && game->state != MOON_STATE_PAUSED) return;

    bool lost = game->resources.population == 0;
    bool won = !lost && !game->victory_claimed && moon_resources_act1_complete(&game->resources);
    if (!lost && !won) return;

    game->state = lost ? MOON_STATE_GAME_OVER : MOON_STATE_VICTORY;
    game->menu_page = lost ? MOON_MENU_GAME_OVER : MOON_MENU_VICTORY;
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

        /* Status */
        const char *status_str = !s->active ? "STANDBY" : (s->powered ? "ONLINE" : "NO POWER");
        uint16_t status_col = !s->active ? RGB565(140, 140, 150) : (s->powered ? COLOR_STATUS_GREEN : COLOR_ALERT_RED);
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 48, status_col, 0, true, "STATUS: [%s]", status_str);

        /* Power metrics */
        if (def->power_kw > 0) {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 66, COLOR_ENERGY_YELLOW, 0, true,
                               "POWER: +%d kW (Gen)", def->power_kw);
        } else if (def->power_kw < 0) {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 66, COLOR_ENERGY_YELLOW, 0, true,
                               "POWER: %d kW (Demand)", def->power_kw);
        } else {
            moon_blit_text(surf, ip_x + 8, ip_y + 66, "POWER: Passive", COLOR_REGOLITH_LIGHT, 0, true);
        }

        /* Storage & Life Support */
        if (def->power_storage > 0) {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 84, COLOR_ENERGY_YELLOW, 0, true,
                               "STORAGE: %d kWh capacity", def->power_storage);
        } else if (def->rate_o2 != 0 || def->rate_water != 0) {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 84, COLOR_LASER_CYAN, 0, true,
                               "LIFE: %+d kg O2 / %+d L H2O", def->rate_o2, def->rate_water);
        } else {
            moon_blit_text_fmt(surf, ip_x + 8, ip_y + 84, COLOR_REGOLITH_LIGHT, 0, true,
                               "COORDS: [%02d, %02d]", s->gx, s->gy);
        }

        /* Integrity */
        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 102, COLOR_STATUS_GREEN, 0, true,
                           "STRUCTURAL INTEGRITY: %d%%", s->health);

        moon_blit_text_fmt(surf, ip_x + 8, ip_y + 122, RGB565(160, 165, 175), 0, true,
                           "Footprint: %dx%d tiles", s->footprint_w, s->footprint_h);

        
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

        if (tile->terrain == MOON_TERRAIN_ICE_DEPOSIT) {
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

    static const char *labels[8] = {
        "E:STANDBY",
        "1:SOLAR 10t", "2:BATT  20t", "3:HAB   50t",
        "4:SINT  15t", "5:OXY   25t", "6:CABLE  2t",
        "R:RECYCLE"
    };

    static const char *labels_narrow[8] = {
        "E:STBY",
        "1:SOL", "2:BAT", "3:HAB",
        "4:SINT", "5:OXY", "6:WIRE",
        "R:RECY"
    };

    const moon_structure_t *sel = selected_structure(game);
    int armed = brush_slot(game);
    for (int i = 0; i < MOON_UI_RIBBON_SLOTS; i++) {
        /* Enabled when the button could act on the selected tile right now */
        bool enabled;
        if (i >= 1 && i <= 6) enabled = build_possible(game, s_build_buttons[i - 1], NULL);
        else enabled = (sel && sel->type != MOON_STRUCT_LANDER);
        bool selected = (i == armed);

        uint16_t bg = selected ? (i == SLOT_RECYCLE ? RGB565(55, 20, 20) : RGB565(30, 45, 60)) : RGB565(20, 24, 30);
        uint16_t border = selected ? (i == SLOT_RECYCLE ? COLOR_ALERT_RED : COLOR_LASER_CYAN)
                                   : (enabled ? RGB565(60, 65, 75) : RGB565(35, 38, 45));
        uint16_t text_col = selected ? COLOR_LUNAR_WHITE : (enabled ? RGB565(170, 175, 185) : RGB565(80, 85, 95));
        const char *label = narrow ? labels_narrow[i] : labels[i];
        if (i == SLOT_STANDBY && sel && !sel->active) label = narrow ? "E:ACT" : "E:ACTIVATE";
        draw_button(surf, &ui->ribbon[i], label, bg, border, text_col);
    }

    /* Pause, speed and menu */
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
        moon_blit_text_fmt(surf, tx, ty, COLOR_GOLD_FOIL, 0, true, "MOONBASE SEED %u", (unsigned)game->seed);
        items[0] = "ESC:RESUME";
        items[1] = game->grid.textured_terrain ? "T:TERRAIN TEXTURED" : "T:TERRAIN FAST";
        items[2] = "R:RESTART";
        items[3] = "Q:QUIT";
        break;
    case MOON_MENU_GAME_OVER:
        moon_blit_text(surf, tx, ty, "BASE LOST", COLOR_ALERT_RED, 0, true);
        moon_blit_text_fmt(surf, tx, row,       COLOR_LUNAR_WHITE, 0, true, "DAY REACHED  %d", (int)res->lunar_day);
        moon_blit_text_fmt(surf, tx, row + 18,  COLOR_LUNAR_WHITE, 0, true, "PEAK CREW    %d", (int)res->peak_population);
        moon_blit_text_fmt(surf, tx, row + 36,  COLOR_LUNAR_WHITE, 0, true, "BUILDINGS    %d", (int)game->structs.count);
        moon_blit_text_fmt(surf, tx, row + 54,  COLOR_ALERT_RED, 0, true, "CAUSE  %s", cause_name(res->death_cause));
        moon_blit_text_fmt(surf, tx, row + 72,  RGB565(110, 115, 125), 0, true, "SEED %u", (unsigned)game->seed);
        items[0] = "R:RESTART";
        items[1] = "Q:QUIT";
        break;
    case MOON_MENU_VICTORY:
        moon_blit_text(surf, tx, ty, "ACT I COMPLETE", COLOR_STATUS_GREEN, 0, true);
        moon_blit_text_fmt(surf, tx, row,      COLOR_LUNAR_WHITE, 0, true, "SELF-SUSTAINING: DAY %d", (int)res->lunar_day);
        moon_blit_text_fmt(surf, tx, row + 18, COLOR_LUNAR_WHITE, 0, true, "CREW %d  SEED %u", (int)res->population, (unsigned)game->seed);
        items[0] = "C:CONTINUE";
        items[1] = "R:RESTART";
        items[2] = "Q:QUIT";
        break;
    default:
        moon_blit_text(surf, tx, ty,
                       game->menu_page == MOON_MENU_CONFIRM_QUIT ? "QUIT GAME?" : "RESTART? BASE IS LOST",
                       COLOR_GOLD_FOIL, 0, true);
        items[0] = (game->menu_page == MOON_MENU_CONFIRM_QUIT) ? "Y:YES, QUIT" : "Y:YES, RESTART";
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

void moon_game_render(moon_game_t *game, uint16_t *fb, int width, int height, int stride) {
    if (!game || !fb) return;

    moon_surface_t surf = moon_surface_make(fb, width, height, stride);

    /* 1. Clear background to lunar deep space */
    moon_blit_clear(&surf, COLOR_SPACE_BLACK);

    /* 2. Terrain and structures in one back-to-front pass, overlays on top */
    moon_world_render(&game->structs, &game->grid, &surf,
                      game->cam_x, game->cam_y,
                      game->hover_tile_x, game->hover_tile_y, game->hover_valid);

    /* Lunar day/night: dim the world, bluer at night. Badges and the UI stay bright. */
    {
        int sun = fixed_to_int(game->resources.solar_intensity * 256); /* 0..256 */
        if (sun < 0) sun = 0;
        if (sun < 256) {
            moon_blit_tint_rect(&surf, 0, 34, width, height - 34 - 40,
                                (uint16_t)(116 + sun * 140 / 256),
                                (uint16_t)(128 + sun * 128 / 256),
                                (uint16_t)(150 + sun * 106 / 256));
        }
    }
    moon_world_render_overlays(&game->structs, &game->grid, &surf, &game->resources,
                               game->cam_x, game->cam_y);

    /* 3. Selection frame around the selected structure's footprint (or tile) */
    render_selection_frame(game, &surf);

    /* 4. Ghost of what the armed brush or the held / hovered build button would build,
     * and the recycle brush's target marker. A brush follows the pointer; a button
     * preview shows on the selected tile. */
    {
        moon_structure_type_t gtype = game->structs.selected_build_type;
        bool brush = (gtype != MOON_STRUCT_NONE);
        if (!brush && game->preview_slot >= 1 && game->preview_slot <= 6) gtype = s_build_buttons[game->preview_slot - 1];
        int tx = game->selected_tile_x, ty = game->selected_tile_y;
        if ((brush || game->demolish_tool_active) && game->hover_valid) {
            tx = game->hover_tile_x;
            ty = game->hover_tile_y;
        }
        if (gtype != MOON_STRUCT_NONE) {
            moon_structure_render_ghost(&game->structs, &game->grid, &surf, game->cam_x, game->cam_y,
                                        gtype, tx, ty);
        } else if (game->demolish_tool_active) {
            const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid, tx, ty);
            int32_t hsx, hsy;
            moon_grid_to_screen(tx, ty, ht ? ht->elevation : 0, game->cam_x, game->cam_y, width, height, &hsx, &hsy);
            moon_blit_iso_tile(&surf, hsx, hsy, MOON_TILE_HALF_W, MOON_TILE_HALF_H, moon_color_dim(COLOR_ALERT_RED, 120), COLOR_ALERT_RED);
            moon_blit_line(&surf, hsx - 10, hsy - 5, hsx + 10, hsy + 5, COLOR_LUNAR_WHITE);
            moon_blit_line(&surf, hsx - 10, hsy + 5, hsx + 10, hsy - 5, COLOR_LUNAR_WHITE);
        }
    }

    /* 5. Render Bottom-Right Minimap Radar */
    moon_grid_render_minimap(&game->grid, &surf, game->ui.minimap.x, game->ui.minimap.y,
                             game->ui.minimap.w, game->ui.minimap.h,
                             game->cam_x, game->cam_y);
    moon_blit_fill_rect(&surf, game->ui.minimap.x + game->selected_tile_x - 1,
                        game->ui.minimap.y + game->selected_tile_y - 1, 3, 3, COLOR_LUNAR_WHITE);

    /* 6. Render Bottom Build Ribbon */
    render_build_ribbon(game, &surf);
    render_undo_button(game, &surf);

    /* 7. Render Top HUD Bar (Title, Day/Night Clock, Power, O2, H2O, Materials, Crew) */
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
    const bool narrow = (width < 800);
    const int hud_h = game->ui.hud.h - 1;
    moon_blit_fill_rect(&surf, 0, 0, width, hud_h, RGB565(14, 16, 20));
    moon_blit_line(&surf, 0, hud_h, width - 1, hud_h, COLOR_GOLD_FOIL);
    const int ty = narrow ? 4 : 8;

    moon_blit_text_fmt(&surf, hud_text_x, ty, clk_col, 0, true, narrow ? "D%02d %02d:%02d" : "D%02d %02d:%02d %s %d:%02d",
                       res->lunar_day, clk.minutes / 60, clk.minutes % 60,
                       clk.next_is_dusk ? "dusk" : "dawn", clk.next_in_min / 60, clk.next_in_min % 60);

    struct { int16_t xw, xn; uint16_t col; const char *fw, *fn; int v[4]; } hud[] = {
        { 226, 110, pwr_col, "PWR %d/%dkW BATT %d/%dkWh", "P:%d/%d",
          { res->power_produced_kw, res->power_consumed_kw, batt_kwh, res->battery_capacity_kwh } },
        { 0, 205, pwr_col, NULL, "B:%d", { batt_kwh } },
        { 476, 280, COLOR_REGOLITH_LIGHT, "RGL %dkg", "R:%d", { rgl_kg } },
        { 576, 360, o2_col, "O2 %dkg", "O2:%d", { o2_kg } },
        { 666, 435, water_col, "H2O %dL", "W:%d", { water_l } },
        { 756, 505, ls_col, "CREW %d/%d (LS:%d%%)", "CRW:%d/%d (%d%%)",
          { res->population, res->max_population, res->life_support_pct } },
    };
    for (unsigned i = 0; i < sizeof(hud) / sizeof(hud[0]); i++) {
        const char *fmt = narrow ? hud[i].fn : hud[i].fw;
        if (!fmt) continue;
        int x = narrow ? hud[i].xn : hud[i].xw + 10;
        if (!narrow) moon_blit_fill_rect(&surf, x - 10, 10, 6, 12, hud[i].col);
        moon_blit_text_fmt(&surf, x, ty, hud[i].col, 0, true, fmt, hud[i].v[0], hud[i].v[1], hud[i].v[2], hud[i].v[3]);
    }

    render_clock(game, &surf);

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
}
