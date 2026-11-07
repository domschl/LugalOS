/* drivers/uxbench.c -- `uxbench`: what the ribbon's interactions cost on the
 * glass, through the real paths (the screen's canvas protocol, the reader and
 * launcher calls the input pump makes), each one waited for until the panel
 * shows it (lcdterm_settle()).
 *
 * Per scenario it prints the wall time of an action (input to glass, median
 * and p95) and, where the backend times its frames (lcdterm_frame_stats(), the
 * P4), the converted frames' stages. "app" is the wall time not spent in
 * frames: the 1-bpp screen drawing, the reader's layout, the waits.
 *
 * Runs in the calling shell task; its reader and launcher calls on the views
 * task, as the input path's. Opens a reader and the launcher when they
 * are not open, and closes what it opened; the focus ends on terminal 0. */

#include "drivers/lcdterm.h"
#include "drivers/launcher_view.h"
#include "drivers/reader_view.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "drivers/view.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include "kernel/time.h"
#include "kernel/vterm.h"
#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_BOARD_ESP32P4)
#include "drivers/dsi_esp32p4.h"
#endif

#if defined(LCDTERM_PRESENT)

#define UXB_MAX_ACTIONS 24u
#define UXB_MAX_FRAMES  128u

typedef struct {
    int     count;              /* ribbon windows */
    int     term0_idx;          /* window of terminal 0, or -1 */
    int     reader_idx, reader_rid, reader_h;
    int     launcher_idx;
} uxb_ribbon_t;

static void uxb_scan(uxb_ribbon_t *rb) {
    memset(rb, 0, sizeof *rb);
    rb->term0_idx = rb->reader_idx = rb->reader_rid = rb->launcher_idx = -1;
    uint8_t req[2] = { 'R', 0 }, reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 1, reply) || reply[0] != 0) return;
    rb->count = reply[1];
    for (int i = 0; i < rb->count; i++) {
        req[1] = (uint8_t)i;
        if (!console_canvas(req, 2, reply) || reply[0] != 0) continue;
        if (reply[1] == RIBBON_WIN_TERM && reply[2] == 0 && rb->term0_idx < 0) rb->term0_idx = i;
        if (reply[1] == RIBBON_WIN_READER && rb->reader_idx < 0) {
            rb->reader_idx = i;
            rb->reader_rid = reply[2];
            rb->reader_h = (int)(reply[10] | (reply[11] << 8));
        }
        if (reply[1] == RIBBON_WIN_LAUNCHER && rb->launcher_idx < 0) rb->launcher_idx = i;
    }
}

/* What run_hotkey() does with a focus reply, so the console follows. */
static void uxb_follow(const uint8_t *reply) {
    if (reply[10] != 0) console_set_active_win_type(reply[10]);
    if (reply[10] == RIBBON_WIN_READER) reader_set_active_instance(reply[11]);
    vterm_set_active(reply[1]);
}

static bool uxb_jump(int idx) {
    if (idx < 0) return false;
    uint8_t req[2] = { 'J', (uint8_t)idx }, reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 2, reply) || reply[0] != 0) return false;
    uxb_follow(reply);
    return true;
}

static bool uxb_open_reader_here(const char *doc) {
    uint8_t req[128];
    uint32_t len = (uint32_t)strlen(doc);
    if (len > sizeof req - 3) len = sizeof req - 3;
    req[0] = 'O';
    req[1] = 0;
    memcpy(req + 2, doc, len);
    req[2 + len] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 3 + len, reply) || reply[0] != 0) return false;
    uint8_t rid = reply[1];
    int w = (int)(reply[2] | (reply[3] << 8));
    int h = (int)(reply[4] | (reply[5] << 8));
    if (reader_open_instance(rid, doc) < 0) return false;
    reader_set_target(rid, w, h);
    reader_update_instance(rid);
    reader_set_active_instance(rid);
    console_set_active_win_type(RIBBON_WIN_READER);
    return true;
}

/* The bench's calls into the views, made on the views task as the input
 * path's are (drivers/view.h), and waited for. */
typedef struct {
    char        op;
    int         rid, dy;
    const char *doc;
    int         result;
} uxb_vcall_t;

static void uxb_view_op(void *arg) {
    uxb_vcall_t *c = (uxb_vcall_t *)arg;
    switch (c->op) {
    case 's': reader_scroll_instance((uint8_t)c->rid, c->dy); break;
    case 'u': reader_update_instance((uint8_t)c->rid); break;
    case 'x': reader_close_instance((uint8_t)c->rid); break;
    case 'k': c->result = reader_layout_check((uint8_t)c->rid); break;
    case 't': c->result = reader_strip_check((uint8_t)c->rid, c->dy); break;
    case 'O': c->result = uxb_open_reader_here(c->doc); break;
    case 'l': launcher_update(); break;
    case 'L': c->result = launcher_open(); break;
    case 'C': launcher_close(); break;
    default: break;
    }
}

static int uxb_view(char op, int rid, int dy, const char *doc) {
    uxb_vcall_t c = { op, rid, dy, doc, 0 };
    view_call(uxb_view_op, &c);
    return c.result;
}

static void uxb_close_window(int idx) {
    if (!uxb_jump(idx)) return;
    uint8_t req[1] = { 'C' }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 1, reply);
}

static void uxb_sort(uint32_t *v, unsigned n) {
    for (unsigned i = 1; i < n; i++) {
        uint32_t x = v[i];
        unsigned j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
}

/* Median and 95th percentile (nearest rank) of n values; sorts them. */
static void uxb_mp(uint32_t *v, unsigned n, uint32_t *med, uint32_t *p95) {
    *med = *p95 = 0;
    if (!n) return;
    uxb_sort(v, n);
    *med = v[n / 2u];
    unsigned k = (95u * n + 99u) / 100u;
    *p95 = v[k ? k - 1u : 0u];
}

typedef enum { UXB_PAN, UXB_SCROLL, UXB_PAGE, UXB_DEEP, UXB_REDRAW, UXB_LAUNCHER } uxb_kind_t;

typedef struct {
    uxb_ribbon_t rb;
    lcdterm_frame_stat_t *frames;    /* UXB_MAX_FRAMES, in PSRAM pages */
    uint32_t col[UXB_MAX_FRAMES];
} uxb_t;

/* One action of a scenario; i counts from 0. false: nothing to do. */
static bool uxb_act(uxb_t *u, uxb_kind_t kind, unsigned i, unsigned n) {
    switch (kind) {
    case UXB_PAN: {
        /* to the last window and back: a pan wherever the pair does not fit */
        unsigned span = (unsigned)(u->rb.count - 1);
        uint8_t op = (span && (i / span) % 2u == 0) ? '>' : '<';
        uint8_t req[1] = { op }, reply[SCREEN_REPLY_LEN];
        if (!console_canvas(req, 1, reply) || reply[0] != 0) return false;
        uxb_follow(reply);
        return true;
    }
    case UXB_SCROLL:
        (void)uxb_view('s', u->rb.reader_rid, i < n / 2u ? 32 : -32, NULL);
        return true;
    case UXB_PAGE: {
        int page = u->rb.reader_h - 40;
        if (page < 40) page = 40;
        (void)uxb_view('s', u->rb.reader_rid, i < n / 2u ? page : -page, NULL);
        return true;
    }
    case UXB_DEEP: {                         /* pages down only, into the document */
        int page = u->rb.reader_h - 40;
        if (page < 40) page = 40;
        (void)uxb_view('s', u->rb.reader_rid, page, NULL);
        return true;
    }
    case UXB_REDRAW:
        (void)uxb_view('u', u->rb.reader_rid, 0, NULL);
        return true;
    case UXB_LAUNCHER:
        (void)uxb_view('l', 0, 0, NULL);
        return true;
    }
    return false;
}

static void uxb_layout_check(const uxb_t *u) {
    if (u->rb.reader_rid < 0) return;
    int c = uxb_view('k', u->rb.reader_rid, 0, NULL);
    if (c == 0) cprintf("                  LAYOUT CHECK FAILED: the view from checkpoints differs from a full layout\n");
    else if (c == 1) cprintf("                  layout check: checkpoints match a full layout\n");
}

static void uxb_strip_check(const uxb_t *u) {
    if (u->rb.reader_rid < 0) return;
    static const int steps[] = { 32, 7, -32, -5, 101, -101 };
    int fails = 0, fallbacks = 0;
    for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
        int c = uxb_view('t', u->rb.reader_rid, steps[k], NULL);
        if (c == 0) {
            int y0, y1;
            reader_strip_diff((uint8_t)u->rb.reader_rid, &y0, &y1);
            fails++;
            cprintf("                  STRIP CHECK FAILED: a step of %d px differs from a whole draw (GRAY8 rows %d..%d)\n",
                    steps[k], y0, y1);
        }
        if (c == 2) fallbacks++;
    }
    if (!fails) cprintf("                  strip check: steps match whole draws (%d of %u fell back)\n",
                        fallbacks, (unsigned)(sizeof steps / sizeof steps[0]));
}

static void uxb_run(uxb_t *u, uxb_kind_t kind, const char *name, unsigned n) {
    uint32_t wall[UXB_MAX_ACTIONS];
    unsigned done = 0;
    if (n > UXB_MAX_ACTIONS) n = UXB_MAX_ACTIONS;
    lcdterm_settle();
    (void)lcdterm_frame_stats(u->frames, UXB_MAX_FRAMES, true);
    for (unsigned i = 0; i < n; i++) {
        uint64_t t0 = time_get_us();
        if (!uxb_act(u, kind, i, n)) break;
        lcdterm_settle();
        wall[done++] = (uint32_t)(time_get_us() - t0);
    }
    if (!done) {
        cprintf("uxbench %-8s: nothing to do\n", name);
        return;
    }
    uint64_t wall_sum = 0;
    for (unsigned i = 0; i < done; i++) wall_sum += wall[i];
    uint32_t wmed, wp95;
    uxb_mp(wall, done, &wmed, &wp95);

    unsigned nf = lcdterm_frame_stats(u->frames, UXB_MAX_FRAMES, true);
    unsigned forced = 0, sync = 0;
    uint64_t frame_sum = 0;
    for (unsigned f = 0; f < nf; f++) {
        forced += u->frames[f].forced;
        sync += u->frames[f].sync;
        frame_sum += u->frames[f].total_us;
    }
    cprintf("uxbench %-8s: %u actions, input to glass median %lu.%lu ms, p95 %lu.%lu ms\n",
            name, done, (unsigned long)(wmed / 1000u), (unsigned long)(wmed % 1000u / 100u),
            (unsigned long)(wp95 / 1000u), (unsigned long)(wp95 % 1000u / 100u));
    uint8_t vreq[1] = { 'E' }, vreply[SCREEN_REPLY_LEN];
    if (console_canvas(vreq, 1, vreply) && vreply[0] == 0) {
        if (vreply[1]) {
            cprintf("                  SCREEN CHECK FAILED: the screen differs from a full repaint, rows %u..%u",
                    (unsigned)vreply[12] * 16u, (unsigned)vreply[13] * 16u + 15u);
            if (vreply[11] != 0xff) cprintf(", band %u changed during the check itself", (unsigned)vreply[11]);
            if (vreply[14] != 0xff)
                cprintf(", top 32 rows x %u..%u", (unsigned)vreply[14] * 32u, (unsigned)vreply[15] * 32u + 31u);
            cprintf("\n");
        }
        else cprintf("                  screen check: matches a full repaint\n");
    }
    unsigned cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    long diff = lcdterm_check(&cx0, &cy0, &cx1, &cy1);
    if (diff > 0) {
        cprintf("                  GLASS CHECK FAILED: %ld pixels differ from a full redraw, frame x %u..%u y %u..%u\n",
                diff, cx0, cx1, cy0, cy1);
    } else if (diff == 0) {
        cprintf("                  glass check: matches a full redraw\n");
    }
    if (!nf) {
        cprintf("                  (this panel does not time its frames)\n");
        return;
    }
    /* Per frame: the median of each stage on its own. */
    uint32_t med[10], p95[10];
    for (unsigned s = 0; s < 10u; s++) {
        for (unsigned f = 0; f < nf; f++) {
            const lcdterm_frame_stat_t *fs = &u->frames[f];
            u->col[f] = s == 0 ? fs->total_us : s == 1 ? fs->snap_us : s == 2 ? fs->conv_us :
                        s == 3 ? fs->reader_us : s == 4 ? fs->canvas_us : s == 5 ? fs->term_us : s == 6 ? fs->wb_us : s == 7 ? fs->mirror_us : s == 8 ? fs->ppa_call_us : fs->ppa_us;
        }
        uxb_mp(u->col, nf, &med[s], &p95[s]);
    }
    uint64_t app = wall_sum > frame_sum ? wall_sum - frame_sum : 0;
    cprintf("                  %u frames%s (%u forced, %u sync), %lu.%lu per action; "
            "frames %lu ms, app %lu ms per action\n",
            nf, nf == UXB_MAX_FRAMES ? " (ring full: the newest)" : "", forced, sync,
            (unsigned long)(nf * 10u / done / 10u), (unsigned long)(nf * 10u / done % 10u),
            (unsigned long)(frame_sum / done / 1000u), (unsigned long)(app / done / 1000u));
    cprintf("                  frame us median/p95: total %lu/%lu snap %lu/%lu conv %lu/%lu "
            "reader %lu/%lu (mirror %lu/%lu ppa %lu/%lu dma %lu/%lu) canvas %lu/%lu term %lu/%lu wb %lu/%lu\n",
            (unsigned long)med[0], (unsigned long)p95[0], (unsigned long)med[1], (unsigned long)p95[1],
            (unsigned long)med[2], (unsigned long)p95[2], (unsigned long)med[3], (unsigned long)p95[3],
            (unsigned long)med[7], (unsigned long)p95[7], (unsigned long)med[8], (unsigned long)p95[8],
            (unsigned long)med[9], (unsigned long)p95[9],
            (unsigned long)med[4], (unsigned long)p95[4], (unsigned long)med[5], (unsigned long)p95[5],
            (unsigned long)med[6], (unsigned long)p95[6]);
}

/* single task: the shell running `uxbench`. 0.5 KB, so in PSRAM, not on its
 * stack or in SRAM. */
static uxb_t u BULK_BSS;

void uxbench_cmd(const char *arg) {
    while (arg && *arg == ' ') arg++;
    const char *doc = (arg && *arg) ? arg : "shell.md";

    const uint32_t pages = (UXB_MAX_FRAMES * sizeof(lcdterm_frame_stat_t) + PAGE_SIZE - 1u) / PAGE_SIZE;
    lcdterm_frame_stat_t *frames = (lcdterm_frame_stat_t *)palloc_pages_bulk(pages);
    if (!frames) {
        cprintf("uxbench: no memory\n");
        return;
    }
    u.frames = frames;

    uxb_scan(&u.rb);
    if (u.rb.count == 0) {
        cprintf("uxbench: no ribbon on this console\n");
        palloc_free(frames, pages);
        return;
    }
    bool opened_reader = false, opened_launcher = false;
    if (u.rb.reader_idx < 0) opened_reader = uxb_view('O', 0, 0, doc) != 0;
    if (u.rb.launcher_idx < 0) opened_launcher = uxb_view('L', 0, 0, NULL) != 0;
    uxb_scan(&u.rb);

#if defined(CONFIG_BOARD_ESP32P4)
    cprintf("uxbench: %d windows, rotation %d, %s\n", u.rb.count, dsi_lcd_get_orientation(),
            lcdterm_is_double_buffered() ? "double-buffered" : "single-buffered");
#else
    cprintf("uxbench: %d windows\n", u.rb.count);
#endif

    if (uxb_jump(u.rb.term0_idx >= 0 ? u.rb.term0_idx : 0)) {
        uxb_run(&u, UXB_PAN, "pan", 8);
    }
    if (u.rb.reader_idx >= 0 && uxb_jump(u.rb.reader_idx)) {
        uxb_run(&u, UXB_SCROLL, "scroll", 12);
        uxb_layout_check(&u);
        uxb_run(&u, UXB_DEEP, "deep", 6);
        uxb_layout_check(&u);
        uxb_strip_check(&u);
        uxb_run(&u, UXB_REDRAW, "deepdraw", 6);     /* a redraw deep in the document */
        uxb_run(&u, UXB_PAGE, "page", 8);
        uxb_run(&u, UXB_REDRAW, "redraw", 6);
    } else {
        cprintf("uxbench: no reader for '%s'; scroll, page and redraw skipped\n", doc);
    }
    if (u.rb.launcher_idx >= 0 && uxb_jump(u.rb.launcher_idx)) {
        uxb_run(&u, UXB_LAUNCHER, "launcher", 8);
    }

    if (opened_reader) {
        uxb_scan(&u.rb);
        int rid = u.rb.reader_rid;
        uxb_close_window(u.rb.reader_idx);
        if (rid >= 0) (void)uxb_view('x', rid, 0, NULL);
    }
    if (opened_launcher) {
        uxb_scan(&u.rb);
        uxb_close_window(u.rb.launcher_idx);
        (void)uxb_view('C', 0, 0, NULL);
    }
    uxb_scan(&u.rb);
    if (uxb_jump(u.rb.term0_idx)) {
        console_set_active_win_type(RIBBON_WIN_TERM);
        vterm_set_active(0);
    }
    console_sync_readers();
    palloc_free(frames, pages);
}

#else

void uxbench_cmd(const char *arg) {
    (void)arg;
    cprintf("uxbench: no panel on this board\n");
}

#endif
