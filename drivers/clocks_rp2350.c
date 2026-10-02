/*
 * The RP2350's clock tree, read back and measured -- 36.1,
 * plan/phase36_rp2350_lcd7_terminal.md.
 *
 * The counterpart of esp32p4_clocks_report() (phase 34.1), with the same
 * rule: a frequency is reported from what the registers say and what a
 * counter measures, never from the constant that asked for it. After 36.1
 * clk_sys is a board fact (CONFIG_CLK_SYS_HZ), so there are now two ways to
 * be wrong -- the build asked for the wrong number, or the PLL did not do what
 * boot_header.S asked -- and only a readback plus a measurement tells them
 * apart.
 *
 * Register provenance (RP2350 datasheet, RP-008373-DS-2):
 *   PLL_SYS_BASE 0x40050000: CS +0x0 ([31] LOCK, [5:0] REFDIV), FBDIV_INT
 *     +0x8 ([11:0]), PRIM +0xc ([18:16] POSTDIV1, [14:12] POSTDIV2).
 *   CLOCKS_BASE 0x40010000: CLK_SYS_CTRL +0x3c ([0] SRC: 0 clk_ref, 1 aux;
 *     [7:5] AUXSRC: 0 PLL_SYS, 1 PLL_USB, 2 ROSC, 3 XOSC, 4/5 GPIN0/1 --
 *     reset value 2, i.e. **not** PLL_SYS: boot_header.S sets SRC only and
 *     relies on the bootrom having left AUXSRC at 0, which this reports),
 *     CLK_SYS_DIV +0x40 ([31:16] INT, 0 meaning 65536; [15:0] FRAC).
 *   CLK_REF_CTRL +0x30 ([1:0] SRC: 0 ROSC, 1 aux, 2 XOSC, 3 LPOSC),
 *     CLK_REF_DIV +0x34 ([23:16] INT, 0 meaning 256).
 *   TICKS_BASE 0x40108000: TIMER0_CYCLES +0x1c ([8:0], clk_ref cycles per
 *     tick). TIMER0 TIMERAWL at 0x400B0000 + 0x28 counts those ticks.
 *
 * **The ruler is checked too.** TIMER0 is independent of clk_sys, which is
 * what makes it a fair ruler for mcycle -- but only if its tick really is
 * 1 us, and on the first board this ran on it was 4 us (the bootrom had left
 * CLK_REF_DIV at 4; boot_header.S now sets it). A measurement that assumed
 * 1 MHz reported 576 MHz for a 144 MHz clock. So the tick rate is computed
 * from clk_ref's own registers and the measurement is scaled by it.
 */

#include "arch/rp2350_clocks.h"
#include "drivers/flash_rp2350.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "lugalos_config.h"

#include <stdint.h>

#if defined(CONFIG_BOARD_RP2350)

_Static_assert(RP2350_CLK_SYS_VALID,
               "CONFIG_CLK_SYS_HZ is not a clk_sys PLL_SYS can produce within the datasheet's limits");

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define PLL_SYS_BASE      0x40050000UL
#define PLL_CS            (PLL_SYS_BASE + 0x0)
#define PLL_FBDIV_INT     (PLL_SYS_BASE + 0x8)
#define PLL_PRIM          (PLL_SYS_BASE + 0xc)

#define CLOCKS_BASE       0x40010000UL
#define CLK_SYS_CTRL      (CLOCKS_BASE + 0x3c)
#define CLK_SYS_DIV       (CLOCKS_BASE + 0x40)

#define CLK_REF_CTRL      (CLOCKS_BASE + 0x30)
#define CLK_REF_DIV       (CLOCKS_BASE + 0x34)

#define TICKS_TIMER0_CYCLES (0x40108000UL + 0x1c)
#define TIMER0_TIMERAWL   (0x400B0000UL + 0x28)

static inline uint32_t mcycle32(void) {
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

static const char *auxsrc_name(unsigned a) {
    static const char *const names[] = {
        "PLL_SYS", "PLL_USB", "ROSC", "XOSC", "GPIN0", "GPIN1",
    };
    return a < sizeof(names) / sizeof(names[0]) ? names[a] : "reserved";
}

/* clk_sys as the registers describe it, in Hz; 0 if it is not on PLL_SYS
 * (then the caller says what it is on instead). */
static uint32_t clk_sys_from_registers(void) {
    uint32_t ctrl = REG(CLK_SYS_CTRL);
    if ((ctrl & 1u) == 0 || ((ctrl >> 5) & 7u) != 0) return 0;

    uint32_t refdiv = REG(PLL_CS) & 0x3fu;
    uint32_t fbdiv = REG(PLL_FBDIV_INT) & 0xfffu;
    uint32_t prim = REG(PLL_PRIM);
    uint32_t pd1 = (prim >> 16) & 7u, pd2 = (prim >> 12) & 7u;
    if (refdiv == 0 || pd1 == 0 || pd2 == 0) return 0;

    uint64_t vco = (uint64_t)RP2350_XOSC_HZ / refdiv * fbdiv;
    uint64_t hz = vco / (pd1 * pd2);

    /* CLK_SYS_DIV: INT 0 means 65536; the fractional part is ignored beyond
     * refusing to call a fractional divider an exact frequency. */
    uint32_t div = REG(CLK_SYS_DIV);
    uint32_t div_int = div >> 16;
    if (div_int == 0) div_int = 65536;
    if ((div & 0xffffu) != 0) return 0;
    return (uint32_t)(hz / div_int);
}

/* TIMER0's tick rate as clk_ref's registers describe it, in Hz; 0 if
 * clk_ref is not on the crystal (then there is no trustworthy ruler). */
static uint32_t timer0_tick_hz(void) {
    if ((REG(CLK_REF_CTRL) & 3u) != 2u) return 0;
    uint32_t div = (REG(CLK_REF_DIV) >> 16) & 0xffu;
    if (div == 0) div = 256;
    uint32_t cycles = REG(TICKS_TIMER0_CYCLES) & 0x1ffu;
    if (cycles == 0) return 0;
    return RP2350_XOSC_HZ / div / cycles;
}

/* The CPU clock counted against TIMER0 over `ticks` TIMER0 ticks. Busy-waits,
 * which is the point: nothing else may run between the two readings.
 *
 * mcycle is **inhibited out of reset on Hazard3** (mcountinhibit.CY, CSR
 * 0x320 bit 0) and nothing in this kernel had ever enabled it -- the first
 * run of this function measured 0 Hz, which is how that was found. The bit is
 * cleared for the window and put back as it was. */
static uint32_t clk_sys_measured(uint32_t ticks) {
    uint32_t tick_hz = timer0_tick_hz();
    if (tick_hz == 0) return 0;
    uint32_t inhibit;
    __asm__ volatile("csrr %0, 0x320" : "=r"(inhibit));
    __asm__ volatile("csrc 0x320, %0" :: "r"(1u));

    uint32_t t0 = REG(TIMER0_TIMERAWL);
    while (REG(TIMER0_TIMERAWL) == t0) { }        /* align to a tick edge */
    t0 = REG(TIMER0_TIMERAWL);
    uint32_t c0 = mcycle32();
    while ((uint32_t)(REG(TIMER0_TIMERAWL) - t0) < ticks) { }
    uint32_t c1 = mcycle32();

    if (inhibit & 1u) __asm__ volatile("csrs 0x320, %0" :: "r"(1u));
    return (uint32_t)((uint64_t)(uint32_t)(c1 - c0) * tick_hz / ticks);
}

void rp2350_clocks_report(void) {
    uint32_t cs = REG(PLL_CS), fbdiv = REG(PLL_FBDIV_INT) & 0xfffu, prim = REG(PLL_PRIM);
    uint32_t ctrl = REG(CLK_SYS_CTRL), div = REG(CLK_SYS_DIV);

    cprintf("PLL_SYS:  %s, REFDIV %lu, FBDIV %lu, POSTDIV %lu/%lu -> VCO %lu MHz\n",
            (cs >> 31) ? "locked" : "NOT LOCKED",
            (unsigned long)(cs & 0x3fu), (unsigned long)fbdiv,
            (unsigned long)((prim >> 16) & 7u), (unsigned long)((prim >> 12) & 7u),
            (unsigned long)((cs & 0x3fu) ? RP2350_XOSC_HZ / (cs & 0x3fu) * fbdiv / 1000000u : 0));
    cprintf("clk_sys:  SRC %s, AUXSRC %s, DIV %lu.%04lx\n",
            (ctrl & 1u) ? "aux" : "clk_ref", auxsrc_name((ctrl >> 5) & 7u),
            (unsigned long)(div >> 16), (unsigned long)(div & 0xffffu));

    uint32_t rdiv = (REG(CLK_REF_DIV) >> 16) & 0xffu;
    cprintf("clk_ref:  SRC %lu%s, DIV %lu; TIMER0 tick %lu Hz%s\n",
            (unsigned long)(REG(CLK_REF_CTRL) & 3u),
            (REG(CLK_REF_CTRL) & 3u) == 2u ? " (XOSC)" : " (NOT the crystal)",
            (unsigned long)(rdiv ? rdiv : 256), (unsigned long)timer0_tick_hz(),
            timer0_tick_hz() == 1000000u ? "" : "  -- NOT 1 MHz: every delay and timestamp is off");

    uint32_t from_regs = clk_sys_from_registers();
    uint32_t measured = clk_sys_measured(20000);
    cprintf("clk_sys:  %lu Hz from the registers, %lu Hz measured (mcycle over 20000 TIMER0 ticks)\n",
            (unsigned long)from_regs, (unsigned long)measured);
    cprintf("config:   CONFIG_CLK_SYS_HZ = %lu%s\n", (unsigned long)CONFIG_CLK_SYS_HZ,
            from_regs == (uint32_t)CONFIG_CLK_SYS_HZ ? " -- agrees with the registers"
                                                     : " -- DISAGREES with the registers");
    /* 38.1: the QSPI windows. A flash write used to leave M0 at the bootrom's
     * serial 03h read until the next reset, which nothing here showed. */
    flash_rp2350_qmi_report();
}

/* One line at boot, the RP2350 equivalent of the P4's "[CLK] CPU at N MHz,
 * measured": a board whose clock is not what its build asked for should say
 * so before anything runs slow or at the wrong baud. */
void rp2350_clocks_boot_check(void) {
    uint32_t from_regs = clk_sys_from_registers();
    uint32_t measured = clk_sys_measured(2000);
    unsigned mhz = (unsigned)((measured + 500000u) / 1000000u);
    printk("[CLK] clk_sys %u MHz measured, %lu Hz from PLL_SYS%s\n", mhz,
           (unsigned long)from_regs,
           from_regs == (uint32_t)CONFIG_CLK_SYS_HZ ? "" : "  -- DISAGREES with CONFIG_CLK_SYS_HZ");

    /* Why the chip last reset -- 36.8: a keyboard hot-plugged onto the
     * RP2350-LCD-7's USB host port reset the board, and "brown-out" versus
     * "watchdog" versus "software" is the first question about that.
     * POWMAN CHIP_RESET (0x40100000 + 0x2c); bit layout from the SDK's
     * hardware/regs/powman.h. The flags describe the reset that started this
     * boot. */
    static const struct { uint32_t bit; const char *name; } causes[] = {
        { 1u << 16, "power-on" },       { 1u << 17, "BROWN-OUT" },
        { 1u << 18, "RUN pin" },        { 1u << 19, "debugger" },
        { 1u << 21, "rescue" },         { 1u << 22, "watchdog (powman async)" },
        { 1u << 23, "watchdog (powman)" }, { 1u << 24, "watchdog (switched core)" },
        { 1u << 25, "switched-core power-down" }, { 1u << 26, "GLITCH DETECTOR" },
        { 1u << 27, "hazard3 reset request" },    { 1u << 28, "watchdog (PSM)" },
    };
    uint32_t cr = *(volatile uint32_t *)(uintptr_t)(0x40100000UL + 0x2c);
    char line[160];
    uint32_t used = (uint32_t)ksnprintf(line, sizeof(line), "[CLK] last reset: CHIP_RESET=0x%08lx", (unsigned long)cr);
    const char *sep = " (";
    for (unsigned i = 0; i < sizeof(causes) / sizeof(causes[0]); i++) {
        if (!(cr & causes[i].bit)) continue;
        used += (uint32_t)ksnprintf(line + used, sizeof(line) - used, "%s%s", sep, causes[i].name);
        sep = ", ";
    }
    printk("%s%s\n", line, sep[0] == ',' ? ")" : "");
}

#endif /* CONFIG_BOARD_RP2350 */
