/* Entry into U-mode (B3, plan/phase5_distributed_design.md §5.4).
 *
 * Written in assembly because the transition cannot survive a compiler's
 * view of the world: the stack pointer is replaced and control leaves via
 * mret/sret, so there is no function epilogue and no return.
 *
 * Rule 0 (§5.1): one implementation, both builds. The only differences are
 * which CSR set is used and which bits name the previous privilege level --
 * MPP (mstatus[12:11]) on the M-mode targets, SPP (sstatus[8]) on the S-mode
 * one. The structure is identical, which is the point.
 */

.section .text
.global arch_enter_user

#if __riscv_xlen == 64
#define REG_S sd
#define REG_L ld
#define REGBYTES 8
#else
#define REG_S sw
#define REG_L lw
#define REGBYTES 4
#endif

#if defined(CONFIG_MODE_S)
#define CSR_SCRATCH sscratch
#define CSR_EPC     sepc
#define CSR_STATUS  sstatus
#define PP_MASK     (1 << 8)          /* SPP */
#define PIE_BIT     (1 << 5)          /* SPIE */
#define TRAP_RET    sret
#else
#define CSR_SCRATCH mscratch
#define CSR_EPC     mepc
#define CSR_STATUS  mstatus
#define PP_MASK     (3 << 11)         /* MPP */
#define PIE_BIT     (1 << 7)          /* MPIE */
#define TRAP_RET    mret
#endif

/* void arch_enter_user(void (*entry)(void), uintptr_t user_sp, uintptr_t user_ra);
 *   a0 = user entry point, a1 = user stack pointer, a2 = user return address
 *
 * Does not return. The task leaves U-mode only by trapping (an ecall, or a
 * fault), at which point the trap vector swaps back to the kernel stack this
 * function parked in CSR_SCRATCH.
 *
 * a2 exists because a compiled user program ends by *returning* from main,
 * not by calling exit -- so ra has to point somewhere meaningful before the
 * first instruction runs. Entering with whatever ra happened to hold gave a
 * normal program termination the appearance of a wild jump: the task was
 * killed by the fault handler with a diagnostic, which is the correct
 * response to what the hardware saw and entirely the wrong story. The ELF
 * loader points it at an exit stub inside the user's own image (see
 * arch/riscv/common/elf.c); callers whose user code never returns pass 0. */
arch_enter_user:
    csrw CSR_EPC, a0
    mv ra, a2

    /* Arm the trap path's invariant: CSR_SCRATCH holds this task's kernel
     * stack pointer while U-mode runs. The value is the sp we are about to
     * stop using -- every trap from user code returns onto exactly the stack
     * that entered user code, so no separate bookkeeping is needed. */
    csrw CSR_SCRATCH, sp

    /* Previous-privilege field := U (zero), so the trap return below drops
     * to user mode rather than resuming at the current level.
     *
     * Defensive rather than load-bearing *on this path*, and worth saying so:
     * entry.S zeroes mstatus at boot, and mret/sret both leave MPP/SPP at U
     * afterwards, so the field is already zero every time this is reached
     * today. Deleting these two instructions changes nothing measurable --
     * confirmed by removing them and watching the whole suite still pass.
     *
     * They stay because the guarantee they provide is not one this path
     * happens to get for free forever: entering U-mode from a context where
     * the field is set (from inside a trap handler, say, where the hardware
     * records the interrupted level) would silently return to the *privileged*
     * level instead. Forcing the bits on does fail the test, so the property
     * is genuinely tested -- it is only this instruction whose absence is
     * currently masked. */
    li t0, PP_MASK
    csrc CSR_STATUS, t0

    /* Interrupts ON in U-mode. mret/sret takes the interrupt-enable bit from
     * the "previous" bit, so setting MPIE/SPIE here is what makes user code
     * preemptible -- on the target where it matters.
     *
     * The reasoning that says this line is unnecessary is seductive and
     * wrong, so it is written down. The privileged spec says interrupts for a
     * *higher* privilege level are always globally enabled while the hart
     * runs at a lower one, which reads as "U-mode is preemptible whatever
     * mstatus.MIE says". QEMU implements exactly that: with this bit
     * deliberately cleared, a U-mode program spinning without a single
     * syscall still watches the kernel's tick counter advance, on both the
     * RV32 and RV64 builds.
     *
     * Real Hazard3 silicon does not. Measured on RP2350, same program, same
     * 0.4 s spin:
     *
     *     MPIE = 0  ->   0 ticks   (never preempted)
     *     MPIE = 1  ->  41 ticks
     *
     * The RP2350 datasheet's MSTATUS table (§3.8.9) describes MIE as a plain
     * interrupt enable and says nothing about the privilege-relative rule,
     * and §3.8.9's preamble warns that portable software should not rely on
     * implementation-defined behaviour here. So this is not a workaround for
     * a quirk -- it is the conservative reading, and it is correct on both
     * implementations.
     *
     * The cost of getting it wrong is not subtle: a user program that never
     * yields holds the only hart with interrupts masked, and nothing in the
     * kernel can reach it. That the QEMU suite could not tell the difference
     * is why tests/runner.py's "U-mode Code Is Preemptible" has a hardware
     * counterpart in tests/hw/test_rp2350.py.
     *
     * The trap path already handles an interrupt arriving from U-mode:
     * entry.S swaps to the task's kernel stack, and its return path re-arms
     * CSR_SCRATCH whenever the return goes back to U-mode. */
    li t0, PIE_BIT
    csrs CSR_STATUS, t0

    mv sp, a1
    TRAP_RET
