/*
 * U-mode feasibility probe for the ESP32-C6 -- 45.3, plan/phase45_esp32c6.md.
 *
 * The question: can the Espressif Wi-Fi blob (and the chip-ROM code it calls)
 * run in U-mode under a PMP domain, instead of in M-mode beside the kernel?
 * What this settles *on the silicon*, with no blob involved:
 *
 *   1. how many PMP entries exist, their granularity, and whether the ROM
 *      left any locked (a locked entry also binds M-mode, and could not be
 *      undone short of a reset);
 *   2. that a U-mode task confined to a few NAPOT regions really faults on a
 *      load/store/fetch outside them (mcause 5/7/1), and runs inside them;
 *   3. that U-mode can read and write a granted MMIO page, call a chip-ROM
 *      function (here ets_delay_us), and touch the ROM's own data at the top
 *      of SRAM -- the exact things the blob does;
 *   4. which CSRs U-mode can reach: 0x802 (the user alias of the cycle
 *      counter, which the ROM reads) versus 0x7e2 (the M-mode one).
 *
 * Each test is a function in .utext run by run_user() (the entry stub); a
 * U-mode trap unwinds back here with the cause, so a fault is just a result.
 * Standalone, loaded to HP SRAM by tools/c6run.py; flash is not written.
 */

#include "c6_standalone.h"

extern uint32_t run_user(void (*entry)(void), uint32_t user_sp, uint32_t arg);
extern void trap_entry(void);
extern char _utext_start[], _ustack_start[], _ustack_end[];

/* [cause, mepc, mtval, user a0, user sp], filled in by trap_entry. */
volatile uint32_t g_trap[5];

/* A word in kernel (M-mode) data that no U-mode grant covers. */
volatile uint32_t g_kernel_secret = 0x5ec2e7u;

/* ---- PMP ---------------------------------------------------------------- */

#define PMP_R (1u << 0)
#define PMP_W (1u << 1)
#define PMP_X (1u << 2)
#define PMP_NAPOT (3u << 3)
#define PMP_L (1u << 7)

#define W_ADDR(n) case n: __asm__ volatile("csrw pmpaddr" #n ", %0" :: "r"(v)); break;
#define R_ADDR(n) case n: __asm__ volatile("csrr %0, pmpaddr" #n : "=r"(v)); break;
#define ALL16(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15)
static void wr_addr(int i, uint32_t v) { switch (i) { ALL16(W_ADDR) } }
static uint32_t rd_addr(int i) { uint32_t v = 0; switch (i) { ALL16(R_ADDR) } return v; }

static void wr_cfgreg(int r, uint32_t v) {
    switch (r) {
    case 0: __asm__ volatile("csrw pmpcfg0, %0" :: "r"(v)); break;
    case 1: __asm__ volatile("csrw pmpcfg1, %0" :: "r"(v)); break;
    case 2: __asm__ volatile("csrw pmpcfg2, %0" :: "r"(v)); break;
    case 3: __asm__ volatile("csrw pmpcfg3, %0" :: "r"(v)); break;
    }
}
static uint32_t rd_cfgreg(int r) {
    uint32_t v = 0;
    switch (r) {
    case 0: __asm__ volatile("csrr %0, pmpcfg0" : "=r"(v)); break;
    case 1: __asm__ volatile("csrr %0, pmpcfg1" : "=r"(v)); break;
    case 2: __asm__ volatile("csrr %0, pmpcfg2" : "=r"(v)); break;
    case 3: __asm__ volatile("csrr %0, pmpcfg3" : "=r"(v)); break;
    }
    return v;
}
static uint8_t rd_cfg(int i) { return (rd_cfgreg(i / 4) >> (8 * (i % 4))) & 0xff; }

static int pmp_next;                       /* next free entry for this test */
static uint32_t cfg_shadow[4];

static void pmp_clear_ours(int first) {
    for (int i = first; i < 16; i++) {
        uint8_t c = rd_cfg(i);
        if (c & PMP_L) continue;           /* locked: cannot, and must not try */
        cfg_shadow[i / 4] = rd_cfgreg(i / 4) & ~(0xffu << (8 * (i % 4)));
        wr_cfgreg(i / 4, cfg_shadow[i / 4]);
    }
}

/* Grant [base, base+size) with `perms`, size a power of two >= 8, aligned. */
static int pmp_grant(uint32_t base, uint32_t size, uint32_t perms) {
    while (pmp_next < 16 && (rd_cfg(pmp_next) & PMP_L)) pmp_next++;
    if (pmp_next >= 16) return -1;
    int i = pmp_next++;
    wr_addr(i, (base >> 2) | ((size >> 3) - 1));
    uint32_t r = rd_cfgreg(i / 4);
    r &= ~(0xffu << (8 * (i % 4)));
    r |= (PMP_NAPOT | perms) << (8 * (i % 4));
    wr_cfgreg(i / 4, r);
    return i;
}

/* ---- the U-mode tests --------------------------------------------------- */

#define UTEXT __attribute__((section(".utext"), noinline, used))

static inline __attribute__((always_inline)) void ucall(uint32_t v) {
    register uint32_t a0 __asm__("a0") = v;
    register uint32_t a7 __asm__("a7") = 0;          /* not the fast-path ping (0x77) */
    __asm__ volatile("ecall" :: "r"(a0), "r"(a7) : "memory");
    for (;;) { }
}

#define ROM_ETS_DELAY_US  0x40000040u       /* esp32c6.rom.ld: ets_delay_us (a jal trampoline) */
#define ROM_G_OSI_FUNCS_P 0x4087ff6cu       /* the blob's own ROM data pointer */

UTEXT void u_hello(uint32_t arg)        { ucall(0x600d0000u | arg); }
UTEXT void u_load_denied(uint32_t arg) { ucall(*(volatile uint32_t *)arg); }
UTEXT void u_store_denied(uint32_t arg){ *(volatile uint32_t *)arg = 1; ucall(0xbad); }
UTEXT void u_store_ok(uint32_t arg) {
    volatile uint32_t *p = (volatile uint32_t *)arg;       /* inside .ustack */
    *p = 0xc0ffee; ucall(*p);
}
UTEXT void u_mmio(uint32_t arg) {                          /* write an MMIO register, read one back */
    (void)arg;
    *(volatile uint32_t *)SYSTIMER_UNIT0_OP = 1u << 30;    /* UPDATE */
    ucall(*(volatile uint32_t *)SYSTIMER_UNIT0_LO | 1u);
}
UTEXT void u_rom_call(uint32_t arg) {                      /* call a chip-ROM function, time it with the user CSR */
    uint32_t c0, c1;
    __asm__ volatile("csrr %0, 0x802" : "=r"(c0));
    ((void (*)(uint32_t))ROM_ETS_DELAY_US)(arg);
    __asm__ volatile("csrr %0, 0x802" : "=r"(c1));
    ucall(c1 - c0);
}
UTEXT void u_rom_data(uint32_t arg) { (void)arg; ucall(*(volatile uint32_t *)ROM_G_OSI_FUNCS_P | 0x80000000u); }
UTEXT void u_csr_user(uint32_t arg) { (void)arg; uint32_t v; __asm__ volatile("csrr %0, 0x802" : "=r"(v)); ucall(v | 1u); }
UTEXT void u_csr_machine(uint32_t arg) { (void)arg; uint32_t v; __asm__ volatile("csrr %0, 0x7e2" : "=r"(v)); ucall(v); }
UTEXT void u_ping_loop(uint32_t n) {                       /* n round trips of the fast-path ecall */
    uint32_t c0, c1;
    __asm__ volatile("csrr %0, 0x802" : "=r"(c0));
    for (uint32_t i = 0; i < n; i++) {
        register uint32_t a7 __asm__("a7") = 0x77;
        __asm__ volatile("ecall" :: "r"(a7) : "memory");
    }
    __asm__ volatile("csrr %0, 0x802" : "=r"(c1));
    ucall(c1 - c0);
}
UTEXT void u_call_loop(uint32_t n) {                       /* the same loop around a plain function call: the M-mode (no U) baseline */
    uint32_t c0, c1;
    __asm__ volatile("csrr %0, 0x802" : "=r"(c0));
    for (uint32_t i = 0; i < n; i++) __asm__ volatile("jal ra, 1f\n j 2f\n 1: ret\n 2:" ::: "ra", "memory");
    __asm__ volatile("csrr %0, 0x802" : "=r"(c1));
    ucall(c1 - c0);
}
UTEXT void u_exec_denied(uint32_t arg) { ((void (*)(void))arg)(); ucall(0xbad); }
UTEXT void u_privileged(uint32_t arg)  { (void)arg; __asm__ volatile("csrr zero, mstatus"); ucall(0xbad); }

/* ---- harness ------------------------------------------------------------ */

enum { G_TEXT = 1, G_STACK = 2, G_SYSTIMER = 4, G_ROM = 8, G_ROMDATA = 16 };
#define NO_CAUSE 0xffffffffu
#define ECALL_U  8u

static int g_pass, g_fail;
#define CPU_MHZ 160

static void run(const char *name, void (*entry)(uint32_t), uint32_t grants, uint32_t arg,
                uint32_t expect_cause, uint32_t expect_a0, const char *note) {
    pmp_next = 0;
    pmp_clear_ours(0);
    if (grants & G_TEXT)     pmp_grant((uint32_t)(uintptr_t)_utext_start, 4096, PMP_R | PMP_X);
    if (grants & G_STACK)    pmp_grant((uint32_t)(uintptr_t)_ustack_start, 4096, PMP_R | PMP_W);
    if (grants & G_SYSTIMER) pmp_grant(0x6000A000u, 4096, PMP_R | PMP_W);
    if (grants & G_ROM)      pmp_grant(0x40000000u, 0x80000, PMP_R | PMP_X);
    if (grants & G_ROMDATA)  pmp_grant(0x4087c000u, 0x4000, PMP_R | PMP_W);   /* the ROM's working data: bootloader_usable_dram_end is 0x4087c610 */
    __asm__ volatile("fence.i");
    uint32_t sp = (uint32_t)(uintptr_t)_ustack_end;
    uint32_t cause = run_user((void (*)(void))entry, sp, arg);
    pmp_clear_ours(0);

    bool ok = cause == expect_cause && (expect_a0 == NO_CAUSE || g_trap[3] == expect_a0
                                        || (expect_a0 == 0 && g_trap[3] != 0));
    puts_(ok ? "  PASS " : "  FAIL ");
    puts_(name);
    puts_("  cause="); putdec(cause);
    puts_(" mepc="); puthex(g_trap[1]);
    puts_(" mtval="); puthex(g_trap[2]);
    puts_(" a0="); puthex(g_trap[3]);
    if (note) { puts_("  "); puts_(note); }
    puts_("\n");
    if (ok) g_pass++; else g_fail++;
}

void probe_main(void) {
    REG(SYSTIMER_CONF_REG) |= SYSTIMER_CLK_EN | SYSTIMER_UNIT0_WORK_EN;
    cycles_start();
    __asm__ volatile("csrw mtvec, %0" :: "r"((uint32_t)(uintptr_t)trap_entry));
    uint32_t mtvec_rb = READ_CSR("mtvec");

    for (;;) {
        g_pass = g_fail = 0;
        puts_("\n[C6_UPROBE] ESP32-C6 U-mode feasibility probe\n");

        puts_("mtvec: wrote "); puthex((uint32_t)(uintptr_t)trap_entry); puts_(", reads back "); puthex(mtvec_rb); puts_("\n");
        puts_("PMP as the ROM left it:\n");
        int locked = 0, active = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t c = rd_cfg(i);
            if (c & PMP_L) locked++;
            if (c & 0x18) active++;
            if (c) { puts_("  entry "); putdec(i); puts_(": cfg="); puthex(c); puts_(" addr="); puthex(rd_addr(i)); puts_("\n"); }
        }
        puts_("  active="); putdec(active); puts_(" locked="); putdec(locked); puts_("\n");

        /* Which entries exist: an address register that cannot hold a value is
         * not implemented. Only entries whose cfg is 0 are touched. */
        int impl = 0;
        for (int i = 0; i < 16; i++) {
            if (rd_cfg(i)) { impl++; continue; }
            uint32_t old = rd_addr(i);
            wr_addr(i, 0x12345678u >> 2);
            if (rd_addr(i) != 0) impl++;
            wr_addr(i, old);
        }
        puts_("  implemented (read back non-zero): "); putdec(impl); puts_(" of 16\n");
        wr_addr(15, 0xffffffffu);
        uint32_t g = rd_addr(15);
        wr_addr(15, 0);
        int gbit = 0; while (gbit < 32 && !((g >> gbit) & 1u)) gbit++;
        puts_("  pmpaddr15 after all-ones: "); puthex(g); puts_("  (lowest set bit "); putdec(gbit);
        puts_(" => granularity "); putdec(4u << gbit); puts_(" bytes, if TOR/OFF semantics)\n");

        puts_("U-mode tests:\n");
        uint32_t secret_addr = (uint32_t)(uintptr_t)&g_kernel_secret;
        uint32_t stack_scratch = (uint32_t)(uintptr_t)_ustack_start;      /* bottom of the U stack page */
        uint32_t m_fn = (uint32_t)(uintptr_t)&puts_;                       /* an M-mode function */

        run("hello: U runs and ecalls (text + stack granted)", u_hello, G_TEXT | G_STACK, 7, ECALL_U, 0x600d0007u, 0);
        run("no text grant: U cannot fetch its own first instruction", u_hello, G_STACK, 0, 1, NO_CAUSE, "(cause 1 = instruction access fault)");
        run("load of kernel data", u_load_denied, G_TEXT | G_STACK, secret_addr, 5, NO_CAUSE, "(cause 5 = load access fault)");
        run("store to kernel data", u_store_denied, G_TEXT | G_STACK, secret_addr, 7, NO_CAUSE, "(cause 7 = store access fault)");
        run("store/load inside the granted stack page", u_store_ok, G_TEXT | G_STACK, stack_scratch, ECALL_U, 0xc0ffee, 0);
        run("fetch from M-mode text", u_exec_denied, G_TEXT | G_STACK, m_fn, 1, NO_CAUSE, 0);
        run("MMIO without a grant", u_mmio, G_TEXT | G_STACK, 0, 7, NO_CAUSE, "(the UPDATE store faults)");
        run("MMIO write+read of a granted page (system timer)", u_mmio, G_TEXT | G_STACK | G_SYSTIMER, 0, ECALL_U, 0, "(a0 = timer value | 1)");
        run("ROM call without a ROM grant", u_rom_call, G_TEXT | G_STACK, 100, 1, NO_CAUSE, 0);
        run("ROM call: ets_delay_us(1000) with ROM+ROM-data grants", u_rom_call, G_TEXT | G_STACK | G_ROM | G_ROMDATA, 1000, ECALL_U, 0, "(a0 = cycles measured by the user CSR)");
        run("ROM data (g_osi_funcs_p) readable with a grant", u_rom_data, G_TEXT | G_STACK | G_ROMDATA, 0, ECALL_U, 0, 0);
        run("user CSR 0x802 (cycle counter alias)", u_csr_user, G_TEXT | G_STACK, 0, ECALL_U, 0, 0);
        run("machine CSR 0x7e2 from U-mode", u_csr_machine, G_TEXT | G_STACK, 0, 2, NO_CAUSE, "(cause 2 = illegal instruction)");
        run("csrr mstatus from U-mode", u_privileged, G_TEXT | G_STACK, 0, 2, NO_CAUSE, 0);

        run("ecall round trip, fast path: 10000 calls", u_ping_loop, G_TEXT | G_STACK, 10000, ECALL_U, 0, "(a0 = total cycles)");
        puts_("    => "); putdec(g_trap[3] / 10000u); puts_("."); putdec((g_trap[3] % 10000u) / 1000u);
        puts_(" cycles per U->M->U round trip at "); putdec(CPU_MHZ); puts_(" MHz\n");
        run("plain call/return loop: 10000 calls (baseline)", u_call_loop, G_TEXT | G_STACK, 10000, ECALL_U, 0, "(a0 = total cycles)");
        puts_("    => "); putdec(g_trap[3] / 10000u); puts_("."); putdec((g_trap[3] % 10000u) / 1000u); puts_(" cycles per jump+return\n");

        puts_("[C6_UPROBE] "); putdec(g_pass); puts_(" passed, "); putdec(g_fail); puts_(" failed\n");
        uint32_t t = systimer_now();
        while ((uint32_t)(systimer_now() - t) < 16000000u * 3u) { }      /* repeat for late-attaching monitors */
    }
}
