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
 * Runs in the calling shell task. Opens a reader and the launcher when they
 * are not open, and closes what it opened; the focus ends on terminal 0. */

#include "drivers/lcdterm.h"
#include "drivers/launcher_view.h"
#include "drivers/reader_view.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
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

static bool uxb_open_reader(const char *doc) {
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

typedef enum { UXB_PAN, UXB_SCROLL, UXB_PAGE, UXB_REDRAW, UXB_LAUNCHER } uxb_kind_t;

typedef struct {
    uxb_ribbon_t rb;
    lcdterm_frame_stat_t *frames;    /* UXB_MAX_FRAMES, one PSRAM page */
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
        reader_scroll_instance((uint8_t)u->rb.reader_rid, i < n / 2u ? 32 : -32);
        return true;
    case UXB_PAGE: {
        int page = u->rb.reader_h - 40;
        if (page < 40) page = 40;
        reader_scroll_instance((uint8_t)u->rb.reader_rid, i < n / 2u ? page : -page);
        return true;
    }
    case UXB_REDRAW:
        reader_update_instance((uint8_t)u->rb.reader_rid);
        return true;
    case UXB_LAUNCHER:
        launcher_update();
        return true;
    }
    return false;
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
    if (!nf) {
        cprintf("                  (this panel does not time its frames)\n");
        return;
    }
    /* Per frame: the median of each stage on its own. */
    uint32_t med[7], p95[7];
    for (unsigned s = 0; s < 7u; s++) {
        for (unsigned f = 0; f < nf; f++) {
            const lcdterm_frame_stat_t *fs = &u->frames[f];
            u->col[f] = s == 0 ? fs->total_us : s == 1 ? fs->snap_us : s == 2 ? fs->conv_us :
                        s == 3 ? fs->reader_us : s == 4 ? fs->canvas_us : s == 5 ? fs->term_us : fs->wb_us;
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
            "reader %lu/%lu canvas %lu/%lu term %lu/%lu wb %lu/%lu\n",
            (unsigned long)med[0], (unsigned long)p95[0], (unsigned long)med[1], (unsigned long)p95[1],
            (unsigned long)med[2], (unsigned long)p95[2], (unsigned long)med[3], (unsigned long)p95[3],
            (unsigned long)med[4], (unsigned long)p95[4], (unsigned long)med[5], (unsigned long)p95[5],
            (unsigned long)med[6], (unsigned long)p95[6]);
}

/* single task: the shell running `uxbench`. 0.5 KB, so in PSRAM, not on its
 * stack or in SRAM. */
static uxb_t u BULK_BSS;

void uxbench_cmd(const char *arg) {
    while (arg && *arg == ' ') arg++;
    const char *doc = (arg && *arg) ? arg : "shell.md";

    lcdterm_frame_stat_t *frames = (lcdterm_frame_stat_t *)palloc_pages_bulk(1);
    if (!frames || PAGE_SIZE < UXB_MAX_FRAMES * sizeof *frames) {
        if (frames) palloc_free(frames, 1);
        cprintf("uxbench: no memory\n");
        return;
    }
    u.frames = frames;

    uxb_scan(&u.rb);
    if (u.rb.count == 0) {
        cprintf("uxbench: no ribbon on this console\n");
        palloc_free(frames, 1);
        return;
    }
    bool opened_reader = false, opened_launcher = false;
    if (u.rb.reader_idx < 0) opened_reader = uxb_open_reader(doc);
    if (u.rb.launcher_idx < 0) opened_launcher = launcher_open();
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
        if (rid >= 0) reader_close_instance((uint8_t)rid);
    }
    if (opened_launcher) {
        uxb_scan(&u.rb);
        uxb_close_window(u.rb.launcher_idx);
        launcher_close();
    }
    uxb_scan(&u.rb);
    if (uxb_jump(u.rb.term0_idx)) {
        console_set_active_win_type(RIBBON_WIN_TERM);
        vterm_set_active(0);
    }
    console_sync_readers();
    palloc_free(frames, 1);
}

#else

void uxbench_cmd(const char *arg) {
    (void)arg;
    cprintf("uxbench: no panel on this board\n");
}

#endif
