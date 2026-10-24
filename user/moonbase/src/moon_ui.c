#include "moon_ui.h"
#include "moon_grid.h"
#include <string.h>

#define MINIMAP_SIZE 64

static moon_ui_rect_t rect(int x, int y, int w, int h) {
    moon_ui_rect_t r = { x, y, w, h };
    return r;
}

void moon_ui_layout(moon_ui_layout_t *ui, int w, int h) {
    memset(ui, 0, sizeof(*ui));
    ui->screen_w = w;
    ui->screen_h = h;
    ui->narrow = (w < 800);

    const int hud_h = ui->narrow ? 24 : 32;
    ui->hud = rect(0, 0, w, hud_h + 1);
    ui->clock_r = ui->narrow ? 9 : 13;
    ui->clock_cx = 6 + ui->clock_r;
    ui->clock_cy = hud_h / 2;

    /* Ribbon: eight tool buttons on the left, pause / speed / menu on the right */
    const int bar_h = MOON_UI_TOUCH_MIN + 8;
    ui->ribbon_bar = rect(0, h - bar_h, w, bar_h);
    const int by = h - bar_h + 4;
    const int bw = ui->narrow ? 52 : 90;
    const int gap = ui->narrow ? 2 : 4;
    int x = ui->narrow ? 4 : 10;
    for (int i = 0; i < MOON_UI_RIBBON_SLOTS; i++) {
        ui->ribbon[i] = rect(x, by, bw, MOON_UI_TOUCH_MIN);
        x += bw + gap;
    }
    const int sw = ui->narrow ? 52 : 68;
    x = w - 3 * (sw + gap) - (ui->narrow ? 0 : 4);
    ui->pause_btn = rect(x, by, sw, MOON_UI_TOUCH_MIN);
    ui->speed_btn = rect(x + sw + gap, by, sw, MOON_UI_TOUCH_MIN);
    ui->menu_btn  = rect(x + 2 * (sw + gap), by, sw, MOON_UI_TOUCH_MIN);

    ui->minimap = rect(w - MINIMAP_SIZE - 10, h - bar_h - MINIMAP_SIZE - 8, MINIMAP_SIZE, MINIMAP_SIZE);
    ui->minimap_zone = rect(ui->minimap.x - 2, ui->minimap.y - 2, MINIMAP_SIZE + 4, MINIMAP_SIZE + 4);

    /* Inspector: header with a touch-sized close button, body, two touch-sized actions */
    const int ip_w = ui->narrow ? 220 : 260;
    const int ip_h = 206;
    const int ip_y = ui->narrow ? hud_h + 2 : 40;
    ui->inspector = rect(w - ip_w - 10, ip_y, ip_w, ip_h);
    ui->insp_close = rect(ui->inspector.x + ip_w - 40, ip_y + 2, 38, 36);
    const int abw = (ip_w - 24) / 2;
    const int aby = ip_y + ip_h - MOON_UI_TOUCH_MIN - 4;
    ui->insp_recycle = rect(ui->inspector.x + 8, aby, abw, MOON_UI_TOUCH_MIN);
    ui->insp_standby = rect(ui->inspector.x + 16 + abw, aby, abw, MOON_UI_TOUCH_MIN);

    const int cw = 120;
    const int cy = h - bar_h - MOON_UI_TOUCH_MIN - 8;
    ui->confirm_ok = rect(w / 2 - cw - 6, cy, cw, MOON_UI_TOUCH_MIN);
    ui->confirm_cancel = rect(w / 2 + 6, cy, cw, MOON_UI_TOUCH_MIN);

    ui->hover_badge = rect(8, h - bar_h - 24, ui->narrow ? 170 : 210, 20);

    const int mw = 260;
    const int mh = 36 + MOON_UI_MENU_ITEMS * (MOON_UI_TOUCH_MIN + 8) + 4;
    ui->menu_panel = rect((w - mw) / 2, (h - mh) / 2, mw, mh);
    for (int i = 0; i < MOON_UI_MENU_ITEMS; i++) {
        ui->menu_item[i] = rect(ui->menu_panel.x + 16, ui->menu_panel.y + 36 + i * (MOON_UI_TOUCH_MIN + 8),
                                mw - 32, MOON_UI_TOUCH_MIN);
    }
}
