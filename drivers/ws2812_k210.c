/*
 * LugalOS driver: the WS2812-class LED on the K210 -- 65.6, variant B.
 *
 * Early Maix Bit boards (the attached one: CH340 serial at 1a86:7523,
 * no MEMS mic, rev ~1.0) carry a single WS2812-class addressable LED
 * instead of the 1.09 schematic's three-GPIO common-anode RGB.
 * Evidence from the board: static GPIO levels latch it to a stuck
 * color -- exactly how a DIN line full of level edges behaves on a
 * NeoPixel part. This backend speaks the part's real protocol.
 *
 * Method: cycle-counted bit-bang with M-mode interrupts masked, the
 * same approach the vendor's arduino-k210 NoePixelWrap takes
 * (_k210Show derives cycle counts from SYSCTL_CLOCK_CPU). The
 * drivers/ws2812.c header's "never bit-bang" rule was written for
 * cores executing from flash, where a cache miss lands inside a bit;
 * this kernel runs from SRAM with the caches off, and the frame runs
 * with interrupts masked, so nothing stretches a pulse. One frame is
 * 24 bits * 1.25 us plus the >=50 us latch -- about 80 us of masked
 * time, and the only callers are the shell's `led` command and the
 * probe loop.
 *
 * Timing is CALIBRATED against the CLINT mtime clock (7.8125 MHz,
 * 65.1's measurement), so no CPU-frequency constant appears anywhere.
 *
 * The pin is not fixed by any schematic we trust: `ledpin <io>` picks
 * it at runtime and `ledprobe` finds it. The pin choice is written
 * only from shell task context (single task: shell); the frame itself
 * is guarded by spin_lock_irqsave (one frame in flight, and IRQs off
 * for its duration).
 */

#include "drivers/ws2812.h"
#include "kernel/lock.h"
#include <stdbool.h>
#include <stdint.h>

/* The SiFive-shaped GPIO block and the FPIOA; drivers/gpio.c's comment
 * tells the story of the pad word (power-on, full drive -- and no pull
 * candidate: a NeoPixel DIN must not be biased, and the vendor headers
 * disagree about which pull bits those are anyway). */
#define K210_GPIO_BASE      0x50200000UL
#define GPIO_DATA           (K210_GPIO_BASE + 0x00u)
#define GPIO_DIRECTION      (K210_GPIO_BASE + 0x04u)
#define GPIO_SOURCE         (K210_GPIO_BASE + 0x08u)
#define K210_FPIOA_BASE     0x502B0000UL
#define FPIOA_PAD_CFG(p)    (K210_FPIOA_BASE + 4u * (uint32_t)(p))
#define K210_CLINT_MTIME      0x0200BFF8UL   /* low word; the 100k-iter window wraps nothing */

#define REG(addr)           (*(volatile uint32_t *)(uintptr_t)(addr))

#define SPIN_BODY           __asm__ volatile("nop\n\tnop\n\tnop\n\tnop")

/* single task: shell (`ledpin` writes, `led` reads the same value). */
static int g_ws_pin = -1;
/* accepted: written once by calibrate(), read-only afterwards. */
static bool g_calibrated;
static uint32_t g_bit_total;   /* spin-body iterations in one 1.25 us bit */
static uint32_t g_one_high;    /* iterations of high time for a 1 (0.8 us) */
static uint32_t g_zero_high;   /* iterations of high time for a 0 (~0.4 us) */
static uint32_t g_latch;       /* iterations of >=50 us low, padded to 100 us */

/* guarded by: spin_lock_irqsave at ws2812_set(); the register writes
 * are one pad's latch/direction words, shared with drivers/gpio.c --
 * whoever holds the GPIO_DATA word for the frame owns the pin. */
static spinlock_t g_frame_lock = SPINLOCK_INIT;

static void calibrate(void) {
    /* Time the exact spin body against mtime: 7.8125 ticks per us.
     * The loop cannot be hoisted: the asm volatile is inside it. */
    uint32_t t0 = REG(K210_CLINT_MTIME);
    for (uint32_t i = 0; i < 100000u; i++) { SPIN_BODY; }
    uint32_t ticks = (uint32_t)((uint32_t)REG(K210_CLINT_MTIME) - t0);
    if (ticks == 0) ticks = 1;
    /* 100k iterations took ticks/7.8125 microseconds, so
     * iters_per_us = 100000 * 7.8125 / ticks = 781250 / ticks. */
    uint64_t iters_us = 781250ull / ticks;
    if (iters_us < 80u) iters_us = 80u;                 /* pathological: stay in range */
    if (iters_us > 1000000ull) iters_us = 1000000ull;   /* ticks read went sideways */
    g_bit_total  = (uint32_t)(iters_us + iters_us / 4u);          /* 1.25 us */
    g_one_high   = (uint32_t)(iters_us * 8u / 10u);               /* 0.80 us */
    g_zero_high  = (uint32_t)(iters_us * 4u / 10u);               /* 0.40 us */
    g_latch      = (uint32_t)(iters_us * 100u);                   /* 100 us  */
    g_calibrated = true;
}

bool ws2812_init(void) {
    calibrate();
    return g_ws_pin >= 0;
}

void k210_ws2812_pin(int pin) {
    g_ws_pin = (pin >= 0 && pin < 32) ? pin : -1;
    if (g_ws_pin < 0) return;
    if (!g_calibrated) calibrate();
    REG(FPIOA_PAD_CFG((uint32_t)pin)) = (56u + (uint32_t)pin) | ((1u << 16) | (3u << 5));
    REG(GPIO_SOURCE) &= ~(1u << pin);          /* driven by the latch, not PWMP */
    REG(GPIO_DATA) &= ~(1u << pin);            /* low before the pad becomes out */
    REG(GPIO_DIRECTION) |= (1u << pin);
}

int k210_ws2812_getpin(void) { return g_ws_pin; }

bool ws2812_set(uint8_t r, uint8_t g, uint8_t b) {
    int pin = g_ws_pin;
    if (pin < 0) return false;
    if (!g_calibrated) calibrate();
    const uint32_t mask = 1u << pin;
    const uint32_t bits = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;

    uintptr_t flags = spin_lock_irqsave(&g_frame_lock);
    for (int i = 23; i >= 0; i--) {
        const uint32_t high = (bits & (1u << i)) ? g_one_high : g_zero_high;
        REG(GPIO_DATA) |= mask;
        for (uint32_t n = 0; n < high; n++) { SPIN_BODY; }
        REG(GPIO_DATA) &= ~mask;
        for (uint32_t n = high; n < g_bit_total; n++) { SPIN_BODY; }
    }
    for (uint32_t n = 0; n < g_latch; n++) { SPIN_BODY; }   /* latch, pin low */
    spin_unlock_irqrestore(&g_frame_lock, flags);
    return true;
}
