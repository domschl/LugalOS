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
#include "drivers/view.h"
#include "kernel/keyseq.h"
#include "drivers/ugui.h"
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

/* Without a panel (the C6, an RP2350 without the LCD) the one instance is for a
 * help page at most: smaller tables, ~2 KB instead of ~7.5 KB of static RAM. */
#if defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO))
#define READER_LINKS_MAX   8
#define READER_TOC_MAX     8
#define READER_STACK_MAX   2
#define READER_CP_MAX      1           /* a help page at most: always the full pass */
#else
#define READER_LINKS_MAX   32
#define READER_TOC_MAX     32
#define READER_STACK_MAX   8
#if defined(CONFIG_PSRAM_BYTES)
#define READER_CP_MAX      512         /* 6 KB a reader, in PSRAM with the rest of it */
#else
#define READER_CP_MAX      32          /* no PSRAM: the state is low RAM */
#endif
#endif

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

/* A place to resume the layout from: the start of a source line, where the
 * document's y and everything the parser carries from line to line are
 * known. Lines are laid out independently given in_code / in_math, and links
 * are numbered through the document, so these four values are all of it. */
typedef struct {
    uint32_t off;               /* byte offset of the line's first byte */
    int32_t  doc_y;
    uint16_t link_idx;
    uint8_t  flags;             /* READER_CP_* */
} reader_cp_t;
#define READER_CP_CODE 0x1u
#define READER_CP_MATH 0x2u

typedef struct {
    bool                active;
    uint8_t             target_cid;
    /* Checkpoints from the last full layout at width cp_w, every cp_step
     * px of the document at least; valid until the document or the width
     * changes. A draw then lays out only from the checkpoint above the
     * view to its bottom, instead of the whole file. */
    reader_cp_t         cp[READER_CP_MAX];
    int                 cp_count;
    int                 cp_step;
    int                 cp_w;
    uint32_t            cp_size;    /* the file's size at that layout */
    bool                cp_valid;
    int                 drawn_scroll_y; /* where the surfaces show the document, -1 unknown */
    bool                strip_failed;   /* the last strip draw could not lay out from checkpoints */
    int                 strip_diff_y0, strip_diff_y1; /* reader_strip_check()'s differing GRAY8 rows */
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
    ugui_ctx_t          ui_ctx;
    uint8_t            *aa_buf;        /* Anti-aliased 8-bpp (GRAY8) surface in PSRAM */
    uint32_t            aa_stride;
    int                 aa_w, aa_h;
    uint32_t            aa_pages;
} reader_state_t;

/* guarded by: console_lock for opening and closing an instance (the surface
 * and the active flag), which the compositor reads under it. accepted: the
 * scroll position and the drawing into a live surface happen from the input
 * path without it; the compositor can show a frame mid-draw. */
/* BULK_BSS: link tables, history and contents, touched at the speed of a person -- PSRAM where there
 * is one, which returns about 30 KB of low RAM on the P4 boards. */
static reader_state_t s_readers[READER_MAX_INSTANCES] BULK_BSS;
static uint8_t        s_active_reader_rid = 0;
static uint32_t       s_reader_seq[READER_MAX_INSTANCES];

static bool reader_scroll_step(uint8_t rid, const canvas1_t *c, int w, int h);

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
    r->cp_valid = false;
    r->drawn_scroll_y = -1;
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
        s_readers[rid].cp_valid = false;
        changed = true;
    }
    if (h > 0 && s_readers[rid].win_h != h) {
        s_readers[rid].win_h = h;
        changed = true;
    }
    return changed;
}

/* The reader's 1-bpp backing, drawn, into its tile on the screen (the
 * screen's 'W'): only that tile, not the whole screen repainted. */
static void reader_show(uint8_t rid) {
    uint8_t req[2] = { 'W', rid }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2, reply);
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
    reader_show(rid);
}

/* FNV-1a over the reader's surfaces as drawn: its GRAY8 surface where it has
 * one, and its 1-bpp backing. */
static uint64_t reader_surface_hash(uint8_t rid) {
    reader_state_t *r = &s_readers[rid];
    uint64_t hsh = 0xcbf29ce484222325ull;
    int w = r->win_w, h = r->win_h;
    if (r->aa_buf && w > 0 && h > 0 && w <= r->aa_w && h <= r->aa_h) {
        for (int y = 0; y < h; y++) {
            const uint8_t *row = r->aa_buf + (uint32_t)y * r->aa_stride;
            for (int x = 0; x < w; x++) { hsh ^= row[x]; hsh *= 0x100000001b3ull; }
        }
    }
    /* The 1-bpp backing is screen-sized; only the window's part is shown (the
     * screen's 'W' copies w x h of it), and a full draw lets a last line run
     * past the window's bottom into rows a strip draw clips. */
    canvas1_t *c = get_reader_canvas(rid);
    if (c && w > 0 && h > 0) {
        for (int y = 0; y < h && y < (int)c->h; y++) {
            const uint8_t *row = c->fb + (uint32_t)y * c->stride;
            for (int b = 0; b < w / 8; b++) { hsh ^= row[b]; hsh *= 0x100000001b3ull; }
            for (int x = (w / 8) * 8; x < w; x++) { hsh ^= (row[x / 8] >> (x % 8)) & 1u; hsh *= 0x100000001b3ull; }
        }
    }
    return hsh;
}

int reader_layout_check(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return -1;
    reader_state_t *r = &s_readers[rid];
    bool had = r->cp_valid;
    reader_update_instance(rid);               /* from the checkpoints, if it has them */
    uint64_t a = reader_surface_hash(rid);
    r->cp_valid = false;
    reader_update_instance(rid);               /* the whole file */
    uint64_t b = reader_surface_hash(rid);
    return !had ? 2 : (a == b ? 1 : 0);
}

int reader_strip_check(uint8_t rid, int dy) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return -1;
    reader_state_t *r = &s_readers[rid];
    canvas1_t *c = get_reader_canvas(rid);
    int w = r->win_w, h = r->win_h;
    if (!c || w <= 0 || h <= 0) return -1;
    int max_scroll = r->total_h - 120;
    int start = r->scroll_y;
    int target = start + dy;
    if (target < 0) target = 0;
    if (target > max_scroll) target = max_scroll > 0 ? max_scroll : 0;
    reader_update_instance(rid);               /* the surfaces at start, whole */
    r->scroll_y = target;
    if (!reader_scroll_step(rid, c, w, h)) { reader_update_instance(rid); return 2; }
    uint64_t a = reader_surface_hash(rid);
    /* Row hashes of the stepped GRAY8 surface, to say where it differs. */
    uint32_t pages = ((uint32_t)h * 8u + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint64_t *rows = r->aa_buf ? (uint64_t *)palloc_pages_bulk(pages) : NULL;
    if (rows)
        for (int y = 0; y < h; y++) {
            uint64_t hh = 0xcbf29ce484222325ull;
            const uint8_t *p = r->aa_buf + (uint32_t)y * r->aa_stride;
            for (int x = 0; x < w; x++) { hh ^= p[x]; hh *= 0x100000001b3ull; }
            rows[y] = hh;
        }
    reader_update_instance(rid);               /* the same place, drawn whole */
    uint64_t b = reader_surface_hash(rid);
    r->strip_diff_y0 = r->strip_diff_y1 = -1;
    if (rows) {
        for (int y = 0; y < h; y++) {
            uint64_t hh = 0xcbf29ce484222325ull;
            const uint8_t *p = r->aa_buf + (uint32_t)y * r->aa_stride;
            for (int x = 0; x < w; x++) { hh ^= p[x]; hh *= 0x100000001b3ull; }
            if (hh != rows[y]) { if (r->strip_diff_y0 < 0) r->strip_diff_y0 = y; r->strip_diff_y1 = y; }
        }
        palloc_free(rows, pages);
    }
    return a == b ? 1 : 0;
}

void reader_strip_diff(uint8_t rid, int *y0, int *y1) {
    *y0 = rid < READER_MAX_INSTANCES ? s_readers[rid].strip_diff_y0 : -1;
    *y1 = rid < READER_MAX_INSTANCES ? s_readers[rid].strip_diff_y1 : -1;
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
            if (!reader_scroll_step(rid, c, w, h)) reader_draw(rid, c, w, h);
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
        if (!reader_scroll_step(rid, c, w, h)) reader_draw(rid, c, w, h);
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

void reader_drag(int dy) {
    uint8_t rid = s_active_reader_rid;
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active || dy == 0) return;
    reader_state_t *r = &s_readers[rid];
    canvas1_t *c = get_reader_canvas(rid);
    int w = r->win_w > 0 ? r->win_w : (c ? (int)c->w : 0);
    int h = r->win_h > 0 ? r->win_h : (c ? (int)c->h : 0);
    if (!c || w <= 0 || h <= 0) return;
    int max_scroll = r->total_h - 120;
    if (max_scroll < 0) max_scroll = 0;
    int target = r->scroll_y - dy;            /* the finger down pulls the document down */
    if (target < 0) target = 0;
    if (target > max_scroll) target = max_scroll;
    if (target == r->scroll_y) return;
    r->scroll_y = target;
    if (!reader_scroll_step(rid, c, w, h)) reader_draw(rid, c, w, h);
#if defined(LCDTERM_PRESENT)
    lcdterm_sync_post();
#endif
}

void reader_drag_end(int dy, uint32_t ms) {
    uint8_t rid = s_active_reader_rid;
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    /* Still moving at least 3 mm over the last ~100 ms: a fling from there. */
#if defined(CONFIG_PANEL_DPI)
    int fling_px = (int)((3u * CONFIG_PANEL_DPI * 10u + 127u) / 254u);
#else
    int fling_px = 16;
#endif
    if (ms > 0 && (dy >= fling_px || dy <= -fling_px)) reader_touch_fling_instance(rid, dy, ms);
    else reader_update_instance(rid);
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

/* The action bar's height. Its buttons are touched over more than they
 * show (ugui_set_min_target()), not drawn bigger. */
static int reader_bar_h(void) {
    return 26 * (int)CHROME_SCALE;
}

enum {
    READER_UI_BACK     = 1,
    READER_UI_CONTENTS = 2,
    READER_UI_ROTATE   = 3,
    READER_UI_TOP      = 4,
};

static bool reader_tap_act(uint8_t rid, int client_x, int client_y);

static void reader_redraw_bar(uint8_t rid);

uint8_t reader_active_instance(void) {
    return s_active_reader_rid;
}

static uint16_t reader_hit(const reader_state_t *r, int x, int y, int *link);

/* Arms what a tap there would act on (reader_hit()): nothing on a link. */
void reader_press(uint8_t rid, int client_x, int client_y) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    reader_state_t *r = &s_readers[rid];
    int link;
    bool changed = reader_hit(r, client_x, client_y, &link) ? ugui_arm(&r->ui_ctx, client_x, client_y)
                                                            : ugui_disarm(&r->ui_ctx);
    if (changed) reader_redraw_bar(rid);
}

void reader_release(void) {
    for (uint8_t rid = 0; rid < READER_MAX_INSTANCES; rid++)
        if (s_readers[rid].active && ugui_disarm(&s_readers[rid].ui_ctx)) reader_redraw_bar(rid);
}

/* A tap: the pressed button lets go first, then acts; the bar is drawn
 * again only if the action did not draw the reader. */
bool reader_tap_instance(uint8_t rid, int client_x, int client_y) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return false;
    bool was_armed = ugui_disarm(&s_readers[rid].ui_ctx);
    uint32_t seq = s_reader_seq[rid];
    bool acted = reader_tap_act(rid, client_x, client_y);
    if (was_armed && s_readers[rid].active && s_reader_seq[rid] == seq) reader_redraw_bar(rid);
    return acted;
}

/* The link under (x, y) -- its words, 4 units taller -- or -1. Below the
 * action bar only. */
static int reader_link_at(const reader_state_t *r, int x, int y) {
    if (y < reader_bar_h()) return -1;
    int pad_y = 4 * (int)CHROME_SCALE;
    for (int i = 0; i < r->link_count; i++)
        if (x >= r->links[i].x0 && x <= r->links[i].x1 &&
            y >= r->links[i].y0 - pad_y && y <= r->links[i].y1 + pad_y) return i;
    return -1;
}

/* What is under a finger at (x, y), in this order: a button as drawn, a
 * link as drawn, a button's grown touch area. So a button is easy to hit
 * where nothing else is, and a link just under the bar stays the link's. */
static uint16_t reader_hit(const reader_state_t *r, int x, int y, int *link) {
    uint16_t hit = ugui_hit_test_drawn(&r->ui_ctx, x, y, NULL);
    *link = -1;
    if (hit) return hit;
    *link = reader_link_at(r, x, y);
    return *link >= 0 ? 0 : ugui_hit_test(&r->ui_ctx, x, y, NULL);
}

static bool reader_tap_act(uint8_t rid, int client_x, int client_y) {
    reader_state_t *r = &s_readers[rid];

    int link = -1;
    uint16_t hit = reader_hit(r, client_x, client_y, &link);
    if (hit == READER_UI_BACK) {
        (void)reader_back_instance(rid);
        return true;
    }
    if (hit == READER_UI_CONTENTS) {
        r->toc_mode = !r->toc_mode;
        reader_update_instance(rid);
        return true;
    }
    if (hit == READER_UI_ROTATE) {
        console_hotkey(CONSOLE_HOTKEY_ORIENT);
        return true;
    }
    if (hit == READER_UI_TOP) {
        r->scroll_y = 0;
        r->toc_mode = false;
        reader_update_instance(rid);
        return true;
    }

    if (link < 0) return false;
    {
        int i = link;
        {
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

/* A key for the active reader (kernel/keyseq.h's codes). Vi- and
 * Emacs-style: j / k / Ctrl-N / Ctrl-P / arrows a line, Space / Ctrl-V /
 * PgDn / Right a page down, Alt-V / PgUp a page up, d / u half a page, g /
 * Home and G / End the ends, b / Backspace / Left back, t the contents, 1-9
 * a link, r reload, o rotate, ? / h / Ctrl-H help, q / Ctrl-C close. The
 * system's Cmd shortcuts never come here: the console takes them. */
static void reader_key(uint8_t rid, uint32_t key, uint8_t mods) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    reader_state_t *r = &s_readers[rid];
    int win_h = r->win_h > 0 ? r->win_h : 480;
    int page_dy = win_h - 40;
    if (page_dy < 40) page_dy = 40;

    if (mods & KMOD_ALT) {
        if (key == 'v' || key == 'V') reader_scroll_instance(rid, -page_dy);   /* Emacs page up */
        return;
    }
    switch (key) {
    case 0x08: case '?': case 'h':
        reader_open_instance(rid, "/sd0/help/reader.md");
        reader_update_instance(rid);
        return;
    case 'q': case 0x03: {
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
        uint8_t req[1] = { 'C' }, reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 1, reply) && reply[0] == 0) {
            if (reply[10] == 3) reader_close_instance(reply[11]);
            vterm_set_active(reply[1]);
        }
        console_set_active_win_type(1 /* RIBBON_WIN_TERM */);
        return;
    }
    case 'b': case 0x7f: case KEY_LEFT:
        reader_back_instance(rid);
        return;
    case ' ': case 0x16: case KEY_PGDN: case KEY_RIGHT:
        reader_scroll_instance(rid, page_dy);
        return;
    case KEY_PGUP:
        reader_scroll_instance(rid, -page_dy);
        return;
    case 'j': case 0x0e: case '\r': case '\n': case KEY_DOWN:
        reader_scroll_instance(rid, 32);
        return;
    case 'k': case 0x10: case KEY_UP:
        reader_scroll_instance(rid, -32);
        return;
    case 'd': case 0x04:
        reader_scroll_instance(rid, page_dy / 2);
        return;
    case 'u': case 0x15:
        reader_scroll_instance(rid, -page_dy / 2);
        return;
    case 'g': case KEY_HOME:
        r->scroll_y = 0;
        reader_update_instance(rid);
        return;
    case 'G': case KEY_END: {
        int max_scroll = r->total_h - 120;
        r->scroll_y = max_scroll > 0 ? max_scroll : 0;
        reader_update_instance(rid);
        return;
    }
    case 'r': case 0x12:
        r->cp_valid = false;                             /* the file may have changed under it */
        reader_update_instance(rid);
        return;
    case 'o': case 'O':
        console_hotkey(CONSOLE_HOTKEY_ORIENT);
        return;
    case 't': case 'T':
        r->toc_mode = !r->toc_mode;
        reader_update_instance(rid);
        return;
    default:
        if (key >= '1' && key <= '9') reader_follow_link_num_instance(rid, (int)(key - '0'));
        return;
    }
}

static bool reader_active_any(void) {
    return reader_is_active();
}

/* The reader as a view (drivers/view.h): each event onto what it did before. */
static void reader_view_event(uint8_t rid, const win_event_t *ev) {
    switch (ev->type) {
    case WIN_EV_POINTER_DOWN:
        if (ev->flags & WIN_EVF_PRESS) reader_press(rid, ev->x, ev->y);
        return;
    case WIN_EV_POINTER_UP:
        if (ev->flags & WIN_EVF_TAP) (void)reader_tap_instance(rid, ev->x, ev->y);
        else if (ev->flags & WIN_EVF_PRESS) reader_release();
        return;
    case WIN_EV_DRAG:
        reader_drag(ev->dy);
        return;
    case WIN_EV_DRAG_END:
        reader_drag_end(ev->dy, ev->key);
        return;
    case WIN_EV_SCROLL:
        if (ev->key > 0 && ev->dy != 0) reader_touch_fling(ev->dy, ev->key);
        else reader_scroll(ev->dy > 0 ? -160 : 160);
        return;
    case WIN_EV_KEY_DOWN:
        reader_key(rid, ev->key, ev->mods);
        return;
    default:
        return;
    }
}

const view_ops_t g_reader_view = {
    .name = "reader",
    .active = reader_active_any,
    .event = reader_view_event,
    .captures = NULL,
};

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

/* The reader draws through ugui's surface and primitives (drivers/ugui.h):
 * the same ink and paper (READER_FG_GRAY8 / READER_BG_GRAY8 are ugui's), the
 * same fonts at the same scale. These keep the reader's inclusive corners
 * and its monospace limit. */
static void rdraw_fill(const ugui_surface_t *rd, int x0, int y0, int x1, int y1, uint8_t gray, unsigned pat1) {
    if (x1 < x0 || y1 < y0) return;
    ugui_fill(rd, (ugui_rect_t){ (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0 + 1), (int16_t)(y1 - y0 + 1) }, gray, pat1);
}

static void rdraw_hline(const ugui_surface_t *rd, int x0, int x1, int y, uint8_t gray, unsigned pat1) {
    ugui_hline(rd, x0, x1, y, 1, gray, pat1);
}

static int rdraw_serif_text(const ugui_surface_t *rd, int x, int y, const char *s, uint32_t len, bool bold, bool italic) {
    return ugui_text_serif(rd, x, y, s, len, bold, italic, false);
}

static void rdraw_mono_text(const ugui_surface_t *rd, int x, int y, const char *s, uint32_t len, int max_chars) {
    uint32_t n = 0;
    while (n < len && s[n] && (int)n < max_chars) n++;
    (void)ugui_text_mono(rd, x, y, s, n, false, false);
}


/* Word wrapping and rich proportional / monospace span rendering */
static int render_wrapped_spans(reader_state_t *r, ugui_surface_t *rd, int x0, int max_w,
                                const char *text, int line_h, uint16_t extra_style,
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
                    if (rd->c1) {
                        latex_render_canvas1(box, cur_x, baseline, rd->c1, 1);
                    }
                    if (rd->aa_fb) {
                        latex_render_gray8(box, cur_x, baseline, rd->aa_fb, rd->aa_stride, rd->w, rd->h, READER_FG_GRAY8, READER_BG_GRAY8);
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
                    bool bold = ((span.style | extra_style) & MD_STYLE_BOLD) != 0;
                    bool italic = ((span.style | extra_style) & MD_STYLE_ITALIC) != 0;
                    rdraw_serif_text(rd, cur_x, screen_y, word_start, (uint32_t)word_len, bold, italic);
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

static void draw_reader_action_bar(reader_state_t *r, ugui_surface_t *rd, int w, int h) {
    int sc = (int)CHROME_SCALE;
    int bar_h = reader_bar_h();
    ugui_surface_t surf = {
        .c1 = rd->c1,
        .aa_fb = rd->aa_fb,
        .aa_stride = rd->aa_stride,
        .w = w,
        .h = h,
        .scale = sc,
    };

    ugui_rect_t bar_rect = { .x = 0, .y = 0, .w = (int16_t)w, .h = (int16_t)bar_h };
    ugui_fill(&surf, bar_rect, UGUI_GRAY_TOOLBAR, CANVAS1_WHITE);
    ugui_hline(&surf, 0, w - 1, bar_h - sc, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);

    int btn_h = 18 * sc;
    int btn_y = (bar_h - btn_h - sc) / 2;
    int pad_x = 8 * sc;
    int gap = 8 * sc;
    /* The buttons' touch areas grow to a target, past the bar into the
     * page: a link drawn there still wins (reader_tap_act()). */
    ugui_set_min_target(&r->ui_ctx, ugui_target_px());

    /* Dimmed when they cannot act: Back with no history and no contents
     * view open, Top at the top. Contents is not a lit toggle (inverted is
     * the finger's): while the contents view is open it says Close. */
    ugui_rect_t b_back = { .x = (int16_t)pad_x, .y = (int16_t)btn_y, .w = (int16_t)(58 * sc), .h = (int16_t)btn_h };
    if (!r->toc_mode && r->history_depth <= 0) ugui_disable_next(&r->ui_ctx, 1);
    ugui_toolbar_button(&surf, &r->ui_ctx, b_back, "< Back", READER_UI_BACK, false);

    ugui_rect_t b_toc = {
        .x = (int16_t)(b_back.x + b_back.w + gap),
        .y = (int16_t)btn_y,
        .w = (int16_t)(72 * sc),
        .h = (int16_t)btn_h,
    };
    ugui_toolbar_button(&surf, &r->ui_ctx, b_toc, r->toc_mode ? "Close" : "Contents", READER_UI_CONTENTS, false);

    ugui_rect_t b_top = {
        .x = (int16_t)(w - pad_x - 46 * sc),
        .y = (int16_t)btn_y,
        .w = (int16_t)(46 * sc),
        .h = (int16_t)btn_h,
    };
    if (!r->toc_mode && r->scroll_y <= 0) ugui_disable_next(&r->ui_ctx, 1);
    ugui_toolbar_button(&surf, &r->ui_ctx, b_top, "Top", READER_UI_TOP, false);

#if defined(CONFIG_BOARD_TAB5)
    ugui_rect_t b_rot = {
        .x = (int16_t)(b_top.x - gap - 60 * sc),
        .y = (int16_t)btn_y,
        .w = (int16_t)(60 * sc),
        .h = (int16_t)btn_h,
    };
    if (b_rot.x > b_toc.x + b_toc.w + gap) {
        ugui_toolbar_button(&surf, &r->ui_ctx, b_rot, "Rotate", READER_UI_ROTATE, false);
    }
#endif
}

/* The action bar alone, drawn again over the page and shown: its buttons
 * are the reader's only hit-boxes, so they are registered afresh. */
static void reader_redraw_bar(uint8_t rid) {
    reader_state_t *r = &s_readers[rid];
    canvas1_t *c = get_reader_canvas(rid);
    if (!c) return;
    int w = r->win_w > 0 ? r->win_w : (int)c->w;
    int h = r->win_h > 0 ? r->win_h : (int)c->h;
    ugui_surface_t rd = { .c1 = c, .aa_fb = r->aa_buf, .aa_stride = r->aa_stride, .w = w, .h = h, .scale = (int)CHROME_SCALE };
    ugui_begin(&r->ui_ctx);
    draw_reader_action_bar(r, &rd, w, h);
    reader_show(rid);
}

/* The scrollbar along the right edge, over the whole height. */
static void draw_reader_scrollbar(reader_state_t *r, ugui_surface_t *rd, int w, int h) {
    if (r->total_h <= h) return;
    int sc = (int)CHROME_SCALE;
    int bar_h = reader_bar_h();
    int sb_w = 4 * sc;
    ugui_surface_t surf = {
        .c1 = rd->c1,
        .aa_fb = rd->aa_fb,
        .aa_stride = rd->aa_stride,
        .w = w,
        .h = h,
        .scale = sc,
    };
    ugui_rect_t track = {
        .x = (int16_t)(w - sb_w - 2 * sc),
        .y = (int16_t)bar_h,
        .w = (int16_t)sb_w,
        .h = (int16_t)(h - bar_h),
    };
    ugui_scrollbar(&surf, track, r->scroll_y, r->total_h, h);
}

/* The reader's view of rows clip_y0..clip_y1-1 of its w x full_h surfaces;
 * the whole of it, chrome included, when that is all rows. A strip (an
 * animation step: reader_scroll_step()) is drawn into the surfaces from its
 * first row as if it were a view that tall, further down the document, and
 * leaves the hit-boxes, links and chrome to the full draw that follows; it
 * lays out from checkpoints only, and sets strip_failed when it cannot. */
static void reader_draw_view(uint8_t rid, const canvas1_t *c, int w, int full_h, int clip_y0, int clip_y1) {
    if (w <= 0 || full_h <= 0 || rid >= READER_MAX_INSTANCES) return;
    reader_state_t *r = &s_readers[rid];
    int sc = (int)CHROME_SCALE;
    int bar_h = reader_bar_h();
    bool strip = clip_y0 > 0 || clip_y1 < full_h;
    int h = strip ? clip_y1 - clip_y0 : full_h;
    if (h <= 0) return;

    canvas1_t sub;
    if (strip && c) canvas1_init(&sub, c->fb + (uint32_t)clip_y0 * c->stride, c->stride, c->w, (unsigned)h);
    if (!strip) ugui_begin(&r->ui_ctx);

    ugui_surface_t rd = {
        .scale = (int)CHROME_SCALE,
        .c1 = strip && c ? &sub : c,
        .aa_fb = r->aa_buf ? r->aa_buf + (uint32_t)(strip ? clip_y0 : 0) * r->aa_stride : NULL,
        .aa_stride = r->aa_stride,
        .w = w,
        .h = h,
    };
    int links_before = r->link_count;

    /* Fill background */
    rdraw_fill(&rd, 0, 0, w - 1, h - 1, READER_BG_GRAY8, CANVAS1_WHITE);

    if (strip && (!r->active || !r->path[0])) { r->strip_failed = true; return; }
    if (!r->active || !r->path[0]) {
        rdraw_serif_text(&rd, READER_MARGIN_X, bar_h + 16 * sc, "No document loaded. Use 'reader <file.md>' to open.", 51, false, false);
        draw_reader_action_bar(r, &rd, w, h);
        return;
    }

    const char *file_to_open = r->path;
    char spartan_tmp[32];
    if (strncmp(r->path, "spartan://", 10) == 0 || strncmp(r->path, "SPARTAN://", 10) == 0) {
        ksnprintf(spartan_tmp, sizeof(spartan_tmp), "/ram0/spartan%d.gmi", (int)rid);
        file_to_open = spartan_tmp;
    }

    int fd = vfs_open(file_to_open, VFS_O_READ);
    if (fd < 0 && strip) { r->strip_failed = true; return; }
    if (fd < 0) {
        char err[128];
        ksnprintf(err, sizeof(err), "Cannot open document: %s", file_to_open);
        rdraw_serif_text(&rd, READER_MARGIN_X, bar_h + 16 * sc, err, strlen(err), false, false);
        draw_reader_action_bar(r, &rd, w, h);
        return;
    }

    int margin_x = READER_MARGIN_X;
    int max_w = w - 2 * margin_x;
    if (max_w < 60) max_w = 60;

    int doc_y = bar_h + 12 * sc;
    int scroll_y = r->scroll_y + (strip ? clip_y0 : 0);
    int link_idx = 0;
    r->link_count = 0;

    uint64_t file_offset = 0;
    char read_buf[512];
    char line_buf[MD_LINE_MAX];
    int line_pos = 0;
    bool in_code = false;
    bool in_math = false;

    /* With checkpoints for this width, from the last one at or above the
     * view, and only down to its bottom: the contents, the links' numbering
     * and the total height are the full pass's. Otherwise the whole file, and
     * checkpoints on the way (the contents view always takes the whole file). */
    vfs_stat_t st;
    uint32_t file_size = vfs_fstat(fd, &st) == 0 ? (uint32_t)st.size : 0xffffffffu;
    bool partial = r->cp_valid && r->cp_w == w && r->cp_count > 0 && !r->toc_mode &&
                   r->cp_size == file_size;     /* rewritten since (editor, a new Spartan page) */
    if (strip && !partial) {
        r->strip_failed = true;
        r->link_count = links_before;
        vfs_close(fd);
        return;
    }
    if (partial) {
        int lo = 0, hi = r->cp_count - 1;
        while (lo < hi) {                     /* the last checkpoint with doc_y <= scroll_y */
            int mid = (lo + hi + 1) / 2;
            if (r->cp[mid].doc_y <= scroll_y) lo = mid; else hi = mid - 1;
        }
        const reader_cp_t *cp = &r->cp[lo];
        file_offset = cp->off;
        doc_y = cp->doc_y;
        link_idx = cp->link_idx;
        in_code = (cp->flags & READER_CP_CODE) != 0;
        in_math = (cp->flags & READER_CP_MATH) != 0;
    } else {
        r->toc_count = 0;
        r->cp_count = 0;
        r->cp_step = h > 0 ? h / 2 : 300;
        r->cp_w = w;
        r->cp_size = file_size;
    }
    int next_cp_y = doc_y;
    uint64_t line_off = file_offset;          /* where the line in line_buf began */
    bool past_view = false;

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
                        render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
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
                uint64_t this_off = line_off;
                line_off = file_offset + (uint64_t)i + 1u;

                if (partial) {
                    if (doc_y > scroll_y + h) { past_view = true; break; }
                } else if (doc_y >= next_cp_y) {
                    if (r->cp_count == READER_CP_MAX) {
                        /* Full: every other one kept, twice the spacing. */
                        for (int k = 0; k < READER_CP_MAX / 2; k++) r->cp[k] = r->cp[2 * k];
                        r->cp_count = READER_CP_MAX / 2;
                        r->cp_step *= 2;
                    }
                    reader_cp_t *cp = &r->cp[r->cp_count++];
                    cp->off = (uint32_t)this_off;
                    cp->doc_y = doc_y;
                    cp->link_idx = (uint16_t)link_idx;
                    cp->flags = (uint8_t)((in_code ? READER_CP_CODE : 0u) | (in_math ? READER_CP_MATH : 0u));
                    next_cp_y = doc_y + r->cp_step;
                }

                md_line_t md;
                if (!md_parse_line(line_buf, &in_code, &in_math, &md)) continue;

                /* Collect headings into Table of Contents */
                if (md.type == MD_LINE_H1 || md.type == MD_LINE_H2 ||
                    md.type == MD_LINE_H3 || md.type == MD_LINE_H4) {
                    if (r->toc_count < READER_TOC_MAX) {
                        reader_toc_entry_t *entry = &r->toc[r->toc_count++];
                        md_heading_clean_title(md.text, entry->title, sizeof(entry->title));
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
                    {
                        int h1_line_h = READER_LINE_H + (READER_IS_HIDPI ? 6 : 4);
                        render_wrapped_spans(r, &rd, margin_x, max_w, md.text, h1_line_h, MD_STYLE_BOLD, scroll_y, h, &doc_y, &link_idx);
                        int rule_sy = doc_y - scroll_y - (READER_IS_HIDPI ? 6 : 3);
                        if (rule_sy >= 0 && rule_sy < h) {
                            rdraw_hline(&rd, margin_x, margin_x + max_w, rule_sy, READER_RULE_GRAY8, CANVAS1_BLACK);
                        }
                        doc_y += (READER_IS_HIDPI ? 8 : 4);
                    }
                    break;

                case MD_LINE_H2:
                    {
                        int h2_line_h = READER_LINE_H + (READER_IS_HIDPI ? 4 : 2);
                        render_wrapped_spans(r, &rd, margin_x, max_w, md.text, h2_line_h, MD_STYLE_BOLD, scroll_y, h, &doc_y, &link_idx);
                        doc_y += (READER_IS_HIDPI ? 8 : 4);
                    }
                    break;

                case MD_LINE_H3:
                    render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, MD_STYLE_BOLD, scroll_y, h, &doc_y, &link_idx);
                    doc_y += (READER_IS_HIDPI ? 6 : 4);
                    break;

                case MD_LINE_H4:
                    render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                    doc_y += (READER_IS_HIDPI ? 4 : 2);
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
                        render_wrapped_spans(r, &rd, margin_x + indent_px + 24, max_w - indent_px - 24, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
#else
                        if (screen_y + 16 >= 0 && screen_y < h) {
                            rdraw_fill(&rd, margin_x + indent_px + 6, screen_y + 6, margin_x + indent_px + 9, screen_y + 9, READER_FG_GRAY8, CANVAS1_BLACK);
                        }
                        render_wrapped_spans(r, &rd, margin_x + indent_px + 16, max_w - indent_px - 16, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
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
                            rdraw_serif_text(&rd, margin_x + indent_px, screen_y, num_str, strlen(num_str), true, false);
                        }
                        render_wrapped_spans(r, &rd, margin_x + indent_px + pfx_w, max_w - indent_px - pfx_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                        doc_y += 4;
                    }
                    break;

                case MD_LINE_QUOTE:
                    {
                        int q_start = doc_y;
#if READER_IS_HIDPI
                        render_wrapped_spans(r, &rd, margin_x + 24, max_w - 24, md.text, READER_LINE_H, MD_STYLE_ITALIC, scroll_y, h, &doc_y, &link_idx);
                        int q_h = doc_y - q_start;
                        int q_sy = q_start - scroll_y;
                        if (q_sy + q_h >= 0 && q_sy < h) {
                            rdraw_fill(&rd, margin_x + 6, q_sy, margin_x + 10, q_sy + q_h - 4, READER_QUOTE_BAR_GRAY8, CANVAS1_GREY);
                        }
                        doc_y += 10;
#else
                        render_wrapped_spans(r, &rd, margin_x + 16, max_w - 16, md.text, READER_LINE_H, MD_STYLE_ITALIC, scroll_y, h, &doc_y, &link_idx);
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
                            rdraw_serif_text(&rd, margin_x + 16, screen_y, full_badge, strlen(full_badge), true, false);
                        }
                        doc_y += READER_LINE_H;
                        render_wrapped_spans(r, &rd, margin_x + 16, max_w - 16, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
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
                                    rdraw_serif_text(&rd, cx, screen_y + 2, cells[c], strlen(cells[c]), is_hdr, false);
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
                            rdraw_serif_text(&rd, margin_x, screen_y, badge, strlen(badge), true, false);
                        }
                        render_wrapped_spans(r, &rd, margin_x + pfx_w, max_w - pfx_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
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
                                if (rd.c1) {
                                    latex_render_canvas1(box, cx, baseline, rd.c1, 1);
                                }
                                if (rd.aa_fb) {
                                    latex_render_gray8(box, cx, baseline, rd.aa_fb, rd.aa_stride, rd.w, rd.h, READER_FG_GRAY8, READER_BG_GRAY8);
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
                        int px_w = rdraw_serif_text(&rd, margin_x, screen_y, prefix, strlen(prefix), true, false);
                        const char *lbl = md.label[0] ? md.label : md.url;
                        int lbl_w = rdraw_serif_text(&rd, margin_x + px_w, screen_y, lbl, strlen(lbl), true, false);
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
                        int px_w = rdraw_serif_text(&rd, margin_x, screen_y, prefix, strlen(prefix), true, false);
                        const char *lbl = md.label[0] ? md.label : md.url;
                        int lbl_w = rdraw_serif_text(&rd, margin_x + px_w, screen_y, lbl, strlen(lbl), true, false);
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
                    render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                    doc_y += 4;
                    break;
                }
            } else if (line_pos < (int)sizeof(line_buf) - 1) {
                line_buf[line_pos++] = ch;
            }
        }
        if (past_view) break;
        file_offset += (uint64_t)n;
    }

    vfs_close(fd);
    if (!partial) {
        r->total_h = doc_y + 32;
        r->cp_valid = !r->toc_mode;
    }

    /* Render interactive Table of Contents overlay if in TOC mode */
    if (r->toc_mode) {
        rdraw_fill(&rd, 0, 0, w - 1, h - 1, READER_BG_GRAY8, CANVAS1_WHITE);
        int ty = bar_h + 12 * sc;
        rdraw_serif_text(&rd, margin_x, ty, "Table of Contents (tap heading or 'Contents' to return)", 55, true, false);
        rdraw_hline(&rd, margin_x, margin_x + max_w, ty + READER_LINE_H + 2 * sc, READER_RULE_GRAY8, CANVAS1_BLACK);
        ty += READER_LINE_H + 12 * sc;

        r->link_count = 0;
        if (r->toc_count == 0) {
            rdraw_serif_text(&rd, margin_x, ty, "No headings found in document.", 30, false, false);
        } else {
            for (int i = 0; i < r->toc_count && i < READER_TOC_MAX; i++) {
                reader_toc_entry_t *entry = &r->toc[i];
                int indent_px = (entry->level > 1) ? (entry->level - 1) * 16 * sc : 0;
                int entry_x = margin_x + indent_px;
                char num_prefix[16];
                ksnprintf(num_prefix, sizeof(num_prefix), "[%d] ", i + 1);
                int pfx_w = rdraw_serif_text(&rd, entry_x, ty, num_prefix, strlen(num_prefix), true, false);
                (void)rdraw_serif_text(&rd, entry_x + pfx_w, ty, entry->title, strlen(entry->title), false, false);

                if (r->link_count < READER_LINKS_MAX) {
                    reader_link_t *lnk = &r->links[r->link_count++];
                    lnk->x0 = (int16_t)margin_x;
                    lnk->y0 = (int16_t)ty;
                    lnk->x1 = (int16_t)(margin_x + max_w);
                    lnk->y1 = (int16_t)(ty + READER_LINE_H);
                    lnk->num = i + 1;
                    char slug[64];
                    md_heading_slug(entry->title, slug, sizeof(slug));
                    ksnprintf(lnk->url, sizeof(lnk->url), "#%s", slug);
                }
                ty += READER_LINE_H + 4 * sc;
                if (ty >= h - 16 * sc) break;
            }
        }
        draw_reader_action_bar(r, &rd, w, h);
        s_reader_seq[rid]++;
        return;
    }

    if (strip) {
        r->link_count = links_before;          /* those found were in strip coordinates */
        s_reader_seq[rid]++;
        return;
    }
    draw_reader_scrollbar(r, &rd, w, h);
    draw_reader_action_bar(r, &rd, w, h);
    r->drawn_scroll_y = r->scroll_y;
    s_reader_seq[rid]++;
}

void reader_draw(uint8_t rid, const canvas1_t *c, int w, int h) {
    reader_draw_view(rid, c, w, h, 0, h);
}

/* An animation step from where the surfaces show the document to scroll_y:
 * the content below the action bar moved in both surfaces, only the rows
 * that came into view laid out and drawn, and the scrollbar. False when that
 * cannot be done (no checkpoints, the contents view, a step as tall as the
 * view): the caller draws it whole. */
static bool reader_scroll_step(uint8_t rid, const canvas1_t *c, int w, int h) {
    reader_state_t *r = &s_readers[rid];
    int bar_h = reader_bar_h();
    int dy = r->scroll_y - r->drawn_scroll_y;
    int area = h - bar_h;
    /* The 1-bpp grey pattern alternates by row (canvas1.c: x + y odd), so a
     * move by an odd number of rows would put moved content out of phase
     * with freshly drawn content: an odd step goes one row less (the next
     * step or the full draw after the animation catches up). */
    if (r->drawn_scroll_y >= 0 && (dy & 1)) {
        r->scroll_y -= dy > 0 ? 1 : -1;
        dy = r->scroll_y - r->drawn_scroll_y;
    }
    if (dy == 0) return true;
    if (r->drawn_scroll_y < 0 || !r->cp_valid || r->cp_w != w || r->toc_mode || !c ||
        dy >= area || -dy >= area || (r->aa_buf && (w > r->aa_w || h > r->aa_h)))
        return false;
    /* Rows y <- y + dy, front to back for dy > 0 and back to front otherwise. */
    for (int k = 0; k < area - (dy > 0 ? dy : -dy); k++) {
        int y = dy > 0 ? bar_h + k : h - 1 - k;
        if (r->aa_buf)
            memcpy(r->aa_buf + (uint32_t)y * r->aa_stride, r->aa_buf + (uint32_t)(y + dy) * r->aa_stride, (size_t)w);
        memcpy(c->fb + (uint32_t)y * c->stride, c->fb + (uint32_t)(y + dy) * c->stride, c->stride);
    }
    r->strip_failed = false;
    /* A strip starts on an even row, for the same pattern (bar_h is even). */
    if (dy > 0) reader_draw_view(rid, c, w, h, (h - dy) & ~1, h);
    else reader_draw_view(rid, c, w, h, bar_h, bar_h - dy);
    if (r->strip_failed) return false;
    ugui_surface_t rd = { .c1 = c, .aa_fb = r->aa_buf, .aa_stride = r->aa_stride, .w = w, .h = h, .scale = (int)CHROME_SCALE };
    draw_reader_scrollbar(r, &rd, w, h);
    r->drawn_scroll_y = r->scroll_y;
    return true;
}

uint32_t reader_get_seq(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return 0;
    return s_reader_seq[rid];
}
