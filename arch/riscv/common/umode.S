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

/* void arch_enter_user(void (*entry)(void), uintptr_t user_sp);
 *   a0 = user entry point, a1 = user stack pointer
 *
 * Does not return. The task leaves U-mode only by trapping (an ecall, or a
 * fault), at which point the trap vector swaps back to the kernel stack this
 * function parked in CSR_SCRATCH. */
arch_enter_user:
    csrw CSR_EPC, a0

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

    /* Leave interrupts disabled in U-mode for now: mret/sret sets the
     * interrupt-enable bit from the "previous" bit, so clearing it keeps
     * U-mode non-interruptible. Preemption is B6, and until then an
     * interrupt taken in U-mode would have nothing useful to do. */
    li t0, PIE_BIT
    csrc CSR_STATUS, t0

    mv sp, a1
    TRAP_RET
