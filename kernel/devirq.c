#include "kernel/devirq.h"
#include "kernel/printk.h"
#include <stdbool.h>

/* See kernel/include/kernel/devirq.h for the rationale. */

#define DEVIRQ_MAX 8

typedef struct {
    uint32_t          irq_num;
    devirq_handler_fn handler;
    void             *ctx;
    bool              in_use;
} devirq_entry_t;

static devirq_entry_t g_table[DEVIRQ_MAX];
static uint32_t       g_count;

int devirq_attach(uint32_t irq_num, devirq_handler_fn handler, void *ctx) {
    if (!handler) return -1;
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_table[i].in_use && g_table[i].irq_num == irq_num) {
            printk("[DevIRQ] IRQ %u already attached, refusing a second handler\n",
                   irq_num);
            return -1;
        }
    }
    if (g_count >= DEVIRQ_MAX) {
        printk("[DevIRQ] Table full, cannot attach IRQ %u\n", irq_num);
        return -1;
    }
    g_table[g_count].irq_num = irq_num;
    g_table[g_count].handler = handler;
    g_table[g_count].ctx     = ctx;
    g_table[g_count].in_use  = true;
    g_count++;
    return 0;
}

void devirq_dispatch(uint32_t irq_num) {
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_table[i].in_use && g_table[i].irq_num == irq_num) {
            g_table[i].handler(g_table[i].ctx);
            return;
        }
    }
    printk("[DevIRQ] Unhandled external interrupt: IRQ %u\n", irq_num);
}
