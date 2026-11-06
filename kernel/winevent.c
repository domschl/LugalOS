/* kernel/winevent.c -- each canvas window's event ring (kernel/event.h). */

#include "kernel/event.h"
#include "kernel/console.h"
#include "kernel/lock.h"
#include "kernel/palloc.h"
#include "drivers/lcdterm.h"
#include "drivers/screen.h"

#include <string.h>

#if defined(LCDTERM_PRESENT)

#define WIN_RING 32u

typedef struct {
    win_event_t ev[WIN_RING];
    uint8_t     head, tail;     /* next to write; next to read */
    uint8_t     claims;         /* WIN_CLAIM_* */
} win_ring_t;

/* The touch task and the input path post, the window's program polls, on
 * either hart. In PSRAM: ~2.6 KB that only panel boards have.
 * guarded by: g_win_lock */
static win_ring_t g_win[SCREEN_MAX_CANVASES] BULK_BSS;
static spinlock_t g_win_lock = SPINLOCK_INIT;

static bool valid(win_t w) {
    return w >= 0 && w < (int)SCREEN_MAX_CANVASES;
}

void window_post(win_t w, const win_event_t *ev) {
    if (!valid(w) || !ev) return;
    uintptr_t f = spin_lock_irqsave(&g_win_lock);
    win_ring_t *r = &g_win[w];
    if (ev->type == WIN_EV_POINTER_MOVE && r->head != r->tail) {
        win_event_t *last = &r->ev[(r->head + WIN_RING - 1u) % WIN_RING];
        if (last->type == WIN_EV_POINTER_MOVE) {          /* unread: one move to here */
            last->x = ev->x;
            last->y = ev->y;
            last->dx = (int16_t)(last->dx + ev->dx);
            last->dy = (int16_t)(last->dy + ev->dy);
            spin_unlock_irqrestore(&g_win_lock, f);
            return;
        }
    }
    uint8_t next = (uint8_t)((r->head + 1u) % WIN_RING);
    if (next != r->tail) {
        r->ev[r->head] = *ev;
        r->head = next;
    }
    spin_unlock_irqrestore(&g_win_lock, f);
}

bool window_poll(win_t w, win_event_t *ev) {
    if (!valid(w)) return false;
    (void)console_has_char();                             /* pumps taps and close requests */
    uintptr_t f = spin_lock_irqsave(&g_win_lock);
    win_ring_t *r = &g_win[w];
    bool got = r->head != r->tail;
    if (got) {
        if (ev) *ev = r->ev[r->tail];
        r->tail = (uint8_t)((r->tail + 1u) % WIN_RING);
    }
    spin_unlock_irqrestore(&g_win_lock, f);
    return got;
}

void window_clear(win_t w) {
    if (!valid(w)) return;
    uintptr_t f = spin_lock_irqsave(&g_win_lock);
    g_win[w].tail = g_win[w].head;
    spin_unlock_irqrestore(&g_win_lock, f);
}

void window_claim(win_t w, uint8_t claims) {
    if (!valid(w)) return;
    uintptr_t f = spin_lock_irqsave(&g_win_lock);
    g_win[w].claims = claims;
    g_win[w].tail = g_win[w].head;                        /* a new owner starts empty */
    spin_unlock_irqrestore(&g_win_lock, f);
}

uint8_t window_claims(win_t w) {
    if (!valid(w)) return 0;
    uintptr_t f = spin_lock_irqsave(&g_win_lock);
    uint8_t c = g_win[w].claims;
    spin_unlock_irqrestore(&g_win_lock, f);
    return c;
}

win_t window_focused(void) {
    return lcdterm_focused_canvas();
}

win_t window_by_name(const char *name) {
    return name ? lcdterm_get_canvas_cid_by_name(name) : -1;
}

bool window_origin(win_t w, int *x0, int *y0) {
    return valid(w) && lcdterm_canvas_origin((uint8_t)w, x0, y0);
}

#else

void window_post(win_t w, const win_event_t *ev) { (void)w; (void)ev; }
bool window_poll(win_t w, win_event_t *ev) { (void)w; (void)ev; return false; }
void window_clear(win_t w) { (void)w; }
void window_claim(win_t w, uint8_t claims) { (void)w; (void)claims; }
uint8_t window_claims(win_t w) { (void)w; return 0; }
win_t window_focused(void) { return -1; }
win_t window_by_name(const char *name) { (void)name; return -1; }
bool window_origin(win_t w, int *x0, int *y0) { (void)w; (void)x0; (void)y0; return false; }

#endif
