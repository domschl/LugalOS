/* The kernel's side of the radio's interrupts (45.6, plan/phase45_esp32c6.md §4.5).
 *
 * The Wi-Fi blob asks for its interrupts through the OS table: "route this source to
 * CPU interrupt n" (`_set_intr`), "run f(arg) for interrupt n" (`_set_isr`), "enable
 * these" (`_ints_on`). It runs in U-mode, and the kernel cannot call into U-mode from
 * an interrupt, so the handler is not called from here. The hardware interrupt lands
 * in a kernel stub that *masks the line* (the MAC and PWR sources are level-triggered:
 * unmasked, they would re-enter forever) and wakes the radio's interrupt thread, which
 * is blocked in KOBJ_OP_ISR_WAIT; the thread calls f(arg) in the radio's domain and
 * then KOBJ_OP_ISR_DONE, which unmasks. One thread serves every line, in the order
 * they fired.
 *
 * **The blob's own interrupt numbers are not used as hardware lines.** IDF reserves
 * lines 1 and 4 and others for the blob (ETS_WMAC_INUM etc.), and this kernel's
 * reserved set (arch/esp32c6_intr.h) differs. `n` is only a name the blob uses to tie
 * `_set_intr`, `_set_isr` and `_ints_on` together; each name gets a free CPU line
 * (12..19) here. */

#include "kernel/radio_intr.h"
#include "kernel/devirq.h"
#include "kernel/sched.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/kobj_abi.h"
#include "kernel/mem_domain.h"
#include "kernel/uaccess.h"
#include "arch/trap.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>

#if defined(CONFIG_BOARD_ESP32C6)
#include "arch/esp32c6_intr.h"

#define RI_SLOTS      8
#define RI_FIRST_LINE 12u

typedef struct {
    bool used;
    uint32_t name;                 /* the blob's number */
    uint32_t line;                 /* the CPU line it was given */
    uintptr_t fn, arg;             /* the handler, in the radio's domain */
    volatile bool pending;
    volatile uint32_t fired;       /* how many times, for the shell */
} ri_slot_t;

static ri_slot_t g_ri[RI_SLOTS];
static volatile int g_ri_waiter = -1;      /* the interrupt thread's pid */

static ri_slot_t *slot_for(uint32_t name, bool create) {
    for (int i = 0; i < RI_SLOTS; i++) if (g_ri[i].used && g_ri[i].name == name) return &g_ri[i];
    if (!create) return 0;
    for (int i = 0; i < RI_SLOTS; i++) {
        if (!g_ri[i].used) {
            g_ri[i] = (ri_slot_t){ .used = true, .name = name, .line = RI_FIRST_LINE + (uint32_t)i };
            return &g_ri[i];
        }
    }
    return 0;
}

/* In the trap handler: mask, mark, wake. */
static void ri_stub(void *ctx) {
    ri_slot_t *s = ctx;
    esp32c6_irq_mask(s->line);
    s->fired++;
    s->pending = true;
    if (g_ri_waiter >= 0) task_unblock(g_ri_waiter);
}

void radio_intr_reset(void) {
    for (int i = 0; i < RI_SLOTS; i++) {
        if (g_ri[i].used) esp32c6_irq_mask(g_ri[i].line);
        g_ri[i] = (ri_slot_t){ 0 };
    }
    g_ri_waiter = -1;
}

long radio_intr_set(uint32_t source, uint32_t name) {
    ri_slot_t *s = slot_for(name, true);
    if (!s) return KO_FAIL;
    if (esp32c6_intmtx_route(source, s->line) != 0) return KO_FAIL;
    return KO_OK;
}

long radio_intr_clear(uint32_t source, uint32_t name) {
    ri_slot_t *s = slot_for(name, false);
    if (!s) return KO_FAIL;
    esp32c6_irq_mask(s->line);
    return esp32c6_intmtx_route(source, 0) == 0 ? KO_OK : KO_FAIL;     /* line 0: nothing */
}

long radio_isr_set(uint32_t name, uintptr_t fn, uintptr_t arg) {
    mem_domain_t *dom = sched_current_domain();
    if (!dom || !mem_domain_permits(dom, fn, 2, MEM_X)) return KO_FAIL;
    ri_slot_t *s = slot_for(name, true);
    if (!s) return KO_FAIL;
    s->fn = fn;
    s->arg = arg;
    devirq_attach(s->line, ri_stub, s);
    return KO_OK;
}

long radio_ints_on(uint32_t mask) {
    for (int i = 0; i < RI_SLOTS; i++) {
        ri_slot_t *s = &g_ri[i];
        if (s->used && s->name < 32 && (mask & (1u << s->name)) && s->fn) {
            arch_irq_enable(s->line);          /* level, priority, controller enable, mie */
            esp32c6_irq_unmask(s->line);
        }
    }
    return KO_OK;
}

long radio_ints_off(uint32_t mask) {
    for (int i = 0; i < RI_SLOTS; i++) {
        ri_slot_t *s = &g_ri[i];
        if (s->used && s->name < 32 && (mask & (1u << s->name))) esp32c6_irq_mask(s->line);
    }
    return KO_OK;
}

/* The interrupt thread's wait. out = { fn, arg, slot } copied to its own memory. */
long radio_isr_wait(uintptr_t out) {
    g_ri_waiter = sched_current_pid();
    for (;;) {
        task_prepare_block();
        for (int i = 0; i < RI_SLOTS; i++) {
            ri_slot_t *s = &g_ri[i];
            if (s->used && s->pending) {
                s->pending = false;
                task_wait_done();
                uintptr_t rec[3] = { s->fn, s->arg, (uintptr_t)i };
                return copy_to_user(out, rec, sizeof rec) < 0 ? KO_FAIL : KO_OK;
            }
        }
        task_block();
    }
}

long radio_isr_done(uint32_t slot) {
    if (slot >= RI_SLOTS || !g_ri[slot].used) return KO_FAIL;
    esp32c6_irq_unmask(g_ri[slot].line);
    return KO_OK;
}

void radio_intr_report(void) {
    for (int i = 0; i < RI_SLOTS; i++)
        if (g_ri[i].used)
            cprintf("radio irq slot %d: blob #%u -> line %u, fired %u times%s\n", i, (unsigned)g_ri[i].name,
                    (unsigned)g_ri[i].line, (unsigned)g_ri[i].fired, g_ri[i].fn ? "" : " (no handler)");
}
#endif
