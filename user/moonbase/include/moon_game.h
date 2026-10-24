#ifndef MOON_GAME_H
#define MOON_GAME_H

#include <stdbool.h>
#include <stdint.h>
#include "moon_platform.h"
#include "moon_grid.h"
#include "moon_structures.h"
#include "moon_resources.h"
#include "moon_ui.h"

/* Main Game State Machine */
typedef enum {
    MOON_STATE_INIT = 0,
    MOON_STATE_LANDING,
    MOON_STATE_PLAYING,
    MOON_STATE_PAUSED,
    MOON_STATE_GAME_OVER
} moon_game_state_t;

/* Menu pages */
enum { MOON_MENU_CLOSED = 0, MOON_MENU_MAIN, MOON_MENU_CONFIRM_QUIT, MOON_MENU_CONFIRM_RESTART };

/* Held scroll keys */
enum { MOON_HELD_UP = 1, MOON_HELD_DOWN = 2, MOON_HELD_LEFT = 4, MOON_HELD_RIGHT = 8 };

typedef struct {
    moon_game_state_t state;
    bool     running;
    int      screen_w;
    int      screen_h;

    /* Camera Viewport Offset */
    int32_t  cam_x;
    int32_t  cam_y;

    /* Pointer / Touch Tracking */
    int32_t  pointer_x;
    int32_t  pointer_y;
    bool     pointer_down;
    int32_t  drag_start_x;
    int32_t  drag_start_y;
    int32_t  cam_start_x;
    int32_t  cam_start_y;

    /* Highlighted / Selected Tile */
    int32_t  hover_tile_x;
    int32_t  hover_tile_y;
    bool     hover_valid;

    /* Inspector Selection State */
    int32_t  selected_tile_x;
    int32_t  selected_tile_y;
    bool     has_selection;
    bool     demolish_tool_active;

    /* World Grid & Structures */
    moon_grid_t          grid;
    moon_structure_mgr_t structs;

    /* Base Resources & Life Support Simulation */
    moon_resources_t     resources;

    /* Drag tracking threshold */
    bool was_dragging;
    uint8_t pointer_button;   /* Button of the current press (1=primary/touch, 2=middle, 3=secondary) */
    int  press_region;        /* UI region the current press started in */

    /* Layout table shared by drawing and hit-testing */
    moon_ui_layout_t ui;

    /* Time control: paused or menu open freezes the simulation */
    bool     paused;
    uint8_t  speed;           /* 1, 2 or 4 */

    /* Modal menu: 0 closed, then the pages below */
    uint8_t  menu_page;
    int      menu_sel;        /* Highlighted item (keyboard) */

    /* Input device in use */
    bool     touch_input;     /* Last pointer event came from a finger */
    bool     cursor_mode;     /* Keyboard map cursor drives hover */
    int32_t  cursor_x, cursor_y;
    bool     pending_valid;   /* Touch: tile awaiting confirm (build / recycle) */
    int32_t  pending_x, pending_y;
    bool     key_up_supported; /* Platform reports key releases (set by the platform glue) */
    uint8_t  keys_held;       /* Scroll keys currently down (MOON_HELD_*) */

    /* Live HUD Notification Badge */
    const char *notify_msg;
    uint32_t notify_timer_ms;

    /* Frame and Tick Counters */
    uint32_t frame_count;
    uint32_t sim_ticks;
    uint32_t last_tick_ms;
} moon_game_t;

/* Global or instance game control */
bool moon_game_init(moon_game_t *game, int width, int height);
void moon_game_shutdown(moon_game_t *game);
void moon_game_clamp_camera(moon_game_t *game);
void moon_game_handle_event(moon_game_t *game, const moon_event_t *ev);
void moon_game_update(moon_game_t *game, uint32_t delta_ms);
void moon_game_render(moon_game_t *game, uint16_t *fb, int width, int height, int stride);

#endif /* MOON_GAME_H */
