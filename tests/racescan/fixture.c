/* A known answer for tools/racescan.py (tests/racescan/test_racescan.py).
 * Not built into any image. */
#include <stdint.h>

typedef struct { volatile uint32_t word; } ylock_t;
int  task_create(const char *name, void (*entry)(void *), void *arg);
int  devirq_attach(uint32_t irq, void (*handler)(void *), void *ctx);
int  chan_call(void *ep, const uint8_t *req, uint32_t len, uint8_t *resp, uint32_t max);
uint64_t time_get_ms(void);
void ylock_acquire_at(ylock_t *l, const char *name, const char *site);
void ylock_release(ylock_t *l);
void consume(const uint32_t *p);
void fill(char *buf, uint32_t n);

static uint32_t g_shared_counter;          /* written by a task and an interrupt: A */
static uint32_t g_boot_cfg;                /* written at boot, read by a task: B */
static uint32_t g_only_task;               /* one task only: not listed */
static const uint32_t g_table[4] = { 1, 2, 3, 4 };   /* read-only: not listed */
static uint32_t g_by_const_ptr = 7;        /* only passed to a const pointer: not written */
static char     g_text[16];                /* filled by two tasks through a call: A */
static ylock_t  g_lock;                    /* a lock object: not listed */
/* guarded by: g_lock */
static uint32_t g_marked;
static void *g_ep;

static void isr(void *ctx) {
    (void)ctx;
    g_shared_counter++;
    *(volatile uint32_t *)0x50000000u = 1u;
}

static void worker(void *arg) {
    (void)arg;
    g_shared_counter = 0;
    g_only_task += g_boot_cfg + g_table[1];
    consume(&g_by_const_ptr);
    fill(g_text, sizeof g_text);
    *(volatile uint32_t *)0x50000004u = 2u;
    ylock_acquire_at(&g_lock, "g_lock", "worker");
    g_marked++;
    ylock_release(&g_lock);
    uint8_t q = 0, r = 0;
    uint64_t t0 = time_get_ms();
    while (chan_call(g_ep, &q, 1, &r, 1) < 0 && time_get_ms() - t0 < 50u) { }
}

static void other(void *arg) {
    (void)arg;
    fill(g_text, sizeof g_text);
    g_marked = 0;
    uint8_t q = 0, r = 0;
    for (int i = 0; i < 8; i++) {
        if (chan_call(g_ep, &q, 1, &r, 1) >= 0) break;
    }
}

void kernel_main(void) {
    g_boot_cfg = 3;
    devirq_attach(5, isr, 0);
    task_create("worker", worker, 0);
    task_create("other", other, 0);
}
