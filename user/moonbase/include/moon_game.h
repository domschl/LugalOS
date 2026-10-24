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

/* Undo records */
enum { MOON_UNDO_NONE = 0, MOON_UNDO_BUILD, MOON_UNDO_RECYCLE };
#define MOON_UNDO_WINDOW_MS 10000

/* Menu pages */
enum { MOON_MENU_CLOSED = 0, MOON_MENU_MAIN, MOON_MENU_CONFIRM_QUIT, MOON_MENU_CONFIRM_RESTART };

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

    /* The selected tile always exists (build / standby / recycle act on it); the
     * inspector panel shows it while panel_open. */
    int32_t  selected_tile_x;
    int32_t  selected_tile_y;
    bool     panel_open;

    /* Brush: a build type (structs.selected_build_type) or recycle stays armed for
     * repeated use; otherwise a build button acts once on the selection. */
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
    int8_t   press_slot;      /* Ribbon button under the current press, or -1 */
    int8_t   preview_slot;    /* Build button hovered or held: the ghost shows it */
    int8_t   last_slot;       /* Slot of the previous press with nothing in between; -1 */

    /* View glide toward a tile selected from the keyboard */
    bool     glide;
    int32_t  glide_x, glide_y;

    /* One-step undo of the last build or recycle */
    uint8_t  undo_kind;       /* MOON_UNDO_* */
    uint8_t  undo_type, undo_x, undo_y;
    int32_t  undo_refund_kg;
    uint32_t undo_ms;

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
