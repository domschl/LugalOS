#include "arch/trap.h"
#include "arch/csr.h"
#include "kernel/printk.h"
#include "kernel/hart.h"
#include "kernel/console.h"
#include "kernel/ipc.h"
#include "kernel/chan.h"
#include "kernel/sched.h"
#include "kernel/uaccess.h"
#include "kernel/ticker.h"
#include "kernel/devirq.h"
#include "kernel/time.h"
#include <stdbool.h>
#include "fs/vfs.h"

#if defined(CONFIG_BOARD_RP2350)
#include "drivers/usb_cdc.h"
#include "arch/rp2350_bootrom.h"
#endif
#if defined(CONFIG_BOARD_ESP32P4)
#include "arch/esp32p4_intr.h"
#endif

#if defined(CONFIG_BOARD_RP2350) || defined(CONFIG_BOARD_ESP32P4)
/* Both boards reach memory-mapped controller registers from this file; the
 * QEMU targets use their own casts inline in the PLIC helpers below. */
#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

/* M5 Phase 5, plan/phase12_microkernel_migration.md: SYS_CHAN_SERVE_WAIT/
 * _REPLY's own kernel-side scratch buffer, shared by every U-mode driver
 * task's serve loop (SYS_CHAN_SERVE_WAIT/_REPLY cases below). Grown from
 * the original 256 bytes (M5 Phase 2) because drivers/spisd_rp2350.c's
 * blk moves whole 512-byte SD sectors -- even a single sector already
 * overflows 256 bytes before counting its 9-byte header. Sized to one
 * sector (512) plus 64 bytes of header/opcode headroom, comfortably
 * covering blk's 521-byte request (BLK_HDR_LEN 9 + 512) and every other
 * driver's much smaller messages -- not sized past a single block
 * transfer, since drivers/spisd_rp2350.c's own chunking keeps every wire
 * message capped at one sector regardless of caller-requested count. */
#define CHAN_SERVE_KBUF_CAP 576

/* M2, plan/phase12_microkernel_migration.md: identifying which external IRQ
 * fired is genuinely different hardware on each target -- there is no Rule 0
 * to apply here, only real controllers. What sits above this (devirq.h) is
 * arch-independent; this is the seam.
 *
 * The discriminator is CONFIG_BOARD_RP2350, deliberately NOT CONFIG_MODE_M:
 * QEMU RV32 is also CONFIG_MODE_M (a plain M-mode target, no S-mode
 * transition -- see entry.S), but it is not RP2350 and has none of Hazard3's
 * Xh3irq CSRs. Conflating "M-mode" with "Hazard3" here is exactly the
 * mistake that would make QEMU hide a real RP2350 divergence in the other
 * direction: not "QEMU is more permissive than hardware" but "QEMU is a
 * different CPU than hardware", and an meinext read on QEMU RV32 is an
 * illegal instruction, not a hardware validation of anything. Found by
 * exactly that: RV32 hung during boot the first time this shipped gated on
 * CONFIG_MODE_M, while RV64 (CONFIG_MODE_S, never reaches this branch) was
 * unaffected -- the asymmetry was the tell. */
#if defined(CONFIG_BOARD_RP2350)
/* Hazard3's Xh3irq external interrupt array (RP2350 datasheet §3.6.7.5
 * region; register layout confirmed against the Pico SDK's own
 * hardware/regs/rvcsr.h and hardware_irq/irq.c rather than guessed --
 * exactly the kind of RP2350-specific register work this tree has gotten
 * wrong before by not doing that (mem_domain.c's erratum E6 notes).
 *
 * meinext, plain-read (no UPDATE bit): bit 31 (NOIRQ) set means nothing is
 * both pending and enabled; otherwise bits [10:2] hold the IRQ number,
 * pre-shifted by 2 for a jump table this kernel does not use. UPDATE (bit 0)
 * is what the Pico SDK's own vectored dispatch writes to advance
 * meicontext's preemption-priority floor -- deliberately never written here:
 * this kernel has one flat interrupt priority, the same way the timer
 * interrupt is already handled with no nesting, so there is no floor to
 * track. A bare read has no side effect beyond returning the current
 * pending+enabled IRQ, which is all this needs. */
static inline uint32_t meinext_irq(bool *valid) {
    uintptr_t v;
    __asm__ __volatile__("csrr %0, 0xbe4" : "=r"(v)); /* RVCSR_MEINEXT */
    *valid = (v & 0x80000000u) == 0;
    return (uint32_t)((v >> 2) & 0x1ffu);
}
#elif defined(CONFIG_BOARD_ESP32P4)
/* ESP32-P4: the CLIC, E3 (plan/phase27_esp32p4_bringup.md).
 *
 * Not the standard one. IDF gates on CONFIG_ESP32P4_SELECTS_REV_LESS_V3 and
 * this board is v1.3, so: the interrupt threshold is a memory-mapped
 * register rather than the mintthresh CSR, and mintstatus sits at 0x346
 * instead of 0xFB1 (components/soc/esp32p4/include/soc/interrupt_reg.h says
 * both, in those words). Neither CSR is read here -- this kernel has one
 * flat interrupt priority and never asks what level it is running at -- but
 * they are why "write it against the CLIC specification" would have been
 * the wrong instruction to follow.
 *
 * Layout from TRM section 2.9.2.6's register summary, cross-checked against
 * IDF's soc/clic_reg.h, which describes the same registers through a
 * word-wide view (CLIC_INT_CTRL_REG(i): IP bit 0, IE bit 8, SHV bit 16,
 * TRIG [18:17], MODE [23:22], CTL [31:24]). Two independent descriptions
 * that agree, which is section 3.2's rule about not inferring register
 * layouts satisfied twice over.
 *
 * The per-interrupt registers are byte-sized and the TRM says so explicitly
 * ("clicintip[i], clicintie[i], clicintattr[i] and clicintctl[i] registers
 * are all byte sized"), so they are accessed as bytes here. */
#define P4_CLIC_BASE        0x20800000UL
#define P4_CLIC_CFG         (P4_CLIC_BASE + 0x0000UL)  /* mcliccfg          */
#define P4_CLIC_INFO        (P4_CLIC_BASE + 0x0004UL)  /* clicinfo (RO)     */
#define P4_CLIC_THRESH      (P4_CLIC_BASE + 0x0008UL)  /* mintthresh, TH<<24 */
#define P4_CLIC_CTRL_BASE   (P4_CLIC_BASE + 0x1000UL)
#define P4_CLIC_IP(i)       (P4_CLIC_CTRL_BASE + 4UL * (i) + 0UL)
#define P4_CLIC_IE(i)       (P4_CLIC_CTRL_BASE + 4UL * (i) + 1UL)
#define P4_CLIC_ATTR(i)     (P4_CLIC_CTRL_BASE + 4UL * (i) + 2UL)
#define P4_CLIC_CTL(i)      (P4_CLIC_CTRL_BASE + 4UL * (i) + 3UL)

/* mcliccfg, legacy (pre-v3) field positions: NVBITS bit 0 (RO, reads 1),
 * MNLBITS [4:1], NMBITS [6:5] (RO, 0 = machine mode only). The v3 silicon
 * moves MNLBITS to [3:0] and adds S/U-mode copies at [19:16] and [27:24] --
 * which is precisely why this is written out rather than borrowed from a
 * header that defines both sets and lets sdkconfig choose. */
#define P4_CLIC_CFG_MNLBITS_SHIFT 1
#define P4_CLIC_CFG_MNLBITS_MASK  (0xfu << P4_CLIC_CFG_MNLBITS_SHIFT)

/* clicintattr[i]: SHV bit 0, TRIG [2:1], MODE [7:6]. */
#define P4_CLIC_ATTR_SHV          (1u << 0)
#define P4_CLIC_ATTR_TRIG_LEVEL   (0u << 1)
#define P4_CLIC_ATTR_MODE_M       (3u << 6)

/* CLICINTCTLBITS is 3 on this implementation, so only clicintctl[7:5] are
 * writable and the low five bits read back as 1 whatever is written. 0xff is
 * therefore the maximum: level 255 under mcliccfg.MNLBITS = 0, and level 7
 * (encoded 255) under any other value of MNLBITS. That the same constant is
 * right under every encoding is the point -- see clic_init(). */
#define P4_CLIC_CTL_MAX           0xffu

#define P4_CLIC_IRQ_TIMER   7u
#define P4_CLIC_IRQ_COUNT   48u

/* The interrupt matrix (TRM chapter 13), per core: CPU0's window at
 * 0x500D6000 and CPU1's 0x800 above it (IDF reg_base.h,
 * DR_REG_INTERRUPT_CORE0_BASE = DR_REG_HPPERIPH1_BASE + 0x16000, and TRM
 * Table 9.3-2 gives INTMTX as 0x500D_6000..0x500D_6FFF).
 *
 * Unlike the CLIC, this block is *not* self-relative: there is no window
 * that means "my core". A core writing CORE0's registers routes interrupts
 * to core 0 no matter which core executed the write, so the base has to be
 * chosen from hart_id() rather than assumed -- the same mistake phase 23's
 * X1 found in the PLIC path here, avoided in advance this time. */
#define P4_INTMTX_CORE0     0x500D6000UL
#define P4_INTMTX_CORE_STEP 0x800UL

static inline uintptr_t p4_intmtx_base(void) {
    return P4_INTMTX_CORE0 + (uintptr_t)hart_id() * P4_INTMTX_CORE_STEP;
}

static inline volatile uint8_t *p4_clic_byte(uintptr_t addr) {
    return (volatile uint8_t *)addr;
}

/* See arch/esp32p4_intr.h. */
int esp32p4_intmtx_route(uint32_t src, uint32_t clic_id) {
    if (clic_id < ESP32P4_CLIC_IRQ_MIN || clic_id > ESP32P4_CLIC_IRQ_MAX) {
        printk("[CLIC] Refusing to route source %u to line %u: "
               "only %u..%u are external interrupts\n",
               (unsigned)src, (unsigned)clic_id,
               (unsigned)ESP32P4_CLIC_IRQ_MIN, (unsigned)ESP32P4_CLIC_IRQ_MAX);
        return -1;
    }
    /* Sources 0..127 have their mapping register at offset 4*src. The chip
     * has three more (128..130) but they do not continue the run: offsets
     * 0x200..0x210 are the interrupt-status registers and the block's clock
     * gate, and the last three mapping registers resume at 0x214. So the
     * bound here is where the arithmetic stops being true, not where the
     * source list stops -- computing 4*130 would write the clock gate. If a
     * later milestone needs one of those three, it needs the offset table,
     * not a bigger constant. */
    if (src > 127u) {
        printk("[CLIC] Refusing to route source %u: only 0..127 are at "
               "offset 4*src\n", (unsigned)src);
        return -1;
    }
    /* The CLIC ID goes in bits [5:0]. The two bits above it
     * (SRC_PASS_IN_SEC, SRC_IN_SEC_FLAG) are the U-mode interrupt-remapping
     * controls this kernel does not use, so a whole-word store of the ID is
     * also the correct way to leave them clear. */
    REG(p4_intmtx_base() + 4UL * src) = clic_id;
    return 0;
}

/* See arch/esp32p4_intr.h. Same three writes arch_irq_enable() makes for a
 * device line, on the one ID it refuses -- level-triggered because the
 * CLINT asserts the timer interrupt for as long as mtime >= mtimecmp and
 * the TRM's own de-assert instruction is to move the comparator (section
 * 2.9.3.4), which is exactly what ticker_next() does on the way out of the
 * handler. */
void esp32p4_clic_timer_enable(void) {
    *p4_clic_byte(P4_CLIC_ATTR(P4_CLIC_IRQ_TIMER)) =
        (uint8_t)(P4_CLIC_ATTR_MODE_M | P4_CLIC_ATTR_TRIG_LEVEL);
    *p4_clic_byte(P4_CLIC_CTL(P4_CLIC_IRQ_TIMER)) = P4_CLIC_CTL_MAX;
    *p4_clic_byte(P4_CLIC_IE(P4_CLIC_IRQ_TIMER)) = 1;
}

/* Drop this core's active interrupt level back to zero, without leaving the
 * handler.
 *
 * E4, and the hazard that milestone existed to find. In CLIC mode taking an
 * interrupt sets mintstatus.MIL to that interrupt's level, and the effective
 * threshold is max(mintthresh.TH, mintstatus.MIL) -- so while a handler runs,
 * every interrupt at or below its level is masked. Exactly one thing lowers
 * MIL again: mret, which restores it from mcause.MPIL.
 *
 * That is fine for a handler that returns. This kernel's timer handler does
 * not: preemption works by calling sched_yield() *inside* the handler, so
 * control leaves through ctx_switch() and the mret that would restore MIL
 * stays stranded in a stack frame, unwound only when that task is resumed.
 * Every interrupt at level 255 -- which is all of them here -- stays masked
 * for as long as the switched-to task runs. The tick that would preempt it
 * is masked too, so nothing brings it back; only a cooperative yield
 * eventually unwinds the chain.
 *
 * Measured, not deduced. `clicdump` samples mintstatus from inside a loop
 * that never yields:
 *
 *     spin0 (nothing else READY):   ticks +200 over 2 s, MIL seen=0
 *     spin1 (a READY task waiting): ticks   +0 over 2 s, MIL seen=255
 *
 * 100 Hz exactly when the tick has nobody to switch to, and a dead timer the
 * moment it has. mstatus.MIE reads 1 throughout both, which is why no
 * existing test on any other target could have caught this: on RP2350 and on
 * QEMU, MIE is the whole story.
 *
 * So the level is dropped deliberately, by performing an mret that returns
 * to the next instruction:
 *
 *   - mcause.MPIL is cleared first, because that is what mret loads into MIL.
 *     Clobbering mcause is safe here: trap_handler() has already copied the
 *     cause it needs into a local, and the trap vector restores mepc and
 *     mstatus from the frame rather than from the live CSRs.
 *   - mstatus.MPIE is cleared, because mret also loads MIE from it and the
 *     rest of the handler must stay masked. The frame still holds the real
 *     MPIE, so the vector's own mret at the end restores it properly.
 *   - MPP is forced to M so the mret does not change privilege.
 *
 * The cost is one mret per interrupt on this board. The alternative designs
 * -- a trampoline that mrets into a stub which then yields, or the CLIC's own
 * mnxti -- are both larger, and neither is needed while the kernel has one
 * flat interrupt level. */
static inline void p4_drop_intlevel(void) {
    uintptr_t cause, status;
    __asm__ __volatile__("csrr %0, mcause" : "=r"(cause));
    cause &= ~((uintptr_t)0xff << 16);        /* MPIL = 0 */
    __asm__ __volatile__("csrw mcause, %0" :: "r"(cause));

    __asm__ __volatile__("csrr %0, mstatus" : "=r"(status));
    status &= ~((uintptr_t)1 << 7);           /* MPIE = 0: stay masked */
    status |= ((uintptr_t)3 << 11);           /* MPP  = M: no privilege change */
    __asm__ __volatile__("csrw mstatus, %0" :: "r"(status));

    __asm__ __volatile__(
        "la   t0, 1f\n\t"
        "csrw mepc, t0\n\t"
        "mret\n\t"
        "1:"
        ::: "t0", "memory");
}

/* Bring the CLIC up. Called from trap_init() only.
 *
 * Order matters in one place and not in the others: everything is masked
 * before anything is unmasked. */
static void p4_clic_init(void) {
    /* 1. Every line off, before anything else.
     *
     * The ROM ran with interrupts of its own, and unlike the CLIC's *pending*
     * bits its enables have no reset this code arrives after. An enable left
     * set on a level-triggered source nobody clears is not a stray interrupt,
     * it is a permanent one: the handler returns, the condition is still
     * asserted, and the core re-enters immediately and forever. This is the
     * same hazard the RP2350 arm's meifa clearing addresses, and it is worth
     * the 45 byte writes for the same reason -- "has always been zero" is not
     * a property this boot path inherits.
     *
     * From 3 rather than 0: 0..2 are not implemented (the CLIC's first real
     * source is the software interrupt at 3). */
    for (unsigned i = 3; i < P4_CLIC_IRQ_COUNT; i++) {
        *p4_clic_byte(P4_CLIC_IE(i)) = 0;
    }

    /* 2. mcliccfg.MNLBITS = 0.
     *
     * With MNLBITS 0 the whole of clicintctl[i] encodes priority and every
     * interrupt has level 255 (TRM Table 2.9-2, first row). That is exactly
     * the shape this kernel wants: one flat level, no preemption of a
     * handler by another interrupt, and no way for an unwritten clicintctl
     * to leave a line masked.
     *
     * That last part is not hypothetical. IDF runs MNLBITS = 3, where the
     * level comes from clicintctl[7:5] -- whose reset value is 0. Level 0
     * against a threshold of 0 is masked, because the TRM's rule is "less
     * than or equal to the effective threshold are not allowed to preempt".
     * So under IDF's configuration an interrupt that is enabled, routed,
     * pending and unmasked still never fires until its clicintctl is
     * written. arch_irq_enable() writes it anyway (0xff is the maximum under
     * either encoding), so this kernel is correct whichever value MNLBITS
     * ends up holding -- but it sets the one where the failure cannot happen.
     *
     * Read-modify-write: bits 0 and [6:5] are read-only and writing back
     * what was read is the only way to leave them alone. */
    uint32_t cfg = REG(P4_CLIC_CFG);
    cfg &= ~P4_CLIC_CFG_MNLBITS_MASK;
    REG(P4_CLIC_CFG) = cfg;

    /* 3. Threshold 0: accept every level above 0, which after step 2 is
     * every interrupt there is.
     *
     * TH lives in the top 8 bits. The read-back is not a check, it is the
     * commit: IDF's rv_utils_restore_intlevel_regval() notes that "after
     * writing the threshold register, the new threshold is not directly
     * taken into account by the CPU" and forces the store with a load before
     * re-enabling MIE. Same store, same reason, same place in the order. */
    REG(P4_CLIC_THRESH) = 0;
    (void)REG(P4_CLIC_THRESH);
}

#else
/* QEMU virt's PLIC -- shared by both QEMU targets, which differ only in
 * which context and cause code they see: standard PLIC convention numbers
 * hart N's M-mode context 2N and its S-mode context 2N+1, and RV64 is the
 * only target that delegates external interrupts to S-mode at all (see
 * entry.S's mideleg, CONFIG_MODE_S-only). Base, context numbering and the
 * claim/complete protocol verified against qemu/include/hw/riscv/virt.h's
 * own memory map rather than assumed, since this is the one target where
 * "standard" still deserves a source. */
#define QEMU_PLIC_BASE     0x0c000000UL
#if defined(CONFIG_MODE_S)
#define QEMU_EXT_CAUSE     9u  /* Supervisor external interrupt */
#else
#define QEMU_EXT_CAUSE     11u /* Machine external interrupt */
#endif

/* The PLIC context of the hart executing this, by the convention documented
 * above: hart N's M-mode context is 2N and its S-mode context is 2N+1.
 *
 * X1, plan/phase23_multicore_scheduling.md §6.1. This used to be the
 * constant 1 (or 0), i.e. hart 0's context, baked into three separate
 * address computations -- the claim/complete register, the priority
 * threshold written at init, and the per-IRQ enable bit. §1 of that plan
 * said the interrupt controllers were "already per-hart-addressable in
 * principle", which was true of the hardware and not of this code.
 *
 * Getting it wrong is not a missing feature but active damage: a second
 * hart claiming from hart 0's context takes an interrupt out of hart 0's
 * queue, and writing hart 0's threshold from hart 1 reconfigures a context
 * that is not its own. */
static inline uint32_t plic_context(void) {
#if defined(CONFIG_MODE_S)
    return 2u * hart_id() + 1u;
#else
    return 2u * hart_id();
#endif
}

static inline volatile uint32_t *plic_claim_reg(void) {
    return (volatile uint32_t *)
        (QEMU_PLIC_BASE + 0x200000u + plic_context() * 0x1000u + 4u);
}

static inline uint32_t plic_claim(void) {
    return *plic_claim_reg();
}
static inline void plic_complete(uint32_t irq_num) {
    *plic_claim_reg() = irq_num;
}
#endif

void trap_init(void) {
#if defined(CONFIG_BOARD_RP2350)
    /* Clear this core's external-interrupt FORCE array before enabling
     * anything (phase 23 X7).
     *
     * meifa (0xbe2) is "a read-write bit for every interrupt request; writing
     * a 1 causes the corresponding bit to become pending in meipa" -- a
     * software-triggered interrupt latch, per core, with no guaranteed reset
     * value. Transcribed from the SDK's own
     * runtime_init_per_core_h3_irq_registers (pico_crt0/crt0_riscv.S), which
     * runs this on *every* core including core 1 at launch, iterating array
     * windows 3..0 for up to 64 IRQs.
     *
     * This kernel never did it, which was survivable while only core 0
     * existed and boot_header.S had left the core in a known state. Core 1
     * comes out of the bootrom instead, having just been driven through a
     * FIFO handshake, and a stale force bit there means it takes an external
     * interrupt the instant MIE goes on -- into a handler that, for an IRQ
     * with no registered owner, printk()s. From core 1. In a storm. That is
     * a wedged board whose stopping point moves from run to run, which is
     * exactly what X7's first three attempts produced.
     *
     * Harmless on core 0, where it has always been zero; the point is that
     * "has always been" is not a property core 1 inherits. */
    for (int w = 3; w >= 0; w--) {
        __asm__ __volatile__("csrw 0xbe2, %0" :: "r"((uintptr_t)w)); /* RVCSR_MEIFA */
    }

    /* Enable M-mode External Interrupts, and only those.
     *
     * csrw rather than csrr/or/csrw, matching the SDK: it also clears MSIE
     * and MTIE, so a core arrives with exactly one interrupt source armed and
     * the timer is switched on deliberately later by ticker_arm_this_hart().
     * The read-modify-write this replaced would have carried whatever the
     * bootrom left in mie on a secondary. */
    __asm__ __volatile__("csrw mie, %0" :: "r"((uintptr_t)(1u << 11)));

    /* Enable M-mode Global Interrupts (MIE bit 3 in mstatus) */
    uintptr_t mstatus_val;
    __asm__ __volatile__("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val |= (1u << 3);
    __asm__ __volatile__("csrw mstatus, %0" :: "r"(mstatus_val));
#elif defined(CONFIG_BOARD_ESP32P4)
    /* mtvt (CSR 0x307): the hardware-vectored jump table.
     *
     * This kernel does not use hardware vectoring. Every line it enables is
     * given clicintattr[i].SHV = 0, and the TRM is unambiguous about what
     * that means: "upon taking this interrupt, the CPU will jump to the
     * address configured in mtvec" -- the one vector entry.S already
     * installed, which saves a full frame and calls trap_handler(), and
     * which mcause then tells which interrupt it was. One entry point for
     * exceptions and interrupts alike, exactly as on the other three
     * targets.
     *
     * mtvt is set anyway, and the reason is the argument
     * drivers/uart_esp32p4.c makes about the console: "it works because of
     * what the loader left behind" is a property of one boot path. If
     * anything ever takes an interrupt with SHV set -- a line this file did
     * not enable, an SHV bit this file did not write, a second-stage
     * bootloader in E6 that leaves the CLIC configured differently -- the
     * core loads a word from mtvt and jumps through it. Leaving that word as
     * whatever the ROM left in the CSR turns a stray interrupt into a jump
     * to an arbitrary address. 192 bytes of table make it a jump to the trap
     * vector instead, where it is reported rather than executed.
     *
     * Aligned to 256: the CLIC specification requires the table to be
     * aligned to a power of two at least as large as itself, and 4 * 48
     * entries is 192. */
    {
        extern void trap_vector_entry(void);
        /* Filled at run time rather than with a 48-entry initializer list:
         * the list would be 48 chances to miscount, and this way the table's
         * length and the loop's bound are the same constant. It is written
         * in full before mtvt is pointed at it, so there is no window in
         * which the CSR names a table of zeroes. */
        static void (*p4_mtvt[P4_CLIC_IRQ_COUNT])(void) __attribute__((aligned(256)));
        for (unsigned i = 0; i < P4_CLIC_IRQ_COUNT; i++) {
            p4_mtvt[i] = trap_vector_entry;
        }
        __asm__ __volatile__("csrw 0x307, %0" :: "r"((uintptr_t)p4_mtvt));
    }

    p4_clic_init();

    /* Global interrupt enable, here rather than in main.c.
     *
     * main.c turns interrupts on with irq_restore(IRQ_ENABLE_BIT) only if
     * ticker_init() succeeded, which on this board it does not: the tick
     * runs off the CLINT and that is E4. Waiting for the ticker would mean a
     * UART interrupt that is routed, enabled, pending and unmasked at the
     * controller, and still never delivered -- the exact failure mode this
     * board specialises in, since mstatus.MIE is the one interrupt CSR here
     * that does still work.
     *
     * Same csrs-in-trap_init() shape the RP2350 arm above uses, and safe for
     * the same reason: nothing is enabled behind it yet. Every source is
     * masked by the loop in p4_clic_init(), and the only driver that unmasks
     * one does so from its own init. */
    set_csr(mstatus, 1UL << 3); /* MIE */
#else
    /* M2, plan/phase12_microkernel_migration.md: the arch-global gate for
     * QEMU's PLIC path (both targets -- see the CONFIG_MODE_S/M split above
     * for why this is one branch, not two), mirroring ticker.c's own STIE
     * enable for the same reason: unmasking the *source* here is what makes
     * it safe for a driver's later per-IRQ PLIC enable bit to actually take
     * effect, without that driver also needing to know this CSR exists. The
     * global interrupt gate itself (sstatus.SIE / mstatus.MIE) is left alone
     * here, same as it always was -- main.c's irq_restore(IRQ_ENABLE_BIT),
     * after ticker_init(), is still what turns any of this on. */
#if defined(CONFIG_MODE_S)
    set_csr(sie, 1UL << 9);  /* SEIE */
#else
    set_csr(mie, 1UL << 11); /* MEIE -- plain M-mode QEMU RV32, not RP2350 */
#endif

    /* PLIC priority threshold for this hart's context: 0 accepts any
     * interrupt with priority > 0, i.e. every priority a driver could set
     * (arch_irq_enable() below). One flat priority level, the same choice
     * trap_handler()'s meinext path makes for RP2350 -- there is no
     * nested/preemptive interrupt priority anywhere in this kernel yet. */
    *(volatile uint32_t *)(QEMU_PLIC_BASE + 0x200000u + plic_context() * 0x1000u) = 0;
#endif
}

/* See arch/trap.h. */
void arch_irq_enable(uint32_t irq_num) {
#if defined(CONFIG_BOARD_RP2350)
    /* MEIEA (0xbe0): a window-indexed enable array. Writing INDEX (bits
     * [4:0]) selects a 16-IRQ window; the same write's WINDOW bits
     * (dependent on the CSR instruction's semantics -- csrrs here, so this
     * *sets* bits rather than replacing the whole window) OR the given mask
     * into that window's enable bits. Confirmed against the Pico SDK's
     * hazard3_irqarray_set() macro (hardware/hazard3.h) rather than derived
     * from the field description alone. */
    uint32_t index = irq_num / 16u;
    uint32_t mask  = 1u << (irq_num % 16u);
    uintptr_t v = index | ((uintptr_t)mask << 16);
    __asm__ __volatile__("csrrs zero, 0xbe0, %0" :: "r"(v)); /* RVCSR_MEIEA */
#elif defined(CONFIG_BOARD_ESP32P4)
    /* CLIC: describe the line, then enable it -- attributes and level first,
     * so the line is never briefly enabled while still carrying whatever the
     * ROM configured.
     *
     * The caller's number IS the CLIC interrupt ID (arch/esp32p4_intr.h),
     * which for an external interrupt is 16..47. A number outside that range
     * is refused rather than clamped: writing clicintie[3] or [7] here would
     * arm the software or timer interrupt from a call that meant to arm a
     * device, and those are E4's to arm. */
    if (irq_num < ESP32P4_CLIC_IRQ_MIN || irq_num > ESP32P4_CLIC_IRQ_MAX) {
        printk("[CLIC] arch_irq_enable(%u) refused: not an external interrupt "
               "(%u..%u)\n", (unsigned)irq_num,
               (unsigned)ESP32P4_CLIC_IRQ_MIN, (unsigned)ESP32P4_CLIC_IRQ_MAX);
        return;
    }

    /* Positive level-triggered, machine mode, not hardware vectored.
     *
     * Level, not edge, because that is what comes out of the interrupt
     * matrix: a peripheral asserts its signal and holds it until its own
     * interrupt-clear register is written. The CLIC follows -- with TRIG = 0
     * the TRM makes clicintip read-only and says "to clear it the interrupt
     * must be cleared from source", which is the correct relationship
     * between this register and a driver's ISR. Choosing edge here would
     * make the pending bit software-clearable and let a handler that forgot
     * to touch the peripheral appear to work. */
    *p4_clic_byte(P4_CLIC_ATTR(irq_num)) =
        (uint8_t)(P4_CLIC_ATTR_MODE_M | P4_CLIC_ATTR_TRIG_LEVEL);

    /* Maximum level/priority. One flat interrupt level, the same choice the
     * two branches either side of this make for their own controllers.
     * p4_clic_init() explains why this write is what makes the kernel
     * independent of mcliccfg.MNLBITS. */
    *p4_clic_byte(P4_CLIC_CTL(irq_num)) = P4_CLIC_CTL_MAX;

    *p4_clic_byte(P4_CLIC_IE(irq_num)) = 1;
#else
    /* PLIC: give the IRQ a nonzero priority (0 permanently masks it,
     * independent of any enable bit -- the PLIC spec's own "never
     * interrupt" value) and set its per-context enable bit. Priority 1 is
     * "the lowest priority that still fires", correct for a kernel with one
     * flat interrupt level. */
    *(volatile uint32_t *)(QEMU_PLIC_BASE + 4u * irq_num) = 1;
    /* The *calling* hart's context. Device drivers all initialise on hart 0,
     * so this enables where it always did -- and phase 23's X2 pins driver
     * tasks there deliberately, so routing a device IRQ to a second hart is
     * a policy decision that belongs with X2 rather than a side effect of
     * whoever happened to call this. */
    volatile uint32_t *enable = (volatile uint32_t *)
        (QEMU_PLIC_BASE + 0x2000u + plic_context() * 0x80u + (irq_num / 32u) * 4u);
    *enable |= (1u << (irq_num % 32u));
#endif
}

/* See arch/trap.h. Not a general exception-handling mechanism -- deliberately
 * narrow, so it cannot accidentally mask a real fault. */
static volatile bool g_probe_active;
static volatile bool g_probe_faulted;

void arch_probe_begin(void) {
    g_probe_faulted = false;
    g_probe_active = true;
}

bool arch_probe_faulted(void) {
    g_probe_active = false;
    return g_probe_faulted;
}

/* Cause code of the most recent environment call. The hardware picks it by
 * the privilege level the ecall came FROM -- 8 = U-mode, 9 = S-mode,
 * 11 = M-mode -- so it is direct evidence of the level, not an inference from
 * something the kernel set itself.
 *
 * That distinction is the whole reason this exists: a U-mode task making
 * syscalls produces output identical to a kernel task making the same
 * syscalls, so "it printed" proves nothing about the mode transition. The
 * cause code does. */
static uintptr_t g_last_ecall_cause = 0;

uintptr_t arch_last_ecall_cause(void) { return g_last_ecall_cause; }

void trap_handler(trap_frame_t *frame) {
    uintptr_t cause = frame->cause;
    uintptr_t is_interrupt = cause & ((uintptr_t)1 << (__riscv_xlen - 1));
#if defined(CONFIG_BOARD_ESP32P4)
    /* In CLIC mode mcause is not just a cause code with an interrupt bit on
     * top. It also carries the state that mret restores: MPP in [29:28],
     * MPIE at 27, and the previous interrupt level MPIL in [23:16]. The code
     * itself is EXCCODE, [11:0] -- widened from the base ISA's [30:0]
     * precisely to make room for those fields.
     *
     * So "everything except the top bit" is the wrong mask here, and wrong
     * in a way that reads as hardware trouble rather than arithmetic: the
     * first UART interrupt this kernel ever took on the P4 arrived as
     * 0x38000010, which is MPP=3, MPIE=1, MPIL=0 and, in the low twelve
     * bits, interrupt 16 -- the console, correctly routed, correctly
     * delivered, and unrecognisable to every comparison below. It fell
     * through to the "Interrupt received" line, returned without touching
     * the peripheral, and a level-triggered source re-entered immediately.
     * An interrupt controller that works perfectly and a console that
     * prints one line forever look identical from the other end of the
     * cable.
     *
     * Masking applies to exceptions too, not only interrupts: MPP and MPIE
     * are in mcause whatever kind of trap it was. */
    uintptr_t code = cause & 0xfffu;
#else
    uintptr_t code = cause & ~((uintptr_t)1 << (__riscv_xlen - 1));
#endif

    if (is_interrupt) {
#if defined(CONFIG_BOARD_ESP32P4)
        /* Before anything that could context-switch -- which is the timer
         * path below, and any device handler that unblocks a task. See
         * p4_drop_intlevel(): on this chip the handler's own interrupt level
         * outlives the handler if control leaves by a switch rather than by
         * mret, and takes every other interrupt with it. */
        p4_drop_intlevel();
#endif
        /* Timer: 7 is machine-mode, 5 is supervisor-mode. This is the
         * preemption tick. */
        if (code == 7 || code == 5) {
            ticker_count_tick();
            /* Rearm FIRST. A RISC-V timer interrupt is level-triggered off
             * mtime >= mtimecmp, so it stays pending until the comparator
             * moves -- returning without rearming does not drop a tick, it
             * re-enters the handler forever. */
            ticker_next();

            /* Switch away from whatever was running. This works through the
             * ordinary cooperative ctx_switch() rather than needing a second,
             * preemption-specific path: the full register state is already
             * saved in the trap frame on this task's kernel stack, so
             * ctx_switch() only has to preserve what a C call would. When
             * something switches back, it returns here, unwinds into the trap
             * vector, and the frame restores the interrupted task exactly.
             *
             * sched_yield() is a no-op when nothing else is runnable, so an
             * idle system just takes the tick and returns. */
            sched_yield();
            return;
        }

        /* External (device) interrupt. Same CONFIG_BOARD_RP2350 discriminator
         * as the controller code above -- not CONFIG_MODE_M, for the same
         * reason: QEMU RV32 is CONFIG_MODE_M without being RP2350. M2,
         * plan/phase12_microkernel_migration.md. */
#if defined(CONFIG_BOARD_RP2350)
        if (code == 11) {
            bool valid;
            uint32_t irq_num = meinext_irq(&valid);
            if (valid) devirq_dispatch(irq_num);
            return;
        }
#elif defined(CONFIG_BOARD_ESP32P4)
        /* CLIC (E3). There is no claim register and nothing to acknowledge:
         * mcause already holds the interrupt ID, and for a level-triggered
         * line the pending bit follows the peripheral, so the *driver's* ISR
         * writing its own interrupt-clear register is the whole acknowledge
         * path. That is why the ID is passed straight through -- the number
         * a driver attached with (arch/esp32p4_intr.h) is the number the
         * hardware reports, with no offset in between to get backwards.
         *
         * An ID with no handler is not merely unserviced here, it is fatal
         * if left alone: level-triggered means the source is still asserted
         * on return, so the core re-enters immediately, forever, printing as
         * it goes. So the line is masked on the way out. Losing an interrupt
         * nobody claimed is the strictly better failure. */
        if (code >= ESP32P4_CLIC_IRQ_MIN && code <= ESP32P4_CLIC_IRQ_MAX) {
            if (devirq_dispatch((uint32_t)code) != 0) {
                *p4_clic_byte(P4_CLIC_IE(code)) = 0;
                printk("[CLIC] Masked interrupt %u: no handler, and a level-"
                       "triggered source would re-enter forever\n", (unsigned)code);
            }
            return;
        }
#else
        if (code == QEMU_EXT_CAUSE) {
            uint32_t irq_num = plic_claim();
            if (irq_num != 0) { /* 0 means "nothing claimable" per the PLIC spec */
                devirq_dispatch(irq_num);
                plic_complete(irq_num);
            }
            return;
        }
#endif
        printk("\n[Trap] Interrupt received: code 0x%lx\n", (unsigned long)code);
    } else {
        /* If ecall (Environment Call from U-mode, S-mode, or M-mode) */
        if (code == 8 || code == 9 || code == 11) {
            g_last_ecall_cause = code;
            uintptr_t sys_nr = frame->a0;

            long ret = 0;
            switch (sys_nr) {
                case 0: /* SYS_EXIT / yield */
                    ret = 0;
                    break;
                case SYS_CHAN_CALL: {
                    /* SYS_CHAN_CALL(name, req, req_len, resp, resp_max)
                     *
                     * U-mode's route to a service (C3,
                     * plan/phase6_memory_and_processes.md). Until this existed
                     * a user program could read and write files and print, and
                     * nothing else -- so "extract a kernel subsystem into a
                     * process" had no way for the result to be reachable. That
                     * is why the old register-IPC entry points were deleted
                     * rather than merely tidied: removing four stubs would have
                     * left U-mode exactly as isolated.
                     *
                     * Every buffer crosses the boundary by copy, validated
                     * against the calling task's own domain, exactly as
                     * SYS_READ_FILE does. The endpoint is named rather than
                     * addressed by pointer or pid, so a user program cannot
                     * express a reference to something it was not given.
                     *
                     * The copies are not redundant with the ones chan_call()
                     * already performs: those protect the *endpoint* from the
                     * caller's buffer changing under it, while these are what
                     * stop the kernel dereferencing a user address at all. */
                    char kname[32];
                    static uint8_t kreq[256];
                    static uint8_t kresp[256];
                    uint32_t req_len = (uint32_t)frame->a3;
                    uint32_t resp_max = (uint32_t)frame->a5;

                    if (strncpy_from_user(kname, frame->a1, sizeof(kname)) < 0) {
                        ret = -1;
                        break;
                    }
                    if (req_len > sizeof(kreq)) { ret = -1; break; }
                    if (resp_max > sizeof(kresp)) resp_max = sizeof(kresp);
                    if (req_len && copy_from_user(kreq, frame->a2, req_len) < 0) {
                        ret = -1;
                        break;
                    }

                    chan_endpoint_t *ep = chan_lookup(kname);
                    if (!ep) { ret = -1; break; }

                    int n = chan_call(ep, kreq, req_len, kresp, resp_max);
                    if (n < 0) { ret = n; break; }
                    /* Copied out only after the call succeeded, and only as
                     * many bytes as the service actually produced. */
                    ret = (resp_max && copy_to_user(frame->a4, kresp, (uint32_t)n) < 0)
                              ? -1 : n;
                    break;
                }
                case 10: { /* SYS_PRINT(const char *) */
                    /* Copied in, never dereferenced in place. A U-mode task
                     * cannot read kernel memory, but before this it could ask
                     * the kernel to print it -- the restriction intact and
                     * entirely bypassed. */
                    char kbuf[128];
                    if (strncpy_from_user(kbuf, frame->a1, sizeof(kbuf)) < 0) {
                        ret = -1;
                        break;
                    }
                    printk("%s", kbuf);
                    ret = 0;
                    break;
                }
                case 11: /* SYS_PUTNUM */
                    /* Was printk("%ld", ...): a user program's numeric output
                     * has nothing to do with kernel diagnostics, but printk()
                     * appends everything it emits to the kernel log ring
                     * (kernel/printk.c), so a tight print loop (see
                     * tools/sd_root/prime.c, fib.c) flooded /proc/kmsg with
                     * program output instead of the log staying a kernel
                     * diagnostic stream. console_putc() writes straight to
                     * whichever wire is actually bound as the console,
                     * without touching the log ring. */
                    {
                        long val = (long)frame->a1;
                        if (val < 0) { console_putc('-'); val = -val; }
                        char digits[20];
                        int n = 0;
                        do { digits[n++] = (char)('0' + (val % 10)); val /= 10; } while (val > 0);
                        while (n > 0) console_putc(digits[--n]);
                    }
                    ret = 0;
                    break;
                case 12: /* SYS_PUTCHAR */
                    /* Takes a value, not a pointer, so it is already safe to
                     * call from U-mode. The pointer-taking syscalls below are
                     * NOT -- they still dereference user-supplied addresses
                     * directly, which is what B3's copy-in/copy-out step
                     * exists to fix. Until then, U-mode code must stick to
                     * value-only syscalls.
                     *
                     * console_putc(), not uart_putc(): the latter hard-codes
                     * the physical UART regardless of what the console is
                     * actually bound to (C8 port binding), and also bypassed
                     * the kernel log ring inconsistently with SYS_PUTNUM
                     * above. */
                    console_putc((char)frame->a1);
                    ret = 0;
                    break;
                case 21: /* SYS_TICKS: the preemption tick counter */
                    /* A value, not a pointer, and read-only -- so it is safe
                     * to expose to U-mode as it stands.
                     *
                     * It exists to make U-mode preemptibility testable from
                     * inside a user program: ticker_count_tick() is reached
                     * only from the timer interrupt handler, so a program
                     * that reads this, spins without making a single syscall,
                     * and reads it again has direct evidence of whether a
                     * timer interrupt was taken while *it* was running. With
                     * interrupts masked in U-mode the count cannot move
                     * across that window, however long the spin. */
                    ret = (long)ticker_ticks();
                    break;
                case SYS_YIELD:
                    /* No pointer, no privileged state to hand out -- a plain
                     * cooperative yield. M5, plan/phase12_microkernel_migration.md:
                     * the first syscall a U-mode *driver* task (as opposed to
                     * a one-shot user program) needs, since its serve/poll
                     * loop must yield the way every kernel-mode driver task
                     * already does via sched_yield(). Completes case 0's own
                     * apparent original intent (see that case's comment)
                     * without touching it -- a new, unambiguous number costs
                     * nothing and leaves 0's existing (if dead) behavior
                     * alone. */
                    sched_yield();
                    ret = 0;
                    break;
                case SYS_TIME_MS:
                    /* A value, not a pointer -- same shape as SYS_TICKS
                     * above, just wall-clock-ish milliseconds instead of the
                     * raw preemption-tick count. M5: what a U-mode driver
                     * task's own pacing loop (e.g. the heartbeat LED's
                     * on/off timing) needs in place of calling time_get_ms()
                     * directly. */
                    ret = (long)time_get_ms();
                    break;
                case SYS_DELAY_US:
                    /* M5 Phase 2, plan/phase12_microkernel_migration.md:
                     * SYS_TIME_MS's millisecond granularity is useless for a
                     * bit-bang protocol's microsecond-wide pulses (tm1638's
                     * ~3us clock high/low times). time_delay_us() itself
                     * reads RP2350's TIMER0 peripheral directly and, on this
                     * build, also services usb_cdc_task() inline inside its
                     * busy loop -- both kernel-only operations no U-mode
                     * domain has ever needed to be granted directly, so the
                     * delay runs here, in the kernel, rather than exposing
                     * that MMIO region to U-mode. Costs one ecall round-trip
                     * per call; for a display that updates a few times a
                     * second this is immaterial, and "coarse but honest" beats
                     * a calibrated busy-loop that would only be as reliable
                     * as its calibration (see user/progs/uspin.c's own
                     * SPIN_ITERATIONS comment for why that's fragile). */
                    time_delay_us((uint64_t)frame->a1);
                    ret = 0;
                    break;
                case SYS_CHAN_SERVE_WAIT: {
                    /* SYS_CHAN_SERVE_WAIT(name, buf, buf_max) -> request
                     * length, or -1. M5 Phase 2: the server half of the
                     * channel API SYS_CHAN_CALL's client half has had since
                     * C3 -- what a U-mode *driver* task needs to receive the
                     * request a chan_call() sent it. Blocks (via
                     * chan_serve_wait_copy()'s task_block()) exactly as a
                     * kernel-mode driver task's own chan_serve_wait() call
                     * already does; SYS_CHAN_CALL calling into a task-owned
                     * endpoint already blocks its caller inside a syscall
                     * the same way, so this is not a new risk, just the
                     * same mechanism used from the other side.
                     *
                     * The endpoint's own request buffer is kernel memory
                     * (chan_register_task()'s req_buf) -- never directly
                     * readable from U-mode, so the request is copied out
                     * through a kernel-owned scratch buffer, validated
                     * against this task's domain the same way SYS_CHAN_CALL
                     * validates its own buffers. */
                    char kname[32];
                    static uint8_t kbuf[CHAN_SERVE_KBUF_CAP];
                    if (strncpy_from_user(kname, frame->a1, sizeof(kname)) < 0) {
                        ret = -1;
                        break;
                    }
                    chan_endpoint_t *ep = chan_lookup(kname);
                    if (!ep) { ret = -1; break; }
                    uint32_t buf_max = (uint32_t)frame->a3;
                    if (buf_max > sizeof(kbuf)) buf_max = sizeof(kbuf);
                    uint32_t len = chan_serve_wait_copy(ep, kbuf, buf_max);
                    /* Truncation is a failure, not a short read -- same
                     * discipline chan_call()'s resp_max enforces on the
                     * client side. */
                    if (len > buf_max) { ret = -1; break; }
                    ret = (len && copy_to_user(frame->a2, kbuf, len) < 0)
                              ? -1 : (long)len;
                    break;
                }
                case SYS_CHAN_SERVE_REPLY: {
                    /* SYS_CHAN_SERVE_REPLY(name, buf, len) -> 0, or -1. The
                     * response is copied in through a kernel-owned scratch
                     * buffer and written into the endpoint's own response
                     * buffer by chan_serve_reply_copy(), which also wakes
                     * the blocked caller -- mirrors SYS_CHAN_CALL's own
                     * copy-in-then-hand-to-the-endpoint shape. */
                    char kname[32];
                    static uint8_t kbuf[CHAN_SERVE_KBUF_CAP];
                    if (strncpy_from_user(kname, frame->a1, sizeof(kname)) < 0) {
                        ret = -1;
                        break;
                    }
                    chan_endpoint_t *ep = chan_lookup(kname);
                    if (!ep) { ret = -1; break; }
                    uint32_t resp_len = (uint32_t)frame->a3;
                    if (resp_len > sizeof(kbuf)) { ret = -1; break; }
                    if (resp_len && copy_from_user(kbuf, frame->a2, resp_len) < 0) {
                        ret = -1;
                        break;
                    }
                    chan_serve_reply_copy(ep, kbuf, resp_len);
                    ret = 0;
                    break;
                }
                case SYS_UEXIT:
                    /* A U-mode task asking to end. task_exit() switches away
                     * and never returns, so this call does not come back here
                     * and the trap frame on this kernel stack is simply
                     * abandoned along with the task.
                     *
                     * a1 carries the program's return value. Recording it
                     * *here* rather than in task_exit() is what makes it
                     * mean something: this path is only reached by a task
                     * that asked to end, so a status recorded on it can
                     * never be confused with one from a task the fault
                     * handler killed. */
                    task_set_exit_status((long)frame->a1);
                    task_exit();
                    ret = 0; /* unreachable */
                    break;
                case SYS_REBOOT_BOOTSEL:
                    /* A U-mode task asking to reboot into BOOTSEL --
                     * drivers/usb_cdc.c's own U-mode "1200-baud touch"
                     * handler can detect the condition but cannot call
                     * rp2350_reboot_to_bootsel() itself (a bootrom
                     * ROM-table lookup and jump, privileged), the same
                     * shape SYS_UEXIT's own comment already describes.
                     * Does not return on success, same as the function
                     * it wraps; ret is only meaningful on the failure
                     * path (bootrom function not found, or a non-RP2350
                     * build, where this syscall is simply refused). */
#if defined(CONFIG_BOARD_RP2350)
                    rp2350_reboot_to_bootsel();
                    ret = -1; /* only reached if the reboot failed */
#else
                    ret = -1;
#endif
                    break;
                case 13: { /* SYS_READ_FILE: vfs_read(path, buf, max_len) */
                    char kpath[128];
                    static char kdata[512]; /* static: too big for the trap stack */
                    uint32_t want = (uint32_t)frame->a3;
                    if (want > sizeof(kdata)) want = sizeof(kdata);
                    if (strncpy_from_user(kpath, frame->a1, sizeof(kpath)) < 0) {
                        ret = -1;
                        break;
                    }
                    int n = vfs_read(kpath, kdata, want);
                    if (n < 0) { ret = n; break; }
                    /* Copied out only after the read succeeded, and only as
                     * many bytes as were actually produced. */
                    ret = (copy_to_user(frame->a2, kdata, (uint32_t)n) < 0) ? -1 : n;
                    break;
                }
                case 14: { /* SYS_WRITE_FILE: vfs_write(path, buf, len) */
                    char kpath[128];
                    static char kdata[512];
                    uint32_t len = (uint32_t)frame->a3;
                    if (len > sizeof(kdata)) { ret = -1; break; }
                    if (strncpy_from_user(kpath, frame->a1, sizeof(kpath)) < 0) {
                        ret = -1;
                        break;
                    }
                    if (copy_from_user(kdata, frame->a2, len) < 0) { ret = -1; break; }
                    ret = vfs_write(kpath, kdata, len);
                    break;
                }
                default:
                    printk("[Syscall] Unknown syscall nr %ld requested\n", (long)sys_nr);
                    ret = -1;
                    break;
            }

            frame->a0 = (uintptr_t)ret;
            /* Advance EPC past 4-byte ecall instruction */
            frame->epc += 4;
            return;
        }

        /* B3: a fault taken *from U-mode* kills that task instead of halting
         * the machine. Containment is the entire point of running a task in
         * U-mode -- a kernel that halts whenever a user task misbehaves has
         * enforcement but no benefit from it.
         *
         * The previous privilege level comes from the saved status word, where
         * the hardware recorded it: MPP is mstatus[12:11], SPP is sstatus[8].
         * Zero means the trap came from U-mode. */
#if defined(CONFIG_MODE_S)
        bool from_user = ((frame->status >> 8) & 1u) == 0;
#else
        bool from_user = ((frame->status >> 11) & 3u) == 0;
#endif
        if (from_user) {
            /* X5: record what the other harts were doing at this instant,
             * before the kill switches us away. A no-op unless a load is
             * running (kernel/smp.c), so it costs an ordinary fault nothing
             * but a predictable branch. */
            smp_load_note_fault();
            printk("\n[Trap] User task faulted: cause %lu, epc=0x%lx, addr=0x%lx -- "
                   "terminating the task\n",
                   (unsigned long)code, (unsigned long)frame->epc,
                   (unsigned long)frame->tval);
            task_exit(); /* switches away; never returns */
        }

        /* An illegal instruction during a deliberate hardware probe is an
         * answer, not a failure: record it and step over the instruction.
         * See arch/trap.h for why this is narrowed to cause 2 only. */
        if (g_probe_active && code == 2) {
            g_probe_faulted = true;
            frame->epc += 4; /* CSR instructions have no compressed encoding */
            return;
        }

        /* Fatal exception hang.
         *
         * The faulting instruction is read back and printed, but only from
         * an address that is known to be readable: a fault whose epc is
         * garbage would otherwise take a second fault inside the handler
         * that is reporting the first, and the register dump -- the whole
         * point of getting here -- would never be printed.
         *
         * The window is per board because it is a fact about a board.
         * 0x10000000..0x20082000 covers RP2350's XIP flash and SRAM and
         * QEMU virt's RAM; the ESP32-P4 links at 0x4FF00000 and would have
         * fallen outside it, printing inst=0x00000000 for every fault on the
         * one target E3 has to demonstrate a fault dump on. Found by reading
         * this line while writing that demonstration, not by seeing the
         * zero -- a zero here is indistinguishable from a genuine zero word,
         * which is what makes it worth stating rather than leaving to be
         * noticed. */
#if defined(CONFIG_BOARD_ESP32P4)
        extern char _ram_start[];
        extern char _ram_end[];
        const uintptr_t inst_lo = (uintptr_t)_ram_start;
        const uintptr_t inst_hi = (uintptr_t)_ram_end;
#else
        const uintptr_t inst_lo = 0x10000000;
        const uintptr_t inst_hi = 0x20082000;
#endif
        uint32_t inst_val = 0;
        if (frame->epc >= inst_lo && frame->epc < inst_hi - 4u) {
            const uint8_t *p = (const uint8_t *)frame->epc;
            inst_val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        printk("\n[Trap Exception] Cause: 0x%lx, epc=0x%lx, tval=0x%lx, inst=0x%08x\n",
               (unsigned long)code, (unsigned long)frame->epc, (unsigned long)frame->tval, (unsigned int)inst_val);
        printk("[Trap Register Dump] a0=0x%lx, a1=0x%lx, sp=0x%lx, ra=0x%lx\n",
               (unsigned long)frame->a0, (unsigned long)frame->a1, (unsigned long)frame->sp, (unsigned long)frame->ra);
        printk("[Fatal] System halted due to unhandled exception.\n");
        while (1) {
            __asm__ __volatile__("wfi");
        }
    }
}
