/* See drivers/radio/osi_impl.h. 45.3b, plan/phase45_esp32c6.md §4.5.
 *
 * U-mode code. It calls nothing outside its own text -- no libc, no kernel
 * function -- so it is built -fno-jump-tables, has no string literal that is
 * not in granted read-only memory, and reaches the kernel only through
 * ksys() below. */

#include "osi_impl.h"
#include "uprintf.h"
#include "nvs_ram.h"
#include "kernel/uheap.h"
#include "kernel/kobj_abi.h"
#include "kernel/ipc.h"

#ifndef RADIO_TEXT
#define RADIO_TEXT
#endif

/* ---- the system-call stub ------------------------------------------------- */

static inline __attribute__((always_inline))
long ksys(long nr, long a1, long a2, long a3, long a4, long a5) {
    register long r0 __asm__("a0") = nr;
    register long r1 __asm__("a1") = a1;
    register long r2 __asm__("a2") = a2;
    register long r3 __asm__("a3") = a3;
    register long r4 __asm__("a4") = a4;
    register long r5 __asm__("a5") = a5;
    __asm__ volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5) : "memory");
    return r0;
}

#define K(op, a1, a2, a3, a4, a5) \
    ksys(SYS_KOBJ(op), (long)(a1), (long)(a2), (long)(a3), (long)(a4), (long)(a5))

#define PD_TRUE  1
#define PD_FALSE 0
#define FOREVER  0xffffffffu

/* ---- state ----------------------------------------------------------------
 *
 * All of it in one structure in the domain's own data. `static` globals would
 * be fine on the C6, where the radio's link places .data/.bss in its RAM region;
 * the QEMU test has no such region for a kernel global, so everything is here,
 * in one place the caller can put where the domain grants read-write. */

#define TSEM_MAX 8

typedef struct {
    uheap_t     heap;
    long        heap_lock;               /* a kernel mutex handle */
    nvs_store_t nvs;
    long        nvs_lock;
    struct { int32_t pid; long sem; } tsem[TSEM_MAX];
    uintptr_t   isr_lo, isr_hi;
    bool        up;
} radio_state_t;

RADIO_DATA static radio_state_t g_r;

/* ---- heap: the radio's own, serialised by one kernel mutex --------------- */

static RADIO_TEXT void *r_malloc(uint32_t n) {
    K(KOBJ_OP_MUTEX_LOCK, g_r.heap_lock, FOREVER, 0, 0, 0);
    void *p = uheap_alloc(&g_r.heap, n);
    K(KOBJ_OP_MUTEX_UNLOCK, g_r.heap_lock, 0, 0, 0, 0);
    return p;
}

static RADIO_TEXT void r_free(void *p) {
    K(KOBJ_OP_MUTEX_LOCK, g_r.heap_lock, FOREVER, 0, 0, 0);
    uheap_free(&g_r.heap, p);
    K(KOBJ_OP_MUTEX_UNLOCK, g_r.heap_lock, 0, 0, 0, 0);
}

static RADIO_TEXT void ev_reset(void);

RADIO_TEXT bool radio_osi_init(void *arena, uint32_t arena_size) {
    ev_reset();
    /* The state lives in a NOLOAD region the kernel does not clear, and SRAM keeps its
     * contents across a reset: on the C6 a stale thread-semaphore table from the
     * previous run made the blob wait on a handle that no longer existed (45.6).
     * Volatile, so the compiler cannot turn the loop into a call to memset. */
    for (volatile uint32_t *p = (volatile uint32_t *)&g_r; p < (volatile uint32_t *)(&g_r + 1); p++) *p = 0;
    if (!uheap_init(&g_r.heap, arena, arena_size)) return false;
    g_r.heap_lock = K(KOBJ_OP_MUTEX_CREATE, 0, 0, 0, 0, 0);
    g_r.nvs_lock = K(KOBJ_OP_MUTEX_CREATE, 0, 0, 0, 0, 0);
    if (g_r.heap_lock <= 0 || g_r.nvs_lock <= 0) return false;
    nvs_ram_init(&g_r.nvs, (nvs_env_t){ r_malloc, r_free });
    g_r.up = true;
    return true;
}

RADIO_TEXT void radio_osi_set_isr_stack(uintptr_t lo, uintptr_t hi) { g_r.isr_lo = lo; g_r.isr_hi = hi; }

RADIO_TEXT void *radio_osi_malloc(size_t size) { return r_malloc((uint32_t)size); }
RADIO_TEXT void  radio_osi_free(void *p) { r_free(p); }

RADIO_TEXT void *radio_osi_calloc(size_t n, size_t size) {
    K(KOBJ_OP_MUTEX_LOCK, g_r.heap_lock, FOREVER, 0, 0, 0);
    void *p = uheap_calloc(&g_r.heap, (uint32_t)n, (uint32_t)size);
    K(KOBJ_OP_MUTEX_UNLOCK, g_r.heap_lock, 0, 0, 0, 0);
    return p;
}

RADIO_TEXT void *radio_osi_zalloc(size_t size) { return radio_osi_calloc(1, size); }

RADIO_TEXT void *radio_osi_realloc(void *ptr, size_t size) {
    K(KOBJ_OP_MUTEX_LOCK, g_r.heap_lock, FOREVER, 0, 0, 0);
    void *p = uheap_realloc(&g_r.heap, ptr, (uint32_t)size);
    K(KOBJ_OP_MUTEX_UNLOCK, g_r.heap_lock, 0, 0, 0, 0);
    return p;
}

RADIO_TEXT uint32_t radio_osi_get_free_heap_size(void) {
    K(KOBJ_OP_MUTEX_LOCK, g_r.heap_lock, FOREVER, 0, 0, 0);
    uint32_t n = uheap_free_bytes(&g_r.heap);
    K(KOBJ_OP_MUTEX_UNLOCK, g_r.heap_lock, 0, 0, 0, 0);
    return n;
}

/* ---- chip facts and entries with nothing to do ----------------------------- */

RADIO_TEXT bool radio_osi_env_is_chip(void) { return true; }
RADIO_TEXT void radio_osi_empty(void) { }
RADIO_TEXT void radio_osi_task_yield_from_isr(void) { }       /* the kernel reschedules when the ISR thread blocks */
/* IDF's sleep-retention wrappers answer 1 ("nothing to do, fine") when the
 * modem power domain is never powered down, which is this radio. */
RADIO_TEXT int32_t radio_osi_one_i32(void) { return 1; }
RADIO_TEXT bool radio_osi_false(void) { return false; }        /* disable_ac_ax: 11ac/11ax cannot be turned off on the C6 */
RADIO_TEXT void radio_osi_regdma_noop(void *addr, uint32_t value, uint32_t mask) { (void)addr; (void)value; (void)mask; }
RADIO_TEXT void *radio_osi_find_link_null(int id) { (void)id; return (void *)0; }

/* The coexistence library's own, second table (coex_adapter_funcs_t) asks for a
 * few things the Wi-Fi one does not. */
RADIO_TEXT int32_t radio_osi_semphr_take_from_isr(void *s, void *hptw) {
    if (hptw) *(int32_t *)hptw = 0;
    return K(KOBJ_OP_SEM_TAKE, (uintptr_t)s, 0, 0, 0, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}
RADIO_TEXT int32_t radio_osi_semphr_give_from_isr(void *s, void *hptw) {
    if (hptw) *(int32_t *)hptw = 0;
    return K(KOBJ_OP_SEM_GIVE, (uintptr_t)s, 0, 0, 0, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}
RADIO_TEXT int radio_osi_is_in_isr(void) { return radio_osi_is_from_isr(); }
RADIO_TEXT int radio_osi_xtal_freq_get(void) { return 40; }     /* MHz: the crystal on every C6 board here */
RADIO_TEXT int radio_osi_debug_matrix_init(int event, int signal, bool rev) { (void)event; (void)signal; (void)rev; return 0; }

/* The RTC slow clock's calibration, in IDF's Q19 microseconds-per-cycle. Only
 * the sleep code reads it, and this radio does not sleep; the nominal 150 kHz
 * RC oscillator is 6.667 us = 3495253 in Q19. Replaced by a measurement if
 * light sleep is ever enabled. */
RADIO_TEXT uint32_t radio_osi_slowclk_cal_get(void) { return 3495253u; }

/* ---- interrupts ------------------------------------------------------------ */

RADIO_TEXT void radio_osi_set_intr(int32_t cpu_no, uint32_t src, uint32_t num, int32_t prio) {
    K(KOBJ_OP_INTR_SET, cpu_no, src, num, prio, 0);
}
RADIO_TEXT void radio_osi_clear_intr(uint32_t src, uint32_t num) { K(KOBJ_OP_INTR_CLEAR, src, num, 0, 0, 0); }
RADIO_TEXT void radio_osi_set_isr(int32_t n, void *f, void *arg) { K(KOBJ_OP_ISR_SET, n, f, arg, 0, 0); }
RADIO_TEXT void radio_osi_ints_on(uint32_t mask) { K(KOBJ_OP_INTS_ON, mask, 0, 0, 0, 0); }
RADIO_TEXT void radio_osi_ints_off(uint32_t mask) { K(KOBJ_OP_INTS_OFF, mask, 0, 0, 0, 0); }

/* Which thread is this? The interrupt thread's stack is a known range, and the
 * stack pointer says where we are -- no system call. */
RADIO_TEXT bool radio_osi_is_from_isr(void) {
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    return g_r.isr_hi != 0 && sp >= g_r.isr_lo && sp < g_r.isr_hi;
}

/* A "spin lock" for the blob is a few bytes it keeps a pointer to; the exclusion
 * itself is wifi_int_disable's, below. */
RADIO_TEXT void *radio_osi_spin_lock_create(void) { return r_malloc(8); }
RADIO_TEXT void radio_osi_spin_lock_delete(void *lock) { r_free(lock); }

/* The blob's critical section ("interrupts off around this") is exclusion among
 * the domain's own threads, which is all an interrupt is here: the hardware
 * interrupt runs only a kernel stub, and the handler is a thread
 * (plan/phase45 §4.5). One lock for every mux the blob passes. */
RADIO_TEXT uint32_t radio_osi_wifi_int_disable(void *mux) { (void)mux; K(KOBJ_OP_CRIT_ENTER, 0, 0, 0, 0, 0); return 0; }
RADIO_TEXT void radio_osi_wifi_int_restore(void *mux, uint32_t tmp) { (void)mux; (void)tmp; K(KOBJ_OP_CRIT_LEAVE, 0, 0, 0, 0, 0); }

/* ---- semaphores ------------------------------------------------------------ */

RADIO_TEXT void *radio_osi_semphr_create(uint32_t max, uint32_t init) {
    return (void *)(uintptr_t)K(KOBJ_OP_SEM_CREATE, max, init, 0, 0, 0);
}
RADIO_TEXT void radio_osi_semphr_delete(void *s) { K(KOBJ_OP_SEM_DELETE, (uintptr_t)s, 0, 0, 0, 0); }
RADIO_TEXT int32_t radio_osi_semphr_take(void *s, uint32_t ticks) {
    return K(KOBJ_OP_SEM_TAKE, (uintptr_t)s, ticks, 0, 0, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}
RADIO_TEXT int32_t radio_osi_semphr_give(void *s) {
    return K(KOBJ_OP_SEM_GIVE, (uintptr_t)s, 0, 0, 0, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}

/* One binary semaphore per calling thread, for the blob's synchronous API
 * calls. Found by the thread's pid; created on first use. */
RADIO_TEXT void *radio_osi_wifi_thread_semphr_get(void) {
    int32_t me = (int32_t)K(KOBJ_OP_THREAD_SELF, 0, 0, 0, 0, 0);
    K(KOBJ_OP_CRIT_ENTER, 0, 0, 0, 0, 0);
    long found = 0;
    int freeslot = -1;
    for (int i = 0; i < TSEM_MAX; i++) {
        if (g_r.tsem[i].sem && g_r.tsem[i].pid == me) { found = g_r.tsem[i].sem; break; }
        if (!g_r.tsem[i].sem && freeslot < 0) freeslot = i;
    }
    if (!found && freeslot >= 0) {
        found = K(KOBJ_OP_SEM_CREATE, 1, 0, 0, 0, 0);
        if (found > 0) { g_r.tsem[freeslot].pid = me; g_r.tsem[freeslot].sem = found; }
        else found = 0;
    }
    K(KOBJ_OP_CRIT_LEAVE, 0, 0, 0, 0, 0);
    return (void *)(uintptr_t)found;
}

/* ---- mutexes --------------------------------------------------------------- */

RADIO_TEXT void *radio_osi_mutex_create(void) { return (void *)(uintptr_t)K(KOBJ_OP_MUTEX_CREATE, 0, 0, 0, 0, 0); }
RADIO_TEXT void *radio_osi_recursive_mutex_create(void) { return (void *)(uintptr_t)K(KOBJ_OP_MUTEX_CREATE, 1, 0, 0, 0, 0); }
RADIO_TEXT void radio_osi_mutex_delete(void *m) { K(KOBJ_OP_MUTEX_DELETE, (uintptr_t)m, 0, 0, 0, 0); }
RADIO_TEXT int32_t radio_osi_mutex_lock(void *m) {
    return K(KOBJ_OP_MUTEX_LOCK, (uintptr_t)m, FOREVER, 0, 0, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}
RADIO_TEXT int32_t radio_osi_mutex_unlock(void *m) {
    return K(KOBJ_OP_MUTEX_UNLOCK, (uintptr_t)m, 0, 0, 0, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}

/* ---- queues ---------------------------------------------------------------- */

RADIO_TEXT void *radio_osi_queue_create(uint32_t len, uint32_t isz) {
    return (void *)(uintptr_t)K(KOBJ_OP_Q_CREATE, len, isz, 0, 0, 0);
}
RADIO_TEXT void radio_osi_queue_delete(void *q) { K(KOBJ_OP_Q_DELETE, (uintptr_t)q, 0, 0, 0, 0); }
RADIO_TEXT int32_t radio_osi_queue_send(void *q, void *item, uint32_t ticks) {
    return K(KOBJ_OP_Q_SEND, (uintptr_t)q, item, 0, ticks, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}
RADIO_TEXT int32_t radio_osi_queue_send_to_back(void *q, void *item, uint32_t ticks) { return radio_osi_queue_send(q, item, ticks); }
RADIO_TEXT int32_t radio_osi_queue_send_to_front(void *q, void *item, uint32_t ticks) {
    return K(KOBJ_OP_Q_SEND, (uintptr_t)q, item, 1, ticks, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}
/* From the interrupt thread: never waits, and "a higher-priority task was woken"
 * is always false -- the kernel reschedules on its own. */
RADIO_TEXT int32_t radio_osi_queue_send_from_isr(void *q, void *item, void *hptw) {
    if (hptw) *(int32_t *)hptw = 0;
    return K(KOBJ_OP_Q_SEND, (uintptr_t)q, item, 0, 0, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}
RADIO_TEXT int32_t radio_osi_queue_recv(void *q, void *item, uint32_t ticks) {
    return K(KOBJ_OP_Q_RECV, (uintptr_t)q, item, ticks, 0, 0) == KO_OK ? PD_TRUE : PD_FALSE;
}
RADIO_TEXT uint32_t radio_osi_queue_msg_waiting(void *q) {
    long n = K(KOBJ_OP_Q_WAITING, (uintptr_t)q, 0, 0, 0, 0);
    return n < 0 ? 0 : (uint32_t)n;
}
/* The blob does not get a queue handle from this entry but a `wifi_static_queue_t *`
 * (IDF's esp_adapter.c: a malloc'd struct whose first and only member is the handle)
 * and reads `->handle` itself; handing it the bare handle, as every other create
 * does, made it dereference 0x30000001 (45.6). */
typedef struct { void *handle; void *storage; } wifi_static_queue_shape_t;

RADIO_TEXT void *radio_osi_wifi_create_queue(int len, int isz) {
    wifi_static_queue_shape_t *w = radio_osi_malloc(sizeof(*w));
    if (!w) return 0;
    w->storage = 0;
    w->handle = radio_osi_queue_create((uint32_t)len, (uint32_t)isz);
    if (!w->handle) { radio_osi_free(w); return 0; }
    return w;
}
RADIO_TEXT void radio_osi_wifi_delete_queue(void *q) {
    wifi_static_queue_shape_t *w = q;
    if (!w) return;
    radio_osi_queue_delete(w->handle);
    radio_osi_free(w);
}

/* ---- event groups ---------------------------------------------------------- */

RADIO_TEXT void *radio_osi_event_group_create(void) { return (void *)(uintptr_t)K(KOBJ_OP_EV_CREATE, 0, 0, 0, 0, 0); }
RADIO_TEXT void radio_osi_event_group_delete(void *e) { K(KOBJ_OP_EV_DELETE, (uintptr_t)e, 0, 0, 0, 0); }
RADIO_TEXT uint32_t radio_osi_event_group_set_bits(void *e, uint32_t bits) {
    return (uint32_t)K(KOBJ_OP_EV_SET, (uintptr_t)e, bits, 0, 0, 0);
}
RADIO_TEXT uint32_t radio_osi_event_group_clear_bits(void *e, uint32_t bits) {
    return (uint32_t)K(KOBJ_OP_EV_CLEAR, (uintptr_t)e, bits, 0, 0, 0);
}
RADIO_TEXT uint32_t radio_osi_event_group_wait_bits(void *e, uint32_t bits, int clear_on_exit,
                                                    int wait_for_all, uint32_t ticks) {
    volatile uint32_t seen = 0;
    uint32_t flags = (wait_for_all ? KOBJ_EV_ALL : 0u) | (clear_on_exit ? KOBJ_EV_CLEAR : 0u);
    K(KOBJ_OP_EV_WAIT, (uintptr_t)e, bits, flags, ticks, &seen);
    return seen;
}

/* ---- threads --------------------------------------------------------------- */

/* The blob's tasks, and the shim's own, start here: the new thread's entry is
 * this trampoline, which calls the real one and, if it returns, ends the
 * thread. FreeRTOS tasks must not return, and the blob's do not. */
typedef struct { void (*fn)(void *); void *param; } tramp_t;

static RADIO_TEXT void thread_tramp(uintptr_t a) {
    tramp_t t = *(tramp_t *)a;
    r_free((void *)a);
    t.fn(t.param);
    ksys(SYS_UEXIT, 0, 0, 0, 0, 0);
    for (;;) { }
}

static RADIO_TEXT int32_t thread_create_ex(void (*entry)(void *), void *param, uint32_t stack_bytes, uint32_t prio,
                                           uint8_t **stack_out) {
    stack_bytes = (stack_bytes + 7u) & ~7u;
    uint8_t *stack = (uint8_t *)r_malloc(stack_bytes);
    if (stack_out) *stack_out = stack;
    tramp_t *t = (tramp_t *)r_malloc(sizeof *t);
    if (!stack || !t) { r_free(stack); r_free(t); return KO_FAIL; }
    t->fn = entry; t->param = param;
    long pid = K(KOBJ_OP_THREAD_CREATE, thread_tramp, t, stack, stack_bytes, prio);
    if (pid < 0) { r_free(stack); r_free(t); }
    return (int32_t)pid;          /* the stack is the thread's for good: a radio thread does not exit */
}

RADIO_TEXT int32_t radio_thread_create(void (*entry)(void *), void *param, uint32_t stack_bytes, uint32_t prio) {
    return thread_create_ex(entry, param, stack_bytes, prio, 0);
}

/* The interrupt thread (plan §4.5): sleeps in the kernel until a line the blob asked for
 * has fired, calls the blob's handler, tells the kernel it may unmask the line. Its stack
 * is how radio_osi_is_from_isr() knows it is in an interrupt without a system call. */
#define ISR_STACK_BYTES 3072u
static RADIO_TEXT void isr_thread(void *unused) {
    (void)unused;
    for (;;) {
        uintptr_t rec[3];
        if (K(KOBJ_OP_ISR_WAIT, (uintptr_t)rec, 0, 0, 0, 0) != KO_OK) {
            ksys(SYS_UEXIT, 0, 0, 0, 0, 0);
            for (;;) { }
        }
        ((void (*)(void *))rec[0])((void *)rec[1]);
        K(KOBJ_OP_ISR_DONE, rec[2], 0, 0, 0, 0);
    }
}

RADIO_TEXT bool radio_osi_start_isr_thread(void) {
    uint8_t *stack = 0;
    int32_t pid = thread_create_ex(isr_thread, 0, ISR_STACK_BYTES, 24, &stack);
    if (pid < 0 || !stack) return false;
    radio_osi_set_isr_stack((uintptr_t)stack, (uintptr_t)stack + ISR_STACK_BYTES);
    return true;
}

RADIO_TEXT int32_t radio_osi_task_create_pinned_to_core(void *func, const char *name, uint32_t stack_depth,
                                                        void *param, uint32_t prio, void *handle, uint32_t core) {
    (void)name; (void)core;                      /* one core; names are for a debugger we do not have */
    int32_t pid = radio_thread_create((void (*)(void *))(uintptr_t)func, param, stack_depth, prio);
    if (pid < 0) return PD_FALSE;
    if (handle) *(uint32_t *)handle = (uint32_t)pid;
    return PD_TRUE;
}
RADIO_TEXT int32_t radio_osi_task_create(void *func, const char *name, uint32_t stack_depth,
                                         void *param, uint32_t prio, void *handle) {
    return radio_osi_task_create_pinned_to_core(func, name, stack_depth, param, prio, handle, 0);
}

/* Only a task may delete itself (vTaskDelete(NULL), or its own handle); the
 * blob does not delete others. */
RADIO_TEXT void radio_osi_task_delete(void *handle) {
    int32_t me = (int32_t)K(KOBJ_OP_THREAD_SELF, 0, 0, 0, 0, 0);
    if (handle != (void *)0 && (int32_t)(uintptr_t)handle != me) return;
    ksys(SYS_UEXIT, 0, 0, 0, 0, 0);
    for (;;) { }
}

RADIO_TEXT void radio_osi_task_delay(uint32_t ticks) { ksys(SYS_SLEEP_MS, ticks, 0, 0, 0, 0); }
RADIO_TEXT int32_t radio_osi_task_ms_to_tick(uint32_t ms) { return (int32_t)ms; }
RADIO_TEXT void *radio_osi_task_get_current_task(void) { return (void *)(uintptr_t)K(KOBJ_OP_THREAD_SELF, 0, 0, 0, 0, 0); }
RADIO_TEXT int32_t radio_osi_task_get_max_priority(void) { return 25; }       /* IDF's configMAX_PRIORITIES */

/* ---- timers ---------------------------------------------------------------- */

RADIO_TEXT void radio_osi_timer_setfn(void *t, void *fn, void *arg) { K(KOBJ_OP_TIMER_SETFN, (uintptr_t)t, fn, arg, 0, 0); }
RADIO_TEXT void radio_osi_timer_arm(void *t, uint32_t ms, bool repeat) {
    K(KOBJ_OP_TIMER_ARM, (uintptr_t)t, 0, 0, (uint64_t)ms * 1000u > 0xffffffffu ? 0xffffffffu : ms * 1000u, repeat);
}
RADIO_TEXT void radio_osi_timer_arm_us(void *t, uint32_t us, bool repeat) { K(KOBJ_OP_TIMER_ARM, (uintptr_t)t, 0, 0, us, repeat); }
RADIO_TEXT void radio_osi_timer_disarm(void *t) { K(KOBJ_OP_TIMER_DISARM, (uintptr_t)t, 0, 0, 0, 0); }
RADIO_TEXT void radio_osi_timer_done(void *t) { K(KOBJ_OP_TIMER_DONE, (uintptr_t)t, 0, 0, 0, 0); }

/* The thread that calls the blob's timer callbacks: waits in the kernel for the
 * next due timer, calls it in this domain, repeats. */
RADIO_TEXT void radio_timer_thread(void *unused) {
    (void)unused;
    volatile uintptr_t rec[3];
    for (;;) {
        if (K(KOBJ_OP_TIMER_WAIT, rec, FOREVER, 0, 0, 0) != KO_OK) continue;
        if (rec[1]) ((void (*)(void *))rec[1])((void *)rec[2]);
    }
}

/* ---- time, randomness, identity -------------------------------------------- */

RADIO_TEXT int64_t radio_osi_esp_timer_get_time(void) {
    volatile uint64_t us = 0;
    K(KOBJ_OP_TIME_US, &us, 0, 0, 0, 0);
    return (int64_t)us;
}

/* `_get_time` is IDF's os_get_time(struct os_time *): the supplicant's
 * `{ os_time_t sec; os_time_t usec; }` with os_time_t a plain `long` (4 bytes on
 * this core). It is read for relative timing by the handshake's timeouts, so
 * the kernel's monotonic microseconds serve; IDF itself reads gettimeofday(),
 * which is wall-clock only once SNTP has set it. */
typedef struct { long sec; long usec; } r_os_time_t;

RADIO_TEXT int radio_osi_get_time(void *t) {
    int64_t us = radio_osi_esp_timer_get_time();
    r_os_time_t *tv = (r_os_time_t *)t;
    if (!tv) return -1;
    long sec = 0;
    while (us >= 1000000) { us -= 1000000; sec++; }      /* no 64-bit division in U-mode text */
    tv->sec = sec; tv->usec = (long)us;
    return 0;
}

RADIO_TEXT int radio_osi_get_random(uint8_t *buf, size_t len) {
    while (len) {
        long n = K(KOBJ_OP_RANDOM, buf, len > 64 ? 64 : len, 0, 0, 0);
        if (n <= 0) return -1;
        buf += n; len -= (size_t)n;
    }
    return 0;
}

RADIO_TEXT unsigned long radio_osi_random(void) {
    uint32_t v = 0;
    radio_osi_get_random((uint8_t *)&v, sizeof v);
    return v;
}
RADIO_TEXT uint32_t radio_osi_rand(void) { return (uint32_t)radio_osi_random(); }

RADIO_TEXT int radio_osi_read_mac(uint8_t *mac, unsigned int type) {
    return K(KOBJ_OP_MAC, type, mac, 0, 0, 0) == KO_OK ? 0 : -1;
}

/* ---- events ---------------------------------------------------------------- */

/* The blob announces what the radio is doing (STA started, connected, disconnected, scan done...)
 * with esp_event_post(). IDF delivers those to an event loop; here the sink is a small ring in
 * the domain that the radio's main thread drains (radio_osi_event_pop). Entries of any other
 * base are dropped, a full ring drops the oldest, and the call always answers 0 (ESP_OK):
 * an event nobody wants is not an error the blob should react to. */
#define EV_RING 16u
#define EV_DATA 48u
typedef struct { int32_t id; uint32_t size; uint8_t data[EV_DATA]; } rev_t;
RADIO_DATA static rev_t g_ev[EV_RING];
RADIO_DATA static volatile uint32_t g_ev_head, g_ev_tail;

RADIO_TEXT int32_t radio_osi_event_post(const char *base, int32_t id, void *data, size_t size, uint32_t ticks) {
    (void)ticks;
    if (!base || base[0] != 'W') return 0;                 /* "WIFI_EVENT" only */
    K(KOBJ_OP_CRIT_ENTER, 0, 0, 0, 0, 0);
    if (g_ev_head - g_ev_tail == EV_RING) g_ev_tail++;
    rev_t *e = &g_ev[g_ev_head % EV_RING];
    e->id = id;
    e->size = size < EV_DATA ? (uint32_t)size : EV_DATA;
    for (uint32_t i = 0; data && i < e->size; i++) e->data[i] = ((const uint8_t *)data)[i];
    g_ev_head++;
    K(KOBJ_OP_CRIT_LEAVE, 0, 0, 0, 0, 0);
    return 0;
}

static RADIO_TEXT void ev_reset(void) { g_ev_head = 0; g_ev_tail = 0; }

RADIO_TEXT bool radio_osi_event_pop(int32_t *id, uint8_t *data, uint32_t *size) {
    bool got = false;
    K(KOBJ_OP_CRIT_ENTER, 0, 0, 0, 0, 0);
    if (g_ev_tail != g_ev_head) {
        const rev_t *e = &g_ev[g_ev_tail % EV_RING];
        *id = e->id;
        *size = e->size;
        for (uint32_t i = 0; data && i < e->size; i++) data[i] = e->data[i];
        g_ev_tail++;
        got = true;
    }
    K(KOBJ_OP_CRIT_LEAVE, 0, 0, 0, 0, 0);
    return got;
}

/* ---- logging --------------------------------------------------------------- */

RADIO_TEXT uint32_t radio_osi_log_timestamp(void) {
    int64_t us = radio_osi_esp_timer_get_time();
    uint32_t ms = 0;
    while (us >= 1000) { us -= 1000; ms++; }
    return ms;
}

RADIO_TEXT void radio_osi_log_writev(unsigned level, const char *tag, const char *fmt, va_list ap) {
    char line[200];
    int n = ku_snprintf(line, sizeof line, "%s: ", tag ? tag : "");
    if (n < 0 || (uint32_t)n >= sizeof line) n = 0;
    int m = ku_vsnprintf(line + n, (uint32_t)(sizeof line) - (uint32_t)n, fmt, ap);
    int total = n + (m < 0 ? 0 : m);
    if ((uint32_t)total >= sizeof line) total = (int)sizeof line - 1;
    while (total > 0 && (line[total - 1] == '\n' || line[total - 1] == '\r')) total--;
    K(KOBJ_OP_LOG, level, line, total, 0, 0);
}

RADIO_TEXT void radio_osi_log_write(unsigned level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    radio_osi_log_writev(level, tag, fmt, ap);
    va_end(ap);
}

/* ---- the key/value store, under its own lock ------------------------------- */

#define NVS_LOCKED(call) do { K(KOBJ_OP_MUTEX_LOCK, g_r.nvs_lock, FOREVER, 0, 0, 0); r = (call); \
                              K(KOBJ_OP_MUTEX_UNLOCK, g_r.nvs_lock, 0, 0, 0, 0); } while (0)

RADIO_TEXT int radio_osi_nvs_open(const char *name, unsigned mode, uint32_t *h) { int r; NVS_LOCKED(nvs_ram_open(&g_r.nvs, name, mode, h)); return r; }
RADIO_TEXT void radio_osi_nvs_close(uint32_t h) { int r; NVS_LOCKED((nvs_ram_close(&g_r.nvs, h), 0)); (void)r; }
RADIO_TEXT int radio_osi_nvs_commit(uint32_t h) { int r; NVS_LOCKED(nvs_ram_commit(&g_r.nvs, h)); return r; }
RADIO_TEXT int radio_osi_nvs_set_i8(uint32_t h, const char *k, int8_t v) { int r; NVS_LOCKED(nvs_ram_set_i8(&g_r.nvs, h, k, v)); return r; }
RADIO_TEXT int radio_osi_nvs_get_i8(uint32_t h, const char *k, int8_t *o) { int r; NVS_LOCKED(nvs_ram_get_i8(&g_r.nvs, h, k, o)); return r; }
RADIO_TEXT int radio_osi_nvs_set_u8(uint32_t h, const char *k, uint8_t v) { int r; NVS_LOCKED(nvs_ram_set_u8(&g_r.nvs, h, k, v)); return r; }
RADIO_TEXT int radio_osi_nvs_get_u8(uint32_t h, const char *k, uint8_t *o) { int r; NVS_LOCKED(nvs_ram_get_u8(&g_r.nvs, h, k, o)); return r; }
RADIO_TEXT int radio_osi_nvs_set_u16(uint32_t h, const char *k, uint16_t v) { int r; NVS_LOCKED(nvs_ram_set_u16(&g_r.nvs, h, k, v)); return r; }
RADIO_TEXT int radio_osi_nvs_get_u16(uint32_t h, const char *k, uint16_t *o) { int r; NVS_LOCKED(nvs_ram_get_u16(&g_r.nvs, h, k, o)); return r; }
RADIO_TEXT int radio_osi_nvs_set_blob(uint32_t h, const char *k, const void *v, size_t n) { int r; NVS_LOCKED(nvs_ram_set_blob(&g_r.nvs, h, k, v, n)); return r; }
RADIO_TEXT int radio_osi_nvs_get_blob(uint32_t h, const char *k, void *o, size_t *n) { int r; NVS_LOCKED(nvs_ram_get_blob(&g_r.nvs, h, k, o, n)); return r; }
RADIO_TEXT int radio_osi_nvs_erase_key(uint32_t h, const char *k) { int r; NVS_LOCKED(nvs_ram_erase_key(&g_r.nvs, h, k)); return r; }
