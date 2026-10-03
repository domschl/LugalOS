/*
 * Phase 44: Horizontal Scrollable Tiling Window Ribbon Geometry Manager
 *
 * Implements a 1D non-overlapping horizontal ribbon of windows (Niri-inspired).
 * Portable and freestanding, suitable for execution both in kernel mode
 * and inside the U-mode lcdterm task on RP2350.
 */

#include "drivers/ribbon.h"
#include "drivers/lcdterm_attr.h"
#include "kernel/console.h"

LCDTERM_UTEXT static void cstr_copy(char *dst, const char *src, unsigned max_len) {
    unsigned i = 0;
    if (src) {
        while (src[i] && i + 1 < max_len) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

LCDTERM_UTEXT void ribbon_init(ribbon_t *r, uint16_t screen_w, uint16_t screen_h) {
    if (!r) return;
    r->count = 0;
    r->active_idx = 0;
    r->x_view = 0;
    r->x_target = 0;
    r->screen_w = screen_w ? screen_w : 800u;
    r->screen_h = screen_h ? screen_h : 480u;
    for (unsigned i = 0; i < RIBBON_MAX_WINDOWS; i++) {
        r->wins[i].type = RIBBON_WIN_EMPTY;
        r->wins[i].vterm_id = 0;
        r->wins[i].cols = 0;
        r->wins[i].flags = 0;
        r->wins[i].rx0 = 0;
        r->wins[i].rx1 = 0;
        r->wins[i].title[0] = '\0';
    }
}

LCDTERM_UTEXT void ribbon_layout(ribbon_t *r) {
    if (!r) return;
    int32_t cur_x = RIBBON_BORDER_PAD;
    for (uint8_t i = 0; i < r->count; i++) {
        ribbon_win_t *w = &r->wins[i];
        uint8_t cols = w->cols;
        if (cols < 20) cols = 20;
        uint8_t max_cols = (uint8_t)(r->screen_w / 8u - 2u);
        if (cols > max_cols) cols = max_cols;
        w->cols = cols;
        w->rx0 = cur_x;
        /* Frame width is 8 * cols + 7 pixels; right edge rx1 is rx0 + 8 * cols + 6 */
        w->rx1 = cur_x + (int32_t)(8u * (uint32_t)cols + 6u);
        cur_x = w->rx1 + RIBBON_GAP;
    }
}

LCDTERM_UTEXT int32_t ribbon_total_width(const ribbon_t *r) {
    if (!r || r->count == 0) return (int32_t)(r ? r->screen_w : 800);
    int32_t last_rx1 = r->wins[r->count - 1].rx1;
    int32_t tot = last_rx1 + 2 + RIBBON_BORDER_PAD;
    if (tot < (int32_t)r->screen_w) tot = (int32_t)r->screen_w;
    return tot;
}

LCDTERM_UTEXT int32_t ribbon_compute_viewport(const ribbon_t *r, uint8_t idx, int32_t current_x) {
    if (!r || r->count == 0 || idx >= r->count) return 0;
    const ribbon_win_t *w = &r->wins[idx];
    int32_t rx0 = w->rx0;
    int32_t rx1 = w->rx1;
    int32_t sw = (int32_t)r->screen_w;
    int32_t win_w = rx1 - rx0 + 1;
    int32_t target_x = current_x;

    if (win_w >= sw - 8) {
        /* Full-width window: snap left edge to margin */
        target_x = rx0 - RIBBON_BORDER_PAD;
    } else if (rx0 < current_x + RIBBON_BORDER_PAD) {
        /* Clipped on left: scroll left to reveal left border */
        target_x = rx0 - RIBBON_BORDER_PAD;
    } else if (rx1 + 2 > current_x + sw - RIBBON_BORDER_PAD) {
        /* Clipped on right: scroll right to reveal right border and shadow */
        target_x = rx1 + 2 - sw + RIBBON_BORDER_PAD;
    }

    int32_t max_x = ribbon_total_width(r) - sw;
    if (max_x < 0) max_x = 0;
    if (target_x < 0) target_x = 0;
    if (target_x > max_x) target_x = max_x;

    return target_x;
}

LCDTERM_UTEXT int ribbon_insert(ribbon_t *r, uint8_t idx, uint8_t type, uint8_t vterm_id, uint8_t cols, const char *title) {
    if (!r || r->count >= RIBBON_MAX_WINDOWS) return -1;
    if (idx > r->count) idx = r->count;

    for (int i = (int)r->count - 1; i >= (int)idx; i--) {
        r->wins[i + 1] = r->wins[i];
    }
    r->wins[idx].type = type;
    r->wins[idx].vterm_id = vterm_id;
    r->wins[idx].cols = cols ? cols : (uint8_t)(r->screen_w / 8u - 2u);
    r->wins[idx].flags = 0;
    cstr_copy(r->wins[idx].title, title, sizeof(r->wins[idx].title));

    r->count++;
    r->active_idx = idx;
    ribbon_layout(r);
    r->x_target = ribbon_compute_viewport(r, r->active_idx, r->x_view);
    return (int)idx;
}

LCDTERM_UTEXT bool ribbon_remove(ribbon_t *r, uint8_t idx) {
    if (!r || r->count == 0 || idx >= r->count) return false;

    for (uint8_t i = idx; i + 1 < r->count; i++) {
        r->wins[i] = r->wins[i + 1];
    }
    r->count--;
    if (r->active_idx >= r->count && r->count > 0) {
        r->active_idx = (uint8_t)(r->count - 1u);
    }
    ribbon_layout(r);
    if (r->count > 0) {
        r->x_target = ribbon_compute_viewport(r, r->active_idx, r->x_view);
    } else {
        r->x_target = 0;
    }
    return true;
}

LCDTERM_UTEXT bool ribbon_swap(ribbon_t *r, uint8_t idx_a, uint8_t idx_b) {
    if (!r || idx_a >= r->count || idx_b >= r->count || idx_a == idx_b) return false;
    ribbon_win_t tmp = r->wins[idx_a];
    r->wins[idx_a] = r->wins[idx_b];
    r->wins[idx_b] = tmp;

    if (r->active_idx == idx_a) r->active_idx = idx_b;
    else if (r->active_idx == idx_b) r->active_idx = idx_a;

    ribbon_layout(r);
    r->x_target = ribbon_compute_viewport(r, r->active_idx, r->x_view);
    return true;
}

LCDTERM_UTEXT bool ribbon_move(ribbon_t *r, uint8_t from_idx, int dir) {
    if (!r || r->count < 2 || from_idx >= r->count) return false;
    if (dir < 0 && from_idx > 0) {
        return ribbon_swap(r, from_idx, (uint8_t)(from_idx - 1u));
    } else if (dir > 0 && from_idx + 1 < r->count) {
        return ribbon_swap(r, from_idx, (uint8_t)(from_idx + 1u));
    }
    return false;
}

LCDTERM_UTEXT bool ribbon_focus(ribbon_t *r, uint8_t idx) {
    if (!r || r->count == 0 || idx >= r->count) return false;
    r->active_idx = idx;
    r->x_target = ribbon_compute_viewport(r, r->active_idx, r->x_view);
    return true;
}

LCDTERM_UTEXT bool ribbon_focus_step(ribbon_t *r, int dir) {
    if (!r || r->count == 0) return false;
    if (dir < 0 && r->active_idx > 0) {
        return ribbon_focus(r, (uint8_t)(r->active_idx - 1u));
    } else if (dir > 0 && r->active_idx + 1 < r->count) {
        return ribbon_focus(r, (uint8_t)(r->active_idx + 1u));
    }
    return false;
}

LCDTERM_UTEXT bool ribbon_resize_active(ribbon_t *r, bool expand) {
    if (!r || r->count == 0 || r->active_idx >= r->count) return false;
    ribbon_win_t *w = &r->wins[r->active_idx];
    uint8_t cur = w->cols;
    uint8_t next = cur;
    uint8_t max_cols = (uint8_t)(r->screen_w / 8u - 2u);

    if (expand) {
        if (cur < RIBBON_COLS_38) next = RIBBON_COLS_38;
        else if (cur < RIBBON_COLS_48) next = RIBBON_COLS_48;
        else if (cur < RIBBON_COLS_64) next = RIBBON_COLS_64;
        else if (cur < max_cols) next = max_cols;
    } else {
        if (cur > RIBBON_COLS_64) next = RIBBON_COLS_64;
        else if (cur > RIBBON_COLS_48) next = RIBBON_COLS_48;
        else if (cur > RIBBON_COLS_38) next = RIBBON_COLS_38;
    }

    if (next == cur) return false;
    w->cols = next;
    ribbon_layout(r);
    r->x_target = ribbon_compute_viewport(r, r->active_idx, r->x_view);
    return true;
}

LCDTERM_UTEXT void ribbon_set_title(ribbon_t *r, uint8_t idx, const char *title) {
    if (!r || idx >= r->count) return;
    cstr_copy(r->wins[idx].title, title, sizeof(r->wins[idx].title));
}

LCDTERM_UTEXT int ribbon_find_vterm(const ribbon_t *r, uint8_t vterm_id) {
    if (!r) return -1;
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].type == RIBBON_WIN_TERM && r->wins[i].vterm_id == vterm_id) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT int ribbon_visible_range(const ribbon_t *r, int32_t x_view, uint8_t *first_idx, uint8_t *last_idx) {
    if (!r || r->count == 0) return 0;
    int32_t sw = (int32_t)r->screen_w;
    int first = -1, last = -1;
    int count = 0;

    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].rx1 + 1 >= x_view && r->wins[i].rx0 < x_view + sw) {
            if (first < 0) first = i;
            last = i;
            count++;
        }
    }
    if (first_idx && first >= 0) *first_idx = (uint8_t)first;
    if (last_idx && last >= 0) *last_idx = (uint8_t)last;
    return count;
}

LCDTERM_UTEXT bool ribbon_step_scroll(ribbon_t *r, int32_t step_px) {
    if (!r || r->x_view == r->x_target) return false;
    int32_t diff = r->x_target - r->x_view;
    if (step_px <= 0) {
        int32_t step = diff / 4;
        if (step == 0) step = (diff > 0) ? 1 : -1;
        if (step > 0 && step < 4 && diff >= 4) step = 4;
        if (step < 0 && step > -4 && diff <= -4) step = -4;
        if ((diff > 0 && r->x_view + step >= r->x_target) ||
            (diff < 0 && r->x_view + step <= r->x_target)) {
            r->x_view = r->x_target;
        } else {
            r->x_view += step;
        }
    } else {
        if (diff > 0) {
            r->x_view = (r->x_view + step_px >= r->x_target) ? r->x_target : r->x_view + step_px;
        } else {
            r->x_view = (r->x_view - step_px <= r->x_target) ? r->x_target : r->x_view - step_px;
        }
    }
    return true;
}

/* --- Standalone Selftest --------------------------------------------------- */

static bool st_expect(const char *name, bool cond, int *fails) {
    cprintf("  %-48s ", name);
    if (cond) {
        cprintf("ok\n");
        return true;
    }
    cprintf("FAIL\n");
    (*fails)++;
    return false;
}

void ribbon_selftest(void) {
    cprintf("ribbon geometry selftest:\n");
    int fails = 0;
    ribbon_t r;

    ribbon_init(&r, 800, 480);
    st_expect("ribbon_init zeroes count and sets geometry",
              r.count == 0 && r.screen_w == 800 && r.screen_h == 480 && r.x_view == 0, &fails);

    /* 1. Insert first window: full 98 cols */
    int idx0 = ribbon_insert(&r, 0, RIBBON_WIN_TERM, 0, 98, "Term 0");
    st_expect("insert win0 (98 cols) at index 0",
              idx0 == 0 && r.count == 1 && r.active_idx == 0, &fails);
    st_expect("win0 coordinates: rx0=4, rx1=794",
              r.wins[0].rx0 == 4 && r.wins[0].rx1 == 794, &fails);
    st_expect("ribbon_total_width for win0 is 800",
              ribbon_total_width(&r) == 800, &fails);
    st_expect("compute_viewport for win0 is 0",
              ribbon_compute_viewport(&r, 0, r.x_view) == 0, &fails);

    /* 2. Resize win0 to 48 cols and insert win1 (48 cols) at index 1 */
    r.wins[0].cols = 48;
    ribbon_layout(&r);
    st_expect("win0 resized to 48 cols: rx0=4, rx1=394",
              r.wins[0].rx0 == 4 && r.wins[0].rx1 == 394, &fails);

    int idx1 = ribbon_insert(&r, 1, RIBBON_WIN_TERM, 1, 48, "Term 1");
    st_expect("insert win1 (48 cols) at index 1",
              idx1 == 1 && r.count == 2 && r.active_idx == 1, &fails);
    st_expect("win1 coordinates: rx0=404, rx1=794",
              r.wins[1].rx0 == 404 && r.wins[1].rx1 == 794, &fails);

    uint8_t first = 0, last = 0;
    int vis = ribbon_visible_range(&r, 0, &first, &last);
    st_expect("both 48-col windows visible at x_view=0",
              vis == 2 && first == 0 && last == 1, &fails);

    /* 3. Insert win2 (48 cols) at index 2 */
    int idx2 = ribbon_insert(&r, 2, RIBBON_WIN_TERM, 2, 48, "Term 2");
    st_expect("insert win2 (48 cols) at index 2",
              idx2 == 2 && r.count == 3 && r.active_idx == 2, &fails);
    st_expect("win2 coordinates: rx0=804, rx1=1194",
              r.wins[2].rx0 == 804 && r.wins[2].rx1 == 1194, &fails);

    int32_t vp2 = ribbon_compute_viewport(&r, 2, 0);
    st_expect("target viewport for win2 shifts to 400",
              vp2 == 400, &fails);

    /* 4. Smooth scrolling simulation */
    r.x_target = vp2;
    int steps = 0;
    while (ribbon_step_scroll(&r, 0) && steps < 50) {
        steps++;
    }
    st_expect("step_scroll smoothly reached x_view=400",
              r.x_view == 400, &fails);

    vis = ribbon_visible_range(&r, r.x_view, &first, &last);
    st_expect("visible range at x_view=400 has win1 and win2",
              vis == 2 && first == 1 && last == 2, &fails);

    /* 5. Window movement: Cmd+Ctrl+Left on win2 */
    bool moved = ribbon_move(&r, 2, -1);
    st_expect("ribbon_move(2, -1) moves win2 left",
              moved && r.active_idx == 1, &fails);
    st_expect("win2 now at index 1 (rx0=404, rx1=794)",
              r.wins[1].vterm_id == 2 && r.wins[1].rx0 == 404 && r.wins[1].rx1 == 794, &fails);
    st_expect("win1 now at index 2 (rx0=804, rx1=1194)",
              r.wins[2].vterm_id == 1 && r.wins[2].rx0 == 804 && r.wins[2].rx1 == 1194, &fails);

    /* 6. Window resize cycling (shrink & expand) */
    ribbon_resize_active(&r, false); /* 48 -> 38 */
    st_expect("shrink active window: 48 -> 38 cols (rx1=714)",
              r.wins[1].cols == 38 && r.wins[1].rx1 == 714, &fails);
    st_expect("downstream win1 shifted left: rx0=724",
              r.wins[2].rx0 == 724, &fails);

    ribbon_resize_active(&r, true);  /* 38 -> 48 */
    st_expect("expand active window: 38 -> 48 cols (rx1=794)",
              r.wins[1].cols == 48 && r.wins[1].rx1 == 794, &fails);

    /* 7. Window removal */
    bool removed = ribbon_remove(&r, 1);
    st_expect("ribbon_remove(1) removes middle window",
              removed && r.count == 2, &fails);
    st_expect("remaining windows re-laid out: win0 rx0=4, win1 rx0=404",
              r.wins[0].rx0 == 4 && r.wins[1].rx0 == 404 && r.wins[1].rx1 == 794, &fails);

    if (fails == 0) {
        cprintf("ribbon geometry selftest: PASSED\n");
    } else {
        cprintf("ribbon geometry selftest: FAILED (%d failures)\n", fails);
    }
}
