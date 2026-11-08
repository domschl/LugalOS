#ifndef LUGALOS_DRIVERS_LCDTERM_H
#define LUGALOS_DRIVERS_LCDTERM_H

/* The panel terminal, portable:
 * the screen_t (drivers/screen.h) on a 1-bpp frame, run by the U-mode
 * `lcdterm` task, fed in batches by the console and the kernel log, with the
 * menu bar's title and status, the tee to a host, and the console's screen
 * hooks. Until 47.6 this lived inside drivers/lcd7_rp2350.c; the panel
 * drivers now only scan a frame out and describe it here.
 *
 * A panel driver brings its glass up, then hands lcdterm_init() a backend:
 * the 1-bpp frame (leftmost pixel in bit 0 of each byte, the order the
 * RP2350's PIO scans and font8x16 is stored in), its size, the naturally
 * aligned pieces of it the task may write (a PMP domain's regions), and
 * optionally a flush for frames that are not scanned out directly. */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "drivers/canvas1.h"
#include "kernel/console.h"
#include "lugalos_config.h"

#if (defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)) || \
    (defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))) || \
    defined(CONFIG_BOARD_VISIONFIVE2)
#define LCDTERM_PRESENT 1
#endif

/* Where the terminal can run as the U-mode `lcdterm` task: boards whose
 * linker script gives its code a region (.lcdtermtext, board_lcdterm_text_
 * region()). The RP2350 since 36.6a. Elsewhere -- the LCD-7B, 47.7 -- the
 * terminal draws from the kernel, the fallback it always had; a P4 text
 * region (32 KB of SRAM, or PMP over the XIP window) is a later step. */
#if defined(LCDTERM_PRESENT) && defined(CONFIG_BOARD_RP2350)
#define LCDTERM_UMODE 1
#endif

#define LCDTERM_MAX_FB_REGIONS 2u

/* One converted frame, as a converting backend times it (`uxbench`). The
 * stages are those of the P4's term_draw(); a backend without one leaves it 0. */
typedef struct {
    uint32_t total_us;     /* snapshot to flip armed */
    uint32_t snap_us;      /* dirty detection and the snapshot */
    uint32_t conv_us;      /* 1-bpp frame into the back buffer */
    uint32_t reader_us;    /* reader and launcher surfaces composited */
    uint32_t mirror_us;    /* of which: surfaces converted for the PPA (P4 RGB565 frame) */
    uint32_t ppa_us;       /* of which: the PPA's own DMA time (the rest of a PPA call is cache upkeep) */
    uint32_t ppa_call_us;  /* of which: PPA calls, cache upkeep included */
    uint32_t canvas_us;    /* RGB565 canvases composited (PPA or CPU) */
    uint32_t term_us;      /* anti-aliased terminal cells */
    uint32_t wb_us;        /* cache write-back of the written rows */
    uint16_t units;        /* bands, rows, surfaces and cells written */
    uint8_t  forced;       /* a full redraw (sync_post, rotation, `lcd repaint`) */
    uint8_t  sync;         /* converted by sync_post, not the flush task */
} lcdterm_frame_stat_t;

typedef struct {
    const char *name;               /* the panel, for reports: "RP2350-LCD-7" */
    const char *text_test_title;    /* `lcd test text`'s first line */
    uint8_t    *fb;                 /* 1-bpp, `stride` bytes per line, 1 = foreground */
    unsigned    w, h, stride;       /* pixels, pixels, bytes; w a multiple of 8 */
    struct { uintptr_t base; uint32_t size; } fb_regions[LCDTERM_MAX_FB_REGIONS];
    unsigned    fb_region_count;    /* the frame as the task's domain grants it */
    /* Rows [y0, y1) of the frame changed. NULL when the scan-out reads the
     * frame itself (RP2350); a converting backend (47.7) does its work here. */
    void      (*flush)(unsigned y0, unsigned y1);
    /* 47.11: the backlight off, or back at its level (idle blanking). */
    void      (*blank)(bool off);
    /* Display synchronization hooks: sync_pre before rendering intermediate
     * frame (e.g. wait for VBLANK to eliminate tearing on RP2350); sync_post
     * after rendering intermediate frame (e.g. convert & flush to PSRAM on P4). */
    void      (*sync_pre)(void);
    void      (*sync_post)(void);
    /* 55.3: capture scanout pixels with format (optional) */
    bool      (*capture)(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride, unsigned *format);
    /* 55.5: whether backend is using double-buffering (optional) */
    bool      (*is_double_buffered)(void);
    /* The panel turns at run time: lcdterm_reinit() with w and h traded.
     * The screen's state is then sized for both orientations. */
    bool        rotates;
    /* Optional, for `uxbench`: the newest frames' stage timings, oldest
     * first, at most max of them; reset empties the record afterwards. */
    unsigned  (*frame_stats)(lcdterm_frame_stat_t *out, unsigned max, bool reset);
    /* Optional: returns once everything drawn so far is on the glass
     * (converted and, double-buffered, flipped). */
    void      (*settle)(void);
    /* Optional: what was drawn incrementally against a full redraw of the
     * same state -- the pixels that differ (0: none), and their bounds in
     * frame coordinates; -1 when the backend cannot compare. */
    long      (*check)(unsigned *x0, unsigned *y0, unsigned *x1, unsigned *y1);
    /* Optional: the mouse pointer moved, or was shown or hidden
     * (console_pointer_shown()): the backend draws its cursor over the
     * frame at its next pass. */
    void      (*pointer)(void);
} lcdterm_backend_t;

/* Takes the backend (kept by pointer: it must outlive the terminal), finds
 * memory for the screen's state and canvas stores, and initialises the
 * screen on the frame. 0, or -1 if there is no memory for the state. */
int lcdterm_init(const lcdterm_backend_t *be);
int lcdterm_reinit(const lcdterm_backend_t *be);

/* The U-mode task. Until it runs, and if it never does, the terminal draws
 * from the kernel. */
int lcdterm_task_start(void);

/* The console stream and the kernel log's `lcd` sink. */
void lcdterm_screen_putc(char c);
void lcdterm_screen_putc_vterm(int vid, char c);
void lcdterm_console_putc(char c);           /* the screen, plus the tee */
void lcdterm_screen_flush(void);

/* Where the `lcd` console device copies its output for a host. */
#define LCD_TEE_UART 0u
#define LCD_TEE_USB  1u
#define LCD_TEE_OFF  2u
void lcdterm_set_tee(unsigned mode);
unsigned lcdterm_tee(void);

const console_screen_t *lcdterm_console_screen(void);
void lcdterm_set_title(const char *title);
void lcdterm_repaint(void);
uint32_t lcdterm_unknown_sequences(void);
uint32_t lcdterm_task_call_count(void);

/* `lcdtermisotest`, `lcd test text`, `lcd scroll N`, and the terminal half
 * of `lcd`. */
bool lcdterm_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);
int lcdterm_text_test(void);
void lcdterm_scroll_test(unsigned n);
void lcdterm_report(void);
canvas1_t *lcdterm_get_canvas_backing(uint8_t cid);
int        lcdterm_get_canvas_cid_by_name(const char *name);
/* Canvas slot cid's name into out (cap bytes); false when it is not in use. */
bool       lcdterm_canvas_name(uint8_t cid, char *out, unsigned cap);
canvas1_t *lcdterm_get_reader_backing(uint8_t rid);
#if defined(LCDTERM_PRESENT)
bool lcdterm_is_animating(void);
bool lcdterm_is_double_buffered(void);
void lcdterm_sync_pre(void);
void lcdterm_sync_post(void);
void lcdterm_damage(void);
unsigned lcdterm_frame_stats(lcdterm_frame_stat_t *out, unsigned max, bool reset);
void lcdterm_settle(void);
long lcdterm_check(unsigned *x0, unsigned *y0, unsigned *x1, unsigned *y1);
/* The visible views' windows (ribbon_is_view()): each one's tag -- its type
 * and instance, LCDTERM_VIEW_TAG() -- and its content rectangle. */
#define LCDTERM_VIEW_TAG(type, inst) ((uint8_t)(((unsigned)(type) << 4) | ((unsigned)(inst) & 0x0Fu)))
#define LCDTERM_VIEW_TYPE(tag)       ((uint8_t)((tag) >> 4))
#define LCDTERM_VIEW_INST(tag)       ((uint8_t)((tag) & 0x0Fu))
int lcdterm_get_visible_readers(uint8_t *out_rids, int *out_x0, int *out_y0, int *out_x1, int *out_y1, int max_wins);
uint16_t *lcdterm_get_canvas565_backing(uint8_t cid, uint32_t *out_stride, int *out_w, int *out_h);
uint16_t *lcdterm_get_canvas565_by_name(const char *name, uint32_t *out_stride, int *out_w, int *out_h);
int lcdterm_get_canvas565_cid_by_name(const char *name);
/* The focused window's canvas slot, or -1 when it is not a canvas. */
int  lcdterm_focused_canvas(void);
/* The focused window's type and id (a terminal's vterm, a canvas's slot, a
 * view's instance); false when the ribbon has none. */
bool lcdterm_focused_window(uint8_t *type, uint8_t *id);
/* Room on the ribbon: windows it can still take, canvas slots free. False
 * without a screen. */
bool lcdterm_room(int *windows, int *canvases);
/* The on-screen keyboard's rectangle when it is shown (`ts osk`). */
bool lcdterm_osk_rect(int *x0, int *y0, int *w, int *h);
/* The window whose content is under screen point (x, y): its type
 * (RIBBON_WIN_*), id and the point in its content; false when none (the
 * chrome, a gap, full-screen). For press feedback: no side effects. */
/* The mouse pointer changed (kernel/console.c): the panel draws its cursor anew. */
void lcdterm_pointer_changed(void);
bool lcdterm_window_at(int x, int y, uint8_t *type, uint8_t *id, int *cx, int *cy);
/* Where canvas cid's content begins on the screen; false when not shown. */
bool lcdterm_canvas_origin(uint8_t cid, int *x0, int *y0);
bool lcdterm_set_canvas565_backing(uint8_t cid, uint16_t *buf);
bool lcdterm_set_canvas565_geometry(uint8_t cid, uint16_t *buf, uint32_t stride, uint16_t w, uint16_t h);
int lcdterm_get_visible_canvases565(uint8_t *out_cids, int *out_x0, int *out_y0, int *out_x1, int *out_y1, int max_wins);

typedef struct {
    uint8_t   vterm_id;
    int       x0, y0;          /* pixel coordinates of top-left text cell */
    uint16_t  cols, rows;      /* visible text cols & rows */
    uint16_t  shadow_cols;     /* stride in cells of shadow */
    uint16_t  col_offset;      /* horizontal scroll offset into shadow */
    const uint16_t *shadow;    /* cell array */
    int       cur_col, cur_row;/* cursor position relative to window, or -1 if hidden */
} lcdterm_visible_term_t;

int lcdterm_get_visible_terms(lcdterm_visible_term_t *out_terms, int max_terms);
#else
#include "drivers/ramscreen.h"
static inline bool lcdterm_is_animating(void) { return false; }
static inline bool lcdterm_is_double_buffered(void) { return false; }
static inline void lcdterm_sync_pre(void) { }
static inline void lcdterm_sync_post(void) { }
static inline unsigned lcdterm_frame_stats(lcdterm_frame_stat_t *out, unsigned max, bool reset) {
    (void)out; (void)max; (void)reset; return 0;
}
static inline void lcdterm_settle(void) { }
static inline long lcdterm_check(unsigned *x0, unsigned *y0, unsigned *x1, unsigned *y1) {
    (void)x0; (void)y0; (void)x1; (void)y1; return -1;
}
static inline void lcdterm_damage(void) { }
static inline int lcdterm_get_visible_readers(uint8_t *out_rids, int *out_x0, int *out_y0, int *out_x1, int *out_y1, int max_wins) {
    (void)out_rids; (void)out_x0; (void)out_y0; (void)out_x1; (void)out_y1; (void)max_wins;
    return 0;
}
static inline uint16_t *lcdterm_get_canvas565_backing(uint8_t cid, uint32_t *out_stride, int *out_w, int *out_h) {
    (void)cid; (void)out_stride; (void)out_w; (void)out_h;
    return NULL;
}
static inline uint16_t *lcdterm_get_canvas565_by_name(const char *name, uint32_t *out_stride, int *out_w, int *out_h) {
    (void)name; (void)out_stride; (void)out_w; (void)out_h;
    return NULL;
}
static inline int lcdterm_get_canvas565_cid_by_name(const char *name) {
    (void)name;
    return -1;
}
static inline int  lcdterm_focused_canvas(void) { return -1; }
static inline bool lcdterm_focused_window(uint8_t *type, uint8_t *id) { return ramscreen_focused_window(type, id); }
static inline bool lcdterm_room(int *windows, int *canvases) { (void)windows; (void)canvases; return false; }
static inline bool lcdterm_osk_rect(int *x0, int *y0, int *w, int *h) { (void)x0; (void)y0; (void)w; (void)h; return false; }
static inline void lcdterm_pointer_changed(void) { }
/* No panel: the RAM screen's windows (drivers/ramscreen.h). */
static inline bool lcdterm_window_at(int x, int y, uint8_t *type, uint8_t *id, int *cx, int *cy) {
    return ramscreen_window_at(x, y, type, id, cx, cy);
}
static inline bool lcdterm_canvas_origin(uint8_t cid, int *x0, int *y0) { (void)cid; (void)x0; (void)y0; return false; }
static inline bool lcdterm_set_canvas565_backing(uint8_t cid, uint16_t *buf) {
    (void)cid; (void)buf;
    return false;
}
static inline bool lcdterm_set_canvas565_geometry(uint8_t cid, uint16_t *buf, uint32_t stride, uint16_t w, uint16_t h) {
    (void)cid; (void)buf; (void)stride; (void)w; (void)h;
    return false;
}
static inline int lcdterm_get_visible_canvases565(uint8_t *out_cids, int *out_x0, int *out_y0, int *out_x1, int *out_y1, int max_wins) {
    (void)out_cids; (void)out_x0; (void)out_y0; (void)out_x1; (void)out_y1; (void)max_wins;
    return 0;
}

typedef struct {
    uint8_t   vterm_id;
    int       x0, y0;
    uint16_t  cols, rows;
    uint16_t  shadow_cols;
    uint16_t  col_offset;
    const uint16_t *shadow;
    int       cur_col, cur_row;
} lcdterm_visible_term_t;

static inline int lcdterm_get_visible_terms(lcdterm_visible_term_t *out_terms, int max_terms) {
    (void)out_terms; (void)max_terms;
    return 0;
}
#endif

#endif /* LUGALOS_DRIVERS_LCDTERM_H */
