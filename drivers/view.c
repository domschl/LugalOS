/* drivers/view.c -- the views behind ribbon windows, and the task they all
 * run in (drivers/include/drivers/view.h). */

#include "drivers/view.h"
#include "drivers/screen.h"
#include "kernel/lock.h"
#include "kernel/palloc.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/vterm.h"
#include "lugalos_config.h"

#include <string.h>

extern const view_ops_t g_reader_view;
extern const view_ops_t g_launcher_view;
#if CONFIG_DEV && !(defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO)))
#include "drivers/viewtest.h"
#define VIEWTEST 1
#endif

const view_ops_t *view_for_window(uint8_t type) {
    switch (type) {
    case RIBBON_WIN_READER:   return &g_reader_view;
    case RIBBON_WIN_LAUNCHER: return &g_launcher_view;
#if defined(VIEWTEST)
    case RIBBON_WIN_VIEWTEST: return &g_viewtest_view;
#endif
    default:                  return 0;
    }
}

/* The boards without a panel, as drivers/launcher_view.c has them: no task. */
#if !(defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO)))
#define VIEW_TASK 1
#endif

typedef struct {
    const view_ops_t *ops;          /* NULL: a call */
    void            (*fn)(void *);
    void             *arg;
    volatile bool    *done;         /* a waiting caller's flag, NULL: none */
    int               waiter;       /* its pid */
    uint8_t           inst;
    win_event_t       ev;
} view_msg_t;

static void view_run(const view_msg_t *m) {
    if (m->fn) {
        m->fn(m->arg);
        return;
    }
    const view_ops_t *v = m->ops;
    if (!v) return;
    if (m->ev.type == WIN_EV_RESIZE) {
        if (v->resize) v->resize(m->inst, m->ev.w, m->ev.h);
    } else if (m->ev.type == WIN_EV_CLOSE_REQUEST) {
        if (v->close) v->close(m->inst);
    } else if (v->active(m->inst)) {
        v->event(m->inst, &m->ev);
    }
}

#if defined(VIEW_TASK)

/* Dozens of events a second at the most (moves and drags are merged before
 * they are posted); 16 covers a burst while a view draws. */
#define VIEW_QUEUE_LEN 16u

/* guarded by: s_view_lock -- any task posts, the views task takes. In PSRAM
 * where there is one (BULK_BSS): nothing posts before the shell starts. */
static view_msg_t     s_view_q[VIEW_QUEUE_LEN] BULK_BSS;
static uint32_t       s_view_head, s_view_tail;   /* guarded by: s_view_lock */
static spinlock_t     s_view_lock = SPINLOCK_INIT;
/* single task: set once by view_start() from the boot shell, before any
 * post can find it running. */
static volatile int   s_view_pid = -1;

static bool view_pop(view_msg_t *m) {
    uintptr_t f = spin_lock_irqsave(&s_view_lock);
    bool got = s_view_tail != s_view_head;
    if (got) {
        *m = s_view_q[s_view_tail % VIEW_QUEUE_LEN];
        s_view_tail++;
    }
    spin_unlock_irqrestore(&s_view_lock, f);
    return got;
}

static bool view_try_push(const view_msg_t *m) {
    uintptr_t f = spin_lock_irqsave(&s_view_lock);
    bool room = s_view_head - s_view_tail < VIEW_QUEUE_LEN;
    if (room) {
        s_view_q[s_view_head % VIEW_QUEUE_LEN] = *m;
        s_view_head++;
    }
    spin_unlock_irqrestore(&s_view_lock, f);
    return room;
}

static void view_push(const view_msg_t *m) {
    for (;;) {
        if (view_try_push(m)) {
            (void)task_unblock(s_view_pid);
            return;
        }
        sched_yield();                                   /* full: the views task drains it */
    }
}

#define VIEW_TICK_MS 50u

static bool   (*s_view_tick)(void);   /* single task: views */
static uint64_t s_view_tick_due;      /* single task: views */

void view_set_tick(bool (*fn)(void)) {
    s_view_tick = fn;
    s_view_tick_due = time_get_ms() + VIEW_TICK_MS;
}

static void view_task_body(void *arg) {
    (void)arg;
    for (;;) {
        if (s_view_tick && time_get_ms() >= s_view_tick_due) {
            s_view_tick_due = time_get_ms() + VIEW_TICK_MS;
            if (!s_view_tick()) s_view_tick = NULL;
        }
        view_msg_t m;
        task_prepare_block();
        if (!view_pop(&m)) {
            if (s_view_tick) task_block_until_ms(s_view_tick_due);
            else task_block();
            continue;
        }
        task_wait_done();
        view_run(&m);
        if (m.done) {
            int waiter = m.waiter;                       /* the flag is on its stack: last touch */
            __atomic_store_n(m.done, true, __ATOMIC_RELEASE);
            (void)task_unblock(waiter);
        }
    }
}

void view_start(void) {
    if (s_view_pid >= 0) return;
    /* A shell's stack: the views draw what the shells' input paths drew. */
    int pid = task_create_sized("views", view_task_body, NULL, VTERM_SHELL_STACK_PAGES);
    if (pid >= 0) s_view_pid = pid;
}

bool view_in_task(void) {
    return s_view_pid < 0 || sched_current_pid() == s_view_pid;
}

bool view_pending(void) {
    if (s_view_pid < 0) return false;
    return __atomic_load_n(&s_view_head, __ATOMIC_ACQUIRE) != __atomic_load_n(&s_view_tail, __ATOMIC_ACQUIRE);
}

static void view_enqueue(view_msg_t *m, bool wait) {
    if (view_in_task() || sched_current_pid() < 0) {
        view_run(m);
        return;
    }
    volatile bool done = false;
    if (wait) {
        m->done = &done;
        m->waiter = sched_current_pid();
    }
    view_push(m);
    if (!wait) return;
    for (;;) {
        task_prepare_block();
        if (__atomic_load_n(&done, __ATOMIC_ACQUIRE)) break;
        task_block();
    }
    task_wait_done();
}

static void view_enqueue_nowait(const view_msg_t *m) {
    if (s_view_pid >= 0 && view_try_push(m)) (void)task_unblock(s_view_pid);
}

#else /* no panel: inline */

void view_set_tick(bool (*fn)(void)) { (void)fn; }
void view_start(void) {}
bool view_in_task(void) { return true; }
bool view_pending(void) { return false; }

static void view_enqueue(view_msg_t *m, bool wait) {
    (void)wait;
    view_run(m);
}

static void view_enqueue_nowait(const view_msg_t *m) { (void)m; }

#endif

void view_post(const view_ops_t *v, uint8_t inst, const win_event_t *ev) {
    view_msg_t m;
    memset(&m, 0, sizeof m);
    m.ops = v;
    m.inst = inst;
    m.ev = *ev;
    view_enqueue(&m, false);
}

void view_post_nowait(const view_ops_t *v, uint8_t inst, const win_event_t *ev) {
    view_msg_t m;
    memset(&m, 0, sizeof m);
    m.ops = v;
    m.inst = inst;
    m.ev = *ev;
    view_enqueue_nowait(&m);
}

void view_post_call(void (*fn)(void *), void *arg) {
    view_msg_t m;
    memset(&m, 0, sizeof m);
    m.fn = fn;
    m.arg = arg;
    view_enqueue(&m, false);
}

void view_call(void (*fn)(void *), void *arg) {
    view_msg_t m;
    memset(&m, 0, sizeof m);
    m.fn = fn;
    m.arg = arg;
    view_enqueue(&m, true);
}

static void view_nothing(void *arg) { (void)arg; }

void view_sync(void) {
    view_call(view_nothing, NULL);
}
