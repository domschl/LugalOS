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
    MOON_STATE_INIT = 0,        /* Zeroed, before moon_game_init() */
    MOON_STATE_PLAYING,
    MOON_STATE_PAUSED,
    MOON_STATE_GAME_OVER,       /* All crew lost: the overlay offers Restart / Quit */
    MOON_STATE_VICTORY          /* Act I complete: Continue / Restart / Quit */
} moon_game_state_t;

/* Undo records */
enum { MOON_UNDO_NONE = 0, MOON_UNDO_BUILD, MOON_UNDO_RECYCLE };
#define MOON_UNDO_WINDOW_MS 10000

/* Frame-time instrumentation: microseconds per phase of the last frame, and a ring of
 * recent whole-frame times (render + present) for the median and the 95th percentile */
#define MOON_PERF_RING 256
typedef struct {
    uint32_t world, overlay, minimap, ui;
} moon_phase_us_t;

/* Menu pages */
enum { MOON_MENU_CLOSED = 0, MOON_MENU_MAIN, MOON_MENU_CONFIRM_QUIT, MOON_MENU_NEW_GAME,
       MOON_MENU_GAME_OVER, MOON_MENU_VICTORY, MOON_MENU_TITLE };

/* Objectives: a path through the first hours that doubles as the tutorial */
#define MOON_OBJECTIVES 7

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

    /* Map seed of this run; seed_request (set before init) picks the first one, later
     * runs derive theirs from it. 0 = the fixed default seed. */
    uint32_t seed;
    uint32_t seed_request;
    uint8_t  difficulty;        /* MOON_DIFFICULTY_*; kept over restarts */
    uint8_t  difficulty_request; /* before init or a restart: level + 1; 0 = keep */
    bool     show_title;      /* Before init: open on the title page (level choice) */
    bool     keep_seed;       /* Transient: the next reset keeps this run's map */
    uint8_t  objective;       /* Current objective; MOON_OBJECTIVES = all done */
    bool     objectives_hidden;
    bool     victory_claimed; /* Act I was completed and the player chose to go on */

    /* Instrumentation: the platform sets clock_us (NULL: no timing) and reports each
     * frame; F shows the overlay */
    uint32_t (*clock_us)(void);
    moon_phase_us_t phase;
    uint32_t present_us;
    uint32_t frame_ring[MOON_PERF_RING];
    uint32_t frames_noted;
    bool     show_perf;

    /* Redraw bookkeeping: what the next frame has to redo, and a digest of everything the
     * world pass shows that a sim tick can change */
    uint8_t  dirty;
    uint32_t world_sig;
    uint8_t  undo_secs_shown;
    bool     notify_shown;
    int32_t  shown_hover_x, shown_hover_y;
    bool     shown_hover_valid;
    int8_t   shown_preview;

    /* Layout table shared by drawing and hit-testing */
    moon_ui_layout_t ui;

    /* Time control: paused or menu open freezes the simulation */
    bool     paused;
    uint8_t  speed;           /* 1, 2 or 4 */

    /* Modal menu: 0 closed, then the pages below */
    uint8_t  menu_page;
    uint8_t  menu_return;     /* Page the new-game page goes back to */
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

/* Where the ghost of a held / hovered build button or an armed brush is drawn, if at all.
 * A brush follows the pointer; with a finger lifted there is nothing to preview (the
 * selection is the tile just built on), but a keyboard brush previews the selection. */
/* The platform loop reports a finished frame: time spent presenting and in all */
/* How out of date the frame on screen is. The platform loop draws and presents only when it
 * is, and only the top bar and the ribbon when that is all that changed (a sim tick that moved
 * nothing but numbers). Reading it resets it. */
enum { MOON_DIRTY_NONE = 0, MOON_DIRTY_HUD = 1, MOON_DIRTY_WORLD = 2 };
int moon_game_take_dirty(moon_game_t *game);

/* Redraw just the top bar and the ribbon (the rows ui.hud and ui.ribbon_bar) over an
 * up-to-date frame; for MOON_DIRTY_HUD */
void moon_game_render_hud(moon_game_t *game, uint16_t *fb, int width, int height, int stride);

void moon_game_note_frame(moon_game_t *game, uint32_t present_us, uint32_t frame_us);

/* Median and 95th percentile of the recent frames (0 when none) */
void moon_game_frame_stats(const moon_game_t *game, uint32_t *median_us, uint32_t *p95_us);

bool moon_game_ghost_target(const moon_game_t *game, int *gx, int *gy);

/* Move to GAME_OVER or VICTORY when the base has failed or Act I is complete */
void moon_game_check_end(moon_game_t *game);
void moon_game_render(moon_game_t *game, uint16_t *fb, int width, int height, int stride);

#endif /* MOON_GAME_H */
