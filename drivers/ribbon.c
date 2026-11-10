/*
 * Horizontal Scrollable Tiling Window Ribbon Geometry Manager
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

LCDTERM_UTEXT static void win_copy(ribbon_win_t *dst, const ribbon_win_t *src) {
    uint32_t *d = (uint32_t *)(void *)dst;
    const uint32_t *s = (const uint32_t *)(const void *)src;
    for (unsigned i = 0; i < sizeof(ribbon_win_t) / sizeof(uint32_t); i++) {
        d[i] = s[i];
    }
}

LCDTERM_UTEXT static void win_zero(ribbon_win_t *dst) {
    uint32_t *d = (uint32_t *)(void *)dst;
    for (unsigned i = 0; i < sizeof(ribbon_win_t) / sizeof(uint32_t); i++) {
        d[i] = 0;
    }
    dst->owner_pid = -1;
}

LCDTERM_UTEXT void ribbon_init(ribbon_t *r, uint16_t screen_w, uint16_t screen_h) {
    if (!r) return;
    r->count = 0;
    r->active_idx = 0;
    r->m_view = 0;
    r->m_target = 0;
    r->screen_w = screen_w ? screen_w : 800u;
    r->screen_h = screen_h ? screen_h : 480u;
#if RIBBON_AXIS_Y_SUPPORTED
    r->axis = RIBBON_AXIS_X;
    r->y_full = 0;
    r->y_pad0 = 0;
    r->y_extra = 0;
#endif
    for (unsigned i = 0; i < RIBBON_MAX_WINDOWS; i++) {
        r->wins[i].type = RIBBON_WIN_EMPTY;
        r->wins[i].vterm_id = 0;
        r->wins[i].size = 0;
        r->wins[i].flags = 0;
        r->wins[i].owner_pid = -1;
        r->wins[i].reserved = 0;
        r->wins[i].m0 = 0;
        r->wins[i].m1 = 0;
        r->wins[i].title[0] = '\0';
    }
}

LCDTERM_UTEXT bool ribbon_set_axis(ribbon_t *r, uint8_t axis, uint16_t y_pad0, uint16_t y_extra, uint8_t y_full) {
    if (!r) return false;
#if RIBBON_AXIS_Y_SUPPORTED
    r->axis = axis == RIBBON_AXIS_Y ? RIBBON_AXIS_Y : RIBBON_AXIS_X;
    r->y_pad0 = y_pad0;
    r->y_extra = y_extra;
    r->y_full = y_full;
    return true;
#else
    (void)y_pad0; (void)y_extra; (void)y_full;
    return axis == RIBBON_AXIS_X;
#endif
}

/* The major axis's geometry. A window `size` units long spans
 * cell * size + extra pixels; the first starts at pad0, windows are
 * RIBBON_GAP apart, and after the last come its 2 px of shadow and pad1. */
#define AXIS_Y(r) (RIBBON_AXIS(r) == RIBBON_AXIS_Y)

LCDTERM_UTEXT static int32_t ax_cell(const ribbon_t *r) {
    return AXIS_Y(r) ? (int32_t)FONT_CELL_H : (int32_t)FONT_CELL_W;
}

LCDTERM_UTEXT static int32_t ax_extra(const ribbon_t *r) {
#if RIBBON_AXIS_Y_SUPPORTED
    if (AXIS_Y(r)) return (int32_t)r->y_extra;
#endif
    (void)r;
    return 7;                                            /* the frame's borders and the text's inset */
}

LCDTERM_UTEXT static int32_t ax_pad0(const ribbon_t *r) {
#if RIBBON_AXIS_Y_SUPPORTED
    if (AXIS_Y(r)) return (int32_t)r->y_pad0;
#endif
    (void)r;
    return RIBBON_BORDER_PAD;
}

LCDTERM_UTEXT static int32_t ax_pad1(const ribbon_t *r) {
    return AXIS_Y(r) ? 0 : RIBBON_BORDER_PAD;
}

LCDTERM_UTEXT static int32_t ax_len(const ribbon_t *r) {
    return AXIS_Y(r) ? (int32_t)r->screen_h : (int32_t)r->screen_w;
}

LCDTERM_UTEXT uint8_t ribbon_size_full(const ribbon_t *r) {
    if (!r) return 1;
#if RIBBON_AXIS_Y_SUPPORTED
    if (AXIS_Y(r)) return r->y_full ? r->y_full : 1u;
#endif
    return RIBBON_COLS_FULL(r->screen_w);
}

LCDTERM_UTEXT uint8_t ribbon_size_pair(const ribbon_t *r) {
    if (!r) return 2;
#if RIBBON_AXIS_Y_SUPPORTED
    if (AXIS_Y(r)) {
        /* Two windows and the gap in the length one window takes alone. */
        int32_t px = ax_cell(r) * (int32_t)ribbon_size_full(r) - ax_extra(r) - RIBBON_GAP;
        int32_t units = px / ax_cell(r);
        return (uint8_t)(units < 2 * (int32_t)RIBBON_ROWS_MIN ? 2 * (int32_t)RIBBON_ROWS_MIN : units);
    }
#endif
    return (uint8_t)RIBBON_COLS_PAIR(r->screen_w);
}

LCDTERM_UTEXT uint8_t ribbon_size_half(const ribbon_t *r) {
    return (uint8_t)(ribbon_size_pair(r) / 2u);
}

LCDTERM_UTEXT void ribbon_layout(ribbon_t *r) {
    if (!r) return;
    int32_t cur = ax_pad0(r);
    int32_t cell = ax_cell(r), extra = ax_extra(r);
    uint8_t max_size = ribbon_size_full(r);
    for (uint8_t i = 0; i < r->count; i++) {
        ribbon_win_t *w = &r->wins[i];
        uint8_t size = w->size;
        uint8_t min_size = AXIS_Y(r) ? RIBBON_ROWS_MIN : (w->type == RIBBON_WIN_TERM ? RIBBON_TERM_MIN_COLS : 1u);
        if (size < min_size) size = min_size;
        if (size > max_size) size = max_size;
        w->size = size;
        w->m0 = cur;
        w->m1 = cur + cell * (int32_t)size + extra - 1;
        cur = w->m1 + RIBBON_GAP;
    }
}

LCDTERM_UTEXT int32_t ribbon_total_len(const ribbon_t *r) {
    if (!r || r->count == 0) return r ? ax_len(r) : 800;
    const ribbon_win_t *last = &r->wins[r->count - 1];
    int32_t tot = last->m1 + 2 + ax_pad1(r);
    /* Down the screen the windows start below the menu bar: the view must
     * reach the last one's start at that margin, whatever is left below it. */
    if (AXIS_Y(r) && tot < last->m0 - ax_pad0(r) + ax_len(r)) tot = last->m0 - ax_pad0(r) + ax_len(r);
    if (tot < ax_len(r)) tot = ax_len(r);
    return tot;
}

LCDTERM_UTEXT int32_t ribbon_compute_viewport(const ribbon_t *r, uint8_t idx, int32_t current) {
    if (!r || r->count == 0 || idx >= r->count) return 0;
    const ribbon_win_t *w = &r->wins[idx];
    int32_t len = ax_len(r), pad0 = ax_pad0(r), pad1 = ax_pad1(r);
    int32_t span = w->m1 - w->m0 + 1;
    int32_t target = current;

    if (span >= len - pad0 - pad1 - 8) {
        /* A window the size of the screen: its start at the margin */
        target = w->m0 - pad0;
    } else if (w->m0 < current + pad0) {
        /* Cut off at the start: back to show its first border */
        target = w->m0 - pad0;
    } else if (w->m1 + 2 > current + len - pad1) {
        /* Cut off at the end: on to show its last border and shadow */
        target = w->m1 + 2 - len + pad1;
    }

    int32_t max_v = ribbon_total_len(r) - len;
    if (max_v < 0) max_v = 0;
    if (target < 0) target = 0;
    if (target > max_v) target = max_v;
    return target;
}

LCDTERM_UTEXT int ribbon_insert(ribbon_t *r, uint8_t idx, uint8_t type, uint8_t vterm_id, uint8_t size, const char *title) {
    if (!r || r->count >= RIBBON_MAX_WINDOWS) return -1;
    if (idx > r->count) idx = r->count;

    for (int i = (int)r->count - 1; i >= (int)idx; i--) {
        win_copy(&r->wins[i + 1], &r->wins[i]);
    }
    r->wins[idx].type = type;
    r->wins[idx].vterm_id = vterm_id;
    r->wins[idx].size = size ? size : ribbon_size_full(r);
    r->wins[idx].flags = 0;
    r->wins[idx].owner_pid = -1;
    r->wins[idx].reserved = 0;
    if (title && title[0]) {
        cstr_copy(r->wins[idx].title, title, sizeof(r->wins[idx].title));
    } else if (type == RIBBON_WIN_CANVAS) {
        r->wins[idx].title[0] = 'C';
        r->wins[idx].title[1] = 'a';
        r->wins[idx].title[2] = 'n';
        r->wins[idx].title[3] = 'v';
        r->wins[idx].title[4] = 'a';
        r->wins[idx].title[5] = 's';
        r->wins[idx].title[6] = '\0';
    } else if (type == RIBBON_WIN_READER) {
        r->wins[idx].title[0] = 'R';
        r->wins[idx].title[1] = 'e';
        r->wins[idx].title[2] = 'a';
        r->wins[idx].title[3] = 'd';
        r->wins[idx].title[4] = 'e';
        r->wins[idx].title[5] = 'r';
        r->wins[idx].title[6] = '\0';
    } else if (type == RIBBON_WIN_LAUNCHER) {
        r->wins[idx].title[0] = 'L';
        r->wins[idx].title[1] = 'a';
        r->wins[idx].title[2] = 'u';
        r->wins[idx].title[3] = 'n';
        r->wins[idx].title[4] = 'c';
        r->wins[idx].title[5] = 'h';
        r->wins[idx].title[6] = 'e';
        r->wins[idx].title[7] = 'r';
        r->wins[idx].title[8] = '\0';
    } else {
        r->wins[idx].title[0] = 'T';
        r->wins[idx].title[1] = 'e';
        r->wins[idx].title[2] = 'r';
        r->wins[idx].title[3] = 'm';
        r->wins[idx].title[4] = '\0';
    }

    r->count++;
    r->active_idx = idx;
    ribbon_layout(r);
    r->m_target = ribbon_compute_viewport(r, r->active_idx, r->m_view);
    return (int)idx;
}

LCDTERM_UTEXT bool ribbon_remove(ribbon_t *r, uint8_t idx) {
    if (!r || r->count == 0 || idx >= r->count) return false;

    for (uint8_t i = idx; i + 1 < r->count; i++) {
        win_copy(&r->wins[i], &r->wins[i + 1]);
    }
    r->count--;
    win_zero(&r->wins[r->count]);

    if (r->active_idx > idx) {
        r->active_idx--;
    } else if (r->active_idx >= r->count && r->count > 0) {
        r->active_idx = (uint8_t)(r->count - 1u);
    }
    ribbon_layout(r);
    if (r->count > 0) {
        r->m_target = ribbon_compute_viewport(r, r->active_idx, r->m_view);
    } else {
        r->m_target = 0;
    }
    return true;
}

LCDTERM_UTEXT bool ribbon_swap(ribbon_t *r, uint8_t idx_a, uint8_t idx_b) {
    if (!r || idx_a >= r->count || idx_b >= r->count || idx_a == idx_b) return false;
    ribbon_win_t tmp;
    win_copy(&tmp, &r->wins[idx_a]);
    win_copy(&r->wins[idx_a], &r->wins[idx_b]);
    win_copy(&r->wins[idx_b], &tmp);

    if (r->active_idx == idx_a) r->active_idx = idx_b;
    else if (r->active_idx == idx_b) r->active_idx = idx_a;

    ribbon_layout(r);
    r->m_target = ribbon_compute_viewport(r, r->active_idx, r->m_view);
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
    r->m_target = ribbon_compute_viewport(r, r->active_idx, r->m_view);
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
    uint8_t cur = w->size;
    uint8_t next = cur;
    uint8_t full = ribbon_size_full(r);
    uint8_t half = ribbon_size_half(r);

    if (AXIS_Y(r)) {
        /* Down the screen: one window alone, or one of a pair. */
        next = expand ? (cur < half ? half : full) : (cur > half ? half : cur);
    } else {
        uint8_t c2_5 = RIBBON_COLS_TWO_FIFTHS(r->screen_w);
        uint8_t c2_3 = RIBBON_COLS_TWO_THIRDS(r->screen_w);
        if (expand) {
            if (cur < c2_5) next = c2_5;
            else if (cur < half) next = half;
            else if (cur < c2_3) next = c2_3;
            else if (cur < full) next = full;
        } else {
            if (cur > c2_3) next = c2_3;
            else if (cur > half) next = half;
            else if (cur > c2_5) next = c2_5;
        }
    }

    if (next == cur) return false;
    w->size = next;
    ribbon_layout(r);
    r->m_target = ribbon_compute_viewport(r, r->active_idx, r->m_view);
    return true;
}

LCDTERM_UTEXT bool ribbon_cycle_active(ribbon_t *r) {
    if (!r || r->count == 0 || r->active_idx >= r->count) return false;
    if (ribbon_resize_active(r, true)) return true;
    ribbon_win_t *w = &r->wins[r->active_idx];
    uint8_t first = AXIS_Y(r) ? ribbon_size_half(r) : RIBBON_COLS_TWO_FIFTHS(r->screen_w);
    if (w->size == first) return false;
    w->size = first;
    ribbon_layout(r);
    r->m_target = ribbon_compute_viewport(r, r->active_idx, r->m_view);
    return true;
}

LCDTERM_UTEXT bool ribbon_toggle_maximize(ribbon_t *r) {
    if (!r || r->count == 0 || r->active_idx >= r->count) return false;
    ribbon_win_t *w = &r->wins[r->active_idx];
    w->size = w->size >= ribbon_size_full(r) ? ribbon_size_half(r) : ribbon_size_full(r);
    ribbon_layout(r);
    r->m_target = ribbon_compute_viewport(r, r->active_idx, r->m_view);
    return true;
}

LCDTERM_UTEXT void ribbon_set_title(ribbon_t *r, uint8_t idx, const char *title) {
    if (!r || idx >= r->count) return;
    cstr_copy(r->wins[idx].title, title, sizeof(r->wins[idx].title));
}

LCDTERM_UTEXT void ribbon_set_owner_pid(ribbon_t *r, uint8_t idx, int16_t pid) {
    if (!r || idx >= r->count) return;
    r->wins[idx].owner_pid = pid;
}

LCDTERM_UTEXT int16_t ribbon_get_owner_pid(const ribbon_t *r, uint8_t idx) {
    if (!r || idx >= r->count) return -1;
    return r->wins[idx].owner_pid;
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

LCDTERM_UTEXT int ribbon_find_term(const ribbon_t *r) {
    if (!r) return -1;
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].type == RIBBON_WIN_TERM) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT int ribbon_find_canvas(const ribbon_t *r) {
    if (!r || r->count == 0) return -1;
    if (r->active_idx < r->count && r->wins[r->active_idx].type == RIBBON_WIN_CANVAS) {
        return (int)r->active_idx;
    }
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].type == RIBBON_WIN_CANVAS) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT int ribbon_find_reader(const ribbon_t *r) {
    if (!r || r->count == 0) return -1;
    if (r->active_idx < r->count && r->wins[r->active_idx].type == RIBBON_WIN_READER) {
        return (int)r->active_idx;
    }
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].type == RIBBON_WIN_READER) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT int ribbon_find_launcher(const ribbon_t *r) {
    if (!r || r->count == 0) return -1;
    if (r->active_idx < r->count && r->wins[r->active_idx].type == RIBBON_WIN_LAUNCHER) {
        return (int)r->active_idx;
    }
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].type == RIBBON_WIN_LAUNCHER) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT int ribbon_find_canvas_slot(const ribbon_t *r, uint8_t canvas_id) {
    if (!r) return -1;
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].type == RIBBON_WIN_CANVAS && r->wins[i].vterm_id == canvas_id) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT int ribbon_find_reader_slot(const ribbon_t *r, uint8_t reader_id) {
    if (!r) return -1;
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].type == RIBBON_WIN_READER && r->wins[i].vterm_id == reader_id) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT int ribbon_visible_range(const ribbon_t *r, int32_t view, uint8_t *first_idx, uint8_t *last_idx) {
    if (!r || r->count == 0) return 0;
    int32_t len = ax_len(r);
    int first = -1, last = -1;
    int count = 0;

    for (uint8_t i = 0; i < r->count; i++) {
        if (r->wins[i].m1 + 1 >= view && r->wins[i].m0 < view + len) {
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
    if (!r || r->m_view == r->m_target) return false;
    int32_t diff = r->m_target - r->m_view;
    if (step_px <= 0) {
        int32_t step = diff / 4;
        if (step == 0) step = (diff > 0) ? 1 : -1;
        if (step > 0 && step < 4 && diff >= 4) step = 4;
        if (step < 0 && step > -4 && diff <= -4) step = -4;
        if ((diff > 0 && r->m_view + step >= r->m_target) ||
            (diff < 0 && r->m_view + step <= r->m_target)) {
            r->m_view = r->m_target;
        } else {
            r->m_view += step;
        }
    } else {
        if (diff > 0) {
            r->m_view = (r->m_view + step_px >= r->m_target) ? r->m_target : r->m_view + step_px;
        } else {
            r->m_view = (r->m_view - step_px <= r->m_target) ? r->m_target : r->m_view - step_px;
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
              r.count == 0 && r.screen_w == 800 && r.screen_h == 480 && r.m_view == 0, &fails);

    /* 1. Insert first window: full 98 cols */
    int idx0 = ribbon_insert(&r, 0, RIBBON_WIN_TERM, 0, 98, "Term 0");
    st_expect("insert win0 (98 cols) at index 0",
              idx0 == 0 && r.count == 1 && r.active_idx == 0, &fails);
    st_expect("win0 coordinates: m0=4, m1=794",
              r.wins[0].m0 == 4 && r.wins[0].m1 == 794, &fails);
    st_expect("ribbon_total_len for win0 is 800",
              ribbon_total_len(&r) == 800, &fails);
    st_expect("compute_viewport for win0 is 0",
              ribbon_compute_viewport(&r, 0, r.m_view) == 0, &fails);

    /* 2. Resize win0 to 48 cols and insert win1 (48 cols) at index 1 */
    r.wins[0].size = 48;
    ribbon_layout(&r);
    st_expect("win0 resized to 48 cols: m0=4, m1=394",
              r.wins[0].m0 == 4 && r.wins[0].m1 == 394, &fails);

    int idx1 = ribbon_insert(&r, 1, RIBBON_WIN_TERM, 1, 48, "Term 1");
    st_expect("insert win1 (48 cols) at index 1",
              idx1 == 1 && r.count == 2 && r.active_idx == 1, &fails);
    st_expect("win1 coordinates: m0=404, m1=794",
              r.wins[1].m0 == 404 && r.wins[1].m1 == 794, &fails);

    uint8_t first = 0, last = 0;
    int vis = ribbon_visible_range(&r, 0, &first, &last);
    st_expect("both 48-col windows visible at m_view=0",
              vis == 2 && first == 0 && last == 1, &fails);

    /* 3. Insert win2 (48 cols) at index 2 */
    int idx2 = ribbon_insert(&r, 2, RIBBON_WIN_TERM, 2, 48, "Term 2");
    st_expect("insert win2 (48 cols) at index 2",
              idx2 == 2 && r.count == 3 && r.active_idx == 2, &fails);
    st_expect("win2 coordinates: m0=804, m1=1194",
              r.wins[2].m0 == 804 && r.wins[2].m1 == 1194, &fails);

    int32_t vp2 = ribbon_compute_viewport(&r, 2, 0);
    st_expect("target viewport for win2 shifts to 400",
              vp2 == 400, &fails);

    /* 4. Smooth scrolling simulation */
    r.m_target = vp2;
    int steps = 0;
    while (ribbon_step_scroll(&r, 0) && steps < 50) {
        steps++;
    }
    st_expect("step_scroll smoothly reached m_view=400",
              r.m_view == 400, &fails);

    vis = ribbon_visible_range(&r, r.m_view, &first, &last);
    st_expect("visible range at m_view=400 has win1 and win2",
              vis == 2 && first == 1 && last == 2, &fails);

    /* 5. Window movement: Cmd+Ctrl+Left on win2 */
    bool moved = ribbon_move(&r, 2, -1);
    st_expect("ribbon_move(2, -1) moves win2 left",
              moved && r.active_idx == 1, &fails);
    st_expect("win2 now at index 1 (m0=404, m1=794)",
              r.wins[1].vterm_id == 2 && r.wins[1].m0 == 404 && r.wins[1].m1 == 794, &fails);
    st_expect("win1 now at index 2 (m0=804, m1=1194)",
              r.wins[2].vterm_id == 1 && r.wins[2].m0 == 804 && r.wins[2].m1 == 1194, &fails);

    /* 6. Window resize cycling (shrink & expand) */
    ribbon_resize_active(&r, false); /* 48 -> 38 */
    st_expect("shrink active window: 48 -> 38 cols (m1=714)",
              r.wins[1].size == 38 && r.wins[1].m1 == 714, &fails);
    st_expect("downstream win1 shifted left: m0=724",
              r.wins[2].m0 == 724, &fails);

    ribbon_resize_active(&r, true);  /* 38 -> 48 */
    st_expect("expand active window: 38 -> 48 cols (m1=794)",
              r.wins[1].size == 48 && r.wins[1].m1 == 794, &fails);

    /* 7. Window removal */
    bool removed = ribbon_remove(&r, 1);
    st_expect("ribbon_remove(1) removes middle window",
              removed && r.count == 2, &fails);
    st_expect("remaining windows re-laid out: win0 m0=4, win1 m0=404",
              r.wins[0].m0 == 4 && r.wins[1].m0 == 404 && r.wins[1].m1 == 794, &fails);

    /* 8. D7, 47.6: the presets from the width. Today's numbers at 800 (the
     * RP2350 must keep them), the LCD-7B's at 1024; and at both widths every
     * split -- a text share and the rest of the pair -- fills the screen to
     * the pixel, as does one full window. */
    st_expect("D7 at 800: full 98, 2/3 64, half 48, 2/5 38",
              RIBBON_COLS_FULL(800) == 98 && RIBBON_COLS_TWO_THIRDS(800) == 64 &&
              RIBBON_COLS_HALF(800) == 48 && RIBBON_COLS_TWO_FIFTHS(800) == 38, &fails);
    st_expect("D7 at 1024: full 126, 2/3 82, half 62, 2/5 49",
              RIBBON_COLS_FULL(1024) == 126 && RIBBON_COLS_TWO_THIRDS(1024) == 82 &&
              RIBBON_COLS_HALF(1024) == 62 && RIBBON_COLS_TWO_FIFTHS(1024) == 49, &fails);
    static const uint16_t widths[2] = { 800, 1024 };
    for (unsigned wi = 0; wi < 2u; wi++) {
        uint16_t w = widths[wi];
        bool tiles = true;
        ribbon_init(&r, w, 480);
        (void)ribbon_insert(&r, 0, RIBBON_WIN_TERM, 0, RIBBON_COLS_FULL(w), "Full");
        tiles &= r.wins[0].m1 == (int32_t)w - 6 && ribbon_total_len(&r) == (int32_t)w;
        const uint8_t shares[3] = { RIBBON_COLS_TWO_FIFTHS(w), RIBBON_COLS_HALF(w), RIBBON_COLS_TWO_THIRDS(w) };
        for (unsigned k = 0; k < 3u; k++) {
            ribbon_init(&r, w, 480);
            (void)ribbon_insert(&r, 0, RIBBON_WIN_TERM, 0, shares[k], "Text");
            (void)ribbon_insert(&r, 1, RIBBON_WIN_CANVAS, 0,
                                (uint8_t)(RIBBON_COLS_PAIR(w) - shares[k]), "Canvas");
            tiles &= r.wins[1].m1 == (int32_t)w - 6 && ribbon_total_len(&r) == (int32_t)w;
        }
        st_expect(w == 800 ? "D7 at 800: full and every split fill the screen"
                           : "D7 at 1024: full and every split fill the screen", tiles, &fails);
    }
    ribbon_init(&r, 1024, 600);
    (void)ribbon_insert(&r, 0, RIBBON_WIN_TERM, 0, RIBBON_COLS_HALF(1024), "Term");
    ribbon_resize_active(&r, false);
    bool c49 = r.wins[0].size == 49;
    ribbon_resize_active(&r, true);
    ribbon_resize_active(&r, true);
    bool c82 = r.wins[0].size == 82;
    ribbon_resize_active(&r, true);
    st_expect("D7 at 1024: resize steps 62 -> 49, then 62 -> 82 -> 126",
              c49 && c82 && r.wins[0].size == 126 && r.wins[0].m1 == 1018, &fails);
    bool round = ribbon_cycle_active(&r) && r.wins[0].size == 49 && ribbon_cycle_active(&r) &&
                 r.wins[0].size == 62 && ribbon_cycle_active(&r) && r.wins[0].size == 82;
    st_expect("Cmd+R at 1024: 126 -> 49 -> 62 -> 82, round", round, &fails);

#if RIBBON_AXIS_Y_SUPPORTED
    /* 76.3: down the screen, at the Tab5's portrait geometry (16 x 32 cells,
     * windows from y 44, 36 px of title bar and borders besides the rows, 37
     * rows alone). A pair is two windows of 17 rows that fit where one of 37
     * does; the viewport moves by the window down to the next pair; maximise
     * and the resize steps go between half and full; and a rotation round
     * trip keeps the order, the focus and the shape (a pair stays a pair). */
    {
        int cell = (int)FONT_CELL_H;
        uint16_t pw = (uint16_t)(FONT_CELL_W == 16u ? 720 : 480), ph = (uint16_t)(FONT_CELL_W == 16u ? 1280 : 800);
        uint16_t pad0 = (uint16_t)(FONT_CELL_W == 16u ? 44 : 22), extra = (uint16_t)(FONT_CELL_W == 16u ? 38 : 20);
        uint8_t full = (uint8_t)(FONT_CELL_W == 16u ? 37 : 47);
        ribbon_init(&r, pw, ph);
        st_expect("axis Y: set", ribbon_set_axis(&r, RIBBON_AXIS_Y, pad0, extra, full) && RIBBON_AXIS(&r) == RIBBON_AXIS_Y, &fails);
        uint8_t half = ribbon_size_half(&r);
        st_expect("axis Y: the pair fits where one window does",
                  ribbon_size_full(&r) == full && 2 * (cell * half + extra) + RIBBON_GAP <= cell * full + extra, &fails);
        (void)ribbon_insert(&r, 0, RIBBON_WIN_TERM, 0, half, "Top");
        (void)ribbon_insert(&r, 1, RIBBON_WIN_READER, 0, half, "Bottom");
        st_expect("axis Y: a pair stacks from pad0, a gap apart",
                  r.wins[0].m0 == pad0 && r.wins[0].m1 == pad0 + cell * half + extra - 1 &&
                  r.wins[1].m0 == r.wins[0].m1 + RIBBON_GAP, &fails);
        st_expect("axis Y: both of a pair on screen at view 0",
                  ribbon_compute_viewport(&r, 1, 0) == 0 && ribbon_visible_range(&r, 0, &first, &last) == 2, &fails);
        (void)ribbon_insert(&r, 2, RIBBON_WIN_TERM, 1, half, "Third");
        int32_t v = ribbon_compute_viewport(&r, 2, 0);
        st_expect("axis Y: the third window brings the view down by a window",
                  v == r.wins[2].m1 + 2 - (int32_t)ph && v > 0 && r.wins[2].m1 + 2 - v <= (int32_t)ph, &fails);
        st_expect("axis Y: the first is back in view at its start",
                  ribbon_compute_viewport(&r, 0, v) == 0, &fails);
        r.active_idx = 2;
        st_expect("axis Y: maximise makes it full, and back half",
                  ribbon_toggle_maximize(&r) && r.wins[2].size == full &&
                  ribbon_toggle_maximize(&r) && r.wins[2].size == half, &fails);
        st_expect("axis Y: resize steps half -> full -> half",
                  ribbon_resize_active(&r, true) && r.wins[2].size == full &&
                  ribbon_resize_active(&r, false) && r.wins[2].size == half &&
                  !ribbon_resize_active(&r, false), &fails);
        st_expect("axis Y: Cmd+R goes half -> full -> half",
                  ribbon_cycle_active(&r) && r.wins[2].size == full &&
                  ribbon_cycle_active(&r) && r.wins[2].size == half, &fails);
        (void)ribbon_insert(&r, 3, RIBBON_WIN_READER, 1, full, "Last, alone");
        v = ribbon_compute_viewport(&r, 3, 0);
        st_expect("axis Y: the last window, alone, is shown whole below the menu bar",
                  r.wins[3].m0 - v == pad0 && r.wins[3].m1 + 2 - v <= (int32_t)ph, &fails);
        (void)ribbon_remove(&r, 3);
        st_expect("axis Y: remove keeps the stack",
                  ribbon_remove(&r, 0) && r.count == 2 && r.wins[0].m0 == pad0 && r.wins[0].vterm_id == 0 &&
                  r.wins[0].type == RIBBON_WIN_READER, &fails);
    }
#endif

    if (fails == 0) {
        cprintf("ribbon geometry selftest: PASSED\n");
    } else {
        cprintf("ribbon geometry selftest: FAILED (%d failures)\n", fails);
    }
}
