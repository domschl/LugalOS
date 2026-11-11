/* drivers/uxbench.c -- `uxbench`: what the ribbon's interactions cost on the
 * glass, through the real paths (the screen's canvas protocol, the reader and
 * launcher calls the input pump makes), each one waited for until the panel
 * shows it (lcdterm_settle()).
 *
 * Per scenario it prints the wall time of an action (input to glass, median
 * and p95), the drawing passes of the launcher and the Settings app with
 * their stages (uxbench_view_note()) and, where the backend times its frames
 * (lcdterm_frame_stats(), the P4), the converted frames' stages. "app" is
 * the wall time not spent in frames: the 1-bpp screen drawing, the views'
 * drawing, the reader's layout, the waits.
 *
 * Runs in the calling shell task; its reader and launcher calls on the views
 * task, as the input path's. Opens a reader and the launcher when they
 * are not open, and closes what it opened; the focus ends on terminal 0. */

#include "drivers/lcdterm.h"
#include "drivers/launcher_view.h"
#include "drivers/reader_view.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "drivers/settings_view.h"
#include "drivers/uxbench.h"
#include "drivers/view.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include "kernel/time.h"
#include "kernel/vterm.h"
#include "fs/vfs.h"
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
#define UXB_MAX_NOTES   256u

/* One view pass, as uxbench_view_note() took it. */
typedef struct {
    uint32_t wait_us, draw_us, copy_us;
} uxb_note_t;

/* The views' passes while the bench runs: notes is NULL otherwise. In
 * PSRAM: only a measurement uses it. */
/* single task: views -- the bench sets and clears it through view_call() and
 * reads it after view_call() or view_sync() has waited for the views. */
static struct {
    uxb_note_t *notes;
    unsigned    n;
} g_vn BULK_BSS;

void uxbench_view_note(uint64_t t_wait, uint64_t t_draw, uint64_t t_copy, uint64_t t_end) {
    if (!g_vn.notes || g_vn.n >= UXB_MAX_NOTES) return;
    uxb_note_t *nt = &g_vn.notes[g_vn.n++];
    nt->wait_us = (uint32_t)(t_draw - t_wait);
    nt->draw_us = (uint32_t)(t_copy - t_draw);
    nt->copy_us = (uint32_t)(t_end - t_copy);
}

typedef struct {
    int     count;              /* ribbon windows */
    int     term0_idx;          /* window of terminal 0, or -1 */
    int     reader_idx, reader_rid, reader_h;
    int     launcher_idx;
    int     settings_idx;
} uxb_ribbon_t;

static void uxb_scan(uxb_ribbon_t *rb) {
    memset(rb, 0, sizeof *rb);
    rb->term0_idx = rb->reader_idx = rb->reader_rid = rb->launcher_idx = rb->settings_idx = -1;
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
        if (reply[1] == RIBBON_WIN_SETTINGS && rb->settings_idx < 0) rb->settings_idx = i;
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
    int        *x, *y;
    char       *dir;              /* 'F': in, then the directory shown before */
    unsigned    dir_cap;
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
    case 'd': launcher_drag(c->dy); break;
    case 'F': {                   /* face rid, in dir (NULL: as it is); result: the face before, -1 */
        int was = 0;
        char tmp[VFS_PATH_MAX];
        bool ok = launcher_bench_show(c->rid, c->dir, &was, tmp, sizeof tmp);
        c->result = ok ? was : -1;
        if (ok && c->dir) {
            strncpy(c->dir, tmp, c->dir_cap - 1);
            c->dir[c->dir_cap - 1] = '\0';
        }
        c->dy = launcher_bench_files();
        break;
    }
    case 'S': {                   /* the stepper's button (dy: 1 the +), slot in rid */
        uint8_t slot = 0;
        c->result = settings_view_bench_stepper(c->dy > 0, &slot, c->x, c->y);
        c->rid = slot;
        break;
    }
    case 'N':                     /* the views' notes: on (doc: the ring) or off */
        g_vn.notes = (uxb_note_t *)(uintptr_t)c->doc;
        g_vn.n = 0;
        break;
    case 'Z': g_vn.n = 0; break;  /* a scenario's first */
    default: break;
    }
}

static int uxb_view(char op, int rid, int dy, const char *doc) {
    uxb_vcall_t c = { op, rid, dy, doc, 0, NULL, NULL, NULL, 0 };
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

typedef enum { UXB_PAN, UXB_SCROLL, UXB_PAGE, UXB_DEEP, UXB_REDRAW, UXB_LAUNCHER, UXB_FSCROLL, UXB_STEPPER } uxb_kind_t;

typedef struct {
    uxb_ribbon_t rb;
    lcdterm_frame_stat_t *frames;    /* UXB_MAX_FRAMES, in PSRAM pages */
    uint32_t col[UXB_MAX_FRAMES];
    int      fscroll_dy;             /* the Files face's drag step, px */
    uint8_t  st_slot;                /* the Settings window's view instance */
    int      st_x[2], st_y[2];       /* its stepper's - and + */
} uxb_t;

/* A tap on the Settings app as the console delivers one (kernel/console.c):
 * the press, its release, the tap. */
static void uxb_settings_tap(const uxb_t *u, int x, int y) {
    static const uint8_t types[3] = { WIN_EV_POINTER_DOWN, WIN_EV_POINTER_UP, WIN_EV_POINTER_UP };
    static const uint8_t flags[3] = { WIN_EVF_PRESS, WIN_EVF_PRESS, WIN_EVF_TAP };
    for (unsigned k = 0; k < 3u; k++) {
        win_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = types[k];
        ev.flags = flags[k];
        ev.button = 1;
        ev.x = (int16_t)(k == 1u ? 0 : x);
        ev.y = (int16_t)(k == 1u ? 0 : y);
        view_post(&g_settings_view, u->st_slot, &ev);
    }
    view_sync();
}

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
    case UXB_FSCROLL:                        /* a finger up the list, then back down */
        (void)uxb_view('d', 0, i < n / 2u ? -u->fscroll_dy : u->fscroll_dy, NULL);
        return true;
    case UXB_STEPPER: {                      /* +, -, +, ...: the speed ends where it began */
        unsigned b = i % 2u == 0 ? 1u : 0u;
        uxb_settings_tap(u, u->st_x[b], u->st_y[b]);
        return true;
    }
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

/* The views' passes in the last scenario, per action: how many, and the
 * median and p95 of each stage summed over an action's passes. */
static void uxb_view_report(uxb_t *u, const unsigned *mark, unsigned done) {
    unsigned total = mark[done];
    if (!g_vn.notes || total == 0) return;
    uint32_t med[4], p95[4];
    for (unsigned s = 0; s < 4u; s++) {
        for (unsigned a = 0; a < done; a++) {
            uint32_t v = 0;
            for (unsigned k = mark[a]; k < mark[a + 1u]; k++) {
                const uxb_note_t *nt = &g_vn.notes[k];
                v += s == 0 ? 1u : s == 1 ? nt->wait_us : s == 2 ? nt->draw_us : nt->copy_us;
            }
            u->col[a] = v;
        }
        uxb_mp(u->col, done, &med[s], &p95[s]);
    }
    cprintf("                  view: %u passes (%lu per action, at most %lu%s); us per action median/p95: "
            "draw %lu/%lu copy %lu/%lu wait %lu/%lu\n",
            total, (unsigned long)med[0], (unsigned long)p95[0], total >= UXB_MAX_NOTES ? ", record full" : "",
            (unsigned long)med[2], (unsigned long)p95[2], (unsigned long)med[3], (unsigned long)p95[3],
            (unsigned long)med[1], (unsigned long)p95[1]);
}

static void uxb_run(uxb_t *u, uxb_kind_t kind, const char *name, unsigned n) {
    uint32_t wall[UXB_MAX_ACTIONS];
    unsigned mark[UXB_MAX_ACTIONS + 1u];
    unsigned done = 0;
    if (n > UXB_MAX_ACTIONS) n = UXB_MAX_ACTIONS;
    lcdterm_settle();
    (void)uxb_view('Z', 0, 0, NULL);
    (void)lcdterm_frame_stats(u->frames, UXB_MAX_FRAMES, true);
    mark[0] = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t t0 = time_get_us();
        if (!uxb_act(u, kind, i, n)) break;
        lcdterm_settle();
        wall[done++] = (uint32_t)(time_get_us() - t0);
        view_sync();
        mark[done] = g_vn.n;
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
    uxb_view_report(u, mark, done);
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
    /* doc, then the Files face's directory */
    char doc[96], dir[VFS_PATH_MAX];
    unsigned k = 0;
    while (arg && *arg && *arg != ' ' && k < sizeof doc - 1u) doc[k++] = *arg++;
    doc[k] = '\0';
    while (arg && *arg == ' ') arg++;
    k = 0;
    while (arg && *arg && *arg != ' ' && k < sizeof dir - 1u) dir[k++] = *arg++;
    dir[k] = '\0';
    if (!doc[0]) strcpy(doc, "shell.md");
    if (!dir[0]) strcpy(dir, "/sd0");

    const uint32_t pages = (UXB_MAX_FRAMES * sizeof(lcdterm_frame_stat_t) + PAGE_SIZE - 1u) / PAGE_SIZE;
    lcdterm_frame_stat_t *frames = (lcdterm_frame_stat_t *)palloc_pages_bulk(pages);
    if (!frames) {
        cprintf("uxbench: no memory\n");
        return;
    }
    u.frames = frames;
    const uint32_t npages = (UXB_MAX_NOTES * sizeof(uxb_note_t) + PAGE_SIZE - 1u) / PAGE_SIZE;
    uxb_note_t *notes = (uxb_note_t *)palloc_pages_bulk(npages);
    if (!notes) {
        cprintf("uxbench: no memory\n");
        palloc_free(frames, pages);
        return;
    }

    uxb_scan(&u.rb);
    if (u.rb.count == 0) {
        cprintf("uxbench: no ribbon on this console\n");
        palloc_free(notes, npages);
        palloc_free(frames, pages);
        return;
    }
    (void)uxb_view('N', 0, 0, (const char *)notes);
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
        uxb_vcall_t c = { 'F', 0, 0, NULL, 0, NULL, NULL, NULL, 0 };
        view_call(uxb_view_op, &c);          /* Shortcuts, the directory as it is */
        int was_face = c.result;
        uxb_run(&u, UXB_LAUNCHER, "launcher", 8);
        char was_dir[VFS_PATH_MAX];
        strcpy(was_dir, dir);
        c = (uxb_vcall_t){ 'F', 1, 0, NULL, 0, NULL, NULL, was_dir, sizeof was_dir };
        view_call(uxb_view_op, &c);
        if (c.result < 0) {
            cprintf("uxbench: '%s' did not open; files and fscroll skipped\n", dir);
        } else {
            cprintf("uxbench: Files in %s, %d entries\n", dir, c.dy);
            uxb_run(&u, UXB_LAUNCHER, "files", 8);
            u.fscroll_dy = 24 * (int)CHROME_SCALE;
            uxb_run(&u, UXB_FSCROLL, "fscroll", 24);
            c = (uxb_vcall_t){ 'F', 1, 0, NULL, 0, NULL, NULL, was_dir, sizeof was_dir };
            view_call(uxb_view_op, &c);      /* the directory shown before */
        }
        c = (uxb_vcall_t){ 'F', was_face, 0, NULL, 0, NULL, NULL, NULL, 0 };
        view_call(uxb_view_op, &c);
        (void)uxb_view('l', 0, 0, NULL);
    }

    /* The Settings app's pointer-speed stepper: open at Input for it. */
    bool opened_settings = u.rb.settings_idx < 0;
    if (settings_view_open_at(SETTINGS_SECTION_INPUT)) {
        uxb_scan(&u.rb);
        bool ok = true;
        for (int b = 0; b < 2 && ok; b++) {
            uxb_vcall_t c = { 'S', 0, b, NULL, 0, &u.st_x[b], &u.st_y[b], NULL, 0 };
            view_call(uxb_view_op, &c);
            ok = c.result != 0;
            u.st_slot = (uint8_t)c.rid;
        }
        if (ok && uxb_jump(u.rb.settings_idx)) uxb_run(&u, UXB_STEPPER, "stepper", 8);
        else cprintf("uxbench: the Settings app's stepper is not shown; stepper skipped\n");
    } else {
        opened_settings = false;
        cprintf("uxbench: the Settings app did not open; stepper skipped\n");
    }
    (void)uxb_view('N', 0, 0, NULL);

    if (opened_reader) {
        uxb_scan(&u.rb);
        int rid = u.rb.reader_rid;
        uxb_close_window(u.rb.reader_idx);
        if (rid >= 0) (void)uxb_view('x', rid, 0, NULL);
    }
    if (opened_settings) {
        uxb_scan(&u.rb);
        uxb_close_window(u.rb.settings_idx);
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
    palloc_free(notes, npages);
    palloc_free(frames, pages);
}

#else

void uxbench_view_note(uint64_t t_wait, uint64_t t_draw, uint64_t t_copy, uint64_t t_end) {
    (void)t_wait; (void)t_draw; (void)t_copy; (void)t_end;
}

void uxbench_cmd(const char *arg) {
    (void)arg;
    cprintf("uxbench: no panel on this board\n");
}

#endif
