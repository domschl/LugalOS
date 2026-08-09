/* Cooperative context switch (B2, plan/phase5_distributed_design.md §5.4).
 *
 * Saves the callee-saved register set on the outgoing task's own stack,
 * parks its sp, loads the incoming task's sp, and restores. Because this is
 * a *cooperative* switch reached through a normal C call, the caller-saved
 * registers are already the C caller's problem -- only ra and s0-s11 need
 * preserving. Timer preemption (B6) is different and will have to save a
 * full trap frame instead; that is a separate path, not a change to this one.
 *
 * Floating-point state is deliberately NOT saved. arch/riscv/common/entry.S
 * does `csrw mstatus, zero` at boot and never sets mstatus.FS, so the FPU is
 * disabled and no task can hold live FP state -- an FP instruction would
 * trap long before it could be clobbered here. If FS is ever enabled, this
 * function must grow fs0-fs11 saves (RV64 builds use the lp64d ABI, where
 * those are callee-saved).
 *
 * Identical on RV32 and RV64 via the REG_S/REG_L width macros, which is
 * Rule 0 (§5.1) at the lowest level: one switch implementation, exercised by
 * both builds, rather than a simplified NOMMU variant that could drift.
 */

.section .text
.global ctx_switch
.global task_trampoline

#if __riscv_xlen == 64
#define REG_S sd
#define REG_L ld
#define REGBYTES 8
#else
#define REG_S sw
#define REG_L lw
#define REGBYTES 4
#endif

/* Frame: ra + s0..s11 = 13 slots, padded to a 16-byte multiple to keep the
 * RISC-V ABI's stack alignment (16 bytes on both ilp32 and lp64d). */
#define CTX_FRAME (16 * REGBYTES)

/* void ctx_switch(uintptr_t *old_sp, uintptr_t new_sp);
 *   a0 = where to store the outgoing sp, a1 = incoming sp */
ctx_switch:
    addi sp, sp, -CTX_FRAME
    REG_S ra,   0*REGBYTES(sp)
    REG_S s0,   1*REGBYTES(sp)
    REG_S s1,   2*REGBYTES(sp)
    REG_S s2,   3*REGBYTES(sp)
    REG_S s3,   4*REGBYTES(sp)
    REG_S s4,   5*REGBYTES(sp)
    REG_S s5,   6*REGBYTES(sp)
    REG_S s6,   7*REGBYTES(sp)
    REG_S s7,   8*REGBYTES(sp)
    REG_S s8,   9*REGBYTES(sp)
    REG_S s9,  10*REGBYTES(sp)
    REG_S s10, 11*REGBYTES(sp)
    REG_S s11, 12*REGBYTES(sp)

    REG_S sp, 0(a0)        /* park the outgoing task's sp */
    mv sp, a1              /* adopt the incoming task's sp */

    REG_L ra,   0*REGBYTES(sp)
    REG_L s0,   1*REGBYTES(sp)
    REG_L s1,   2*REGBYTES(sp)
    REG_L s2,   3*REGBYTES(sp)
    REG_L s3,   4*REGBYTES(sp)
    REG_L s4,   5*REGBYTES(sp)
    REG_L s5,   6*REGBYTES(sp)
    REG_L s6,   7*REGBYTES(sp)
    REG_L s7,   8*REGBYTES(sp)
    REG_L s8,   9*REGBYTES(sp)
    REG_L s9,  10*REGBYTES(sp)
    REG_L s10, 11*REGBYTES(sp)
    REG_L s11, 12*REGBYTES(sp)
    addi sp, sp, CTX_FRAME
    ret

/* First entry into a freshly created task. sched.c primes the new stack so
 * that the ctx_switch above "returns" here with s0 = entry function and
 * s1 = its argument -- there is no earlier frame to return to, so the task
 * starts by calling its entry point rather than resuming one. */
task_trampoline:
    /* Hand off to C rather than calling the entry point directly. A task's
     * first run needs setup that belongs in C -- notably enabling interrupts,
     * which every *later* resume gets from the irq_restore() in sched_yield()
     * but a first entry never reaches, because it arrives here instead of
     * returning from ctx_switch(). Without that a new task would run with
     * interrupts masked forever and could never be preempted. */
    mv a0, s0
    mv a1, s1
    call task_start
.Lhang:
    /* task_start() never returns; if it somehow did, stopping here is far
     * better than falling into whatever bytes follow. */
    j .Lhang
