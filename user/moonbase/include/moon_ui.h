#ifndef MOON_UI_H
#define MOON_UI_H

#include <stdbool.h>
#include <stdint.h>

/* Smallest touch target edge in virtual pixels (about 7 mm on the 7B at 1x and on
 * the Tab5, whose 640x360 frame is shown at 2x). */
#define MOON_UI_TOUCH_MIN 44

typedef struct {
    int x, y, w, h;
} moon_ui_rect_t;

/* Ribbon slots: 0 = inspect, 1..6 = build tools, 7 = recycle */
#define MOON_UI_RIBBON_SLOTS 8
#define MOON_UI_MENU_ITEMS   4

/* Every rectangle the game draws or hit-tests, computed once per screen size.
 * Render and input code both read this table, so they cannot disagree. */
typedef struct {
    int screen_w, screen_h;
    bool narrow;                       /* Compact layout (Tab5's 640x360) */

    moon_ui_rect_t hud;
    int            clock_cx, clock_cy, clock_r;   /* Day/night dial inside the HUD */
    moon_ui_rect_t ribbon_bar;
    moon_ui_rect_t ribbon[MOON_UI_RIBBON_SLOTS];
    moon_ui_rect_t cat_btn, pause_btn, speed_btn, menu_btn;
    moon_ui_rect_t minimap;            /* The 64x64 map itself */
    moon_ui_rect_t minimap_zone;       /* Map plus frame: the hit area */
    moon_ui_rect_t inspector;
    moon_ui_rect_t insp_close, insp_recycle, insp_standby;
    moon_ui_rect_t undo_btn;           /* Shown while the last action can be undone */
    moon_ui_rect_t hover_badge;
    moon_ui_rect_t menu_panel;
    moon_ui_rect_t menu_item[MOON_UI_MENU_ITEMS];
} moon_ui_layout_t;

void moon_ui_layout(moon_ui_layout_t *ui, int screen_w, int screen_h);

static inline bool moon_ui_hit(const moon_ui_rect_t *r, int px, int py) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

#endif /* MOON_UI_H */
