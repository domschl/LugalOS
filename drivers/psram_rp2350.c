/*
 * The RP2350's QSPI PSRAM -- 38.2, plan/phase38_psram.md.
 *
 * The RP2350-LCD-7 carries an APS6404-class QSPI PSRAM on the same QSPI bus
 * as its flash, selected by GP0 as the QMI's second chip select (XIP_CS1).
 * It is **8 MB** (KGD 0x5D, EID 0x53), not the 2 MB the board file used to
 * say: found by aliasing in the preliminaries, checked again at every boot.
 * Once window 1 is programmed it is plain memory:
 *
 *   0x11000000  cached, through the 16 KB XIP cache shared with flash
 *   0x15000000  uncached, the same chip
 *
 * Measured at 72 MHz SCK (plan/phase38_preliminaries.md §2): ~25 MB/s
 * sequential read (flash is 19.5), 31 MB/s uncached write, ~500 ns for a
 * missed load against ~35 for SRAM. Storage, not a frame buffer.
 *
 * ## The hardware rules (plan §2), stated where the next reader looks
 *
 * H1  Two aliases. Write through one and read through the other only after
 *     cleaning (cached -> uncached) or invalidating (uncached -> cached) the
 *     range; the bulk zone (38.4) hands out cached addresses.
 * H2  A flash write turns XIP off: PSRAM is unreachable while it runs, and
 *     its dirty lines are lost unless cleaned first. drivers/flash_rp2350.c
 *     cleans the cache and restores window 1 (38.1); nothing may touch PSRAM
 *     inside that section -- no interrupt handler, not core 1 (PIO-USB keeps
 *     its data in SRAM), no DMA. A flash write's source buffer is SRAM.
 * H3  A full cache clean is 2 048 maintenance writes: fine per flash write,
 *     not per sector.
 * H4  Code can execute from here like from flash; whether U-mode images may,
 *     under PMP, is 38.7's measurement.
 * H5  No cache besides the XIP cache, no instruction cache on Hazard3: code
 *     written here needs only the fence.i the ELF loader issues.
 *
 * ## Bring-up, from the spike and the SDK
 *
 * Direct mode on CS1 at a slow divider: leave QPI first (a warm reset finds
 * the chip still in it), read the ID in SPI mode, reset, enter QPI. Then
 * window 1's read/write formats (EBh quad read with 6 wait cycles, 38h quad
 * write) and its timing, by pico-sdk's arithmetic (rp_pico_alloc.c) for the
 * chip's 8 us maximum CS-low time and 18 ns minimum deselect. While
 * DIRECT_CSR.EN is set the QMI stalls memory-mapped accesses -- flash
 * included -- so the direct-mode code is in .ramfunc and runs with
 * interrupts off.
 *
 * Register provenance: pico-sdk 2.x hardware_regs (qmi.h, xip.h,
 * io_bank0.h, pads_bank0.h) and the Waveshare demo's psram_tool.c.
 */

#include "lugalos_config.h"
#include "drivers/psram_rp2350.h"   /* outside the guard: never an empty unit */

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PSRAM_BYTES)

#include "drivers/flash_rp2350.h"
#include "arch/rp2350_clocks.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/palloc.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if !defined(CONFIG_PSRAM_CS_GPIO)
#error "CONFIG_PSRAM_BYTES without CONFIG_PSRAM_CS_GPIO: the board file names a PSRAM but not its chip select"
#endif

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define QMI_BASE          0x400d0000UL
#define QMI_DIRECT_CSR    (QMI_BASE + 0x00)
#define QMI_DIRECT_TX     (QMI_BASE + 0x04)
#define QMI_DIRECT_RX     (QMI_BASE + 0x08)
#define QMI_M1_TIMING     (QMI_BASE + 0x20)
#define QMI_M1_RFMT       (QMI_BASE + 0x24)
#define QMI_M1_RCMD       (QMI_BASE + 0x28)
#define QMI_M1_WFMT       (QMI_BASE + 0x2c)
#define QMI_M1_WCMD       (QMI_BASE + 0x30)

#define CSR_EN            (1u << 0)
#define CSR_BUSY          (1u << 1)
#define CSR_ASSERT_CS1N   (1u << 3)
#define CSR_TXEMPTY       (1u << 11)
#define CSR_CLKDIV(d)     ((uint32_t)(d) << 22)
#define TX_OE             (1u << 19)
#define TX_IWIDTH_Q       (2u << 16)

#define XIP_CTRL          0x400c8000UL
#define XIP_CTRL_WRITABLE_M1 (1u << 11)
#define XIP_MAINT_BASE    0x18000000UL

#define FLASH_CACHED      0x10000000UL
#define FLASH_UNCACHED    0x14000000UL

#define IO_BANK0_GPIO_CTRL(n) (0x40028000UL + 8u * (n) + 4u)
#define PADS_BANK0_GPIO(n)    (0x40038000UL + 4u + 4u * (n))
#define FUNCSEL_XIP_CS1   9u
/* PUE, 4 mA drive, Schmitt trigger; ISO (bit 8) clear so the pad connects. */
#define PAD_CS1           ((1u << 3) | (1u << 4) | (1u << 1))

#define CMD_QUAD_END      0xF5u
#define CMD_QUAD_ENABLE   0x35u
#define CMD_READ_ID       0x9Fu
#define CMD_RSTEN         0x66u
#define CMD_RST           0x99u
#define CMD_QUAD_READ     0xEBu
#define CMD_QUAD_WRITE    0x38u
#define KGD_PASS          0x5Du

/* The fastest SCK the chip allows: 133 MHz in QPI (APS6404L datasheet).
 * clk_sys / 1 is never safe, so the divider is at least 2 -- at the LCD-7's
 * 144 MHz that is 72 MHz, which is what the preliminaries measured. */
#define PSRAM_SCK_MAX_HZ  133000000u
#define PSRAM_CLKDIV_RAW  ((CONFIG_CLK_SYS_HZ + PSRAM_SCK_MAX_HZ - 1) / PSRAM_SCK_MAX_HZ)
#define PSRAM_CLKDIV      (PSRAM_CLKDIV_RAW < 2 ? 2 : PSRAM_CLKDIV_RAW)
_Static_assert(CONFIG_CLK_SYS_HZ / PSRAM_CLKDIV <= PSRAM_SCK_MAX_HZ,
               "PSRAM SCK above the chip's 133 MHz");
_Static_assert((CONFIG_PSRAM_BYTES & (CONFIG_PSRAM_BYTES - 1)) == 0 &&
               CONFIG_PSRAM_BYTES <= (16u << 20),
               "CONFIG_PSRAM_BYTES must be a power of two within the 16 MB window");

static bool        g_up;
static uint32_t    g_bytes;
static uint8_t     g_kgd, g_eid;
static uint32_t    g_aliased;          /* the size the chip showed by aliasing */
static const char *g_reason = "psram_init() never ran";

/* ---- direct mode: RAM-resident, interrupts off ---------------------------- */

__attribute__((section(".ramfunc"), noinline))
static void dm_wait(void) {
    while (REG(QMI_DIRECT_CSR) & CSR_BUSY) { }
}

__attribute__((section(".ramfunc"), noinline))
static void dm_cmd(uint32_t tx) {
    REG(QMI_DIRECT_CSR) |= CSR_ASSERT_CS1N;
    REG(QMI_DIRECT_TX) = tx;
    while (!(REG(QMI_DIRECT_CSR) & CSR_TXEMPTY)) { }
    dm_wait();
    (void)REG(QMI_DIRECT_RX);
    REG(QMI_DIRECT_CSR) &= ~CSR_ASSERT_CS1N;
    for (volatile int i = 0; i < 50; i++) { }
}

/* Leave QPI if a previous boot entered it (sent quad-wide: a chip in SPI mode
 * sees two stray clocks with CS then released, which it ignores), read the ID
 * in SPI mode, reset the chip, enter QPI. Returns KGD in [7:0], EID in
 * [15:8]. Read ID is 9Fh, three address bytes, then MFID, KGD, EID. */
__attribute__((section(".ramfunc"), noinline))
static uint32_t dm_probe_and_enter_qpi(void) {
    REG(QMI_DIRECT_CSR) = CSR_CLKDIV(30) | CSR_EN;
    dm_wait();
    dm_cmd(TX_OE | TX_IWIDTH_Q | CMD_QUAD_END);

    uint8_t kgd = 0, eid = 0;
    REG(QMI_DIRECT_CSR) |= CSR_ASSERT_CS1N;
    for (int i = 0; i < 7; i++) {
        REG(QMI_DIRECT_TX) = (i == 0) ? CMD_READ_ID : 0xFFu;
        while (!(REG(QMI_DIRECT_CSR) & CSR_TXEMPTY)) { }
        dm_wait();
        uint8_t b = (uint8_t)REG(QMI_DIRECT_RX);
        if (i == 5) kgd = b;
        if (i == 6) eid = b;
    }
    REG(QMI_DIRECT_CSR) &= ~CSR_ASSERT_CS1N;

    if (kgd == KGD_PASS) {
        dm_cmd(CMD_RSTEN);
        dm_cmd(CMD_RST);
        for (volatile int i = 0; i < 2000; i++) { }   /* tRST is 50 ns; generous */
        dm_cmd(CMD_QUAD_ENABLE);
    }
    REG(QMI_DIRECT_CSR) &= ~(CSR_ASSERT_CS1N | CSR_EN);
    return (uint32_t)kgd | ((uint32_t)eid << 8);
}

static uintptr_t irq_off(void) {
    uintptr_t s;
    __asm__ __volatile__("csrrci %0, mstatus, 0x8" : "=r"(s));
    return s;
}

static void irq_back(uintptr_t s) {
    if (s & 0x8) __asm__ __volatile__("csrsi mstatus, 0x8");
}

/* pico-sdk's arithmetic (rp_pico_alloc.c), in femtoseconds: MAX_SELECT is
 * the chip's 8 us CS-low limit in units of 64 clk_sys cycles, MIN_DESELECT
 * its 18 ns less what the divider already gives. */
static uint32_t m1_timing(void) {
    const uint32_t hz = CONFIG_CLK_SYS_HZ, clkdiv = PSRAM_CLKDIV;
    const uint64_t period_fs = 1000000000000000ull / hz;
    uint32_t max_select = (uint32_t)((125ull * 1000000ull) / period_fs);
    int32_t min_deselect = (int32_t)((18ull * 1000000ull + period_fs - 1) / period_fs)
                         - (int32_t)((clkdiv + 1) / 2);
    if (min_deselect < 0) min_deselect = 0;
    uint32_t rxdelay = clkdiv + ((hz / clkdiv > 100000000u) ? 1u : 0u);
    if (max_select > 63) max_select = 63;
    return (1u << 30)               /* COOLDOWN 1 */
         | (2u << 28)               /* PAGEBREAK 1024 */
         | (max_select << 17)
         | ((uint32_t)min_deselect << 12)
         | (rxdelay << 8)
         | (clkdiv & 0xffu);
}

static void m1_program(void) {
    REG(QMI_M1_TIMING) = m1_timing();
    /* prefix 8 bits, address, dummy 24 bits (6 x 4), data: all quad */
    REG(QMI_M1_RFMT) = (2u << 0) | (2u << 2) | (2u << 4) | (2u << 6) | (2u << 8) | (1u << 12) | (6u << 16);
    REG(QMI_M1_RCMD) = CMD_QUAD_READ;
    REG(QMI_M1_WFMT) = (2u << 0) | (2u << 2) | (2u << 4) | (2u << 6) | (2u << 8) | (1u << 12);
    REG(QMI_M1_WCMD) = CMD_QUAD_WRITE;
    REG(XIP_CTRL) |= XIP_CTRL_WRITABLE_M1;
}

/* Writes back every dirty line (by set/way over the top 16 KB of the XIP
 * space: the RP2350-E11 workaround, as pico-sdk's xip_cache_clean_all()).
 * 38.4 replaces this with the shared by-range helpers of H1. */
static void xip_clean_all(void) {
    for (uintptr_t a = 0x04000000UL - 16384u; a < 0x04000000UL; a += 8) {
        *(volatile uint8_t *)(XIP_MAINT_BASE + a + 1u) = 0;
    }
    __asm__ __volatile__("fence" ::: "memory");
}

/* The chip's size by aliasing rather than from the EID's decoding: a
 * distinct word at each power of two, and see where the address bits wrap.
 * A chip that does not hold data at all stops at the first step. */
static uint32_t size_by_aliasing(void) {
    volatile uint32_t *u = (volatile uint32_t *)PSRAM_UNCACHED_BASE;
    u[0] = 0xA5A50000u;
    if (u[0] != 0xA5A50000u) return 0;
    uint32_t sz = 1u << 20;
    for (; sz < (16u << 20); sz <<= 1) {
        u[sz / 4] = 0xA5A50000u | (sz >> 20);
        if (u[0] != 0xA5A50000u) break;   /* wrote over word 0: wrapped */
    }
    u[0] = 0;
    return sz;
}

void psram_init(void) {
    REG(PADS_BANK0_GPIO(CONFIG_PSRAM_CS_GPIO)) = PAD_CS1;
    REG(IO_BANK0_GPIO_CTRL(CONFIG_PSRAM_CS_GPIO)) = FUNCSEL_XIP_CS1;

    uintptr_t s = irq_off();
    uint32_t id = dm_probe_and_enter_qpi();
    if ((uint8_t)id == KGD_PASS) m1_program();
    irq_back(s);
    g_kgd = (uint8_t)id;
    g_eid = (uint8_t)(id >> 8);

    if (g_kgd != KGD_PASS) {
        g_reason = "no PSRAM chip answered its ID read";
        printk("[PSRAM] GP%u -> XIP_CS1: ID read KGD 0x%02x, EID 0x%02x -- no chip\n",
               (unsigned)CONFIG_PSRAM_CS_GPIO, g_kgd, g_eid);
        return;
    }
    g_aliased = size_by_aliasing();
    if (g_aliased < (uint32_t)CONFIG_PSRAM_BYTES) {
        g_reason = "the PSRAM is smaller than the board file says, or does not hold data";
        printk("[PSRAM] KGD 0x%02x EID 0x%02x, but %lu KB by aliasing against %lu KB "
               "configured -- not using it\n", g_kgd, g_eid,
               (unsigned long)(g_aliased / 1024), (unsigned long)(CONFIG_PSRAM_BYTES / 1024));
        return;
    }
    g_bytes = (uint32_t)CONFIG_PSRAM_BYTES;
    g_up = true;
    g_reason = "";
    printk("[PSRAM] %lu KB on GP%u (KGD 0x%02x EID 0x%02x), QPI at %lu MHz (clk_sys/%u)\n",
           (unsigned long)(g_bytes / 1024), (unsigned)CONFIG_PSRAM_CS_GPIO, g_kgd, g_eid,
           (unsigned long)(CONFIG_CLK_SYS_HZ / PSRAM_CLKDIV / 1000000u), (unsigned)PSRAM_CLKDIV);
}

bool psram_is_up(void) { return g_up; }
uint32_t psram_bytes(void) { return g_up ? g_bytes : 0; }

void psram_require(void) {
    if (g_up) return;
    for (;;) {
        cprintf("\n[PSRAM] HALTED: this persona is built for its %lu KB PSRAM, and %s "
                "(KGD 0x%02x). Nothing else will start (plan/phase38_psram.md, S1). "
                "The board can still be reflashed over USB.\n",
                (unsigned long)(CONFIG_PSRAM_BYTES / 1024), g_reason, g_kgd);
        task_sleep_ms(5000);
    }
}

int psram_meminfo(char *buf, uint32_t cap) {
    if (!g_up) return ksnprintf(buf, cap, "PSRAM: not up (%s)\n", g_reason);
    return ksnprintf(buf, cap, "PSRAM: %lu KB at 0x%08lx, QPI %lu MHz\n",
                     (unsigned long)(g_bytes / 1024), (unsigned long)PSRAM_CACHED_BASE,
                     (unsigned long)(CONFIG_CLK_SYS_HZ / PSRAM_CLKDIV / 1000000u));
}

/* ---- psram test ------------------------------------------------------------ */

static uint32_t pat(uint32_t i, uint32_t salt) {
    uint32_t x = i * 2654435761u + salt;
    return x ^ (x >> 15);
}

static uint32_t test_pass(uintptr_t wbase, uintptr_t rbase, uint32_t salt, bool clean) {
    uint32_t words = g_bytes / 4, bad = 0;
    volatile uint32_t *w = (volatile uint32_t *)wbase;
    volatile uint32_t *r = (volatile uint32_t *)rbase;
    for (uint32_t i = 0; i < words; i++) w[i] = pat(i, salt);
    if (clean) xip_clean_all();
    for (uint32_t i = 0; i < words; i++) {
        uint32_t v = r[i], e = pat(i, salt);
        if (v != e && bad++ < 4) {
            cprintf("  [%06lx] 0x%08lx != 0x%08lx\n", (unsigned long)(i * 4),
                    (unsigned long)v, (unsigned long)e);
        }
    }
    return bad;
}

/* H2's test: 4 KB written through the cache and left dirty, then the flash
 * write's XIP exit and re-entry (without a write), then read uncached. Before
 * 38.1 1008 of these 1024 words were lost. */
static uint32_t flash_path_pass(void) {
    const uint32_t n = 1024, off = 0x1F0000u;
    volatile uint32_t *c = (volatile uint32_t *)(PSRAM_CACHED_BASE + off);
    volatile uint32_t *u = (volatile uint32_t *)(PSRAM_UNCACHED_BASE + off);
    for (uint32_t i = 0; i < n; i++) u[i] = 0xDEAD0000u | i;
    for (uint32_t i = 0; i < n; i++) c[i] = pat(i, 0x5555u);
    if (flash_rp2350_xip_cycle() != 0) return n;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) bad += u[i] != pat(i, 0x5555u);
    return bad;
}

/* Destructive over the whole chip: right while nothing lives in PSRAM. 38.4
 * gives it a page allocator, and from then on this tests free pages only. */
static void psram_test(void) {
    uint64_t t0 = time_get_us();
    uint32_t b1 = test_pass(PSRAM_UNCACHED_BASE, PSRAM_UNCACHED_BASE, 0x1111u, false);
    uint32_t b2 = test_pass(PSRAM_CACHED_BASE, PSRAM_UNCACHED_BASE, 0x2222u, true);
    uint32_t b3 = test_pass(PSRAM_CACHED_BASE, PSRAM_CACHED_BASE, 0x4444u, false);
    xip_clean_all();
    uint32_t b4 = flash_path_pass();
    cprintf("psram test: %lu KB, mismatching words: uncached %lu, cached->clean->uncached %lu, "
            "cached %lu; flash path %lu of 1024 (%lu ms) -- %s\n",
            (unsigned long)(g_bytes / 1024), (unsigned long)b1, (unsigned long)b2,
            (unsigned long)b3, (unsigned long)b4,
            (unsigned long)((time_get_us() - t0) / 1000),
            (b1 | b2 | b3 | b4) ? "FAIL" : "PASS");
}

/* ---- psram bench: the instrument for every later milestone ---------------- */

#define BENCH_BYTES (64u * 1024u)

static volatile uint32_t g_sink;

__attribute__((noinline)) static uint32_t rd_words(uintptr_t base, uint32_t bytes) {
    const volatile uint32_t *p = (const volatile uint32_t *)base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4; i += 4) acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
    return acc;
}

__attribute__((noinline)) static void wr_words(uintptr_t base, uint32_t bytes) {
    volatile uint32_t *p = (volatile uint32_t *)base;
    for (uint32_t i = 0; i < bytes / 4; i += 4) { p[i] = i; p[i + 1] = i; p[i + 2] = i; p[i + 3] = i; }
}

__attribute__((noinline)) static void cp_words(uintptr_t dst, uintptr_t src, uint32_t bytes) {
    volatile uint32_t *d = (volatile uint32_t *)dst;
    const volatile uint32_t *s = (const volatile uint32_t *)src;
    for (uint32_t i = 0; i < bytes / 4; i++) d[i] = s[i];
}

static void line(const char *what, uint32_t bytes, uint64_t t0) {
    uint64_t us = time_get_us() - t0;
    if (us == 0) us = 1;
    cprintf("  %-34s %6lu KB/s  (%lu us for %lu KB)\n", what,
            (unsigned long)(((uint64_t)bytes * 1000000ull / 1024ull) / us),
            (unsigned long)us, (unsigned long)(bytes / 1024));
}

/* Pointer chase over `ws` bytes in 64-byte nodes, one random cycle
 * (Sattolo), so every hop is a dependent load to a line not recently used. */
static uint32_t chase_ns(uintptr_t base, uint32_t ws, uint32_t hops) {
    uint32_t n = ws / 64;
    volatile uint32_t *node = (volatile uint32_t *)base;
    for (uint32_t i = 0; i < n; i++) node[i * 16] = i;
    uint32_t rng = 0x12345u;
    for (uint32_t i = n - 1; i > 0; i--) {
        rng = rng * 1664525u + 1013904223u;
        uint32_t j = (rng >> 8) % i;
        uint32_t a = node[i * 16], b = node[j * 16];
        node[i * 16] = b;
        node[j * 16] = a;
    }
    xip_clean_all();
    uint32_t idx = 0;
    uint64_t t0 = time_get_us();
    for (uint32_t h = 0; h < hops; h++) idx = node[idx * 16];
    uint64_t us = time_get_us() - t0;
    g_sink = idx;
    return (uint32_t)(us * 1000u / hops);
}

static void psram_bench(void) {
    const uint32_t pages = BENCH_BYTES / 4096u, B = BENCH_BYTES;
    uint8_t *sram = palloc_pages(pages);
    if (!sram) {
        cprintf("psram bench: no %u pages of SRAM for the reference buffer\n", (unsigned)pages);
        return;
    }
    const uintptr_t s = (uintptr_t)sram;
    /* Two PSRAM areas 256 KB apart, so a pass over one evicts the other. */
    const uintptr_t pc = PSRAM_CACHED_BASE + 0x40000u, pu = PSRAM_UNCACHED_BASE + 0x80000u;
    uint64_t t;
    cprintf("psram bench: clk_sys %lu Hz, SCK clk_sys/%u, %lu KB a pass\n",
            (unsigned long)CONFIG_CLK_SYS_HZ, (unsigned)PSRAM_CLKDIV, (unsigned long)(B / 1024));

    cprintf(" sequential read (32-bit words):\n");
    t = time_get_us(); g_sink = rd_words(s, B);  line("SRAM", B, t);
    t = time_get_us(); g_sink = rd_words(pc, B); line("PSRAM cached (cold, 64 KB > cache)", B, t);
    t = time_get_us(); g_sink = rd_words(pu, B); line("PSRAM uncached", B, t);
    t = time_get_us();
    for (int k = 0; k < 8; k++) g_sink = rd_words(pc, 8192);
    line("PSRAM cached, 8 KB hot x8", 8 * 8192, t);
    t = time_get_us(); g_sink = rd_words(FLASH_CACHED + 0x20000u, B);   line("flash cached (kernel image)", B, t);
    t = time_get_us(); g_sink = rd_words(FLASH_UNCACHED + 0x20000u, B); line("flash uncached", B, t);

    cprintf(" sequential write:\n");
    t = time_get_us(); wr_words(s, B);                  line("SRAM", B, t);
    t = time_get_us(); wr_words(pc, B); xip_clean_all(); line("PSRAM cached (+ clean_all)", B, t);
    t = time_get_us(); wr_words(pu, B);                 line("PSRAM uncached", B, t);

    cprintf(" copy (word loop / libc):\n");
    t = time_get_us(); cp_words(s, pu, B); line("PSRAM unc -> SRAM, words", B, t);
    t = time_get_us(); cp_words(s, pc, B); line("PSRAM cached -> SRAM, words", B, t);
    t = time_get_us(); cp_words(pu, s, B); line("SRAM -> PSRAM unc, words", B, t);
    t = time_get_us(); memcpy(sram, (const void *)pu, B);       line("PSRAM unc -> SRAM, memcpy", B, t);
    t = time_get_us(); memcpy(sram, sram + B / 2, B / 2);        line("SRAM -> SRAM, memcpy", B / 2, t);
    t = time_get_us(); memmove(sram + 4, sram, B / 2);           line("SRAM -> SRAM, memmove (overlap)", B / 2, t);
    t = time_get_us(); memset(sram, 0x5a, B);                    line("SRAM, memset", B, t);

    cprintf(" pointer chase, ns per dependent load (64-byte nodes, random cycle):\n");
    static const uint32_t ws_sram[] = { 4096, 16384, 65536 };
    static const uint32_t ws_ps[]   = { 4096, 16384, 65536, 262144, 1048576 };
    for (unsigned i = 0; i < sizeof ws_sram / sizeof ws_sram[0]; i++)
        cprintf("  SRAM           %5lu KB: %4lu ns\n", (unsigned long)(ws_sram[i] / 1024),
                (unsigned long)chase_ns(s, ws_sram[i], 100000));
    for (unsigned i = 0; i < sizeof ws_ps / sizeof ws_ps[0]; i++)
        cprintf("  PSRAM cached   %5lu KB: %4lu ns\n", (unsigned long)(ws_ps[i] / 1024),
                (unsigned long)chase_ns(PSRAM_CACHED_BASE, ws_ps[i], 100000));
    for (unsigned i = 0; i < sizeof ws_ps / sizeof ws_ps[0]; i++)
        cprintf("  PSRAM uncached %5lu KB: %4lu ns\n", (unsigned long)(ws_ps[i] / 1024),
                (unsigned long)chase_ns(PSRAM_UNCACHED_BASE, ws_ps[i], 100000));

    palloc_free(sram, pages);
}

void psram_command(const char *args) {
    while (*args == ' ') args++;
    if (!g_up) {
        cprintf("psram: not up -- %s (KGD 0x%02x EID 0x%02x)\n", g_reason, g_kgd, g_eid);
        return;
    }
    if (strcmp(args, "test") == 0) {
        psram_test();
    } else if (strcmp(args, "bench") == 0) {
        psram_bench();
    } else if (*args == '\0') {
        cprintf("psram: %lu KB (%lu KB by aliasing) on GP%u, KGD 0x%02x EID 0x%02x\n",
                (unsigned long)(g_bytes / 1024), (unsigned long)(g_aliased / 1024),
                (unsigned)CONFIG_PSRAM_CS_GPIO, g_kgd, g_eid);
        cprintf("       QPI, SCK clk_sys/%u = %lu kHz, M1_TIMING 0x%08lx, writable %s\n",
                (unsigned)PSRAM_CLKDIV, (unsigned long)(CONFIG_CLK_SYS_HZ / PSRAM_CLKDIV / 1000u),
                (unsigned long)REG(QMI_M1_TIMING), (REG(XIP_CTRL) & XIP_CTRL_WRITABLE_M1) ? "yes" : "NO");
        cprintf("       cached 0x%08lx, uncached 0x%08lx; nothing allocates from it yet (38.4)\n",
                (unsigned long)PSRAM_CACHED_BASE, (unsigned long)PSRAM_UNCACHED_BASE);
    } else {
        cprintf("usage: psram [test|bench]\n");
    }
}

#endif /* CONFIG_BOARD_RP2350 && CONFIG_PSRAM_BYTES */
