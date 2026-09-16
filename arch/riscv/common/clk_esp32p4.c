/* Reading the ESP32-P4's clock tree. 34.1,
 * plan/phase34_esp32p4_pll_bringup.md.
 *
 * This file writes nothing. That is the whole of 34.1: before any milestone
 * moves a clock on this board, there has to be a way to say what the clocks
 * *are* that does not consist of believing a constant. Until now this kernel
 * had no such way and no such constant either -- drivers/emac_esp32p4.c:123
 * says so in the course of picking an MDC divider it cannot compute:
 *
 *     "This kernel has never configured the P4's clock tree ... so the
 *      system clock is whatever the boot ROM left, and this file does not
 *      know it."
 *
 * ## Where these register definitions come from
 *
 * Two independent ESP-IDF sources, which is the bar plan/phase34 34.2 sets
 * and this file already meets for the four registers it reads:
 *
 *   * the field shifts and widths from components/soc/esp32p4/register/
 *     hw_ver1/soc/{hp_sys_clkrst,lp_clkrst}_reg.h, and
 *   * the *meaning* of each field from components/esp_hal_clock/esp32p4/
 *     include/hal/clk_tree_ll.h's own accessors (clk_ll_cpu_get_divider(),
 *     clk_ll_{mem,sys,apb}_get_divider(), clk_ll_cpu_get_src()).
 *
 * **hw_ver1 and hw_ver3 were diffed, not assumed equal.** This board is
 * revision v1.3, so hw_ver1 is the right map; the two headers turn out to
 * agree exactly on all four of these registers and on every field used here.
 * That check is not ceremony -- plan/phase28_esp32p4_ethernet.md established
 * that the two maps must be diffed rather than assumed equal, and phase 29's
 * planning found a PTP pad that exists in one and not the other.
 *
 * The TRM cross-check is 34.2's milestone, not this one's. What makes it
 * acceptable to land the reads first is that this code has a known answer to
 * produce: at 34.1 the board is still on the crystal, so `clocks` must print
 * 40 / 20 / 20 / 10 MHz, and any misread field gets a different number.
 *
 * ## The one thing that is easy to get wrong
 *
 * **The dividers are a cascade, not four taps off HP_ROOT_CLK.**
 * clk_tree_ll.h is explicit in each function's doc comment:
 *
 *     CPU_CLK = HP_ROOT_CLK / cpu_div
 *     MEM_CLK = CPU_CLK    / mem_div
 *     SYS_CLK = MEM_CLK    / sys_div
 *     APB_CLK = SYS_CLK    / apb_div
 *
 * which is what makes the power-on default of 40-20-20-10 MHz come out of
 * the divider set (1, 2, 1, 2) rather than (1, 2, 2, 4). Reading it as four
 * parallel dividers gives the right answer for CPU and MEM and the wrong one
 * for SYS and APB -- a failure mode that looks like a working function.
 *
 * **And the source mux is not in HP_SYS_CLKRST.** It lives in LP_CLKRST, a
 * different peripheral in a different power domain, which is why a search of
 * the block this tree already knows (0x500E6000, uart_esp32p4.c and
 * emac_esp32p4.c both use it) does not find it.
 */

#include "lugalos_config.h"

#if defined(CONFIG_BOARD_ESP32P4)

#include <stdint.h>
#include "arch/clk_esp32p4.h"
#include "kernel/console.h"
#include "kernel/ticker.h"
#include "kernel/time.h"

/* HP_SYS_CLKRST: HPPERIPH1 (0x500C0000) + 0x26000. The same base
 * drivers/uart_esp32p4.c:203 and drivers/emac_esp32p4.c:39 already use. */
#define P4_CLKRST_BASE          0x500E6000UL
#define P4_ROOT_CLK_CTRL0       (P4_CLKRST_BASE + 0x04)
#define P4_ROOT_CLK_CTRL1       (P4_CLKRST_BASE + 0x08)
#define P4_ROOT_CLK_CTRL2       (P4_CLKRST_BASE + 0x0c)

/* ROOT_CLK_CTRL0: CPU_CLK_DIV_NUM [12:5], NUMERATOR [20:13],
 * DENOMINATOR [28:21]. (Bit 4 is SOC_CLK_DIV_UPDATE, write-triggered, and
 * belongs to 34.4 rather than here.) */
#define CPU_DIV_NUM_S           5
#define CPU_DIV_NUMERATOR_S     13
#define CPU_DIV_DENOMINATOR_S   21

/* ROOT_CLK_CTRL1: MEM_CLK_DIV_NUM [7:0], SYS_CLK_DIV_NUM [31:24]. */
#define MEM_DIV_NUM_S           0
#define SYS_DIV_NUM_S           24

/* ROOT_CLK_CTRL2: APB_CLK_DIV_NUM [23:16]. */
#define APB_DIV_NUM_S           16

#define DIV_FIELD_MASK          0xFFu   /* every one of them is 8 bits */

/* LP_CLKRST: LPAON (0x50110000) + 0x1000. LPAON is the base
 * arch/riscv/common/xip_esp32p4.c already derives its watchdog addresses
 * from ("LPAON + 0x6000"), so the two agree on where this domain starts. */
#define P4_LP_CLKRST_BASE       0x50111000UL
#define P4_LP_HP_CLK_CTRL       (P4_LP_CLKRST_BASE + 0x40)
#define HP_ROOT_CLK_SRC_SEL_M   0x3u    /* [1:0] */

/* The register header names the three selectable roots in its own comment:
 * "2'd0: xtal_40m, 2'd1: cpll_400m, 2'd2: fosc_20m".
 *
 * Note what the CPLL case does *not* say. The name is the mux's, not a
 * frequency: IDF keeps CPLL at 360 MHz on revisions below 3.0 and this board
 * is v1.3, so a board running from CPLL has a root that depends on how CPLL
 * itself is configured. Reading that back is 34.2's job -- until then the
 * value below is nominal and esp32p4_clocks_report() says so rather than
 * printing a derived megahertz figure as though it were measured. */
#define SRC_XTAL                0u
#define SRC_CPLL                1u
#define SRC_RC_FAST             2u

#define CPLL_NOMINAL_HZ         360000000u  /* rev < 3.0; see above */
#define RC_FAST_NOMINAL_HZ      20000000u   /* "fosc_20m", untrimmed */

#define P4_REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* Divider fields are encoded as (divider - 1) -- clk_tree_ll.h's getters all
 * add 1 on the way out, which is the detail that makes a raw 0 mean "divide
 * by one" rather than a division by zero. */
static inline uint32_t div_of(uint32_t reg, unsigned shift) {
    return ((reg >> shift) & DIV_FIELD_MASK) + 1u;
}

void esp32p4_clocks_read(esp32p4_clocks_t *c) {
    uint32_t ctrl0 = P4_REG(P4_ROOT_CLK_CTRL0);
    uint32_t ctrl1 = P4_REG(P4_ROOT_CLK_CTRL1);
    uint32_t ctrl2 = P4_REG(P4_ROOT_CLK_CTRL2);

    c->src = (uint8_t)(P4_REG(P4_LP_HP_CLK_CTRL) & HP_ROOT_CLK_SRC_SEL_M);

    switch (c->src) {
    case SRC_XTAL:    c->root_hz = (uint32_t)CONFIG_XTAL_HZ; break;
    case SRC_CPLL:    c->root_hz = CPLL_NOMINAL_HZ;          break;
    case SRC_RC_FAST: c->root_hz = RC_FAST_NOMINAL_HZ;       break;
    default:          c->root_hz = 0u;                       break;
    }

    c->cpu_div = div_of(ctrl0, CPU_DIV_NUM_S);
    c->cpu_num = (ctrl0 >> CPU_DIV_NUMERATOR_S)   & DIV_FIELD_MASK;
    c->cpu_den = (ctrl0 >> CPU_DIV_DENOMINATOR_S) & DIV_FIELD_MASK;
    c->mem_div = div_of(ctrl1, MEM_DIV_NUM_S);
    c->sys_div = div_of(ctrl1, SYS_DIV_NUM_S);
    c->apb_div = div_of(ctrl2, APB_DIV_NUM_S);

    /* The cascade, in the order the header comment gives it. Integer part
     * only: the fractional numerator/denominator are reported raw beside it
     * rather than folded in, because nothing has ever set them (both are 0 at
     * reset and IDF only uses them for frequencies this phase does not ask
     * for) and a division that silently rounds is worse than one a reader can
     * see is integer. */
    c->cpu_hz = c->cpu_div ? c->root_hz / c->cpu_div : 0u;
    c->mem_hz = c->mem_div ? c->cpu_hz  / c->mem_div : 0u;
    c->sys_hz = c->sys_div ? c->mem_hz  / c->sys_div : 0u;
    c->apb_hz = c->apb_div ? c->sys_hz  / c->apb_div : 0u;
}

/* The CLINT's own rate, measured here over a window the caller chooses.
 *
 * kernel/ticker.c already measures this at init, and this function exists
 * because that measurement's window is 2 ms -- *"long enough to divide by and
 * short enough not to be noticed at boot"*, which is the right trade for a
 * boot step and the wrong one for deciding what a clock is. Reported to the
 * hertz it invites a precision it does not have.
 *
 * A second is affordable in a diagnostic command and settles the question by
 * a factor of 500. The two figures agreeing is the evidence; either alone is
 * a number.
 *
 * Reads the CLINT directly rather than through ticker.c, whose now() is
 * static -- the same addresses kernel/shell.c's cmd_clicdump() already reads,
 * and the same re-read loop, for the reason ticker.c:195 gives at length:
 * the two halves cannot be read atomically and the hardware's own sampling
 * mode latches the *other* half, which silently mixes two instants. */
#define P4_CLINT_MTIMELO  (*(volatile uint32_t *)(uintptr_t)(0x20000000UL + 0xBFF8))
#define P4_CLINT_MTIMEHI  (*(volatile uint32_t *)(uintptr_t)(0x20000000UL + 0xBFFC))

static uint64_t clint_now(void) {
    uint32_t hi, lo;
    do { hi = P4_CLINT_MTIMEHI; lo = P4_CLINT_MTIMELO; } while (hi != P4_CLINT_MTIMEHI);
    return ((uint64_t)hi << 32) | lo;
}

uint32_t esp32p4_clint_measure_hz(uint32_t window_us) {
    /* Both counters are free-running, so a preemption inside the window
     * lengthens it for both and cancels. Nothing here needs interrupts off.
     *
     * The reads are ordered so that neither window can be systematically
     * longer than the other: ticks first on the way in, ticks first on the
     * way out. */
    uint64_t t0 = clint_now();
    uint64_t u0 = time_get_us();
    while (time_get_us() - u0 < window_us) { /* spin */ }
    uint64_t t1 = clint_now();
    uint64_t u1 = time_get_us();

    uint64_t dt = t1 - t0, du = u1 - u0;
    if (du == 0u) return 0u;
    return (uint32_t)((dt * 1000000ULL) / du);
}

/* ets_get_cpu_frequency(), ROM 0x4fc00040.
 *
 * **Deliberately not ets_clk_get_cpu_freq().** That one looked like the
 * better call -- 34.1 as planned named it -- and it is the one symbol of the
 * four this phase needs whose address *moves between ROM revisions*:
 * 0x4fc00554 in components/esp_rom/esp32p4/ld/esp32p4.rom.ld against
 * 0x4fc00560 in esp32p4.rom.eco0_4.ld. Calling it would mean first
 * establishing which ROM this chip carries, to read a number that is
 * bookkeeping anyway. ets_get_cpu_frequency, ets_update_cpu_frequency and
 * ets_set_appcpu_boot_addr are at identical addresses in both variants,
 * which is what makes them safe to hardcode the way this tree already
 * hardcodes the cache and MMU entry points. */
#define ROM_ETS_GET_CPU_FREQUENCY   0x4fc00040u

typedef uint32_t (*rom_get_cpu_freq_t)(void);

uint32_t esp32p4_rom_cpu_freq_mhz(void) {
    rom_get_cpu_freq_t f = (rom_get_cpu_freq_t)(uintptr_t)ROM_ETS_GET_CPU_FREQUENCY;
    return f();
}

/* Three answers to one question, from three places that can disagree.
 *
 * The registers are the authority: they are what the hardware is actually
 * doing. The ROM's figure is bookkeeping and will be *wrong* the moment 34.4
 * changes a clock without calling ets_update_cpu_frequency() -- which is
 * precisely why it is printed rather than trusted, since that call is the
 * easiest thing in this phase to forget and its failure is silent. The
 * ticker's rate is the only independent measurement on the board: kernel/
 * ticker.c:285 counts CLINT ticks against the systimer, and the systimer runs
 * from the crystal (kernel/time.c), so it does not move when the CPU does.
 *
 * At 34.1 all three should agree on 40 MHz. After 34.4 the ticker's figure
 * answers a question kernel/ticker.c:181 has had open since E4 -- whether the
 * CLINT is clocked from the CPU or from the crystal -- because it is the one
 * of the three that is measured rather than derived. */
void esp32p4_clocks_report(void) {
    esp32p4_clocks_t c;
    esp32p4_clocks_read(&c);

    static const char *const src_name[4] = {
        "XTAL", "CPLL", "RC_FAST", "(reserved)"
    };

    cprintf("[CLK] root      = %s", src_name[c.src & 3u]);
    if (c.src == SRC_XTAL) {
        cprintf(" (%u MHz, CONFIG_XTAL_HZ)\n", (unsigned)(c.root_hz / 1000000u));
    } else {
        cprintf(" (%u MHz nominal -- not read back, see 34.2)\n",
                (unsigned)(c.root_hz / 1000000u));
    }
    cprintf("[CLK] CPU       = %u MHz   (root / %u)\n",
            (unsigned)(c.cpu_hz / 1000000u), (unsigned)c.cpu_div);
    cprintf("[CLK] MEM       = %u MHz   (CPU / %u)\n",
            (unsigned)(c.mem_hz / 1000000u), (unsigned)c.mem_div);
    cprintf("[CLK] SYS       = %u MHz   (MEM / %u)\n",
            (unsigned)(c.sys_hz / 1000000u), (unsigned)c.sys_div);
    cprintf("[CLK] APB       = %u MHz   (SYS / %u)\n",
            (unsigned)(c.apb_hz / 1000000u), (unsigned)c.apb_div);

    /* Raw, so a wrong derivation above is separable from a wrong read. */
    cprintf("[CLK] dividers  = cpu %u (frac %u/%u)  mem %u  sys %u  apb %u\n",
            (unsigned)c.cpu_div, (unsigned)c.cpu_num, (unsigned)c.cpu_den,
            (unsigned)c.mem_div, (unsigned)c.sys_div, (unsigned)c.apb_div);

    cprintf("[CLK] ROM says  = %u MHz   (bookkeeping: ets_get_cpu_frequency)\n",
            (unsigned)esp32p4_rom_cpu_freq_mhz());

    uint64_t hz = ticker_measured_hz();
    if (hz) {
        cprintf("[CLK] CLINT     = %u Hz   (ticker's 2 ms window, at boot)\n",
                (unsigned)hz);
    } else {
        cprintf("[CLK] CLINT     = not measured (ticker disabled)\n");
    }
    cprintf("[CLK] CLINT     = %u Hz   (1 s window, now)\n",
            (unsigned)esp32p4_clint_measure_hz(1000000u));
}

#endif /* CONFIG_BOARD_ESP32P4 */
