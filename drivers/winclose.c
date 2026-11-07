/* drivers/winclose.c -- the close protocol for canvas windows
 * (drivers/include/drivers/winclose.h). */

#include "drivers/winclose.h"
#include "drivers/launcher_view.h"
#include "drivers/lcdterm.h"
#include "drivers/screen.h"
#include "drivers/view.h"
#include "kernel/apps.h"
#include "kernel/console.h"
#include "kernel/event.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#include <stdbool.h>
#include <string.h>

#if defined(LCDTERM_PRESENT)

#define WINCLOSE_ANSWER_MS 200u    /* a program reading its events answers within a frame or two */
#define WINCLOSE_WAIT_MS   5000u   /* "Wait": asked again after; and the time a program that has
                                    * read the request gets to close its window */

typedef struct {
    bool     used;
    bool     asking;               /* its alert is up */
    bool     detached;             /* its window closed, the program still running */
    bool     read;                 /* the program has read the request: given WINCLOSE_WAIT_MS */
    uint8_t  cid;
    int      pid;
    uint64_t due;                  /* the alert from then */
    char     title[48];            /* the program, as the alert names it */
} winclose_t;

/* single task: views (armed through view_post_call(), ticked by it). In
 * PSRAM where there is one: nothing is armed before the shell starts. */
static winclose_t s_wc[SCREEN_MAX_CANVASES] BULK_BSS;

/* single task: views; read by `launcher status` (accepted: a counter). */
static uint32_t s_wc_asks;

static const char *const k_buttons[2] = { "Close Window", "Wait" };   /* Wait: the default and Esc */

static bool task_alive(int pid) {
    int st = pid >= 0 ? sched_task_state(pid) : TASK_UNUSED;
    return st != TASK_UNUSED && st != TASK_DEAD;
}

static void winclose_answer(int choice, void *arg) {
    winclose_t *w = (winclose_t *)arg;
    if (!w->used) return;
    w->asking = false;
    if (choice == 0) {
        if (console_close_canvas(w->cid, false)) w->detached = true;
        else w->used = false;                            /* gone meanwhile */
    } else {
        w->due = time_get_ms() + WINCLOSE_WAIT_MS;
    }
}

static void winclose_ask(winclose_t *w) {
    char title[96], msg[200];
    ksnprintf(title, sizeof title, "%s is not responding.", w->title);
    ksnprintf(msg, sizeof msg,
              "It was asked to close its window and has not.\n"
              "Wait for it, or close the window: %s keeps\n"
              "running until it ends, and cannot be started\n"
              "again meanwhile.", w->title);
    w->asking = true;
    s_wc_asks++;
    launcher_ask("bomb", title, msg, k_buttons, 2, 1, winclose_answer, w);
}

static bool winclose_tick(void) {
    bool any = false;
    uint64_t now = time_get_ms();
    for (unsigned i = 0; i < SCREEN_MAX_CANVASES; i++) {
        winclose_t *w = &s_wc[i];
        if (!w->used) continue;
        bool present = console_window_present(RIBBON_WIN_CANVAS, w->cid);
        if (!task_alive(w->pid)) {                       /* ended: its canvas is free now */
            if (w->asking) launcher_ask_dismiss(winclose_answer, w);
            if (w->detached || present) (void)console_close_canvas(w->cid, true);
            w->used = false;
            continue;
        }
        if (w->detached) {                               /* until it ends, or lets go of its canvas */
            char name[2];
            if (!lcdterm_canvas_name(w->cid, name, sizeof name)) w->used = false;
            else any = true;
            continue;
        }
        if (!present) {                                   /* it answered: its window is gone */
            if (w->asking) launcher_ask_dismiss(winclose_answer, w);
            w->used = false;
            continue;
        }
        if (!w->read && window_close_read((win_t)w->cid)) {   /* answering, only slowly */
            w->read = true;
            if (!w->asking) w->due = now + WINCLOSE_WAIT_MS;
        }
        if (!w->asking && now >= w->due) winclose_ask(w);
        any = true;
    }
    return any;
}

static void winclose_arm(void *arg) {
    uintptr_t v = (uintptr_t)arg;
    uint8_t cid = (uint8_t)(v & 0xFFu);
    int pid = (int)(v >> 8);
    if (cid >= SCREEN_MAX_CANVASES) return;
    winclose_t *w = &s_wc[cid];
    if (w->used && w->pid == pid && !w->detached) return; /* asked again while it is watched */
    if (w->used && w->asking) launcher_ask_dismiss(winclose_answer, w);
    memset(w, 0, sizeof *w);
    w->used = true;
    w->cid = cid;
    w->pid = pid;
    w->due = time_get_ms() + WINCLOSE_ANSWER_MS;
    char name[SCREEN_CTITLE_MAX];
    if (!lcdterm_canvas_name(cid, name, sizeof name) || !name[0]) strcpy(name, "The program");
    const app_desc_t *a = app_find(name);
    const char *t = a ? a->title : name;
    strncpy(w->title, t, sizeof w->title - 1);
    if (w->title[0] >= 'a' && w->title[0] <= 'z') w->title[0] = (char)(w->title[0] - 'a' + 'A');
    view_set_tick(winclose_tick);
}

void winclose_report(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < SCREEN_MAX_CANVASES; i++) n += s_wc[i].used;
    cprintf("close watch: %u window%s watched, %lu not-responding alert%s since boot\n", n, n == 1 ? "" : "s",
            (unsigned long)s_wc_asks, s_wc_asks == 1 ? "" : "s");
}

void winclose_watch(uint8_t cid, int pid) {
    if (pid < 0) return;
    view_post_call(winclose_arm, (void *)(((uintptr_t)(unsigned)pid << 8) | cid));
}

#else

void winclose_watch(uint8_t cid, int pid) { (void)cid; (void)pid; }
void winclose_report(void) {}

#endif
