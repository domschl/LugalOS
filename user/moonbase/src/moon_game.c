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
    bool key_up = game->key_up_supported;
    bool textured = (game->state != MOON_STATE_INIT) ? game->grid.textured_terrain : true;
    memset(game, 0, sizeof(*game));
    game->key_up_supported = key_up;

    moon_textures_init();

    game->state = MOON_STATE_PLAYING;
    game->running = true;
    game->screen_w = width;
    game->screen_h = height;
    game->speed = 1;
    moon_ui_layout(&game->ui, width, height);

    /* Generate procedural lunar terrain */
    moon_grid_generate(&game->grid, 19690720);
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
    game->cursor_x = HOME_GX;
    game->cursor_y = HOME_GY;
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

/* Mapping from the 6 Ribbon buttons / keys to placeable structure types */
static const moon_structure_type_t s_build_buttons[6] = {
    MOON_STRUCT_SOLAR_ARRAY,    /* Button 1 / Key 1 */
    MOON_STRUCT_BATTERY_BANK,   /* Button 2 / Key 2 */
    MOON_STRUCT_HABITAT_DOME,   /* Button 3 / Key 3 */
    MOON_STRUCT_REGOLITH_PLANT, /* Button 4 / Key 4 */
    MOON_STRUCT_O2_EXTRACTOR,   /* Button 5 / Key 5 */
    MOON_STRUCT_POWER_CONDUIT   /* Button 6 / Key 6 */
};

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
enum { UI_MAP = 0, UI_HUD, UI_RIBBON, UI_MINIMAP, UI_INSPECTOR, UI_CONFIRM, UI_MENU };

static int ui_region(const moon_game_t *game, int px, int py) {
    const moon_ui_layout_t *ui = &game->ui;
    if (game->menu_page != MOON_MENU_CLOSED) return UI_MENU;
    if (game->pending_valid &&
        (moon_ui_hit(&ui->confirm_ok, px, py) || moon_ui_hit(&ui->confirm_cancel, px, py))) {
        return UI_CONFIRM;
    }
    if (moon_ui_hit(&ui->ribbon_bar, px, py)) return UI_RIBBON;
    if (moon_ui_hit(&ui->minimap_zone, px, py)) return UI_MINIMAP;
    if (game->has_selection && moon_ui_hit(&ui->inspector, px, py)) return UI_INSPECTOR;
    if (py < ui->hud.h) return UI_HUD;
    return UI_MAP;
}

static const moon_structure_t *find_structure(const moon_game_t *game, uint8_t id) {
    if (id == 0 || id >= MOON_MAX_STRUCTURES) return NULL;
    return (game->structs.list[id].id == id) ? &game->structs.list[id] : NULL;
}

static const moon_structure_t *selected_structure(const moon_game_t *game) {
    if (!game->has_selection) return NULL;
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, game->selected_tile_x, game->selected_tile_y);
    return t ? find_structure(game, t->structure_id) : NULL;
}

static void notify(moon_game_t *game, const char *msg, uint32_t ms) {
    game->notify_msg = msg;
    game->notify_timer_ms = ms;
}

static void update_hover(moon_game_t *game, int px, int py) {
    /* Inspecting or recycling: a building's sprite hides the tile behind it, so
     * the front-most sprite under the pointer wins over the ground tile. */
    if (game->structs.selected_build_type == MOON_STRUCT_NONE) {
        int id;
        if (moon_structure_pick(&game->structs, &game->grid, px, py, game->cam_x, game->cam_y,
                                game->screen_w, game->screen_h, &id)) {
            game->hover_tile_x = game->structs.list[id].gx;
            game->hover_tile_y = game->structs.list[id].gy;
            game->hover_valid = true;
            return;
        }
    }
    game->hover_valid = moon_grid_pick(&game->grid, px, py, game->cam_x, game->cam_y,
                                       game->screen_w, game->screen_h,
                                       &game->hover_tile_x, &game->hover_tile_y);
}

/* The highlighted tile of a touch session is the pending one, if any */
static void touch_restore_hover(moon_game_t *game) {
    if (game->pending_valid) {
        game->hover_tile_x = game->pending_x;
        game->hover_tile_y = game->pending_y;
        game->hover_valid = true;
    } else {
        game->hover_valid = false;
    }
}

/* Recycle the structure on tile (gx, gy) for a 50% refund; the Lander is permanent. */
static void recycle_structure_at(moon_game_t *game, int gx, int gy) {
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, gx, gy);
    if (!t) return;
    const moon_structure_t *s = find_structure(game, t->structure_id);
    if (!s || s->type == MOON_STRUCT_LANDER) return;

    const moon_structure_def_t *def = moon_structure_get_def((moon_structure_type_t)s->type);
    if (def) {
        game->resources.regolith_stored_kg += fixed_from_int(def->cost_regolith / 2);
        fixed32_t cap = fixed_from_int(game->resources.regolith_capacity_kg);
        if (game->resources.regolith_stored_kg > cap) game->resources.regolith_stored_kg = cap;
    }
    moon_structure_demolish(&game->grid, &game->structs, gx, gy);
    moon_resources_solve_power_grid(&game->grid, &game->structs);
}

static bool try_place_selected_structure(moon_game_t *game) {
    if (game->structs.selected_build_type == MOON_STRUCT_NONE || !game->hover_valid) return false;

    const moon_structure_def_t *def = moon_structure_get_def(game->structs.selected_build_type);
    if (!def) return false;

    fixed32_t cost = fixed_from_int(def->cost_regolith);
    if (game->resources.regolith_stored_kg < cost) {
        notify(game, "INSUFFICIENT REGOLITH!", 2500);
        return false;
    }

    moon_structure_t *placed = moon_structure_place(&game->grid, &game->structs,
                                                    game->structs.selected_build_type,
                                                    game->hover_tile_x, game->hover_tile_y);
    if (placed) {
        game->resources.regolith_stored_kg -= cost;
        moon_resources_solve_power_grid(&game->grid, &game->structs);
        /* Deselect tool on placement so ghost does not obscure the newly built structure */
        game->structs.selected_build_type = MOON_STRUCT_NONE;
        return true;
    }
    const char *reason = NULL;
    (void)moon_structure_can_place(&game->grid, &game->structs,
                                   game->structs.selected_build_type,
                                   game->hover_tile_x, game->hover_tile_y, &reason);
    if (reason) notify(game, reason, 2000);
    return false;
}

/* ---- Tools and actions shared by mouse, touch and keyboard ---- */

/* Ribbon slot of the active tool: 0 inspect, 1..6 build, 7 recycle */
static int active_slot(const moon_game_t *game) {
    if (game->demolish_tool_active) return 7;
    for (int i = 0; i < 6; i++) {
        if (game->structs.selected_build_type == s_build_buttons[i]) return i + 1;
    }
    return 0;
}

static void select_slot(moon_game_t *game, int slot) {
    game->pending_valid = false;
    game->structs.selected_build_type = MOON_STRUCT_NONE;
    if (slot == 7) {
        game->demolish_tool_active = !game->demolish_tool_active;
    } else {
        game->demolish_tool_active = false;
        if (slot >= 1 && slot <= 6) game->structs.selected_build_type = s_build_buttons[slot - 1];
    }
    if (game->touch_input) game->hover_valid = false;
}

/* One map action on the highlighted tile: build, recycle or inspect */
static void map_action(moon_game_t *game) {
    if (game->demolish_tool_active) {
        if (game->hover_valid) recycle_structure_at(game, game->hover_tile_x, game->hover_tile_y);
    } else if (game->structs.selected_build_type != MOON_STRUCT_NONE) {
        try_place_selected_structure(game);
    } else if (game->hover_valid) {
        game->selected_tile_x = game->hover_tile_x;
        game->selected_tile_y = game->hover_tile_y;
        game->has_selection = true;
    } else {
        game->has_selection = false;
    }
}

static void confirm_pending(moon_game_t *game) {
    if (!game->pending_valid) return;
    game->hover_tile_x = game->pending_x;
    game->hover_tile_y = game->pending_y;
    game->hover_valid = true;
    game->pending_valid = false;
    map_action(game);
    if (game->touch_input) game->hover_valid = false;
}

static void toggle_standby_selected(moon_game_t *game) {
    const moon_structure_t *s = selected_structure(game);
    if (!s || s->type == MOON_STRUCT_LANDER) return;
    game->structs.list[s->id].active = !s->active;
    moon_structure_mark_dirty(&game->structs);
    moon_resources_solve_power_grid(&game->grid, &game->structs);
}

static void recycle_selected(moon_game_t *game) {
    const moon_structure_t *s = selected_structure(game);
    if (!s || s->type == MOON_STRUCT_LANDER) return;
    recycle_structure_at(game, game->selected_tile_x, game->selected_tile_y);
    game->has_selection = false;
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

/* ---- Menu ---- */

static int menu_item_count(const moon_game_t *game) {
    return game->menu_page == MOON_MENU_MAIN ? MOON_UI_MENU_ITEMS : 2;
}

static void menu_open(moon_game_t *game) {
    game->menu_page = MOON_MENU_MAIN;
    game->menu_sel = 0;
    game->pending_valid = false;
    game->pointer_down = false;
    game->keys_held = 0;
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
    default:
        break;
    }
}

/* Same on every device: back out of the innermost thing (menu page, tool, selection).
 * Returns false when there was nothing to back out of. */
static bool cancel_action(moon_game_t *game) {
    if (game->menu_page == MOON_MENU_CONFIRM_QUIT || game->menu_page == MOON_MENU_CONFIRM_RESTART) {
        game->menu_page = MOON_MENU_MAIN;
        return true;
    }
    if (game->menu_page == MOON_MENU_MAIN) {
        game->menu_page = MOON_MENU_CLOSED;
        return true;
    }
    if (game->structs.selected_build_type != MOON_STRUCT_NONE || game->demolish_tool_active ||
        game->pending_valid) {
        game->structs.selected_build_type = MOON_STRUCT_NONE;
        game->demolish_tool_active = false;
        game->pending_valid = false;
        if (game->touch_input) game->hover_valid = false;
        return true;
    }
    if (game->has_selection) {
        game->has_selection = false;
        return true;
    }
    return false;
}

/* ESC / Q: cancel, and with nothing to cancel open the menu (never quits directly) */
static void escape_action(moon_game_t *game) {
    if (!cancel_action(game)) menu_open(game);
}

/* ---- Keyboard map cursor ---- */

static void cursor_show(moon_game_t *game) {
    game->hover_tile_x = game->cursor_x;
    game->hover_tile_y = game->cursor_y;
    game->hover_valid = true;
}

/* Bring the cursor tile into the middle of the view if it left the visible map area */
static void cursor_scroll_into_view(moon_game_t *game) {
    const moon_tile_t *t = moon_grid_get_tile_const(&game->grid, game->cursor_x, game->cursor_y);
    int32_t sx, sy;
    moon_grid_to_screen(game->cursor_x, game->cursor_y, t ? t->elevation : 0,
                        game->cam_x, game->cam_y, game->screen_w, game->screen_h, &sx, &sy);
    int top = game->ui.hud.h + 40;
    int bottom = game->ui.ribbon_bar.y - 40;
    if (sx < 60 || sx > game->screen_w - 60 || sy < top || sy > bottom) {
        game->cam_x = (game->cursor_x - game->cursor_y) * MOON_TILE_HALF_W;
        game->cam_y = (game->cursor_x + game->cursor_y) * MOON_TILE_HALF_H;
        clamp_camera(game);
    }
}

static void cursor_move(moon_game_t *game, int dgx, int dgy) {
    if (!game->cursor_mode) {
        game->cursor_mode = true;
        if (game->hover_valid) {
            game->cursor_x = game->hover_tile_x;
            game->cursor_y = game->hover_tile_y;
        }
    }
    int nx = game->cursor_x + dgx, ny = game->cursor_y + dgy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= MOON_MAP_SIZE_X) nx = MOON_MAP_SIZE_X - 1;
    if (ny >= MOON_MAP_SIZE_Y) ny = MOON_MAP_SIZE_Y - 1;
    game->cursor_x = nx;
    game->cursor_y = ny;
    cursor_scroll_into_view(game);
    cursor_show(game);
}

static int32_t drag_limit_sq(const moon_game_t *game) {
    int32_t px = game->touch_input ? DRAG_LIMIT_TOUCH_PX : DRAG_LIMIT_MOUSE_PX;
    return px * px;
}

static void pan_by(moon_game_t *game, int dx, int dy) {
    game->cam_x += dx;
    game->cam_y += dy;
    clamp_camera(game);
}

static void handle_key_down(moon_game_t *game, uint32_t key) {
    /* Menu keys */
    if (game->menu_page != MOON_MENU_CLOSED) {
        int n = menu_item_count(game);
        switch (key) {
        case MOON_KEY_ESCAPE: case MOON_KEY_Q: case MOON_KEY_BACKSPACE:
            cancel_action(game);
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

    switch (key) {
    case MOON_KEY_ESCAPE:
    case MOON_KEY_Q:
        escape_action(game);
        break;
    case MOON_KEY_BACKSPACE:
        cancel_action(game);
        break;

    case MOON_KEY_0: select_slot(game, 0); break;
    case MOON_KEY_1: case MOON_KEY_2: case MOON_KEY_3:
    case MOON_KEY_4: case MOON_KEY_5: case MOON_KEY_6:
        select_slot(game, (int)(key - MOON_KEY_0));
        break;
    case MOON_KEY_R: select_slot(game, 7); break;
    case MOON_KEY_TAB: select_slot(game, (active_slot(game) + 1) % MOON_UI_RIBBON_SLOTS); break;

    case MOON_KEY_T: toggle_texture(game); break;
    case MOON_KEY_P: toggle_pause(game); break;
    case MOON_KEY_MINUS: set_speed(game, game->speed / 2); break;
    case MOON_KEY_PLUS:  set_speed(game, game->speed * 2); break;
    case MOON_KEY_E: toggle_standby_selected(game); break;

    /* Arrows move the map cursor; Enter acts on it (or confirms a touch placement) */
    case MOON_KEY_UP:    cursor_move(game, -1, -1); break;
    case MOON_KEY_DOWN:  cursor_move(game,  1,  1); break;
    case MOON_KEY_LEFT:  cursor_move(game, -1,  1); break;
    case MOON_KEY_RIGHT: cursor_move(game,  1, -1); break;
    case MOON_KEY_RETURN:
        if (game->pending_valid) {
            confirm_pending(game);
        } else {
            if (!game->cursor_mode) { game->cursor_mode = true; }
            cursor_show(game);
            map_action(game);
        }
        break;

    /* WASD scrolls the view: continuous while held, or one step per key repeat */
    case MOON_KEY_W: case MOON_KEY_A: case MOON_KEY_S: case MOON_KEY_D: {
        uint8_t bit = (key == MOON_KEY_W) ? MOON_HELD_UP : (key == MOON_KEY_S) ? MOON_HELD_DOWN :
                      (key == MOON_KEY_A) ? MOON_HELD_LEFT : MOON_HELD_RIGHT;
        if (game->key_up_supported) {
            game->keys_held |= bit;
        } else {
            pan_by(game, (bit == MOON_HELD_LEFT ? -KEY_STEP_PX : bit == MOON_HELD_RIGHT ? KEY_STEP_PX : 0),
                         (bit == MOON_HELD_UP ? -KEY_STEP_PX : bit == MOON_HELD_DOWN ? KEY_STEP_PX : 0));
        }
        break;
    }
    case MOON_KEY_SPACE:
        camera_home(game);
        break;
    default:
        break;
    }
}

static void handle_key_up(moon_game_t *game, uint32_t key) {
    switch (key) {
    case MOON_KEY_W: game->keys_held &= (uint8_t)~MOON_HELD_UP; break;
    case MOON_KEY_S: game->keys_held &= (uint8_t)~MOON_HELD_DOWN; break;
    case MOON_KEY_A: game->keys_held &= (uint8_t)~MOON_HELD_LEFT; break;
    case MOON_KEY_D: game->keys_held &= (uint8_t)~MOON_HELD_RIGHT; break;
    default: break;
    }
}

/* ---- Pointer clicks per region ---- */

static void click_menu(moon_game_t *game, int px, int py) {
    for (int i = 0; i < menu_item_count(game); i++) {
        if (moon_ui_hit(&game->ui.menu_item[i], px, py)) {
            menu_activate(game, i);
            return;
        }
    }
}

static void click_ribbon(moon_game_t *game, int px, int py) {
    const moon_ui_layout_t *ui = &game->ui;
    for (int i = 0; i < MOON_UI_RIBBON_SLOTS; i++) {
        if (moon_ui_hit(&ui->ribbon[i], px, py)) {
            select_slot(game, i);
            return;
        }
    }
    if (moon_ui_hit(&ui->pause_btn, px, py)) toggle_pause(game);
    else if (moon_ui_hit(&ui->speed_btn, px, py)) set_speed(game, game->speed >= 4 ? 1 : game->speed * 2);
    else if (moon_ui_hit(&ui->menu_btn, px, py)) menu_open(game);
}

static void click_minimap(moon_game_t *game, int px, int py) {
    int tx = px - game->ui.minimap.x;
    int ty = py - game->ui.minimap.y;
    game->cam_x = (tx - ty) * MOON_TILE_HALF_W;
    game->cam_y = (tx + ty) * MOON_TILE_HALF_H;
    clamp_camera(game);
}

static void click_inspector(moon_game_t *game, int px, int py) {
    const moon_ui_layout_t *ui = &game->ui;
    if (moon_ui_hit(&ui->insp_close, px, py)) {
        game->has_selection = false;
        return;
    }
    const moon_structure_t *s = selected_structure(game);
    if (!s || s->type == MOON_STRUCT_LANDER) return;
    if (moon_ui_hit(&ui->insp_recycle, px, py)) recycle_selected(game);
    else if (moon_ui_hit(&ui->insp_standby, px, py)) toggle_standby_selected(game);
}

static void click_map(moon_game_t *game) {
    bool tool = game->demolish_tool_active || game->structs.selected_build_type != MOON_STRUCT_NONE;
    if (game->touch_input && tool) {
        /* Two steps on a finger: the tap positions the ghost, the confirm bar acts */
        if (game->hover_valid) {
            game->pending_valid = true;
            game->pending_x = game->hover_tile_x;
            game->pending_y = game->hover_tile_y;
        }
        return;
    }
    map_action(game);
    if (game->touch_input) game->hover_valid = false;
}

void moon_game_handle_event(moon_game_t *game, const moon_event_t *ev) {
    if (!game || !ev) return;

    switch (ev->type) {
        case MOON_EVENT_QUIT:
            game->running = false;
            break;

        case MOON_EVENT_KEY_DOWN:
            handle_key_down(game, ev->key);
            break;

        case MOON_EVENT_KEY_UP:
            handle_key_up(game, ev->key);
            break;

        case MOON_EVENT_POINTER_LEAVE:
            if (!game->touch_input && !game->cursor_mode) game->hover_valid = false;
            break;

        case MOON_EVENT_POINTER_DOWN:
            game->touch_input = ev->touch != 0;
            game->cursor_mode = false;
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
            if (game->press_region == UI_MAP) update_hover(game, ev->x, ev->y);
            else if (game->touch_input) touch_restore_hover(game);
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

            int region = ui_region(game, ev->x, ev->y);
            if (region == UI_MAP) update_hover(game, ev->x, ev->y);

            /* Only a press and release in the same region is a click */
            if (had_press && region == game->press_region) {
                if (ev->button == 3) {
                    cancel_action(game);
                } else if (ev->button == 1 && !game->was_dragging) {
                    switch (region) {
                    case UI_MENU:      click_menu(game, ev->x, ev->y); break;
                    case UI_CONFIRM:
                        if (moon_ui_hit(&game->ui.confirm_ok, ev->x, ev->y)) confirm_pending(game);
                        else cancel_action(game);
                        break;
                    case UI_RIBBON:    click_ribbon(game, ev->x, ev->y); break;
                    case UI_MINIMAP:   click_minimap(game, ev->x, ev->y); break;
                    case UI_INSPECTOR: click_inspector(game, ev->x, ev->y); break;
                    case UI_HUD:       break;
                    default:           click_map(game); break;
                    }
                }
            }
            if (game->touch_input) touch_restore_hover(game);
            else if (region != UI_MAP) game->hover_valid = false;
            break;
        }

        case MOON_EVENT_POINTER_MOVE:
            game->pointer_x = ev->x;
            game->pointer_y = ev->y;
            if (game->pointer_down) {
                int32_t dx = ev->x - game->drag_start_x;
                int32_t dy = ev->y - game->drag_start_y;
                if (dx * dx + dy * dy > drag_limit_sq(game)) game->was_dragging = true;
                /* Only a drag that started on the map (left, middle or touch) pans it */
                if (game->press_region == UI_MAP && game->pointer_button != 3 && game->was_dragging) {
                    game->cam_x = game->cam_start_x - dx;
                    game->cam_y = game->cam_start_y - dy;
                    clamp_camera(game);
                }
            }
            game->touch_input = ev->touch != 0;
            if (!game->touch_input) game->cursor_mode = false;
            if (game->touch_input) {
                if (game->pointer_down && game->press_region == UI_MAP && !game->was_dragging) {
                    update_hover(game, ev->x, ev->y); /* finger preview */
                } else {
                    touch_restore_hover(game);
                }
            } else if (ui_region(game, ev->x, ev->y) == UI_MAP) {
                update_hover(game, ev->x, ev->y);
            } else {
                game->hover_valid = false;
            }
            break;

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

    /* Held scroll keys work while paused too */
    if (game->keys_held) {
        int step = (int)(SCROLL_PX_PER_S * delta_ms / 1000);
        if (step < 1) step = 1;
        pan_by(game,
               ((game->keys_held & MOON_HELD_RIGHT) ? step : 0) - ((game->keys_held & MOON_HELD_LEFT) ? step : 0),
               ((game->keys_held & MOON_HELD_DOWN) ? step : 0) - ((game->keys_held & MOON_HELD_UP) ? step : 0));
    }

    if (game->paused || game->menu_page != MOON_MENU_CLOSED) return;

    game->last_tick_ms += delta_ms * game->speed;
    while (game->last_tick_ms >= MOON_SIM_TICK_INTERVAL_MS) {
        game->last_tick_ms -= MOON_SIM_TICK_INTERVAL_MS;
        game->sim_ticks++;
        moon_resources_tick(&game->resources, &game->grid, &game->structs);
    }
}

static void render_inspector_panel(const moon_game_t *game, const moon_surface_t *surf) {
    if (!game->has_selection) return;

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
            moon_blit_text(surf, rb->x + 6, rb->y + rb->h / 2 - 8, ui->narrow ? "RECYCLE" : "RECYCLE 50%", COLOR_LUNAR_WHITE, 0, true);

            const moon_ui_rect_t *ab = &ui->insp_standby;
            uint16_t act_bg = s->active ? RGB565(20, 45, 30) : RGB565(45, 40, 20);
            uint16_t act_border = s->active ? COLOR_STATUS_GREEN : COLOR_ENERGY_YELLOW;
            moon_blit_fill_rect(surf, ab->x, ab->y, ab->w, ab->h, act_bg);
            moon_blit_rect_outline(surf, ab->x, ab->y, ab->w, ab->h, act_border);
            moon_blit_text(surf, ab->x + 6, ab->y + ab->h / 2 - 8, s->active ? "STANDBY" : "ACTIVATE", COLOR_LUNAR_WHITE, 0, true);
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
        "0:INSPECT",
        "1:SOLAR 10t", "2:BATT  20t", "3:HAB   50t",
        "4:SINT  15t", "5:OXY   25t", "6:CABLE  2t",
        "R:RECYCLE"
    };

    static const char *labels_narrow[8] = {
        "0:INSP",
        "1:SOL", "2:BAT", "3:HAB",
        "4:SINT", "5:OXY", "6:WIRE",
        "R:DEM"
    };

    int active = active_slot(game);
    for (int i = 0; i < MOON_UI_RIBBON_SLOTS; i++) {
        bool selected = (i == active);
        bool can_afford = true;
        if (i >= 1 && i <= 6) {
            const moon_structure_def_t *def = moon_structure_get_def(s_build_buttons[i - 1]);
            can_afford = def && (game->resources.regolith_stored_kg >= fixed_from_int(def->cost_regolith));
        }

        uint16_t bg = selected ? (i == 7 ? RGB565(55, 20, 20) : RGB565(30, 45, 60)) : RGB565(20, 24, 30);
        uint16_t border = selected ? (i == 7 ? COLOR_ALERT_RED : COLOR_LASER_CYAN) : (can_afford ? RGB565(60, 65, 75) : RGB565(35, 38, 45));
        uint16_t text_col = selected ? COLOR_LUNAR_WHITE : (can_afford ? RGB565(170, 175, 185) : RGB565(80, 85, 95));
        draw_button(surf, &ui->ribbon[i], narrow ? labels_narrow[i] : labels[i], bg, border, text_col);
    }

    /* Pause, speed and menu */
    const char *spd = game->speed == 4 ? (narrow ? "4x" : "SPD:4x") :
                      game->speed == 2 ? (narrow ? "2x" : "SPD:2x") : (narrow ? "1x" : "SPD:1x");
    draw_button(surf, &ui->pause_btn, game->paused ? (narrow ? "RUN" : "P:RUN") : (narrow ? "PAUSE" : "P:PAUSE"),
                game->paused ? RGB565(55, 45, 15) : RGB565(20, 24, 30),
                game->paused ? COLOR_ENERGY_YELLOW : RGB565(60, 65, 75), COLOR_LUNAR_WHITE);
    draw_button(surf, &ui->speed_btn, spd,
                game->speed > 1 ? RGB565(20, 40, 55) : RGB565(20, 24, 30),
                game->speed > 1 ? COLOR_LASER_CYAN : RGB565(60, 65, 75), COLOR_LUNAR_WHITE);
    draw_button(surf, &ui->menu_btn, "MENU", RGB565(20, 24, 30), RGB565(60, 65, 75), COLOR_LUNAR_WHITE);
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

static void render_confirm_bar(const moon_game_t *game, const moon_surface_t *surf) {
    if (!game->pending_valid) return;
    const moon_ui_layout_t *ui = &game->ui;
    bool demo = game->demolish_tool_active;
    draw_button(surf, &ui->confirm_ok, demo ? "RECYCLE" : "BUILD",
                demo ? RGB565(55, 20, 20) : RGB565(20, 55, 30),
                demo ? COLOR_ALERT_RED : COLOR_STATUS_GREEN, COLOR_LUNAR_WHITE);
    draw_button(surf, &ui->confirm_cancel, "CANCEL", RGB565(30, 32, 40), RGB565(110, 115, 125), COLOR_LUNAR_WHITE);
}

static void render_menu(const moon_game_t *game, const moon_surface_t *surf) {
    if (game->menu_page == MOON_MENU_CLOSED) return;
    const moon_ui_layout_t *ui = &game->ui;
    const moon_ui_rect_t *p = &ui->menu_panel;

    moon_blit_tint_rect(surf, 0, 0, surf->width, surf->height, 110, 110, 120);
    moon_blit_fill_rect(surf, p->x, p->y, p->w, p->h, RGB565(14, 18, 24));
    moon_blit_rect_outline(surf, p->x, p->y, p->w, p->h, COLOR_GOLD_FOIL);

    const char *title = "MOONBASE LAMBDA";
    const char *items[MOON_UI_MENU_ITEMS];
    int n = 2;
    if (game->menu_page == MOON_MENU_MAIN) {
        items[0] = "RESUME";
        items[1] = game->grid.textured_terrain ? "TERRAIN: TEX" : "TERRAIN: FAST";
        items[2] = "RESTART";
        items[3] = "QUIT";
        n = MOON_UI_MENU_ITEMS;
    } else {
        title = (game->menu_page == MOON_MENU_CONFIRM_QUIT) ? "QUIT GAME?" : "RESTART? BASE IS LOST";
        items[0] = (game->menu_page == MOON_MENU_CONFIRM_QUIT) ? "YES, QUIT" : "YES, RESTART";
        items[1] = "NO";
    }
    moon_blit_text(surf, p->x + 16, p->y + 10, title, COLOR_GOLD_FOIL, 0, true);
    for (int i = 0; i < n; i++) {
        bool sel = (i == game->menu_sel);
        draw_button(surf, &ui->menu_item[i], items[i],
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
    if (game->has_selection) {
        render_selection_frame(game, &surf);
    }

    /* 4. Placement ghost or demolish marker */
    if (game->structs.selected_build_type != MOON_STRUCT_NONE && game->hover_valid) {
        moon_structure_render_ghost(&game->structs, &game->grid, &surf,
                                    game->cam_x, game->cam_y,
                                    game->hover_tile_x, game->hover_tile_y);
    } else if (game->demolish_tool_active && game->hover_valid) {
        /* Render bulldozer / demolish target indicator */
        const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid, game->hover_tile_x, game->hover_tile_y);
        int elev = ht ? ht->elevation : 0;
        int32_t hsx, hsy;
        moon_grid_to_screen(game->hover_tile_x, game->hover_tile_y, elev, game->cam_x, game->cam_y, width, height, &hsx, &hsy);
        moon_blit_iso_tile(&surf, hsx, hsy, MOON_TILE_HALF_W, MOON_TILE_HALF_H, moon_color_dim(COLOR_ALERT_RED, 120), COLOR_ALERT_RED);
        moon_blit_line(&surf, hsx - 10, hsy - 5, hsx + 10, hsy + 5, COLOR_LUNAR_WHITE);
        moon_blit_line(&surf, hsx - 10, hsy + 5, hsx + 10, hsy - 5, COLOR_LUNAR_WHITE);
    }

    /* 5. Render Bottom-Right Minimap Radar */
    moon_grid_render_minimap(&game->grid, &surf, game->ui.minimap.x, game->ui.minimap.y,
                             game->ui.minimap.w, game->ui.minimap.h,
                             game->cam_x, game->cam_y);

    /* 6. Render Bottom Build Ribbon */
    render_build_ribbon(game, &surf);
    render_confirm_bar(game, &surf);

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

    /* Right HUD Info (Hover Tile Coordinates & Resource Inspector) */
    if (game->hover_valid) {
        const moon_tile_t *ht = moon_grid_get_tile_const(&game->grid,
                                                         game->hover_tile_x,
                                                         game->hover_tile_y);
        const char *tname = ht ? get_terrain_name((moon_terrain_type_t)ht->terrain) : "VOID";
        int elev = ht ? ht->elevation : 0;
        int rval = ht ? ht->resource_val : 0;

        const moon_ui_rect_t *hb = &game->ui.hover_badge;
        int badge_x = hb->x, badge_y = hb->y;
        int badge_w = (22 + (int)strlen(tname)) * MOON_FONT_W + 8; /* "[xx,yy] NAME E:n R:nnn%" */
        moon_blit_fill_rect(&surf, badge_x, badge_y, badge_w, hb->h, RGB565(26, 30, 36));
        moon_blit_rect_outline(&surf, badge_x, badge_y, badge_w, hb->h, COLOR_LASER_CYAN);

        moon_blit_text_fmt(&surf, badge_x + 4, badge_y + 2, COLOR_LASER_CYAN, 0, true,
                           "[%02d,%02d] %s E:%d R:%d%%",
                           game->hover_tile_x, game->hover_tile_y,
                           tname, elev, rval);
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
