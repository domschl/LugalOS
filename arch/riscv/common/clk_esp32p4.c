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
#include "arch/csr.h"
#include "kernel/printk.h"

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

/* 360 MHz, and the TRM says so itself rather than this being IDF's table
 * transcribed: Register 11.67's encoding for this very field reads
 * "1: CPLL_CLK (360 MHz)". Worth pinning down because the surrounding
 * nomenclature disagrees -- the register header comment says "2'd1:
 * cpll_400m" and the adjacent gate is named HP_CPLL_400M_CLK_EN. The 400 is
 * a name; 360 is the documented frequency for this silicon. */
#define CPLL_NOMINAL_HZ         360000000u
#define RC_FAST_NOMINAL_HZ      20000000u   /* "fosc_20m", untrimmed */

#define P4_REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

static bool regi2c_read_cpll(uint8_t reg_addr, uint8_t *out);   /* below */

/* CPLL's configured rate, from its own analog divider.
 *
 * 34.2 could not trust this and said so: the divider read 8, which is a
 * value ESP-IDF never programs, in a field IDF's source says has two bits
 * swapped from ECO1. **34.4 settled it by measurement.** With the mux on
 * CPLL and cpu_div = 4 the CPU measures 80 MHz, so CPLL is 320 -- exactly
 * what `xtal * div / (ref_div + 1)` gives for div 8. The formula is right,
 * the register is truthful, and the ROM simply configures this PLL to a
 * frequency IDF has no table entry for.
 *
 * So this is now derived rather than hedged, and every figure below it
 * (CPU, MEM, SYS, APB while the root is CPLL) is correct instead of being
 * nominal. Returns 0 if the analog bus cannot be read, and callers fall
 * back to the nominal 360 rather than printing zeroes. */
static uint32_t cpll_hz(void);   /* defined with the REGI2C reader below */

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
    case SRC_CPLL: {
        uint32_t hz = cpll_hz();
        c->root_hz = hz ? hz : CPLL_NOMINAL_HZ;
        break;
    }
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

/* --- 34.2: the sources, the regulator and the flash clock ---------------
 *
 * Three questions the phase plan asks before 34.3 can be scoped, and what it
 * takes to answer each one *from the board* rather than from a datasheet.
 *
 * ## "No writes" and the one exception
 *
 * 34.2 is a read-only milestone and this code changes no clock, no divider
 * and no power state. It does write two registers, and both are part of
 * performing a read: the analog I2C master's block-select (ANA_CONF1/CONF2)
 * and its command word. A REGI2C "read" is a bus transaction, so there is no
 * version of it that only loads. What it cannot do is alter the thing being
 * measured -- the selector routes the master at a block, and nothing else in
 * this kernel uses the master at all, so there is no transaction to collide
 * with and no lock to take. ESP-IDF serialises these because IDF has many
 * callers; this has one, reached only from a shell command.
 *
 * ## CPLL's power state is not readable, and that is the finding
 *
 * Every bit that controls it -- PMU_TIE_HIGH_XPD_CPLL, _XPD_CPLL_I2C,
 * _GLOBAL_CPLL_ICG and their TIE_LOW counterparts -- is **WT**,
 * write-triggered, in PMU_IMM_HP_CK_POWER_REG. Writing one performs an
 * action; reading the register does not report a state. So "is CPLL running"
 * has no direct register answer and has to be assembled from two indirect
 * ones, which is what the two reads below are:
 *
 *   * its analog configuration, over REGI2C -- a block that answers with a
 *     plausible divider is a block with its power and its I2C alive, and the
 *     divider gives the frequency; and
 *   * what the flash is clocked from. This matters more than it looks:
 *     since phase 32 the CPU fetches `.text` over the MSPI, so if
 *     FLASH_CLK_SRC_SEL names a PLL then that PLL is demonstrably running --
 *     the evidence is that this function's own instructions arrived.
 *
 * ## What the flash clock is *not*
 *
 * FLASH_CLK_SRC_SEL picks between XTAL, CPLL and SPLL directly
 * (SOC_FLASH_CLKS in clk_tree_defs.h). It does **not** derive from MEM_CLK,
 * which is the cascade 34.1 established for the four root clocks. That makes
 * the flash clock independent of the CPU frequency, and it is the answer to
 * the phase plan's third question -- see 34.2's write-up for what follows
 * from it.
 *
 * Nothing in ESP-IDF ever programs this field; a grep of the whole tree finds
 * it only in the register header. Whatever the ROM left is what runs.
 */

/* LP_I2C_ANA_MST: LPPERIPH (0x50120000) + 0x4000. */
#define P4_ANA_MST_BASE         0x50124000UL
#define P4_ANA_MST_I2C0_CTRL    (P4_ANA_MST_BASE + 0x00)
#define P4_ANA_MST_ANA_CONF1    (P4_ANA_MST_BASE + 0x1c)
#define P4_ANA_MST_ANA_CONF2    (P4_ANA_MST_BASE + 0x20)
#define ANA_CONF_FIELD_M        0x00FFFFFFu   /* [23:0] in both */
#define REGI2C_PLL_CPU_MST_SEL  (1u << 11)    /* in ANA_CONF2 */

/* I2C0_CTRL layout: SLAVE_ID [7:0], ADDR [15:8], DATA [23:16],
 * WR_CNTL [24] (0 = read), BUSY [25]. */
#define REGI2C_BUSY             (1u << 25)
#define REGI2C_DATA_S           16

/* LPPERI_CLK_EN, LPPERIPH + 0x0. CK_EN_LP_I2CMST is bit 27 and its reset
 * default is 1, so the master is clocked before anyone asks. Checked rather
 * than set: turning it on would be a write this milestone does not get to
 * make, and finding it off would itself be the answer. */
#define P4_LPPERI_CLK_EN        0x50120000UL
#define LPPERI_CK_EN_LP_I2CMST  (1u << 27)

/* The CPLL analog block. Slave 0x67; register 2 holds OC_REF_DIV in [3:0]
 * and register 3 holds OC_DIV_7_0 in [7:0] (soc/regi2c_cpll.h). */
#define I2C_CPLL_SLAVE          0x67u
#define I2C_CPLL_REG_REF_DIV    2u
#define I2C_CPLL_REG_DIV_7_0    3u
#define I2C_CPLL_REG_DCUR       6u

/* REGI2C write. Same transaction as the read, plus WR_CNTL and the data
 * byte -- this is the first thing in phase 34 to write the analog bus.
 *
 * Deliberately a whole-byte write rather than IDF's read-modify-write mask
 * form: every field this file sets is one the CPLL configuration owns
 * completely, and the values come from ESP-IDF's own table rather than being
 * merged into whatever was there. Merging is how a 360 MHz divider ends up
 * beside a 320 MHz loop filter. */
#define REGI2C_WR_CNTL          (1u << 24)
#define REGI2C_ADDR_S           8

/* The analog master's own source clock. LP_I2C_ANA_MST_CLK160M bit 0,
 * **reset default 0**, and ESP-IDF sets it before every PLL configuration
 * (`regi2c_ctrl_ll_master_configure_clock()`, called from its
 * ANALOG_CLOCK_ENABLE macro).
 *
 * Reads work without it -- 34.2's register dumps were taken with this bit at
 * 0 and returned sensible, repeatable bytes. Calibration does not: the first
 * attempt at 34.5 left this alone and the board hung in the CAL_END poll
 * with the console silent after the PMU line. That is the whole reason the
 * waits below are bounded. */
#define P4_ANA_MST_CLK160M      (P4_ANA_MST_BASE + 0x34)
#define ANA_MST_SEL_160M        (1u << 0)

/* Bounded, because an unbounded poll on an analog block is a board that
 * stops with no output -- which is exactly what the first attempt did. */
#define REGI2C_SPIN_LIMIT       200000u

static bool regi2c_wait_idle(void) {
    for (uint32_t i = 0; i < REGI2C_SPIN_LIMIT; i++) {
        if (!(P4_REG(P4_ANA_MST_I2C0_CTRL) & REGI2C_BUSY)) return true;
    }
    return false;
}

/* Point the analog master at one block. Every transaction begins here;
 * ESP-IDF's regi2c_enable_block() does the same two clears and one set. */
static bool regi2c_select(uint32_t mst_sel) {
    if (!(P4_REG(P4_LPPERI_CLK_EN) & LPPERI_CK_EN_LP_I2CMST)) {
        return false;   /* master is gated; say so rather than ungate it */
    }
    P4_REG(P4_ANA_MST_ANA_CONF1) &= ~ANA_CONF_FIELD_M;
    P4_REG(P4_ANA_MST_ANA_CONF2) &= ~ANA_CONF_FIELD_M;
    P4_REG(P4_ANA_MST_ANA_CONF2) |= mst_sel;
    return true;
}

static bool regi2c_read(uint32_t blk, uint32_t mst_sel, uint8_t reg_addr,
                        uint8_t *out) {
    if (!regi2c_select(mst_sel)) return false;
    if (!regi2c_wait_idle()) return false;
    P4_REG(P4_ANA_MST_I2C0_CTRL) = blk | ((uint32_t)reg_addr << REGI2C_ADDR_S);
    if (!regi2c_wait_idle()) return false;
    *out = (uint8_t)((P4_REG(P4_ANA_MST_I2C0_CTRL) >> REGI2C_DATA_S) & 0xFFu);
    return true;
}

static bool regi2c_read_cpll(uint8_t reg_addr, uint8_t *out) {
    return regi2c_read(I2C_CPLL_SLAVE, REGI2C_PLL_CPU_MST_SEL, reg_addr, out);
}

static uint32_t cpll_hz(void) {
    uint8_t div = 0, ref = 0;
    if (!regi2c_read_cpll(I2C_CPLL_REG_DIV_7_0, &div)) return 0u;
    if (!regi2c_read_cpll(I2C_CPLL_REG_REF_DIV, &ref)) return 0u;
    return (uint32_t)CONFIG_XTAL_HZ * (uint32_t)div / ((uint32_t)(ref & 0xFu) + 1u);
}


static bool regi2c_write(uint32_t blk, uint32_t mst_sel, uint8_t reg_addr,
                         uint8_t val) {
    if (!regi2c_select(mst_sel)) return false;
    if (!regi2c_wait_idle()) return false;
    P4_REG(P4_ANA_MST_I2C0_CTRL) =
        blk | ((uint32_t)reg_addr << REGI2C_ADDR_S) | REGI2C_WR_CNTL
        | ((uint32_t)val << REGI2C_DATA_S);
    return regi2c_wait_idle();
}

/* Read-modify-write of one bit, which is what ESP-IDF's REGI2C_WRITE_MASK
 * reduces to for every field this file touches. The read and the write are
 * separate transactions on the same selected block. */
static bool regi2c_write_bit(uint32_t blk, uint32_t mst_sel, uint8_t reg_addr,
                             unsigned bit, unsigned value) {
    uint8_t v = 0;
    if (!regi2c_read(blk, mst_sel, reg_addr, &v)) return false;
    if (value) v |= (uint8_t)(1u << bit);
    else       v &= (uint8_t)~(1u << bit);
    return regi2c_write(blk, mst_sel, reg_addr, v);
}

static bool regi2c_write_cpll(uint8_t reg_addr, uint8_t val) {
    return regi2c_write(I2C_CPLL_SLAVE, REGI2C_PLL_CPU_MST_SEL, reg_addr, val);
}


/* HP_SYS_CLKRST_ANA_PLL_CTRL0 (0x00BC), TRM Register 11.47: five PLL
 * calibration pairs, *_CAL_END at even bits (RO, "1: Calibration done") and
 * *_CAL_STOP above each. All reset to 0.
 *
 * This is the closest thing to a readable "is that PLL up" on this chip, and
 * it exists because a PLL that has never been calibrated has never been
 * brought up. It is not the same as a power bit -- those are write-triggered
 * and unreadable -- but it is evidence rather than inference, which is the
 * whole difference this milestone is about. */
#define P4_ANA_PLL_CTRL0        (P4_CLKRST_BASE + 0xbc)
#define PLLA_CAL_END_B          0u
#define CPU_PLL_CAL_END_B       2u
#define SDIO_PLL_CAL_END_B      4u
#define SYS_PLL_CAL_END_B       6u
#define MSPI_CAL_END_B          8u

/* --- 34.5: moving CPLL from the ROM's 320 MHz to 360 -------------------
 *
 * 34.4 measured what the ROM leaves: CPLL at 320 MHz, div 8. IDF's table for
 * this silicon programs div 9 for 360 and div 10 for 400, and 400 is not
 * available below revision v3.0. So 360 is the ceiling and div 9 is the way
 * to it -- 12.5% over what 34.4 reached, for three analog-bus writes.
 *
 * ## The three bytes, and why all three
 *
 * ESP-IDF's `clk_ll_cpll_set_config(360, 40)` writes exactly these, and this
 * writes the same three even though only one differs from what the ROM left:
 *
 *     reg 2  0x50   (ENB_FCAL 0, DCHGP 5, REF_DIV 0)  -- ROM already 0x50
 *     reg 3  0x09   (OC_DIV_7_0)                      -- ROM has 0x08 = 320
 *     reg 6  0x73   (DLREF_SEL 1, DHREF_SEL 3, DCUR 3) -- ROM has 0x63
 *
 * **Register 6 is the one worth pausing on.** It is charge-pump and
 * reference-current settings for the PLL loop, and the ROM's 0x63 differs
 * from IDF's 0x73 in DHREF_SEL alone (2 against 3). It would be tempting to
 * change only the divider and leave the loop as found. That is exactly the
 * mix-and-match IDF's table exists to prevent: 0x63 is the setting that goes
 * with *the ROM's 320 MHz*, and a loop filter tuned for one frequency beside
 * a divider for another is how a PLL locks badly rather than not at all.
 * Vendor table, taken whole.
 *
 * ## Order, and the one hard precondition
 *
 * **The CPU must not be running from CPLL while this runs.** It is called
 * from esp32p4_cpu_freq_set() before the source mux is touched, at a point
 * in boot where HP_ROOT_CLK is still the crystal. Reprogramming the PLL the
 * CPU is executing from would not produce a wrong number, it would stop the
 * board.
 *
 * The sequence is IDF's `rtc_clk_cpll_configure()`: start calibration, write
 * the three bytes, wait for CAL_END, settle 10 us, stop calibration. Note
 * that CAL_END is the same RO bit 34.2 used to establish the ROM had brought
 * CPLL up in the first place -- it is a status this code now both reads and
 * causes.
 *
 * CPLL's power is not re-asserted: `clk_ll_cpll_enable()` exists for a PLL
 * that is off, and 34.2 established this one is on (CAL_END was already 1 at
 * hand-over). Writing TIE_HIGH bits to a PLL that is already powered would
 * be harmless and is still a write this does not need to make.
 */
#define CPLL_REG_REF_DIV_BYTE   0x50u
#define CPLL_REG_DIV_360        0x09u
#define CPLL_REG_DCUR_BYTE      0x73u
#define CPU_PLL_CAL_STOP_B      (1u << 3)

/* 0 = configured, otherwise the step that failed. Reported rather than
 * retried: a PLL that will not calibrate is a finding, and the board must
 * stay on the crystal and keep its console rather than spin. */
static int cpll_configure_360(void) {
    /* The analog master's source clock, first. IDF does this before every
     * PLL configuration and the first attempt here did not -- see the
     * constant's comment for what that cost. */
    P4_REG(P4_ANA_MST_CLK160M) |= ANA_MST_SEL_160M;

    /* Calibration start: CAL_STOP = 0. */
    P4_REG(P4_ANA_PLL_CTRL0) &= ~CPU_PLL_CAL_STOP_B;

    if (!regi2c_write_cpll(I2C_CPLL_REG_REF_DIV, CPLL_REG_REF_DIV_BYTE)) return 1;
    if (!regi2c_write_cpll(I2C_CPLL_REG_DIV_7_0, CPLL_REG_DIV_360))      return 2;
    if (!regi2c_write_cpll(I2C_CPLL_REG_DCUR,    CPLL_REG_DCUR_BYTE))    return 3;

    /* CAL_END, bounded. 10 ms is four orders of magnitude over the ~10 us
     * IDF expects, so a timeout here means "never" and not "not yet". */
    uint64_t t0 = time_get_us();
    while (!(P4_REG(P4_ANA_PLL_CTRL0) & (1u << CPU_PLL_CAL_END_B))) {
        if (time_get_us() - t0 > 10000u) return 4;
    }

    /* "wait for true stop" -- IDF's words and its 10 us. A local spin rather
     * than time_delay_us(), which pumps the USB CDC task; this runs early in
     * boot and should depend on nothing but the systimer. */
    t0 = time_get_us();
    while (time_get_us() - t0 < 10u) { }

    P4_REG(P4_ANA_PLL_CTRL0) |= CPU_PLL_CAL_STOP_B;
    return 0;
}

/* HP_SYS_CLKRST_PERI_CLK_CTRL00: FLASH_CLK_SRC_SEL [1:0],
 * FLASH_PLL_CLK_EN [2], FLASH_CORE_CLK_EN [3], FLASH_CORE_CLK_DIV_NUM [11:4]
 * (reset default 3; the ROM does not leave it there).
 *
 * The source encoding is the TRM's, and it is **not** the order an IDF array
 * initializer suggests. SOC_FLASH_CLKS lists {XTAL, CPLL, SPLL}, which is a
 * set and not a mapping; the field's actual values, from the TRM's own
 * description of FLASH_CLK_SRC_SEL, are
 *
 *     0: XTAL_CLK   1: SPLL_CLK (480 MHz)   2: CPLL_CLK (360 MHz)   3: Invalid
 *
 * -- 1 and 2 the other way round. This was inferred wrongly once and printed
 * a table that happened to be right only because this board reads 0. */
#define P4_PERI_CLK_CTRL00      (P4_CLKRST_BASE + 0x30)
#define FLASH_CORE_CLK_DIV_S    4

/* PMU: LPAON + 0x5000. HP_ACTIVE_HP_REGULATOR0 carries LP_DBIAS_VOL [8:4] and
 * HP_DBIAS_VOL [13:9], both **RO**, and DIG_REGULATOR0_DBIAS_SEL [14], R/W,
 * reset default 1. */
#define P4_PMU_BASE             0x50115000UL
#define P4_PMU_HP_ACT_REGULATOR0 (P4_PMU_BASE + 0x28)
#define PMU_HP_DBIAS_VOL_S      9
#define PMU_DBIAS_VOL_M         0x1Fu
#define PMU_DIG_REG0_DBIAS_SEL  (1u << 14)
#define PMU_HP_ACT_REGULATOR_XPD (1u << 18)

/* The external DC-DC, and the registers that hand the core over to it.
 * PMU_HP_ACTIVE_BIAS (0x18) DCM_VSET [22:18]; PMU_POWER_DCDC_SWITCH (0x10c)
 * FORCE_PU bit 0 (reset 1) and FORCE_PD bit 1; PMU_DCM_CTRL (0x204) ON_REQ
 * bit 0 and DONE_FORCE bit 7. */
#define P4_PMU_HP_ACTIVE_BIAS   (P4_PMU_BASE + 0x18)
#define PMU_DCM_VSET_S          18
#define PMU_DCM_VSET_M          0x1Fu
#define P4_PMU_DCDC_SWITCH      (P4_PMU_BASE + 0x10c)
#define PMU_FORCE_DCDC_SW_PU    (1u << 0)
#define PMU_FORCE_DCDC_SW_PD    (1u << 1)
#define P4_PMU_DCM_CTRL         (P4_PMU_BASE + 0x204)
#define PMU_DCDC_ON_REQ         (1u << 0)
#define PMU_DCDC_DONE_FORCE     (1u << 7)

/* IDF's HP_CALI_ACTIVE_DCM_VSET_DEFAULT, "For DCDC, about 1.25v". */
#define HP_CALI_ACTIVE_DCM_VSET 27u

/* eFuse BLK1 word 4, the same block drivers/efuse_esp32p4.c reads the factory
 * MAC out of: active_hp_dbias in [19:16]. IDF's get_act_hp_dbias() uses
 * (efuse + 16), capped at 31, and falls back to HP_CALI_ACTIVE_DBIAS_DEFAULT
 * = 24 when the field is zero -- which is also this register's reset value,
 * so on an uncalibrated part IDF's sequence would write back what is already
 * there. Whether that is true of *this* part is exactly what 34.3 needs. */
/* The *writable* twin of HP_DBIAS_VOL, and not the same field.
 * PMU_HP_ACTIVE_HP_REGULATOR_DBIAS is R/W at [31:27] with reset default 24;
 * HP_DBIAS_VOL at [13:9] is RO and reports what the regulator is actually
 * at. Reading one and writing the other is what `pmu_ll_hp_set_regulator_
 * dbias(hw, HP_ACTIVE, v)` does -- `hw->hp_sys[mode].regulator0.dbias`.
 * Both read 24 on this board, so the readback is a real check and not an
 * echo of the write. */
#define PMU_HP_ACT_REG_DBIAS_S  27
#define PMU_DBIAS_FIELD_M       0x1Fu

#define P4_EFUSE_RD_MAC_SYS_4   0x5012D054UL
#define EFUSE_ACT_HP_DBIAS_S    16
#define EFUSE_ACT_HP_DBIAS_M    0xFu
#define IDF_HP_DBIAS_DEFAULT    24u

void esp32p4_clock_sources_report(void) {
    /* --- CPLL, as configured (its power state is not readable; see above) */
    uint8_t div = 0, ref = 0;
    bool got = regi2c_read_cpll(I2C_CPLL_REG_DIV_7_0, &div) &&
               regi2c_read_cpll(I2C_CPLL_REG_REF_DIV, &ref);
    if (!got) {
        cprintf("[CLK] CPLL      = unreadable (analog I2C master clock gated)\n");
    } else {
        /* Raw dump of the block's first six registers, because the *number*
         * above is only worth reading if the bus is. CPLL's power state is
         * not readable (see this section's header), so a block that is off
         * cannot be distinguished from one that is on except by whether it
         * answers: identical bytes at every address mean a dead transaction,
         * and varied bytes mean the address field is being honoured. Six
         * lines of evidence for a one-line conclusion is the right ratio
         * when the conclusion is "this PLL is configured for N MHz". */
        cprintf("[CLK] CPLL raw  =");
        for (uint8_t r = 0; r < 8u; r++) {
            uint8_t v = 0;
            if (regi2c_read_cpll(r, &v)) cprintf(" %02x", (unsigned)v);
        }
        cprintf("   (regi2c 0x67, registers 0..7)\n");

        uint32_t refdiv = (uint32_t)(ref & 0xFu) + 1u;
        /* xtal*div/(ref_div+1), IDF's clk_ll_cpll_get_freq_mhz() above rev
         * v0.1, which this v1.3 board is.
         *
         * **Treat the result as a hint, not a frequency**, and 34.2's
         * write-up says why at length. Two reasons in short. IDF programs
         * div = 9 for 360 MHz and 10 for 400 on this revision, so any other
         * value is a configuration IDF never produces. And
         * clk_ll_cpll_set_config() carries the comment "div7_0 bit2 & bit3
         * is swapped from ECO1" -- a field whose write encoding is known to
         * differ from its bit order is not one to read a megahertz figure
         * out of and believe.
         *
         * The frequency this PLL actually runs at has to be *measured*,
         * which esp32p4_clint_measure_hz() above is able to do once 34.4
         * points HP_ROOT_CLK at it. */
        uint32_t xtal_mhz = (uint32_t)CONFIG_XTAL_HZ / 1000000u;
        cprintf("[CLK] CPLL      = div %u  ref_div %u  -> %u MHz "
                "(measurement-confirmed, 34.4; IDF programs 9/360, 10/400)\n",
                (unsigned)div, (unsigned)(ref & 0xFu),
                (unsigned)(xtal_mhz * (uint32_t)div / refdiv));
    }

    /* --- which PLLs have ever been calibrated on this boot */
    uint32_t ana = P4_REG(P4_ANA_PLL_CTRL0);
    cprintf("[CLK] pll cal   = cpu %u  sys %u  sdio %u  plla %u  mspi %u   "
            "[ANA_PLL_CTRL0 = 0x%08x]\n",
            (unsigned)((ana >> CPU_PLL_CAL_END_B) & 1u),
            (unsigned)((ana >> SYS_PLL_CAL_END_B) & 1u),
            (unsigned)((ana >> SDIO_PLL_CAL_END_B) & 1u),
            (unsigned)((ana >> PLLA_CAL_END_B) & 1u),
            (unsigned)((ana >> MSPI_CAL_END_B) & 1u),
            (unsigned)ana);

    /* --- what the flash -- and therefore .text -- is clocked from */
    uint32_t pc0 = P4_REG(P4_PERI_CLK_CTRL00);
    static const char *const flash_src[4] = {
        "XTAL", "SPLL 480M", "CPLL 360M", "INVALID"
    };
    static const uint32_t flash_src_hz[4] = {
        (uint32_t)CONFIG_XTAL_HZ, 480000000u, 360000000u, 0u
    };
    uint32_t fdiv = ((pc0 >> FLASH_CORE_CLK_DIV_S) & DIV_FIELD_MASK) + 1u;
    cprintf("[CLK] flash     = src %u (%s) / %u = %u MHz  "
            "pll_en %u core_en %u  [PERI_CLK_CTRL00 = 0x%08x]\n",
            (unsigned)(pc0 & 3u), flash_src[pc0 & 3u], (unsigned)fdiv,
            (unsigned)(flash_src_hz[pc0 & 3u] / fdiv / 1000000u),
            (unsigned)((pc0 >> 2) & 1u), (unsigned)((pc0 >> 3) & 1u),
            (unsigned)pc0);

    /* --- the regulator the ROM left behind */
    uint32_t reg0 = P4_REG(P4_PMU_HP_ACT_REGULATOR0);
    uint32_t hp_dbias = (reg0 >> PMU_HP_DBIAS_VOL_S) & PMU_DBIAS_VOL_M;
    uint32_t efuse = (P4_REG(P4_EFUSE_RD_MAC_SYS_4) >> EFUSE_ACT_HP_DBIAS_S)
                     & EFUSE_ACT_HP_DBIAS_M;
    uint32_t idf_would = efuse ? (efuse + 16u > 31u ? 31u : efuse + 16u)
                               : IDF_HP_DBIAS_DEFAULT;
    cprintf("[CLK] regulator = dbias ctrl %u (R/W)  indicated %u (RO)  "
            "sel %u (%s)  [0x%08x]\n",
            (unsigned)((reg0 >> PMU_HP_ACT_REG_DBIAS_S) & PMU_DBIAS_FIELD_M),
            (unsigned)hp_dbias,
            (unsigned)((reg0 & PMU_DIG_REG0_DBIAS_SEL) ? 1u : 0u),
            (reg0 & PMU_DIG_REG0_DBIAS_SEL) ? "software" : "hardware",
            (unsigned)reg0);
    uint32_t ctrl = (reg0 >> PMU_HP_ACT_REG_DBIAS_S) & PMU_DBIAS_FIELD_M;
    cprintf("[CLK] dbias cal = efuse %u -> trim %u; control %s (34.3)\n",
            (unsigned)efuse, (unsigned)idf_would,
            (idf_would == ctrl) ? "applied" : "NOT applied");
}

/* The CPU's own clock rate, measured -- the instrument 34.4 onwards is
 * checked against, and the only one that reports the thing this phase
 * actually changes.
 *
 * `mcycle` counts CPU clock cycles, so timing a window of it against the
 * systimer (which runs from the crystal and does not move when the CPU does,
 * kernel/time.c) gives the CPU frequency directly. Nothing derived, nothing
 * inferred from a divider.
 *
 * That matters more here than it looks. 34.2 established that CPLL's
 * divider cannot be trusted to mean a frequency -- IDF's own comment says
 * two bits of that field are swapped from ECO1 -- so the only way to learn
 * what a CPLL-sourced CPU clock actually is, is to measure it. And the
 * CLINT cannot stand in: whether *it* follows the CPU or the crystal is
 * itself an open question (kernel/ticker.c:181), which this function is
 * what finally answers.
 *
 * RV32, so `mcycle` is the low 32 bits and wraps every 2^32 cycles -- 11.9 s
 * at 360 MHz, far longer than any window here. Unsigned subtraction is
 * correct across a wrap, so the high half is not read and there is no
 * two-register sampling race to get wrong. */
uint32_t esp32p4_cpu_measure_hz(uint32_t window_us) {
    uint32_t c0 = (uint32_t)read_csr(mcycle);
    uint64_t u0 = time_get_us();
    while (time_get_us() - u0 < window_us) { /* spin */ }
    uint32_t c1 = (uint32_t)read_csr(mcycle);
    uint64_t u1 = time_get_us();

    uint64_t du = u1 - u0;
    if (du == 0u) return 0u;
    return (uint32_t)(((uint64_t)(c1 - c0) * 1000000ULL) / du);
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

    cprintf("[CLK] root      = %s (%u MHz, %s)\n", src_name[c.src & 3u],
            (unsigned)(c.root_hz / 1000000u),
            (c.src == SRC_XTAL) ? "CONFIG_XTAL_HZ" : "from its own divider");
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
    /* The same 2 ms window the ticker uses, but taken from a shell command
     * where every instruction on the path is already in cache. If this
     * agrees with the 1 s figure while the boot figure does not, the window
     * length is innocent and what differs is the state of the cache -- which
     * is the whole diagnosis of the boot-time bias. */
    cprintf("[CLK] CLINT     = %u Hz   (2 ms window, now -- warm)\n",
            (unsigned)esp32p4_clint_measure_hz(2000u));
    /* 34.4's instrument: mcycle against the crystal-referenced systimer.
     * The one figure here that measures the clock this phase changes. */
    cprintf("[CLK] CPU meas   = %u Hz   (mcycle, 100 ms window)\n",
            (unsigned)esp32p4_cpu_measure_hz(100000u));

    esp32p4_clock_sources_report();   /* 34.2 */
}


/* --- 34.3: the regulator, before anything moves ------------------------
 *
 * One field. That is the whole milestone, and 34.2 is why: the question
 * "does IDF's PMU/REGI2C analog bring-up have to be ported" was answered by
 * reading the board rather than by reading `rtc_clk_init()`, and most of
 * that sequence turns out to be already satisfied.
 *
 *   * `DIG_REGULATOR0_DBIAS_SEL` reads 1 -- control is already the PMU's,
 *     which is where IDF's sequence ends up.
 *   * `HP_ACTIVE_HP_REGULATOR_XPD` reads 1 -- the regulator is already on.
 *   * `HP_DBIAS_VOL` and the writable `..._REGULATOR_DBIAS` both read 24.
 *
 * 24 is `HP_CALI_ACTIVE_DBIAS_DEFAULT`, IDF's *uncalibrated fallback*. This
 * part is not uncalibrated: its eFuse `active_hp_dbias` reads 9, and IDF's
 * `get_act_hp_dbias()` turns that into 9 + 16 = 25. So the ROM left the
 * generic default in place and never applied this chip's own trim.
 *
 * ## Why one step matters, when 40 MHz plainly works without it
 *
 * The eFuse value is not a performance setting; it is process compensation.
 * IDF's own comment says what it is for -- *"hp_cali_dbias is read from
 * efuse to ensure that the hp_active_voltage is close to 1.15V"* -- so a
 * part whose trim says 25 is a part that needs 25 to reach the voltage the
 * silicon is characterised at. Leaving it at 24 leaves this chip slightly
 * *below* that, which is free at 40 MHz and is exactly the margin phase 34
 * intends to spend.
 *
 * ## Why the DCDC is deliberately not touched
 *
 * `rtc_clk_init()` also enables the DCDC and programs `dcm_vset`, and this
 * does neither. Three reasons, in order of weight:
 *
 *   1. **It is a hardware question this phase cannot answer from software.**
 *      The P4's internal buck needs an external inductor. The NANO
 *      schematic carries `EN_DCDC`, `FB_DCDC` and `VDDPST_DCDC` nets with a
 *      470K 1% feedback divider -- which reads far more like an *external*
 *      regulator IC than like the chip's own converter, and "reads like" is
 *      not a basis for enabling a buck converter.
 *   2. **The voltage this phase needs arrives without it.** The DCDC is an
 *      efficiency path; the LDO delivers the same dbias-selected rail.
 *   3. **The board is stable on the LDO path today**, at a known-good
 *      setting, and 34.3's job is to change one thing and prove it changed
 *      nothing else.
 *
 * If a later milestone measures a power or thermal problem at 360 MHz, this
 * is the first thing to revisit -- with the board's inductor identified
 * first.
 *
 * ## The two fields are not the same field, and they disagree
 *
 * TRM Register 11.x, verbatim:
 *
 *   * `HP_ACTIVE_HP_REGULATOR_DBIAS` [31:27], R/W -- *"Regulates the voltage
 *     of the HP sys regulator in HP_ACTIVE state. The higher the value, the
 *     higher the voltage."* This is the control, and it is what
 *     `pmu_ll_hp_set_regulator_dbias()` writes.
 *   * `HP_DBIAS_VOL` [13:9], RO -- *"Indicates the current voltage of the HP
 *     system regulator."*
 *   * `DIG_REGULATOR0_DBIAS_SEL` [14] -- *"0: Regulated by Hardware
 *     automatically, 1: Regulated by Software."* This board reads 1.
 *
 * After writing 25 into the control, the register reads back 0xce677180 --
 * the control field is 25 -- and **the indicator still reads 24**. That is
 * recorded rather than explained: it is not a failed write, since the
 * control holds the value, but it does mean the resulting voltage is not
 * independently confirmed from software. Settling it needs a meter on the
 * core rail, which is a question for the bench and not for this file.
 *
 * **`DIG_DBIAS_INIT` is not the missing commit, and was tried.** Bit 15 is
 * WT and the TRM calls it *"Initializes the PVT voltage configurations"*,
 * which reads like the trigger that would make the indicator follow. Setting
 * it drove the indicator to **20** -- down, not up, and away from the trim
 * this function exists to apply. Not used. Written down so the next reader
 * does not spend the same reboot finding out.
 *
 * ## What this function does not do
 *
 * It does not lower anything, ever, and it does not write a value it made
 * up. If the eFuse is unburnt (0) it leaves the register alone: IDF's
 * fallback for that case is 24, which is what is already there.
 */
/* --- 34.5a: putting the digital regulator under register control --------
 *
 * 34.3 wrote this chip's eFuse trim into HP_ACTIVE_HP_REGULATOR_DBIAS and the
 * regulator's own indicator did not move. 34.5 then found 360 MHz failing
 * deterministically while 180 -- same MEM, same flash, same MSPI -- is fine,
 * which leaves the core voltage as the only thing that differs. The missing
 * piece is the part of ESP-IDF's rtc_clk_init() that 34.3 skipped.
 *
 * Ported from ~/gith/esp/esp-idf, components/esp_hw_support/port/esp32p4/
 * rtc_clk_init.c, in its order, with field definitions from
 * components/soc/esp32p4/include/soc/regi2c_dig_reg.h and regi2c_bias.h:
 *
 *     REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_FORCE_RTC_DREG, 1);
 *     REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_FORCE_DIG_DREG, 1);
 *     REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_XPD_RTC_REG,    0);
 *     REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_XPD_DIG_REG,    0);
 *     REGI2C_WRITE_MASK(I2C_BIAS, I2C_BIAS_OR_FORCE_XPD_CK,           0);
 *     REGI2C_WRITE_MASK(I2C_BIAS, I2C_BIAS_OR_FORCE_XPD_REF_OUT_BUF,  0);
 *     REGI2C_WRITE_MASK(I2C_BIAS, I2C_BIAS_OR_FORCE_XPD_IPH,          0);
 *     REGI2C_WRITE_MASK(I2C_BIAS, I2C_BIAS_OR_FORCE_XPD_VGATE_BUF,    0);
 *
 * FORCE_*_DREG = 1 is the one that matters: it takes the regulator voltage
 * off the analog block's own control and puts it on the register this kernel
 * has been writing since 34.3. The XPD_*_REG = 0 pair powers down the
 * analog-side regulators that the PMU is superseding, and the four I2C_BIAS
 * bits clear force-on overrides for the bias network.
 *
 * ## What is deliberately NOT ported, and why that is a judgement not an
 * ## oversight
 *
 * IDF continues past this point to **switch the core onto an external DC-DC
 * converter** and then disable the internal LDO:
 *
 *     pmu_ll_set_dcdc_en(&PMU, true);
 *     pmu_ll_hp_set_dcm_vset(&PMU, PMU_MODE_HP_ACTIVE, hp_dcmvset);
 *     ...
 *     pmu_ll_hp_set_regulator_xpd(&PMU, PMU_MODE_HP_ACTIVE, false);
 *
 * That DC-DC is a chip on the board, not a block in the SoC --
 * components/esp_hw_support/port/esp32p4/Kconfig.dcdc says so plainly, naming
 * "TI-TLV62569/TLV62569P" and warning that "the same parameter value may
 * correspond to different voltage values on different models of DCDC chips".
 * The NANO has `EN_DCDC` and `FB_DCDC` nets with a 470K 1% feedback divider
 * and carries MPS parts (MP1605/MP1658), **not** the TI one those vset
 * numbers are calibrated for. `HP_CALI_ACTIVE_DCM_VSET_DEFAULT = 27` is a
 * number for somebody else's regulator.
 *
 * So the LDO keeps supplying the core here, at the voltage this chip's own
 * eFuse asks for, and the external converter is left exactly as the ROM left
 * it. If 360 MHz needs more than the LDO can give, that is a finding to
 * report -- not a reason to program a foreign vset into a feedback divider.
 *
 * LP dbias is skipped for the same reason: IDF's get_act_lp_dbias() adds
 * "+16 +4" with the comment "efuse dbias need to add 4 to near to dcdc
 * voltage", which is an adjustment relative to a DC-DC this is not enabling.
 */
#define I2C_DIG_REG_BLK         0x6Du
#define REGI2C_DIG_REG_MST_SEL  (1u << 10)
#define I2C_BIAS_BLK            0x6Au
#define REGI2C_BIAS_MST_SEL     (1u << 12)

#define DIG_REG_FORCE_REG       10u   /* FORCE_RTC_DREG bit 0, FORCE_DIG_DREG bit 1 */
#define DIG_REG_XPD_REG         13u   /* XPD_RTC_REG bit 2, XPD_DIG_REG bit 3 */
#define BIAS_FORCE_XPD_REG      4u    /* four force-on overrides, bits 0..3 */

static bool regulator_force_register_control(void) {
    bool ok = true;
    ok &= regi2c_write_bit(I2C_DIG_REG_BLK, REGI2C_DIG_REG_MST_SEL,
                           DIG_REG_FORCE_REG, 0, 1);   /* FORCE_RTC_DREG */
    ok &= regi2c_write_bit(I2C_DIG_REG_BLK, REGI2C_DIG_REG_MST_SEL,
                           DIG_REG_FORCE_REG, 1, 1);   /* FORCE_DIG_DREG */
    ok &= regi2c_write_bit(I2C_DIG_REG_BLK, REGI2C_DIG_REG_MST_SEL,
                           DIG_REG_XPD_REG, 2, 0);     /* XPD_RTC_REG */
    ok &= regi2c_write_bit(I2C_DIG_REG_BLK, REGI2C_DIG_REG_MST_SEL,
                           DIG_REG_XPD_REG, 3, 0);     /* XPD_DIG_REG */

    for (unsigned b = 0; b < 4u; b++) {                /* I2C_BIAS reg 4 [3:0] */
        ok &= regi2c_write_bit(I2C_BIAS_BLK, REGI2C_BIAS_MST_SEL,
                               BIAS_FORCE_XPD_REG, b, 0);
    }
    return ok;
}

bool esp32p4_regulator_apply_efuse_dbias(uint32_t *from, uint32_t *to,
                                         uint32_t *indicated) {
    uint32_t reg0 = P4_REG(P4_PMU_HP_ACT_REGULATOR0);
    uint32_t now_dbias = (reg0 >> PMU_HP_ACT_REG_DBIAS_S) & PMU_DBIAS_FIELD_M;
    uint32_t efuse = (P4_REG(P4_EFUSE_RD_MAC_SYS_4) >> EFUSE_ACT_HP_DBIAS_S)
                     & EFUSE_ACT_HP_DBIAS_M;

    if (from) *from = now_dbias;
    if (to)   *to   = now_dbias;
    if (indicated) {
        *indicated = (reg0 >> PMU_HP_DBIAS_VOL_S) & PMU_DBIAS_FIELD_M;
    }

    if (efuse == 0u) {
        return false;   /* unburnt: IDF's fallback is the reset value */
    }
    uint32_t want = efuse + 16u;
    if (want > 31u) want = 31u;
    if (want <= now_dbias) {
        return false;   /* never step down; see the header */
    }

    /* The analog master needs its own source clock before any of this, the
     * same lesson 34.5's CPLL work learned the hard way. */
    P4_REG(P4_ANA_MST_CLK160M) |= ANA_MST_SEL_160M;

    /* Take the regulator voltage off analog control and put it on the
     * register about to be written. Without this the write below lands and
     * the regulator ignores it -- which is what 34.3 measured. */
    if (!regulator_force_register_control()) {
        /* The analog bus refused. Say so: the dbias write below will land in
         * a register the regulator is not listening to, which is precisely
         * the state 34.3 spent a milestone not understanding. */
        printk("[PMU] regi2c: could not put the regulator under register "
               "control; dbias will not take effect\n");
    }

    /* HP_ACTIVE regulator enabled (IDF sets this true before the dbias;
     * reset default is already 1 on this board). */
    reg0 |= PMU_HP_ACT_REGULATOR_XPD;

    reg0 &= ~(PMU_DBIAS_FIELD_M << PMU_HP_ACT_REG_DBIAS_S);
    reg0 |= (want & PMU_DBIAS_FIELD_M) << PMU_HP_ACT_REG_DBIAS_S;
    P4_REG(P4_PMU_HP_ACT_REGULATOR0) = reg0;

    /* --- and the core moves onto the external DC-DC -------------------
     *
     * This is what 34.5 found the LDO could not do. With the regulator under
     * register control at this chip's eFuse trim -- verified by reading the
     * analog bits back -- 360 MHz still failed, while 180 was fine. The LDO's
     * calibration target is ~1.15 V; the DC-DC's active setting is ~1.25 V,
     * and that gap is the top clock step.
     *
     * ESP-IDF does this unconditionally on every ESP32-P4 boot -- it is not
     * behind a Kconfig gate -- so every board running stock IDF has its core
     * on the external converter, this one included: Waveshare's own shipped
     * firmware (~/gith/esp/ESP32-P4-Platform/firmware/brookesia) overrides
     * neither the CPU frequency nor anything in the DCDC menu.
     *
     * Order is rtc_clk_init.c's, exactly:
     *
     *     pmu_ll_set_dcdc_en(&PMU, true);                  // done_force=0, on_req=1
     *     pmu_ll_set_dcdc_switch_force_power_down(&PMU, false);  // force_pu=0, force_pd=0
     *     pmu_ll_hp_set_dcm_vset(&PMU, HP_ACTIVE, hp_dcmvset);
     *     SET_PERI_REG_MASK(..., PMU_DIG_REGULATOR0_DBIAS_SEL);
     *     esp_rom_delay_us(1000);
     *     pmu_ll_hp_set_regulator_xpd(&PMU, HP_ACTIVE, false);   // LDO off
     *
     * The vset is IDF's own arithmetic: max(PVT, 27), where PVT is the RO
     * HP_DBIAS_VOL field. **That field is not a dbias readback** -- IDF reads
     * it as `pvt_hp_dcmvset` and uses it as a floor for the DC-DC setting,
     * which finally explains why it never followed the dbias writes in 34.3.
     * It reads 24 here, so the floor does not bind and the setting is 27.
     *
     * Not invented, and deliberately not adjusted: 27 is the vendor's active
     * figure. The board carries MPS parts (MP1605/MP1658) rather than the
     * TI TLV62569 IDF's Kconfig help names, and its feedback network is
     * Waveshare's -- which is exactly why the number to use is the one the
     * stock firmware this board ships with uses, rather than one derived
     * here from a different regulator's datasheet. */
    uint32_t pvt = (P4_REG(P4_PMU_HP_ACT_REGULATOR0) >> PMU_HP_DBIAS_VOL_S)
                   & PMU_DBIAS_FIELD_M;
    uint32_t vset = (pvt > HP_CALI_ACTIVE_DCM_VSET) ? pvt : HP_CALI_ACTIVE_DCM_VSET;

    P4_REG(P4_PMU_DCM_CTRL) &= ~PMU_DCDC_DONE_FORCE;
    P4_REG(P4_PMU_DCM_CTRL) |= PMU_DCDC_ON_REQ;

    uint32_t sw = P4_REG(P4_PMU_DCDC_SWITCH);
    sw &= ~(PMU_FORCE_DCDC_SW_PU | PMU_FORCE_DCDC_SW_PD);
    P4_REG(P4_PMU_DCDC_SWITCH) = sw;

    uint32_t bias = P4_REG(P4_PMU_HP_ACTIVE_BIAS);
    bias &= ~(PMU_DCM_VSET_M << PMU_DCM_VSET_S);
    bias |= (vset & PMU_DCM_VSET_M) << PMU_DCM_VSET_S;
    P4_REG(P4_PMU_HP_ACTIVE_BIAS) = bias;

    /* DBIAS_SEL, as IDF does -- "Hand over control of dbias to pmu" -- then
     * its 1 ms settle before the LDO is taken away. */
    P4_REG(P4_PMU_HP_ACT_REGULATOR0) |= PMU_DIG_REG0_DBIAS_SEL;
    uint64_t t0 = time_get_us();
    while (time_get_us() - t0 < 1000u) { }

    /* The LDO off, last, with the converter carrying the core. */
    P4_REG(P4_PMU_HP_ACT_REGULATOR0) &= ~PMU_HP_ACT_REGULATOR_XPD;


    /* Both, because they disagree and hiding either would be the wrong kind
     * of tidy: `to` is the control as it now reads, `indicated` is what the
     * regulator says it is delivering. */
    uint32_t after = P4_REG(P4_PMU_HP_ACT_REGULATOR0);
    *to = (after >> PMU_HP_ACT_REG_DBIAS_S) & PMU_DBIAS_FIELD_M;
    if (indicated) *indicated = (after >> PMU_HP_DBIAS_VOL_S) & PMU_DBIAS_FIELD_M;
    return true;
}


/* --- 34.4: pointing HP_ROOT_CLK at the PLL -----------------------------
 *
 * The first thing in this phase that changes a clock.
 *
 * ## Why this does not touch CPLL, and measures instead
 *
 * 34.2 left CPLL's frequency genuinely unknown: the PLL is up and calibrated
 * (CPU_PLL_CAL_END reads 1, and nothing in this kernel calibrates anything,
 * so the ROM did it), but its divider reads 8 -- a value ESP-IDF never
 * programs, and in a field IDF's own source says has two bits swapped from
 * ECO1. So the register cannot be read for a megahertz figure.
 *
 * Rather than guess or reprogram, this milestone **selects the PLL and finds
 * out**. CPU_CLK = CPLL / 4 either way, so the measured result names the
 * PLL: 90 MHz means CPLL is 360, 80 MHz means it is 320. One change, one
 * measurement, and an answer 34.2 could not get by reading.
 *
 * Reprogramming CPLL is 34.5's problem if the number comes out wrong, and
 * keeping it out of here is the same discipline as everywhere else in this
 * phase: move one thing.
 *
 * ## The divider table is not ours to invent
 *
 * MEM_CLK <= 200 MHz and APB_CLK <= 100 MHz, and the dividers cascade
 * (34.1), so the combinations are constrained. ESP-IDF enumerates exactly
 * three for revisions below v3.0 and **aborts on anything else**, with a
 * reason worth repeating in full:
 *
 *     "This is dangerous to modify dividers. Hardware could automatically
 *      correct the divider, and it won't be reflected to the registers.
 *      Therefore, you won't even be able to calculate out the real mem_clk,
 *      apb_clk freq."
 *
 * A silently corrected divider is a board whose clocks are not what any
 * register says they are -- the one failure this phase has no instrument
 * for. So this refuses unknown frequencies rather than computing dividers.
 *
 *     CPU   cpu_div  mem_div  sys_div  apb_div     MEM   SYS   APB
 *     360      1        2        1        2        180   180    90
 *     180      2        1        1        2        180   180    90
 *      90      4        1        1        1         90    90    90
 *
 * ## Order, and the commit nobody can see
 *
 * TRM 11.2.4.1: "Updates to HP_SYS_CLKRST_ROOT_CLK_CTRL0/1/2/3_REG will take
 * effect only after HP_SYS_CLKRST_SOC_CLK_DIV_UPDATE is set." A divider
 * written without that is a divider that did nothing, and the symptom is
 * simply that the board keeps running at its old speed.
 *
 * Upscaling goes APB, SYS, MEM, then CPU -- each committed -- and the source
 * mux **last**, because the mux is not covered by the update bit and
 * switching it first would run the old dividers against the new root. IDF's
 * own comment: in the intermediate state "the frequency of APB/MEM does not
 * meet the timing requirements ... some exception might occur".
 */

#define SOC_CLK_DIV_UPDATE_B    (1u << 4)   /* ROOT_CLK_CTRL0, WT */

static void root_div_commit(void) {
    P4_REG(P4_ROOT_CLK_CTRL0) |= SOC_CLK_DIV_UPDATE_B;
    while (P4_REG(P4_ROOT_CLK_CTRL0) & SOC_CLK_DIV_UPDATE_B) { }
}

static void root_field_set(uintptr_t reg, unsigned shift, uint32_t div) {
    uint32_t v = P4_REG(reg);
    v &= ~(DIV_FIELD_MASK << shift);
    v |= ((div - 1u) & DIV_FIELD_MASK) << shift;   /* encoded as div-1 */
    P4_REG(reg) = v;
    root_div_commit();
}

/* ets_update_cpu_frequency(), ROM 0x4fc00044 -- identical in both ROM
 * variants (34.1 checked). Bookkeeping, and not optional: the ROM's own
 * microsecond delay loops are calibrated from this number, and drivers/
 * flash_esp32p4.c reaches ROM code that delays. Forgetting it leaves those
 * delays short by exactly the ratio this function just changed, with a
 * failure that is timing-dependent and does not point at itself. */
#define ROM_ETS_UPDATE_CPU_FREQUENCY  0x4fc00044u
typedef void (*rom_update_cpu_freq_t)(uint32_t ticks_per_us);

/* Draining the console before the mux moves, without the ROM's help.
 *
 * `uart_tx_wait_idle` is in the ROM at 0x4fc00078 and at the same address in
 * both variants, and IDF calls its equivalent for exactly this purpose. It
 * was tried here and **it hangs**: this kernel reconfigured UART0 -- source
 * clock, baud, FIFO thresholds -- in drivers/uart_esp32p4.c, and the ROM
 * routine polls for a condition that never arrives against that setup. The
 * symptom was a board that printed "CPLL 320 -> 360" and then nothing at
 * all, with the console dead to input as well; the mux was never reached.
 *
 * So: our own drain, against the register drivers/uart_esp32p4.c:295 already
 * uses and has confirmed against TRM Register 45.21 -- TXFIFO_CNT in
 * [23:16]. Bounded, for the reason every wait in this file is bounded.
 *
 * The FIFO going empty is necessary but not sufficient: the last character
 * is still in the transmit shift register. One character at 115200 8N1 is
 * 87 us, so 200 us of settle covers it with margin and costs nothing once at
 * boot. */
#define P4_UART0_STATUS   ((uintptr_t)CONFIG_UART0_BASE + 0x1c)
#define UART_TXFIFO_CNT_S 16
#define UART_TXFIFO_CNT_M 0xffu

static void console_tx_drain(void) {
    uint64_t t0 = time_get_us();
    while (((P4_REG(P4_UART0_STATUS) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT_M) != 0u) {
        if (time_get_us() - t0 > 20000u) break;   /* 20 ms is "never" here */
    }
    t0 = time_get_us();
    while (time_get_us() - t0 < 200u) { }         /* shift register */
}

bool esp32p4_cpu_freq_set(uint32_t mhz, uint32_t *measured_hz) {
    uint32_t cpu_div, mem_div, sys_div = 1u, apb_div;

    switch (mhz) {
    case 40:  return false;               /* the crystal: nothing to do */
    case 90:  cpu_div = 4u; mem_div = 1u; apb_div = 1u; break;
    case 180: cpu_div = 2u; mem_div = 1u; apb_div = 2u; break;
    case 360: cpu_div = 1u; mem_div = 2u; apb_div = 2u; break;
    default:  return false;               /* refuse; see the header */
    }

    /* CPLL first, while HP_ROOT_CLK is still the crystal and the PLL is not
     * under anyone's feet. The ROM leaves it at 320 MHz (34.4 measured it),
     * which would make every entry in the divider table 8/9 of its nominal;
     * at 360 the table's names and the board's frequencies finally agree. */
    if (cpll_hz() != 360000000u) {
        uint32_t was = cpll_hz() / 1000000u;
        int rc = cpll_configure_360();
        printk("[CLK] CPLL %u -> %u MHz (rc=%d)\n", (unsigned)was,
               (unsigned)(cpll_hz() / 1000000u), rc);
        if (rc != 0) {
            /* Stay on the crystal. A board at 40 MHz with a console is worth
             * far more than one at 360 without, and the step number says
             * which half failed: 1-3 are the analog bus, 4 is the PLL. */
            printk("[CLK] CPLL would not take 360 MHz (step %d); staying on "
                   "the crystal\n", rc);
            return false;
        }
    }

    /* Upscaling order, slowest-moving first, each committed before the next. */
    root_field_set(P4_ROOT_CLK_CTRL2, APB_DIV_NUM_S, apb_div);
    root_field_set(P4_ROOT_CLK_CTRL1, SYS_DIV_NUM_S, sys_div);
    root_field_set(P4_ROOT_CLK_CTRL1, MEM_DIV_NUM_S, mem_div);
    root_field_set(P4_ROOT_CLK_CTRL0, CPU_DIV_NUM_S, cpu_div);

    /* Drain the console before the mux moves.
     *
     * The UART's *baud* is immune -- it is clocked from the crystal, which is
     * the whole reason phase 34 can debug itself (§2). Its APB-side register
     * interface is not: APB goes 10 -> 90 MHz at this instant, and a
     * character already in the transmit path comes out mangled.
     *
     * Measured, because it looked like something far worse. The first 360 MHz
     * attempt printed the PMU line and then apparently nothing, which reads
     * exactly like a board that died at the switch. It had not: it was
     * running fine and its console output was garbage. Instrumenting the
     * steps showed a clean "dividers committed", then
     * `[CLK]<garbage>alive`, then clean lines again -- corruption confined to
     * the bytes in flight.
     *
     * The ROM has a routine for this and it cannot be used here; see
     * console_tx_drain() for what happened when it was tried. */
    console_tx_drain();

    /* And the mux, last. Not covered by the update bit. */
    uint32_t sel = P4_REG(P4_LP_HP_CLK_CTRL);
    sel = (sel & ~HP_ROOT_CLK_SRC_SEL_M) | SRC_CPLL;
    P4_REG(P4_LP_HP_CLK_CTRL) = sel;

    /* Tell the ROM what the CPU frequency *is*, not what was asked for.
     *
     * Those differ on this board and the difference is the whole of 34.4:
     * CONFIG_CPU_FREQ_MHZ names an entry in IDF's divider table, whose
     * nominal assumes CPLL is 360 MHz, and this board's ROM configures CPLL
     * to 320. Asking for the "90" entry therefore yields 80.
     *
     * Handing the nominal to the ROM would leave every ROM delay loop --
     * flash, cache, PMU -- calibrated 12.5% wrong. Long rather than short
     * here, so it would not have broken anything and would not have shown
     * up; measuring first costs 20 ms once at boot and removes the class. */
    uint32_t hz = esp32p4_cpu_measure_hz(20000u);
    if (measured_hz) *measured_hz = hz;
    uint32_t got_mhz = (hz + 500000u) / 1000000u;   /* nearest MHz */
    if (got_mhz == 0u) got_mhz = mhz;               /* measurement failed */
    ((rom_update_cpu_freq_t)(uintptr_t)ROM_ETS_UPDATE_CPU_FREQUENCY)(got_mhz);
    return true;
}

#endif /* CONFIG_BOARD_ESP32P4 */
