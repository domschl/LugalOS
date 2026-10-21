/* drivers/reader_view.c — Ribbon Reader Window Engine with Proportional Typography
 *
 * Implements a dedicated long-form reading window for the horizontal window ribbon:
 * - Proportional serif typography (font_serif) with word-wrapping.
 * - Monospace fallback (font8x16) for preformatted blocks (``` ... ```).
 * - Touch hit-testing: tapping links navigates to targets.
 * - Smooth vertical scrolling & page panning with GT911 gesture support.
 * - Freestanding C, zero heap allocations, zero FPU in M-mode.
 */

#include "kernel/orient.h"
#include "drivers/reader_view.h"
#include "drivers/font_serif.h"
#include "drivers/font_noto_serif.h"
#include "drivers/font_noto_mono.h"
#include "drivers/font8x16.h"
#include "drivers/canvas1.h"
#include "drivers/lcdterm_attr.h"
#include "drivers/lcdterm.h"
#include "drivers/ramscreen.h"
#include "drivers/screen.h"
#include "kernel/console.h"
#include "drivers/touch.h"
#include "fs/gemtext.h"
#include "fs/markdown.h"
#include "fs/latex_math.h"
#include "drivers/latex_layout.h"
#include "fs/vfs.h"
#include "net/spartan.h"
#include "kernel/vterm.h"
#include "kernel/palloc.h"
#include <string.h>

#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))
#include "drivers/dsi_esp32p4.h"
#endif

#define READER_LINKS_MAX   32
#define READER_TOC_MAX     32
#define READER_STACK_MAX   8

#if defined(CONFIG_FONT_HIDPI) || (defined(CONFIG_FONT_CELL_H) && (CONFIG_FONT_CELL_H == 32))
#define READER_IS_HIDPI    1
#define READER_MARGIN_X    36
#define READER_LINE_H      30
#else
#define READER_IS_HIDPI    0
#define READER_MARGIN_X    24
#define READER_LINE_H      18
#endif

#define READER_BG_GRAY8          250   /* Soft book-grade paper/cream (0xFA) */
#define READER_FG_GRAY8          10    /* Deep charcoal dark slate ink (0x0A) */
#define READER_RULE_GRAY8        70    /* Heading underline rule */
#define READER_QUOTE_BAR_GRAY8   180   /* Blockquote left vertical bar */
#define READER_LINK_LINE_GRAY8   120   /* Hyperlink underline */
#define READER_SB_TRACK_GRAY8    230   /* Scrollbar track */
#define READER_SB_THUMB_GRAY8    90    /* Scrollbar thumb */

typedef struct {
    int16_t x0, y0, x1, y1;  /* Exact 2D bounding box */
    int     num;             /* 1-based index (e.g. 1..9) */
    char    url[128];
} reader_link_t;

typedef struct {
    char path[128];
    int  scroll_y;
} reader_hist_entry_t;

typedef struct {
    char    title[48];
    uint8_t level;
    int     doc_y;
} reader_toc_entry_t;

typedef struct {
    bool                active;
    uint8_t             target_cid;
    int                 win_w, win_h;
    char                path[128];
    char                title[64];
    int                 scroll_y;
    int                 total_h;
    reader_link_t       links[READER_LINKS_MAX];
    int                 link_count;
    reader_hist_entry_t history[READER_STACK_MAX];
    int                 history_depth;
    reader_toc_entry_t  toc[READER_TOC_MAX];
    int                 toc_count;
    bool                toc_mode;
    uint8_t            *aa_buf;        /* Anti-aliased 8-bpp (GRAY8) surface in PSRAM */
    uint32_t            aa_stride;
    int                 aa_w, aa_h;
    uint32_t            aa_pages;
} reader_state_t;

/* guarded by: console_lock for opening and closing an instance (the surface
 * and the active flag), which the compositor reads under it. accepted: the
 * scroll position and the drawing into a live surface happen from the input
 * path without it; the compositor can show a frame mid-draw. */
static reader_state_t s_readers[READER_MAX_INSTANCES];
static uint8_t        s_active_reader_rid = 0;
static uint32_t       s_reader_seq[READER_MAX_INSTANCES];

static bool resolve_doc_path(const char *topic, char *out_path, size_t out_sz) {
    if (!topic || !*topic) return false;

    if (strncmp(topic, "spartan://", 10) == 0 || strncmp(topic, "SPARTAN://", 10) == 0) {
        strncpy(out_path, topic, out_sz - 1);
        out_path[out_sz - 1] = '\0';
        return true;
    }

    if (topic[0] == '/' || topic[0] == '.') {
        int fd = vfs_open(topic, VFS_O_READ);
        if (fd >= 0) {
            vfs_close(fd);
            strncpy(out_path, topic, out_sz - 1);
            out_path[out_sz - 1] = '\0';
            return true;
        }
    }

    char clean[64];
    strncpy(clean, topic, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    char *dot = NULL;
    for (char *p = clean; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (dot && (strcmp(dot, ".md") == 0 || strcmp(dot, ".gmi") == 0 || strcmp(dot, ".txt") == 0)) {
        *dot = '\0';
    }

    /* 1. Try .md then .gmi on /sd0/help */
    ksnprintf(out_path, out_sz, "/sd0/help/%s.md", clean);
    int fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    ksnprintf(out_path, out_sz, "/sd0/help/%s.gmi", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    /* 2. Try .md then .gmi on /flash0/help */
    ksnprintf(out_path, out_sz, "/flash0/help/%s.md", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    ksnprintf(out_path, out_sz, "/flash0/help/%s.gmi", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    /* 3. Try /sd0/pub/ */
    ksnprintf(out_path, out_sz, "/sd0/pub/%s.md", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    ksnprintf(out_path, out_sz, "/sd0/pub/%s.gmi", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    return false;
}

static bool reader_open_internal(uint8_t rid, const char *resource, bool push_history) {
    if (!resource || !*resource || rid >= READER_MAX_INSTANCES) return false;
    reader_state_t *r = &s_readers[rid];

    char resolved[128];
    if (!resolve_doc_path(resource, resolved, sizeof(resolved))) {
        strncpy(resolved, resource, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    if (push_history && r->path[0] && r->history_depth < READER_STACK_MAX) {
        strncpy(r->history[r->history_depth].path, r->path, sizeof(r->history[0].path) - 1);
        r->history[r->history_depth].path[sizeof(r->history[0].path) - 1] = '\0';
        r->history[r->history_depth].scroll_y = r->scroll_y;
        r->history_depth++;
    }

    bool is_spartan = (strncmp(resolved, "spartan://", 10) == 0 || strncmp(resolved, "SPARTAN://", 10) == 0);
    if (is_spartan) {
        char redir[128] = {0};
        char err[128] = {0};
        char tmp_path[32];
        ksnprintf(tmp_path, sizeof(tmp_path), "/ram0/spartan%d.gmi", (int)rid);
        int rc = spartan_fetch(resolved, tmp_path, redir, sizeof(redir), err, sizeof(err));
        if (rc == SPARTAN_REDIRECT && redir[0]) {
            rc = spartan_fetch(redir, tmp_path, NULL, 0, err, sizeof(err));
        }
        if (rc != SPARTAN_OK) {
            return false;
        }
    }

    /* From here the instance changes under what the compositor reads it
     * with; the fetch above is not, it may take seconds. */
    console_lock();
    strncpy(r->path, resolved, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';

    /* Set default title from filename */
    const char *slash = NULL;
    for (const char *sp = resolved; *sp; sp++) {
        if (*sp == '/') slash = sp;
    }
    const char *name = slash ? slash + 1 : resolved;
    strncpy(r->title, name, sizeof(r->title) - 1);
    r->title[sizeof(r->title) - 1] = '\0';

    r->scroll_y = 0;
    r->total_h = 0;
    r->link_count = 0;
    r->active = true;

    if (!r->aa_buf) {
        uint32_t aa_w = 1280u;
        uint32_t aa_h = 1280u;
        uint32_t pages = (aa_w * aa_h + 4095u) / 4096u;
        r->aa_buf = (uint8_t *)palloc_pages_bulk(pages);
        if (r->aa_buf) {
            r->aa_pages = pages;
            r->aa_stride = aa_w;
            r->aa_w = aa_w;
            r->aa_h = aa_h;
        }
    }
    console_unlock();
    return true;
}

void reader_init(void) {
    memset(s_readers, 0, sizeof(s_readers));
    s_active_reader_rid = 0;
}

int reader_open_instance(uint8_t rid, const char *resource) {
    if (rid >= READER_MAX_INSTANCES) return -1;
    if (reader_open_internal(rid, resource, true)) {
        s_active_reader_rid = rid;
        return (int)rid;
    }
    return -1;
}

bool reader_open(const char *resource) {
    return reader_open_instance(s_active_reader_rid, resource) >= 0;
}

/* Under console_lock and the panel's frame lock: the conversion composites
 * from aa_buf under the frame lock, and the page must not be freed in the
 * middle of that. */
void reader_close_instance(uint8_t rid) {
    if (rid < READER_MAX_INSTANCES) {
        console_lock();
        s_readers[rid].active = false;
        s_readers[rid].path[0] = '\0';
        s_readers[rid].title[0] = '\0';
        if (s_readers[rid].aa_buf) {
            console_frame_lock();
            palloc_free(s_readers[rid].aa_buf, s_readers[rid].aa_pages);
            s_readers[rid].aa_buf = NULL;
            s_readers[rid].aa_pages = 0;
            console_frame_unlock();
        }
        console_unlock();
    }
}

void reader_close(void) {
    reader_close_instance(s_active_reader_rid);
}

uint8_t *reader_get_aa_backing(uint8_t rid, uint32_t *out_stride, int *out_w, int *out_h) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active || !s_readers[rid].aa_buf)
        return NULL;
    if (out_stride) *out_stride = s_readers[rid].aa_stride;
    if (out_w) *out_w = s_readers[rid].win_w > 0 ? s_readers[rid].win_w : s_readers[rid].aa_w;
    if (out_h) *out_h = s_readers[rid].win_h > 0 ? s_readers[rid].win_h : s_readers[rid].aa_h;
    return s_readers[rid].aa_buf;
}

static canvas1_t *get_reader_canvas(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return 0;
#if defined(LCDTERM_PRESENT)
    canvas1_t *c = lcdterm_get_reader_backing(rid);
    if (c) return c;
#endif
    return ramscreen_get_reader_backing(rid);
}

bool reader_set_target(uint8_t rid, int w, int h) {
    if (rid >= READER_MAX_INSTANCES) return false;
    bool changed = false;
    if (w > 0 && s_readers[rid].win_w != w) {
        s_readers[rid].win_w = w;
        changed = true;
    }
    if (h > 0 && s_readers[rid].win_h != h) {
        s_readers[rid].win_h = h;
        changed = true;
    }
    return changed;
}

void reader_update_instance(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    canvas1_t *c = get_reader_canvas(rid);
    if (!c) return;
    int w = s_readers[rid].win_w > 0 ? s_readers[rid].win_w : (int)c->w;
    int h = s_readers[rid].win_h > 0 ? s_readers[rid].win_h : (int)c->h;
    reader_draw(rid, c, w, h);

    /* Update title on ribbon reader window */
    const char *title = reader_get_title_instance(rid);
    uint8_t req[SCREEN_CTITLE_MAX + 2];
    req[0] = 'T';
    uint32_t tl = (uint32_t)strlen(title);
    if (tl > sizeof(req) - 2) tl = sizeof(req) - 2;
    memcpy(req + 1, title, tl);
    req[1 + tl] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2 + tl, reply);
#if defined(LCDTERM_PRESENT)
    lcdterm_repaint();
#endif
}

void reader_update(void) {
    reader_update_instance(s_active_reader_rid);
}

void reader_scroll_instance(uint8_t rid, int delta_y) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active || delta_y == 0) return;
    int start_y = s_readers[rid].scroll_y;
    int target_y = start_y + delta_y;
    if (target_y < 0) target_y = 0;
    int max_scroll = s_readers[rid].total_h - 120;
    if (max_scroll < 0) max_scroll = 0;
    if (target_y > max_scroll) target_y = max_scroll;
    if (target_y == start_y) return;
#if defined(LCDTERM_PRESENT)
    int diff = target_y - start_y;
    canvas1_t *c = get_reader_canvas(rid);
    int w = s_readers[rid].win_w > 0 ? s_readers[rid].win_w : (c ? (int)c->w : 0);
    int h = s_readers[rid].win_h > 0 ? s_readers[rid].win_h : (c ? (int)c->h : 0);
    int abs_diff = diff < 0 ? -diff : diff;

    if (c && w > 0 && h > 0 && abs_diff > 16) {
        /* Smooth ease-out scrolling */
        const int steps = abs_diff <= 48 ? 3 : 5;
        static const int weights3[3] = { 450, 800, 1000 };
        static const int weights5[5] = { 280, 540, 750, 900, 1000 };
        const int *w_table = (steps == 3) ? weights3 : weights5;

        for (int s = 0; s < steps; s++) {
            s_readers[rid].scroll_y = start_y + (diff * w_table[s]) / 1000;
            reader_draw(rid, c, w, h);
            lcdterm_sync_post();
            if (console_has_char() || console_interrupt_requested()) break;
        }
    }
#endif

    s_readers[rid].scroll_y = target_y;
    reader_update_instance(rid);
}

void reader_scroll(int delta_y) {
    reader_scroll_instance(s_active_reader_rid, delta_y);
}

void reader_touch_fling_instance(uint8_t rid, int dy, uint32_t ms) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    if (ms < 10) ms = 10;
    if (ms > 500) ms = 500;

    int max_scroll = s_readers[rid].total_h - 120;
    if (max_scroll < 0) max_scroll = 0;
    if (max_scroll == 0) return;

    canvas1_t *c = get_reader_canvas(rid);
    int w = s_readers[rid].win_w > 0 ? s_readers[rid].win_w : (c ? (int)c->w : 0);
    int h = s_readers[rid].win_h > 0 ? s_readers[rid].win_h : (c ? (int)c->h : 0);
    if (!c || w <= 0 || h <= 0) return;

    /* Moving finger up (dy < 0) scrolls down (positive scroll_y change).
     * Scaled for responsive, rapid fling speed with smooth inertia. */
#if READER_IS_HIDPI
    int v = -(dy * 110) / (int)ms;
    if (v > 360) v = 360;
    if (v < -360) v = -360;
    if (v >= 0 && v < 24) v = 36;
    if (v < 0 && v > -24) v = -36;
#else
    int v = -(dy * 70) / (int)ms;
    if (v > 220) v = 220;
    if (v < -220) v = -220;
    if (v >= 0 && v < 16) v = 24;
    if (v < 0 && v > -16) v = -24;
#endif

    uint32_t start_presses = touch_press_count();

    for (int frame = 0; frame < 120; frame++) {
        int next_y = s_readers[rid].scroll_y + v;
        if (next_y < 0) {
            s_readers[rid].scroll_y = 0;
            break;
        }
        if (next_y > max_scroll) {
            s_readers[rid].scroll_y = max_scroll;
            break;
        }
        s_readers[rid].scroll_y = next_y;
        reader_draw(rid, c, w, h);
#if defined(LCDTERM_PRESENT)
        lcdterm_sync_post();
#endif
        /* Low friction decay: ~5% loss per frame at 60 Hz */
        v = (v * 95) / 100;
        int abs_v = v < 0 ? -v : v;
        if (abs_v <= 1) break;

        /* Break immediately if user touches screen or presses a key */
        if (console_has_char() || console_interrupt_requested() ||
            touch_press_count() != start_presses) break;
    }

    reader_update_instance(rid);
}

void reader_touch_fling(int dy, uint32_t ms) {
    reader_touch_fling_instance(s_active_reader_rid, dy, ms);
}

static bool reader_jump_anchor(uint8_t rid, const char *anchor) {
    if (rid >= READER_MAX_INSTANCES || !anchor || anchor[0] != '#') return false;
    reader_state_t *r = &s_readers[rid];
    const char *target = anchor + 1;

    /* Check if target matches footnote #fn:id */
    bool is_footnote = (strncmp(target, "fn:", 3) == 0);
    const char *fn_id = is_footnote ? target + 3 : NULL;

    for (int i = 0; i < r->toc_count; i++) {
        char slug[64];
        md_heading_slug(r->toc[i].title, slug, sizeof(slug));
        if (strcmp(slug, target) == 0) {
            if (r->history_depth < READER_STACK_MAX) {
                strncpy(r->history[r->history_depth].path, r->path, sizeof(r->history[0].path) - 1);
                r->history[r->history_depth].path[sizeof(r->history[0].path) - 1] = '\0';
                r->history[r->history_depth].scroll_y = r->scroll_y;
                r->history_depth++;
            }
            int target_y = r->toc[i].doc_y - 20;
            if (target_y < 0) target_y = 0;
            r->scroll_y = target_y;
            reader_update_instance(rid);
            return true;
        }
    }
    (void)fn_id;
    return false;
}

bool reader_back_instance(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return false;
    reader_state_t *r = &s_readers[rid];
    if (r->toc_mode) {
        r->toc_mode = false;
        reader_update_instance(rid);
        return true;
    }
    if (r->history_depth > 0) {
        r->history_depth--;
        reader_hist_entry_t entry = r->history[r->history_depth];
        if (strcmp(entry.path, r->path) == 0) {
            r->scroll_y = entry.scroll_y;
            reader_update_instance(rid);
            return true;
        }
        bool ok = reader_open_internal(rid, entry.path, false);
        if (ok) {
            r->scroll_y = entry.scroll_y;
            reader_update_instance(rid);
        }
        return ok;
    }
    return false;
}

bool reader_back(void) {
    return reader_back_instance(s_active_reader_rid);
}

bool reader_tap_instance(uint8_t rid, int client_x, int client_y) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return false;
    reader_state_t *r = &s_readers[rid];

    for (int i = 0; i < r->link_count; i++) {
        if (client_x >= r->links[i].x0 && client_x <= r->links[i].x1 &&
            client_y >= r->links[i].y0 && client_y <= r->links[i].y1) {
            char target[128];
            strncpy(target, r->links[i].url, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';

            if (r->toc_mode) {
                r->toc_mode = false;
            }

            if (target[0] == '#') {
                if (reader_jump_anchor(rid, target)) return true;
            }

            char resolved[128];
            bool ok = false;
            if (md_resolve_url(r->path, target, resolved, sizeof(resolved)) ||
                resolve_doc_path(target, resolved, sizeof(resolved))) {
                ok = reader_open_internal(rid, resolved, true);
            } else {
                ok = reader_open_internal(rid, target, true);
            }
            if (ok) reader_update_instance(rid);
            return ok;
        }
    }
    return false;
}

bool reader_tap(int client_x, int client_y) {
    return reader_tap_instance(s_active_reader_rid, client_x, client_y);
}

bool reader_follow_link_num_instance(uint8_t rid, int num) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return false;
    reader_state_t *r = &s_readers[rid];
    for (int i = 0; i < r->link_count; i++) {
        if (r->links[i].num == num) {
            char target[128];
            strncpy(target, r->links[i].url, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';

            if (r->toc_mode) {
                r->toc_mode = false;
            }

            if (target[0] == '#') {
                if (reader_jump_anchor(rid, target)) return true;
            }

            char resolved[128];
            bool ok = false;
            if (md_resolve_url(r->path, target, resolved, sizeof(resolved)) ||
                resolve_doc_path(target, resolved, sizeof(resolved))) {
                ok = reader_open_internal(rid, resolved, true);
            } else {
                ok = reader_open_internal(rid, target, true);
            }
            if (ok) reader_update_instance(rid);
            return ok;
        }
    }
    return false;
}

bool reader_follow_link_num(int num) {
    return reader_follow_link_num_instance(s_active_reader_rid, num);
}

void reader_feed_key(int c) {
    uint8_t rid = s_active_reader_rid;
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    reader_state_t *r = &s_readers[rid];
    /* guarded by: g_input_lock (kernel/console.c): the console pump is the
     * only caller. */
    static int esc_state = 0;
    static int esc_arg = 0;
    static int esc_mod = 0;

    int win_h = r->win_h > 0 ? r->win_h : 480;
    int page_dy = win_h - 40;
    if (page_dy < 40) page_dy = 40;

    if (esc_state == 0) {
        if (c == 0x1b) {
            esc_state = 1;
            return;
        }
        if (c == 0x08 || c == '?' || c == 'h') {
            /* Emacs Ctrl-H or '?' / 'h' -> open Reader Help */
            reader_open_instance(rid, "/sd0/help/reader.gmi");
            reader_update_instance(rid);
            return;
        }
        if (c == 'q' || c == 0x03) {
            if (r->toc_mode) {
                r->toc_mode = false;
                reader_update_instance(rid);
                return;
            }
#if defined(CONFIG_BOARD_TAB5)
            int rot = dsi_lcd_get_orientation();
            if (rot == 0 || rot == 180) {
                reader_close_instance(rid);
                orient_manual();
                dsi_lcd_set_orientation(270);
                console_set_active_win_type(1 /* RIBBON_WIN_TERM */);
                return;
            }
#endif
            /* Close reader window */
            uint8_t req[1] = { 'C' }, reply[SCREEN_REPLY_LEN];
            if (console_canvas(req, 1, reply) && reply[0] == 0) {
                uint8_t closed_type = reply[10];
                uint8_t closed_vid = reply[11];
                if (closed_type == 3) {
                    reader_close_instance(closed_vid);
                }
                vterm_set_active(reply[1]);
            }
            console_set_active_win_type(1 /* RIBBON_WIN_TERM */);
            return;
        }
        if (c == 'b' || c == 0x7f) {
            /* Back in history */
            reader_back_instance(rid);
            return;
        }
        if (c == ' ' || c == 0x16 /* Ctrl-V */) {
            /* Page down */
            reader_scroll_instance(rid, page_dy);
            return;
        }
        if (c == 'j' || c == 0x0e /* Ctrl-N */ || c == '\r' || c == '\n') {
            /* Line down */
            reader_scroll_instance(rid, 32);
            return;
        }
        if (c == 'k' || c == 0x10 /* Ctrl-P */) {
            /* Line up */
            reader_scroll_instance(rid, -32);
            return;
        }
        if (c == 'd' || c == 0x04 /* Ctrl-D */) {
            /* Half page down */
            reader_scroll_instance(rid, page_dy / 2);
            return;
        }
        if (c == 'u' || c == 0x15 /* Ctrl-U */) {
            /* Half page up */
            reader_scroll_instance(rid, -page_dy / 2);
            return;
        }
        if (c == 'g') {
            /* Top of document */
            r->scroll_y = 0;
            reader_update_instance(rid);
            return;
        }
        if (c == 'G') {
            /* Bottom of document */
            int max_scroll = r->total_h - 120;
            r->scroll_y = max_scroll > 0 ? max_scroll : 0;
            reader_update_instance(rid);
            return;
        }
        if (c == 'r' || c == 0x12 /* Ctrl-R */) {
            /* Reload / Redraw */
            reader_update_instance(rid);
            return;
        }
        if (c == 'o' || c == 'O') {
            /* Toggle display orientation between Portrait (0) and Landscape (270) */
            console_hotkey(CONSOLE_HOTKEY_ORIENT);
            return;
        }
        if (c == 't' || c == 'T') {
            /* Toggle Table of Contents outline view */
            r->toc_mode = !r->toc_mode;
            reader_update_instance(rid);
            return;
        }
        if (c >= '1' && c <= '9') {
            /* Follow visible link */
            reader_follow_link_num_instance(rid, c - '0');
            return;
        }
        return;
    } else if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_arg = 0;
            esc_mod = 0;
            return;
        }
        if (c == 'v' || c == 'V') {
            /* Alt-V: Page up (Emacs) */
            reader_scroll_instance(rid, -page_dy);
            esc_state = 0;
            return;
        }
        esc_state = 0;
        return;
    } else if (esc_state == 2) {
        if (c == 'A') {
            /* Up Arrow */
            reader_scroll_instance(rid, -32);
            esc_state = 0;
            return;
        }
        if (c == 'B') {
            /* Down Arrow */
            reader_scroll_instance(rid, 32);
            esc_state = 0;
            return;
        }
        if (c == 'C') {
            /* Right Arrow */
            reader_scroll_instance(rid, page_dy);
            esc_state = 0;
            return;
        }
        if (c == 'D') {
            /* Left Arrow */
            reader_back_instance(rid);
            esc_state = 0;
            return;
        }
        if (c >= '0' && c <= '9') {
            esc_arg = esc_arg * 10 + (c - '0');
            esc_state = 3;
            return;
        }
        esc_state = 0;
        return;
    } else if (esc_state == 3) {
        if (c == ';') {
            esc_state = 4;
            esc_mod = 0;
            return;
        }
        if (c == '~') {
            if (esc_arg == 5) {
                /* Page Up */
                reader_scroll_instance(rid, -page_dy);
            } else if (esc_arg == 6) {
                /* Page Down */
                reader_scroll_instance(rid, page_dy);
            } else if (esc_arg == 1 || esc_arg == 7) {
                /* Home */
                r->scroll_y = 0;
                reader_update_instance(rid);
            } else if (esc_arg == 4 || esc_arg == 8) {
                /* End */
                int max_scroll = r->total_h - 120;
                r->scroll_y = max_scroll > 0 ? max_scroll : 0;
                reader_update_instance(rid);
            }
            esc_state = 0;
            return;
        }
        if (c >= '0' && c <= '9') {
            esc_arg = esc_arg * 10 + (c - '0');
            return;
        }
        esc_state = 0;
        return;
    } else if (esc_state == 4) {
        if (c >= '0' && c <= '9') {
            esc_mod = esc_mod * 10 + (c - '0');
            return;
        }
        if (c == 'u') {
            if (esc_mod == 9) {
                /* Super (Cmd) modifier */
                if (esc_arg >= '1' && esc_arg <= '9') {
                    console_hotkey(CONSOLE_HOTKEY_JUMP((unsigned)(esc_arg - '0')));
                } else if (esc_arg == 'w' || esc_arg == 'W') {
                    console_hotkey(CONSOLE_HOTKEY_CLOSE);
                } else if (esc_arg == '\\') {
                    console_hotkey(CONSOLE_HOTKEY_TOGGLE_MAX);
                } else if (esc_arg == '[') {
                    console_hotkey(CONSOLE_HOTKEY_LEFT);
                } else if (esc_arg == ']') {
                    console_hotkey(CONSOLE_HOTKEY_RIGHT);
                }
            } else if (esc_mod == 10 && esc_arg == '3') {
                console_hotkey(CONSOLE_HOTKEY_SCREENSHOT);
            }
            esc_state = 0;
            return;
        }
        if (c == 'C') {
            if (esc_mod == 9) {
                console_hotkey(CONSOLE_HOTKEY_FOCUS_RIGHT);
            } else {
                reader_scroll_instance(rid, page_dy);
            }
            esc_state = 0;
            return;
        }
        if (c == 'D') {
            if (esc_mod == 9) {
                console_hotkey(CONSOLE_HOTKEY_FOCUS_LEFT);
            } else {
                reader_back_instance(rid);
            }
            esc_state = 0;
            return;
        }
        esc_state = 0;
        return;
    }
}

const char *reader_get_title_instance(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return "Reader";
    return s_readers[rid].title[0] ? s_readers[rid].title : "Reader";
}

const char *reader_get_title(void) {
    return reader_get_title_instance(s_active_reader_rid);
}

const char *reader_get_path_instance(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return "";
    return s_readers[rid].path;
}

const char *reader_get_path(void) {
    return reader_get_path_instance(s_active_reader_rid);
}

bool reader_instance_is_active(uint8_t rid) {
    return (rid < READER_MAX_INSTANCES && s_readers[rid].active);
}

bool reader_is_active(void) {
    return reader_instance_is_active(s_active_reader_rid);
}

uint8_t reader_get_active_instance(void) {
    return s_active_reader_rid;
}

void reader_set_active_instance(uint8_t rid) {
    if (rid < READER_MAX_INSTANCES) {
        s_active_reader_rid = rid;
    }
}

int reader_find_by_path(const char *path) {
    if (!path || !*path) return -1;
    char resolved[128];
    if (!resolve_doc_path(path, resolved, sizeof(resolved))) {
        strncpy(resolved, path, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    for (int i = 0; i < READER_MAX_INSTANCES; i++) {
        if (s_readers[i].active && (strcmp(s_readers[i].path, path) == 0 || strcmp(s_readers[i].path, resolved) == 0)) {
            return i;
        }
    }
    return -1;
}

typedef struct {
    const canvas1_t *c;
    uint8_t         *aa_fb;
    uint32_t         aa_stride;
    int              w, h;
} rdraw_t;

static void rdraw_fill(rdraw_t *rd, int x0, int y0, int x1, int y1, uint8_t gray, unsigned pat1) {
    if (rd->c) {
        canvas1_fill(rd->c, x0, y0, x1, y1, pat1);
    }
    if (rd->aa_fb) {
        int cx0 = x0 < 0 ? 0 : x0;
        int cx1 = x1 >= rd->w ? rd->w - 1 : x1;
        int cy0 = y0 < 0 ? 0 : y0;
        int cy1 = y1 >= rd->h ? rd->h - 1 : y1;
        if (cx0 <= cx1 && cy0 <= cy1) {
            int len = cx1 - cx0 + 1;
            for (int y = cy0; y <= cy1; y++) {
                memset(rd->aa_fb + (uint32_t)y * rd->aa_stride + cx0, gray, (size_t)len);
            }
        }
    }
}

static void rdraw_hline(rdraw_t *rd, int x0, int x1, int y, uint8_t gray, unsigned pat1) {
    if (rd->c) {
        canvas1_hline(rd->c, x0, x1, y, pat1);
    }
    if (rd->aa_fb && y >= 0 && y < rd->h) {
        int cx0 = x0 < 0 ? 0 : x0;
        int cx1 = x1 >= rd->w ? rd->w - 1 : x1;
        if (cx0 <= cx1) {
            memset(rd->aa_fb + (uint32_t)y * rd->aa_stride + cx0, gray, (size_t)(cx1 - cx0 + 1));
        }
    }
}

static int rdraw_serif_text(rdraw_t *rd, int x, int y, const char *s, uint32_t len, bool bold) {
    int drawn_w = 0;
#if READER_IS_HIDPI
    if (rd->c) {
        drawn_w = canvas1_noto_serif_text(rd->c, x, y, s, len, bold);
    }
    if (rd->aa_fb) {
        int aa_w = font_noto_serif_aa_text_gray8(rd->aa_fb, rd->aa_stride, rd->w, rd->h,
                                                 x, y, s, len,
                                                 READER_FG_GRAY8, READER_BG_GRAY8, bold);
        if (!rd->c) drawn_w = aa_w;
    }
#else
    if (rd->c) {
        drawn_w = canvas1_serif_text(rd->c, x, y, s, len, bold);
    }
    if (rd->aa_fb) {
        int aa_w = font_serif_aa_text_gray8(rd->aa_fb, rd->aa_stride, rd->w, rd->h,
                                            x, y, s, len,
                                            READER_FG_GRAY8, READER_BG_GRAY8, bold);
        if (!rd->c) drawn_w = aa_w;
    }
#endif
    return drawn_w;
}

static void rdraw_mono_text(rdraw_t *rd, int x, int y, const char *s, uint32_t len, int max_chars) {
#if READER_IS_HIDPI
    if (rd->c) {
        for (uint32_t i = 0; i < len && s[i] && (int)i < max_chars; i++) {
            uint8_t code = (uint8_t)s[i];
            if (code < FONT_NOTO_MONO_FIRST) code = FONT_NOTO_MONO_REPLACEMENT;
            const uint16_t *glyph = font_noto_mono_get_glyph(code);
            int cx = x + (int)i * 16;
            for (int r = 0; r < 32; r++) {
                int py = y + r;
                if (py < 0 || py >= (int)rd->c->h) continue;
                uint16_t bits = glyph[r];
                for (int b = 0; b < 16; b++) {
                    int px = cx + b;
                    if (px >= 0 && px < (int)rd->c->w && ((bits >> b) & 1u)) {
                        canvas1_fill(rd->c, px, py, px, py, CANVAS1_BLACK);
                    }
                }
            }
        }
    }
    if (rd->aa_fb) {
        int cx = x;
        for (uint32_t i = 0; i < len && s[i] && (int)i < max_chars; i++) {
            font_noto_mono_aa_glyph_gray8(rd->aa_fb, rd->aa_stride, rd->w, rd->h,
                                          cx, y, (uint8_t)s[i], READER_FG_GRAY8, READER_BG_GRAY8);
            cx += 16;
        }
    }
#else
    if (rd->c) {
        canvas1_text(rd->c, x, y, s, len, max_chars, 0, 16, false);
    }
    if (rd->aa_fb) {
        int cx = x;
        for (uint32_t i = 0; i < len && s[i] && (int)i < max_chars; i++) {
            uint8_t code = (uint8_t)s[i];
            if (code < FONT8X16_FIRST) code = FONT8X16_REPLACEMENT;
            const uint8_t *glyph = font8x16_glyphs[code - FONT8X16_FIRST];
            for (int r = 0; r < 16; r++) {
                int py = y + r;
                if (py < 0 || py >= rd->h) continue;
                uint8_t *dst = rd->aa_fb + (uint32_t)py * rd->aa_stride;
                uint8_t bits = glyph[r];
                for (int b = 0; b < 8; b++) {
                    int px = cx + b;
                    if (px >= 0 && px < rd->w && ((bits >> b) & 1u)) {
                        dst[px] = READER_FG_GRAY8;
                    }
                }
            }
            cx += 8;
        }
    }
#endif
}

/* Word wrapping and rich proportional / monospace span rendering */
static int render_wrapped_spans(reader_state_t *r, rdraw_t *rd, int x0, int max_w,
                                const char *text, int line_h,
                                int scroll_y, int win_h, int *out_doc_y,
                                int *inout_link_idx) {
    int cur_x = x0;
    int cur_y = *out_doc_y;

    md_span_iter_t iter;
    md_span_init(&iter, text);
    md_span_t span;

    while (md_span_next(&iter, &span)) {
        int link_num = 0;
        char target_url[128] = {0};
        bool is_link = false;

        if (span.style & MD_STYLE_LINK) {
            is_link = true;
            link_num = ++(*inout_link_idx);
            strncpy(target_url, span.link_url, sizeof(target_url) - 1);
        } else if (span.style & MD_STYLE_FOOTNOTE) {
            is_link = true;
            link_num = ++(*inout_link_idx);
            ksnprintf(target_url, sizeof(target_url), "#fn:%s", span.footnote_id);
        } else if (span.style & MD_STYLE_MATH) {
            char mbuf[128];
            size_t mlen = span.len < sizeof(mbuf) - 1 ? span.len : sizeof(mbuf) - 1;
            memcpy(mbuf, span.text, mlen);
            mbuf[mlen] = '\0';

            uint8_t arena_buf[2048];
            math_arena_t arena;
            math_arena_init(&arena, arena_buf, sizeof(arena_buf));
            math_box_t *box = latex_parse(mbuf, STYLE_TEXT, &arena);
            if (box) {
                int base_scale = READER_IS_HIDPI ? 2 : 1;
                latex_layout(box, base_scale);
                int math_w = box->w;

                if (cur_x + math_w > x0 + max_w && cur_x > x0) {
                    cur_x = x0;
                    cur_y += line_h;
                }

                int screen_y = cur_y - scroll_y;
                if (screen_y + line_h >= 0 && screen_y < win_h) {
                    int baseline = screen_y + line_h - 4 * base_scale;
                    if (rd->c) {
                        latex_render_canvas1(box, cur_x, baseline, rd->c, 1);
                    } else if (rd->aa_fb) {
                        latex_render_gray8(box, cur_x, baseline, rd->aa_fb, rd->aa_stride, rd->w, rd->h, 0x00, 0xFF);
                    }
                }
                cur_x += math_w + 4 * base_scale;
            }
            continue;
        }

        /* Iterate through words inside the span */
        const char *p = span.text;
        size_t span_remaining = span.len;

        while (span_remaining > 0 && *p) {
            while (span_remaining > 0 && *p == ' ') {
                p++;
                span_remaining--;
            }
            if (span_remaining == 0 || !*p) break;

            const char *word_start = p;
            size_t word_len = 0;
            while (span_remaining > 0 && *p && *p != ' ') {
                word_len++;
                p++;
                span_remaining--;
            }

            unsigned word_w = 0;
            unsigned space_w = 0;
            if (span.style & MD_STYLE_CODE) {
#if READER_IS_HIDPI
                word_w = (unsigned)word_len * 16;
                space_w = 16;
#else
                word_w = (unsigned)word_len * 8;
                space_w = 8;
#endif
            } else {
#if READER_IS_HIDPI
                word_w = font_noto_serif_text_width(word_start, (uint32_t)word_len);
                space_w = font_noto_serif_char_width(' ');
#else
                word_w = font_serif_text_width(word_start, (uint32_t)word_len);
                space_w = font_serif_char_width(' ');
#endif
            }

            if (cur_x + (int)word_w > x0 + max_w && cur_x > x0) {
                cur_x = x0;
                cur_y += line_h;
            }

            int screen_y = cur_y - scroll_y;
            if (screen_y + line_h >= 0 && screen_y < win_h) {
                if (span.style & MD_STYLE_CODE) {
                    /* Monospace code pill with subtle shaded background */
                    rdraw_fill(rd, cur_x - 2, screen_y + 1, cur_x + (int)word_w + 1, screen_y + line_h - 3, 236, CANVAS1_GREY);
                    rdraw_mono_text(rd, cur_x, screen_y + 1, word_start, (uint32_t)word_len, (int)word_len);
                } else {
                    bool bold = (span.style & MD_STYLE_BOLD) != 0;
                    rdraw_serif_text(rd, cur_x, screen_y, word_start, (uint32_t)word_len, bold);
                    if (is_link) {
                        rdraw_hline(rd, cur_x, cur_x + (int)word_w, screen_y + line_h - 2, READER_LINK_LINE_GRAY8, CANVAS1_BLACK);
                    }
                }

                /* Register 2D bounding box for touch and numbered keys */
                if (is_link && r && r->link_count < READER_LINKS_MAX) {
                    reader_link_t *lnk = &r->links[r->link_count++];
                    lnk->x0 = (int16_t)cur_x;
                    lnk->y0 = (int16_t)screen_y;
                    lnk->x1 = (int16_t)(cur_x + (int)word_w);
                    lnk->y1 = (int16_t)(screen_y + line_h);
                    lnk->num = link_num;
                    strncpy(lnk->url, target_url, sizeof(lnk->url) - 1);
                    lnk->url[sizeof(lnk->url) - 1] = '\0';
                }
            }
            cur_x += (int)word_w + (int)space_w;
        }
    }

    cur_y += line_h;
    *out_doc_y = cur_y;
    return cur_y;
}

void reader_draw(uint8_t rid, const canvas1_t *c, int w, int h) {
    if (w <= 0 || h <= 0 || rid >= READER_MAX_INSTANCES) return;
    reader_state_t *r = &s_readers[rid];

    rdraw_t rd = {
        .c = c,
        .aa_fb = r->aa_buf,
        .aa_stride = r->aa_stride,
        .w = w,
        .h = h,
    };

    /* Fill background */
    rdraw_fill(&rd, 0, 0, w - 1, h - 1, READER_BG_GRAY8, CANVAS1_WHITE);

    if (!r->active || !r->path[0]) {
        rdraw_serif_text(&rd, READER_MARGIN_X, 32, "No document loaded. Use 'reader <file.md>' to open.", 51, false);
        return;
    }

    const char *file_to_open = r->path;
    char spartan_tmp[32];
    if (strncmp(r->path, "spartan://", 10) == 0 || strncmp(r->path, "SPARTAN://", 10) == 0) {
        ksnprintf(spartan_tmp, sizeof(spartan_tmp), "/ram0/spartan%d.gmi", (int)rid);
        file_to_open = spartan_tmp;
    }

    int fd = vfs_open(file_to_open, VFS_O_READ);
    if (fd < 0) {
        char err[128];
        ksnprintf(err, sizeof(err), "Cannot open document: %s", file_to_open);
        rdraw_serif_text(&rd, READER_MARGIN_X, 32, err, strlen(err), false);
        return;
    }

    int margin_x = READER_MARGIN_X;
    int max_w = w - 2 * margin_x;
    if (max_w < 60) max_w = 60;

    int doc_y = 16;
    int scroll_y = r->scroll_y;
    int link_idx = 0;
    r->link_count = 0;
    r->toc_count = 0;

    uint64_t file_offset = 0;
    char read_buf[512];
    char line_buf[MD_LINE_MAX];
    int line_pos = 0;
    bool in_code = false;
    bool in_math = false;

    /* Parse all document lines */
    for (;;) {
        int n = vfs_pread(fd, read_buf, sizeof(read_buf), file_offset);
        if (n <= 0) {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                line_pos = 0;
                md_line_t md;
                if (md_parse_line(line_buf, &in_code, &in_math, &md)) {
                    if (md.type == MD_LINE_TEXT) {
                        render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
                        doc_y += 4;
                    }
                }
            }
            break;
        }

        for (int i = 0; i < n; i++) {
            char ch = read_buf[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                line_buf[line_pos] = '\0';
                line_pos = 0;

                md_line_t md;
                if (!md_parse_line(line_buf, &in_code, &in_math, &md)) continue;

                /* Collect headings into Table of Contents */
                if (md.type == MD_LINE_H1 || md.type == MD_LINE_H2 ||
                    md.type == MD_LINE_H3 || md.type == MD_LINE_H4) {
                    if (r->toc_count < READER_TOC_MAX) {
                        reader_toc_entry_t *entry = &r->toc[r->toc_count++];
                        strncpy(entry->title, md.text, sizeof(entry->title) - 1);
                        entry->title[sizeof(entry->title) - 1] = '\0';
                        entry->level = (md.type == MD_LINE_H1) ? 1 :
                                       (md.type == MD_LINE_H2) ? 2 :
                                       (md.type == MD_LINE_H3) ? 3 : 4;
                        entry->doc_y = doc_y;
                    }
                }

                /* If TOC mode is active, skip body rendering */
                if (r->toc_mode) {
                    continue;
                }

                int screen_y = doc_y - scroll_y;

                switch (md.type) {
                case MD_LINE_H1:
#if READER_IS_HIDPI
                    if (screen_y + 40 >= 0 && screen_y < h) {
                        rdraw_serif_text(&rd, margin_x, screen_y, md.text, strlen(md.text), true);
                        rdraw_hline(&rd, margin_x, margin_x + max_w, screen_y + 32, READER_RULE_GRAY8, CANVAS1_BLACK);
                    }
                    doc_y += 44;
#else
                    if (screen_y + 26 >= 0 && screen_y < h) {
                        rdraw_serif_text(&rd, margin_x, screen_y, md.text, strlen(md.text), true);
                        rdraw_hline(&rd, margin_x, margin_x + max_w, screen_y + 19, READER_RULE_GRAY8, CANVAS1_BLACK);
                    }
                    doc_y += 30;
#endif
                    break;

                case MD_LINE_H2:
#if READER_IS_HIDPI
                    if (screen_y + 34 >= 0 && screen_y < h) {
                        rdraw_serif_text(&rd, margin_x, screen_y, md.text, strlen(md.text), true);
                    }
                    doc_y += 38;
#else
                    if (screen_y + 22 >= 0 && screen_y < h) {
                        rdraw_serif_text(&rd, margin_x, screen_y, md.text, strlen(md.text), true);
                    }
                    doc_y += 24;
#endif
                    break;

                case MD_LINE_H3:
#if READER_IS_HIDPI
                    if (screen_y + 32 >= 0 && screen_y < h) {
                        rdraw_serif_text(&rd, margin_x, screen_y, md.text, strlen(md.text), false);
                    }
                    doc_y += 34;
#else
                    if (screen_y + 20 >= 0 && screen_y < h) {
                        rdraw_serif_text(&rd, margin_x, screen_y, md.text, strlen(md.text), false);
                    }
                    doc_y += 22;
#endif
                    break;

                case MD_LINE_H4:
#if READER_IS_HIDPI
                    if (screen_y + 30 >= 0 && screen_y < h) {
                        rdraw_serif_text(&rd, margin_x, screen_y, md.text, strlen(md.text), false);
                    }
                    doc_y += 32;
#else
                    if (screen_y + 18 >= 0 && screen_y < h) {
                        rdraw_serif_text(&rd, margin_x, screen_y, md.text, strlen(md.text), false);
                    }
                    doc_y += 20;
#endif
                    break;

                case MD_LINE_BLANK:
#if READER_IS_HIDPI
                    doc_y += 14;
#else
                    doc_y += 12;
#endif
                    break;

                case MD_LINE_ULIST:
                    {
                        int indent_px = md.indent_level * 20;
#if READER_IS_HIDPI
                        if (screen_y + 30 >= 0 && screen_y < h) {
                            rdraw_fill(&rd, margin_x + indent_px + 8, screen_y + 12, margin_x + indent_px + 14, screen_y + 18, READER_FG_GRAY8, CANVAS1_BLACK);
                        }
                        render_wrapped_spans(r, &rd, margin_x + indent_px + 24, max_w - indent_px - 24, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
#else
                        if (screen_y + 16 >= 0 && screen_y < h) {
                            rdraw_fill(&rd, margin_x + indent_px + 6, screen_y + 6, margin_x + indent_px + 9, screen_y + 9, READER_FG_GRAY8, CANVAS1_BLACK);
                        }
                        render_wrapped_spans(r, &rd, margin_x + indent_px + 16, max_w - indent_px - 16, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
#endif
                        doc_y += 4;
                    }
                    break;

                case MD_LINE_OLIST:
                    {
                        int indent_px = md.indent_level * 20;
                        char num_str[16];
                        ksnprintf(num_str, sizeof(num_str), "%d.", md.order_num);
                        int pfx_w = (READER_IS_HIDPI ? 32 : 20);
                        if (screen_y + READER_LINE_H >= 0 && screen_y < h) {
                            rdraw_serif_text(&rd, margin_x + indent_px, screen_y, num_str, strlen(num_str), true);
                        }
                        render_wrapped_spans(r, &rd, margin_x + indent_px + pfx_w, max_w - indent_px - pfx_w, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
                        doc_y += 4;
                    }
                    break;

                case MD_LINE_QUOTE:
                    {
                        int q_start = doc_y;
#if READER_IS_HIDPI
                        render_wrapped_spans(r, &rd, margin_x + 24, max_w - 24, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
                        int q_h = doc_y - q_start;
                        int q_sy = q_start - scroll_y;
                        if (q_sy + q_h >= 0 && q_sy < h) {
                            rdraw_fill(&rd, margin_x + 6, q_sy, margin_x + 10, q_sy + q_h - 4, READER_QUOTE_BAR_GRAY8, CANVAS1_GREY);
                        }
                        doc_y += 10;
#else
                        render_wrapped_spans(r, &rd, margin_x + 16, max_w - 16, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
                        int q_h = doc_y - q_start;
                        int q_sy = q_start - scroll_y;
                        if (q_sy + q_h >= 0 && q_sy < h) {
                            rdraw_fill(&rd, margin_x + 4, q_sy, margin_x + 6, q_sy + q_h - 2, READER_QUOTE_BAR_GRAY8, CANVAS1_GREY);
                        }
                        doc_y += 6;
#endif
                    }
                    break;

                case MD_LINE_ALERT:
                    {
                        const char *badge = "NOTE";
                        uint8_t bar_gray = 120;
                        if (md.alert_type == MD_ALERT_TIP) { badge = "TIP"; bar_gray = 90; }
                        else if (md.alert_type == MD_ALERT_WARNING) { badge = "WARNING"; bar_gray = 40; }
                        else if (md.alert_type == MD_ALERT_IMPORTANT) { badge = "IMPORTANT"; bar_gray = 50; }
                        else if (md.alert_type == MD_ALERT_CAUTION) { badge = "CAUTION"; bar_gray = 20; }

                        int q_start = doc_y;
                        char full_badge[32];
                        ksnprintf(full_badge, sizeof(full_badge), "[%s]", badge);
                        if (screen_y + READER_LINE_H >= 0 && screen_y < h) {
                            rdraw_serif_text(&rd, margin_x + 16, screen_y, full_badge, strlen(full_badge), true);
                        }
                        doc_y += READER_LINE_H;
                        render_wrapped_spans(r, &rd, margin_x + 16, max_w - 16, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
                        int q_h = doc_y - q_start;
                        int q_sy = q_start - scroll_y;
                        if (q_sy + q_h >= 0 && q_sy < h) {
                            rdraw_fill(&rd, margin_x + 4, q_sy, margin_x + 8, q_sy + q_h - 2, bar_gray, CANVAS1_BLACK);
                        }
                        doc_y += 8;
                    }
                    break;

                case MD_LINE_HRULE:
                    if (screen_y + 12 >= 0 && screen_y < h) {
                        rdraw_hline(&rd, margin_x, margin_x + max_w, screen_y + 6, READER_RULE_GRAY8, CANVAS1_BLACK);
                    }
                    doc_y += 18;
                    break;

                case MD_LINE_TABLE_HDR:
                case MD_LINE_TABLE_ROW:
                    {
                        char cells[MD_TABLE_MAX_COLS][MD_CELL_MAX];
                        int cols = md_parse_table_cells(line_buf, cells);
                        if (cols > 0) {
                            int cell_w = max_w / cols;
                            int row_h = READER_LINE_H + 4;
                            if (screen_y + row_h >= 0 && screen_y < h) {
                                bool is_hdr = (md.type == MD_LINE_TABLE_HDR);
                                uint8_t bg_gray = is_hdr ? 238 : 250;
                                rdraw_fill(&rd, margin_x, screen_y, margin_x + max_w, screen_y + row_h - 1, bg_gray, CANVAS1_GREY);
                                for (int c = 0; c < cols; c++) {
                                    int cx = margin_x + c * cell_w + 6;
                                    rdraw_serif_text(&rd, cx, screen_y + 2, cells[c], strlen(cells[c]), is_hdr);
                                    if (c > 0) {
                                        rdraw_fill(&rd, margin_x + c * cell_w, screen_y, margin_x + c * cell_w, screen_y + row_h - 1, 210, CANVAS1_BLACK);
                                    }
                                }
                                rdraw_hline(&rd, margin_x, margin_x + max_w, screen_y + row_h - 1, is_hdr ? READER_RULE_GRAY8 : 210, CANVAS1_BLACK);
                            }
                            doc_y += row_h;
                        }
                    }
                    break;

                case MD_LINE_TABLE_SEP:
                    /* Handled by table header bottom border */
                    break;

                case MD_LINE_FOOTNOTE_DEF:
                    {
                        char badge[32];
                        ksnprintf(badge, sizeof(badge), "[^%s] ", md.footnote_id);
                        int pfx_w = (READER_IS_HIDPI ? 48 : 32);
                        if (screen_y + READER_LINE_H >= 0 && screen_y < h) {
                            rdraw_serif_text(&rd, margin_x, screen_y, badge, strlen(badge), true);
                        }
                        render_wrapped_spans(r, &rd, margin_x + pfx_w, max_w - pfx_w, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
                        doc_y += 6;
                    }
                    break;

                case MD_LINE_MATH_TOGGLE:
                    doc_y += 8;
                    break;

                case MD_LINE_MATH_TEXT:
                    {
                        uint8_t arena_buf[4096];
                        math_arena_t arena;
                        math_arena_init(&arena, arena_buf, sizeof(arena_buf));
                        math_box_t *box = latex_parse(md.text, STYLE_DISPLAY, &arena);
                        int base_scale = READER_IS_HIDPI ? 2 : 1;
                        if (box) {
                            latex_layout(box, base_scale);
                            int math_w = box->w;
                            int total_h = box->h + box->d + 16 * base_scale;
                            int cx = margin_x + (max_w - math_w) / 2;
                            if (cx < margin_x) cx = margin_x;

                            int my = doc_y - scroll_y;
                            if (my + total_h >= 0 && my < h) {
                                int baseline = my + box->h + 8 * base_scale;
                                if (rd.c) {
                                    latex_render_canvas1(box, cx, baseline, rd.c, 1);
                                } else if (rd.aa_fb) {
                                    latex_render_gray8(box, cx, baseline, rd.aa_fb, rd.aa_stride, rd.w, rd.h, 0x00, 0xFF);
                                }
                            }
                            doc_y += total_h;
                        } else {
                            int line_len = strlen(md.text);
                            int txt_w = line_len * (READER_IS_HIDPI ? 16 : 8);
                            int cx = margin_x + (max_w - txt_w) / 2;
                            if (cx < margin_x) cx = margin_x;
                            if (screen_y + READER_LINE_H >= 0 && screen_y < h) {
                                rdraw_fill(&rd, cx - 8, screen_y - 2, cx + txt_w + 8, screen_y + READER_LINE_H, 244, CANVAS1_GREY);
                                rdraw_mono_text(&rd, cx, screen_y, md.text, line_len, max_w / (READER_IS_HIDPI ? 16 : 8));
                            }
                            doc_y += READER_LINE_H + 8;
                        }
                    }
                    break;

                case MD_LINE_CODE_TOGGLE:
#if READER_IS_HIDPI
                    doc_y += 16;
#else
                    doc_y += 8;
#endif
                    break;

                case MD_LINE_CODE_TEXT:
#if READER_IS_HIDPI
                    if (screen_y + 32 >= 0 && screen_y < h) {
                        rdraw_mono_text(&rd, margin_x + 16, screen_y, md.text, strlen(md.text), (max_w - 16) / 16);
                    }
                    doc_y += 34;
#else
                    if (screen_y + 16 >= 0 && screen_y < h) {
                        rdraw_mono_text(&rd, margin_x + 12, screen_y, md.text, strlen(md.text), (max_w - 12) / 8);
                    }
                    doc_y += 16;
#endif
                    break;

                case MD_LINE_GEM_LINK:
                    link_idx++;
#if READER_IS_HIDPI
                    if (screen_y + 36 >= 0 && screen_y < h) {
                        if (r->link_count < READER_LINKS_MAX) {
                            r->links[r->link_count].x0 = (int16_t)margin_x;
                            r->links[r->link_count].y0 = (int16_t)screen_y;
                            r->links[r->link_count].x1 = (int16_t)(margin_x + max_w);
                            r->links[r->link_count].y1 = (int16_t)(screen_y + 36);
                            r->links[r->link_count].num = link_idx;
                            strncpy(r->links[r->link_count].url, md.url, sizeof(r->links[0].url) - 1);
                            r->links[r->link_count].url[sizeof(r->links[0].url) - 1] = '\0';
                            r->link_count++;
                        }

                        char prefix[16];
                        ksnprintf(prefix, sizeof(prefix), "[%d] ", link_idx);
                        int px_w = rdraw_serif_text(&rd, margin_x, screen_y, prefix, strlen(prefix), true);
                        const char *lbl = md.label[0] ? md.label : md.url;
                        int lbl_w = rdraw_serif_text(&rd, margin_x + px_w, screen_y, lbl, strlen(lbl), true);
                        rdraw_hline(&rd, margin_x + px_w, margin_x + px_w + lbl_w, screen_y + 33, READER_LINK_LINE_GRAY8, CANVAS1_BLACK);
                    }
                    doc_y += 42;
#else
                    if (screen_y + 20 >= 0 && screen_y < h) {
                        if (r->link_count < READER_LINKS_MAX) {
                            r->links[r->link_count].x0 = (int16_t)margin_x;
                            r->links[r->link_count].y0 = (int16_t)screen_y;
                            r->links[r->link_count].x1 = (int16_t)(margin_x + max_w);
                            r->links[r->link_count].y1 = (int16_t)(screen_y + 18);
                            r->links[r->link_count].num = link_idx;
                            strncpy(r->links[r->link_count].url, md.url, sizeof(r->links[0].url) - 1);
                            r->links[r->link_count].url[sizeof(r->links[0].url) - 1] = '\0';
                            r->link_count++;
                        }

                        char prefix[16];
                        ksnprintf(prefix, sizeof(prefix), "[%d] ", link_idx);
                        int px_w = rdraw_serif_text(&rd, margin_x, screen_y, prefix, strlen(prefix), true);
                        const char *lbl = md.label[0] ? md.label : md.url;
                        int lbl_w = rdraw_serif_text(&rd, margin_x + px_w, screen_y, lbl, strlen(lbl), true);
                        rdraw_hline(&rd, margin_x + px_w, margin_x + px_w + lbl_w, screen_y + 16, READER_LINK_LINE_GRAY8, CANVAS1_BLACK);
                    }
                    doc_y += 22;
#endif
                    break;

                case MD_LINE_TOC:
                    doc_y += 8;
                    break;

                case MD_LINE_TEXT:
                default:
                    render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, scroll_y, h, &doc_y, &link_idx);
                    doc_y += 4;
                    break;
                }
            } else if (line_pos < (int)sizeof(line_buf) - 1) {
                line_buf[line_pos++] = ch;
            }
        }
        file_offset += (uint64_t)n;
    }

    vfs_close(fd);
    r->total_h = doc_y + 32;

    /* Render interactive Table of Contents overlay if in TOC mode */
    if (r->toc_mode) {
        rdraw_fill(&rd, 0, 0, w - 1, h - 1, READER_BG_GRAY8, CANVAS1_WHITE);
        int ty = 24;
        rdraw_serif_text(&rd, margin_x, ty, "Table of Contents (press 't' to return)", 39, true);
        rdraw_hline(&rd, margin_x, margin_x + max_w, ty + 20, READER_RULE_GRAY8, CANVAS1_BLACK);
        ty += 34;

        r->link_count = 0;
        if (r->toc_count == 0) {
            rdraw_serif_text(&rd, margin_x, ty, "No headings found in document.", 30, false);
        } else {
            for (int i = 0; i < r->toc_count && i < READER_TOC_MAX; i++) {
                reader_toc_entry_t *entry = &r->toc[i];
                int indent_px = (entry->level > 1) ? (entry->level - 1) * 20 : 0;
                int entry_x = margin_x + indent_px;
                char num_prefix[16];
                ksnprintf(num_prefix, sizeof(num_prefix), "[%d] ", i + 1);
                int pfx_w = rdraw_serif_text(&rd, entry_x, ty, num_prefix, strlen(num_prefix), true);
                int title_w = rdraw_serif_text(&rd, entry_x + pfx_w, ty, entry->title, strlen(entry->title), false);

                if (r->link_count < READER_LINKS_MAX) {
                    reader_link_t *lnk = &r->links[r->link_count++];
                    lnk->x0 = (int16_t)entry_x;
                    lnk->y0 = (int16_t)ty;
                    lnk->x1 = (int16_t)(entry_x + pfx_w + title_w);
                    lnk->y1 = (int16_t)(ty + READER_LINE_H);
                    lnk->num = i + 1;
                    char slug[64];
                    md_heading_slug(entry->title, slug, sizeof(slug));
                    ksnprintf(lnk->url, sizeof(lnk->url), "#%s", slug);
                }
                ty += READER_LINE_H + 4;
                if (ty >= h - 24) break;
            }
        }
        s_reader_seq[rid]++;
        return;
    }

    /* Render right scrollbar if document exceeds window height */
    if (r->total_h > h) {
#if READER_IS_HIDPI
        int sb_w = 8;
        int sb_x = w - sb_w - 4;
        int thumb_h = (h * h) / r->total_h;
        if (thumb_h < 32) thumb_h = 32;
#else
        int sb_w = 4;
        int sb_x = w - sb_w - 2;
        int thumb_h = (h * h) / r->total_h;
        if (thumb_h < 16) thumb_h = 16;
#endif
        int thumb_y = (r->scroll_y * (h - thumb_h)) / (r->total_h - h);
        rdraw_fill(&rd, sb_x, 0, sb_x + sb_w, h - 1, READER_SB_TRACK_GRAY8, CANVAS1_GREY);
        rdraw_fill(&rd, sb_x, thumb_y, sb_x + sb_w, thumb_y + thumb_h, READER_SB_THUMB_GRAY8, CANVAS1_BLACK);
    }
    s_reader_seq[rid]++;

    /* Render right scrollbar if document exceeds window height */
    if (r->total_h > h) {
#if READER_IS_HIDPI
        int sb_w = 8;
        int sb_x = w - sb_w - 4;
        int thumb_h = (h * h) / r->total_h;
        if (thumb_h < 32) thumb_h = 32;
#else
        int sb_w = 4;
        int sb_x = w - sb_w - 2;
        int thumb_h = (h * h) / r->total_h;
        if (thumb_h < 16) thumb_h = 16;
#endif
        int thumb_y = (r->scroll_y * (h - thumb_h)) / (r->total_h - h);
        rdraw_fill(&rd, sb_x, 0, sb_x + sb_w, h - 1, READER_SB_TRACK_GRAY8, CANVAS1_GREY);
        rdraw_fill(&rd, sb_x, thumb_y, sb_x + sb_w, thumb_y + thumb_h, READER_SB_THUMB_GRAY8, CANVAS1_BLACK);
    }
    s_reader_seq[rid]++;
}

uint32_t reader_get_seq(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return 0;
    return s_reader_seq[rid];
}
