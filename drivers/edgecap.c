/* See drivers/include/drivers/edgecap.h for what this is and why it is shared
 * rather than part of the DCF-77 driver. */

#include "drivers/edgecap.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <stddef.h>

#if defined(CONFIG_BOARD_RP2350)
#include "kernel/devirq.h"
#include "arch/trap.h"

/* Confirmed against the Pico SDK's own hardware/regs/io_bank0.h rather than
 * derived from the datasheet's prose -- the same discipline the Hazard3 IRQ
 * work in arch/riscv/common/trap.c used, and for the same reason: this tree
 * has gotten RP2350 register layouts wrong before by not doing that. */
#define IO_BANK0_BASE        0x40028000UL
#define IO_BANK0_INTR(n)     (*(volatile uint32_t *)(IO_BANK0_BASE + 0x230u + (n) * 4u))
#define IO_BANK0_INTE(n)     (*(volatile uint32_t *)(IO_BANK0_BASE + 0x248u + (n) * 4u))
#define IO_BANK0_INTS(n)     (*(volatile uint32_t *)(IO_BANK0_BASE + 0x278u + (n) * 4u))
#define SIO_GPIO_IN          (*(volatile uint32_t *)0xD0000004UL)

/* Four bits per pin, eight pins per register: LEVEL_LOW, LEVEL_HIGH,
 * EDGE_LOW, EDGE_HIGH. Only the two edge bits are used here, and they are
 * write-1-to-clear in INTR -- the level bits are not, which is why nothing
 * writes them. */
#define EDGE_LOW_BIT   0x4u
#define EDGE_HIGH_BIT  0x8u
#define EDGE_BOTH      (EDGE_LOW_BIT | EDGE_HIGH_BIT)

#define IO_IRQ_BANK0 21u
#endif

#define EDGECAP_MAX 4
static edgecap_t *g_reg[EDGECAP_MAX];
static uint32_t   g_count;

void edgecap_push(edgecap_t *e, uint64_t t_us, bool level) {
    if (!e || !e->buf || e->cap < 2) return;
    uint16_t next = (uint16_t)((e->head + 1u) % e->cap);
    e->total++;
    if (next == e->tail) {
        /* Full. The *new* edge is dropped rather than the oldest overwritten,
         * so what survives is a contiguous run and not a series with a hole
         * in the middle -- a decoder walking edges in order can work with a
         * short history and cannot work with a discontinuous one. The count
         * is what says it happened. */
        e->dropped++;
        return;
    }
    e->buf[e->head].t_us  = t_us;
    e->buf[e->head].level = level ? 1u : 0u;
    e->head = next;
}

bool edgecap_pop(edgecap_t *e, edge_t *out) {
    if (!e || !out || !e->buf) return false;
    if (e->tail == e->head) return false;
    *out = e->buf[e->tail];
    e->tail = (uint16_t)((e->tail + 1u) % e->cap);
    return true;
}

uint32_t edgecap_dropped(const edgecap_t *e) { return e ? e->dropped : 0u; }
uint32_t edgecap_total(const edgecap_t *e)   { return e ? e->total   : 0u; }

#if defined(CONFIG_BOARD_RP2350)
/* One handler for the whole bank, because there is one interrupt for the whole
 * bank. It reads the clock once and gives every pin that fired the same
 * timestamp: they are within microseconds of each other by construction, and a
 * second read would be a second answer to the same question. */
static void edgecap_isr(void *ctx) {
    (void)ctx;
    uint64_t now = time_get_us();
    uint32_t levels = SIO_GPIO_IN;

    for (uint32_t r = 0; r < 4u; r++) {
        uint32_t status = IO_BANK0_INTS(r);
        if (!status) continue;
        /* Cleared before dispatching, not after: an edge arriving while this
         * handler runs must set the bit again rather than be swallowed by a
         * clear that happens later and covers it. */
        IO_BANK0_INTR(r) = status & 0x88888888u;   /* edge bits only */

        for (uint32_t slot = 0; slot < 8u; slot++) {
            if (!(status & (EDGE_BOTH << (slot * 4u)))) continue;
            uint32_t gpio = r * 8u + slot;
            for (uint32_t i = 0; i < g_count; i++) {
                edgecap_t *e = g_reg[i];
                if (e && e->active && e->gpio == gpio) {
                    edgecap_push(e, now, (levels >> gpio) & 1u);
                    break;
                }
            }
        }
    }
}

static bool g_irq_ready;
#endif

int edgecap_attach(edgecap_t *e, uint8_t gpio, edge_t *storage, uint16_t cap) {
    if (!e || !storage || cap < 2u) return -1;
    if (g_count >= EDGECAP_MAX) return -1;
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_reg[i] && g_reg[i]->active && g_reg[i]->gpio == gpio) {
            printk("[EdgeCap] GP%u already captured; refusing a second owner\n",
                   (unsigned)gpio);
            return -1;
        }
    }

    e->buf = storage; e->cap = cap;
    e->head = e->tail = 0; e->dropped = e->total = 0;
    e->gpio = gpio; e->active = true;
    g_reg[g_count++] = e;

#if defined(CONFIG_BOARD_RP2350)
    if (!g_irq_ready) {
        if (devirq_attach(IO_IRQ_BANK0, edgecap_isr, NULL) != 0) return -1;
        arch_irq_enable(IO_IRQ_BANK0);
        g_irq_ready = true;
    }
    uint32_t r = gpio / 8u, shift = (gpio % 8u) * 4u;
    IO_BANK0_INTR(r) = EDGE_BOTH << shift;   /* discard anything already latched */
    IO_BANK0_INTE(r) |= EDGE_BOTH << shift;
#endif
    return 0;
}

/* --------------------------------------------------------- selftest --- */

void edgecap_selftest(void) {
    int pass = 0, fail = 0;
    #define CHECK(cond, what) do { \
        if (cond) { pass++; cprintf("  [ok] %s\n", what); } \
        else      { fail++; cprintf("  [FAIL] %s\n", what); } \
    } while (0)

    cprintf("\nEdge capture ring (P3)\n");

    edge_t storage[4];
    edgecap_t e;
    /* Built by hand rather than through edgecap_attach(), which would claim a
     * pin and an interrupt this test has no business touching. The ring is
     * the part with the off-by-one in it and it is entirely portable. */
    e.buf = storage; e.cap = 4; e.head = e.tail = 0;
    e.dropped = e.total = 0; e.gpio = 0xFF; e.active = false;

    edge_t got;
    CHECK(!edgecap_pop(&e, &got), "an empty ring yields nothing");

    edgecap_push(&e, 1000, true);
    edgecap_push(&e, 2000, false);
    CHECK(edgecap_pop(&e, &got) && got.t_us == 1000 && got.level == 1,
          "the first edge out is the first edge in");
    CHECK(edgecap_pop(&e, &got) && got.t_us == 2000 && got.level == 0,
          "and the second is the second");
    CHECK(!edgecap_pop(&e, &got), "then the ring is empty again");

    /* A ring of n holds n-1: one slot always separates head from tail, which
     * is what makes "empty" and "full" distinguishable without a count. */
    e.head = e.tail = 0; e.dropped = e.total = 0;
    for (int i = 0; i < 3; i++) edgecap_push(&e, (uint64_t)(i + 1), true);
    CHECK(edgecap_dropped(&e) == 0, "a ring of 4 accepts 3 edges");
    edgecap_push(&e, 99, true);
    CHECK(edgecap_dropped(&e) == 1, "the 4th is dropped, and counted");
    CHECK(edgecap_total(&e) == 4, "every arrival is counted, dropped or not");
    CHECK(edgecap_pop(&e, &got) && got.t_us == 1,
          "the oldest survived -- the NEW edge was dropped, not the old one");

    /* Wrapping. The indices are modular, so a ring that has wrapped several
     * times must still hand back what was put in most recently. */
    e.head = e.tail = 0; e.dropped = e.total = 0;
    for (int i = 0; i < 20; i++) {
        edgecap_push(&e, (uint64_t)(100 + i), (i & 1) == 0);
        edgecap_pop(&e, &got);
    }
    CHECK(got.t_us == 119, "after 20 push/pop pairs the last one out is the last one in");

    cprintf("%s (%d passed, %d failed)\n",
            fail ? "EDGECAP_SELFTEST_FAIL" : "EDGECAP_SELFTEST_OK", pass, fail);
    #undef CHECK
}
