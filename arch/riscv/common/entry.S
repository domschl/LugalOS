#include "kernel/meminfo.h"
#include "kernel/hart.h"

.section .text.entry
.global _start
.global trap_vector_entry

#if __riscv_xlen == 64
#define REG_S sd
#define REG_L ld
#define REGBYTES 8
#else
#define REG_S sw
#define REG_L lw
#define REGBYTES 4
#endif

_start:
    /* RP2350 boots both cores; only core 0 (mhartid == 0) executes kernel_main */
    csrr t0, mhartid
    bnez t0, .Lsecondary_hart_loop

    /* Park the raw hart id in tp until .bss has been cleared and
     * SETUP_HART_POINTER can turn it into this hart's record pointer.
     *
     * It has to be captured *here*, not there: on CONFIG_MODE_S this code
     * mret's down to S-mode a few instructions below, and mhartid is an
     * M-mode CSR -- reading it after the transition traps as an illegal
     * instruction. tp survives the transition, the .data copy and the .bss
     * clear, none of which touch it. */
    mv tp, t0

    /* Setup Machine trap vector base address first */
    la t0, trap_vector_entry
    csrw mtvec, t0

    /* Disable M-mode interrupts */
    csrw mstatus, zero

    /* Setup stack pointer */
    la sp, _stack_top

#if defined(CONFIG_MODE_S)
    /* Configure PMP0 to grant S/U-mode full access to all memory */
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x1f
    csrw pmpcfg0, t0

    /* Let S-mode read the time counter. `rdtime` is not a plain instruction:
     * it traps as illegal unless M-mode grants it via mcounteren.TM. The
     * timer code reads the current time before programming a deadline, so
     * without this the whole path looks like "Sstc unavailable" when in fact
     * it was the clock read that faulted. Bits 0..2 are CY, TM, IR. */
    li t0, 0x7
    csrw mcounteren, t0

    /* Unlock Sstc so S-mode can program its own timer via stimecmp.
     * Without menvcfg.STCE the CSR access traps, and this build has no SBI
     * firmware underneath to ask instead -- entry.S performs the M->S
     * transition itself. menvcfg is a 64-bit CSR on RV64; STCE is bit 63. */
    li t0, 1
    slli t0, t0, 63
    csrs 0x30a, t0          /* menvcfg */

    /* Delegate all exceptions and interrupts to S-mode */
    li t0, 0xffff
    csrw mideleg, t0
    csrw medeleg, t0

    /* Set mstatus.MPP = 01 (Supervisor Mode: bits 12:11 = 01) */
    csrr t0, mstatus
    li t1, ~(3 << 11)
    and t0, t0, t1
    li t1, (1 << 11)
    or t0, t0, t1
    csrw mstatus, t0

    /* Set mepc to S-mode entry point */
    la t0, s_mode_start
    csrw mepc, t0

    /* Enter S-mode via mret */
    mret

s_mode_start:
    /* Setup Supervisor trap vector base address */
    la t0, trap_vector_entry
    csrw stvec, t0
#endif

    /* Copy .data section from Flash to SRAM if _sidata != _data_start */
    la t0, _sidata
    la t1, _data_start
    la t2, _data_end
    beq t0, t1, .Lskip_data_copy
.Lcopy_data_loop:
    bgeu t1, t2, .Lskip_data_copy
    REG_L t3, 0(t0)
    REG_S t3, 0(t1)
    addi t0, t0, REGBYTES
    addi t1, t1, REGBYTES
    j .Lcopy_data_loop
.Lskip_data_copy:

    /* Clear BSS section */
    la t0, _bss_start
    la t1, _bss_end

1:
    bgeu t0, t1, 2f
    REG_S zero, 0(t0)
    addi t0, t0, REGBYTES
    j 1b

2:
    /* Paint the unused boot stack so its high-water mark can be recovered
     * later (kernel/meminfo.c, /proc/meminfo). [_stack_bottom, sp) is exactly
     * the part nothing has touched: sp is still _stack_top for the primary
     * hart -- the .data copy and .bss clear above use no stack, and anything
     * that did would have pushed *below* sp and already returned. So this
     * cannot clobber live data no matter what precedes it.
     *
     * Deliberately last, after .bss is cleared: .bss and .stack are adjacent
     * in every linker script here, and clearing .bss with a length that ran
     * past _bss_end would silently erase the paint rather than corrupt
     * anything, making the bug look like "the stack is never used".
     *
     * t0-t2 are all dead at this point (the copy and clear loops are done).
     *
     * NOTE: this is the *QEMU* boot path only. RP2350 enters at
     * _reset_handler in arch/riscv/rp2350/boot_header.S and never runs any of
     * the code above, which is why the paint is a shared macro rather than
     * something written out here. */
    PAINT_BOOT_STACK t0, t1, t2

    /* tp stops being the raw hart id and becomes this hart's record (S0,
     * kernel/include/kernel/hart.h). After the .bss clear above, because
     * g_harts lives in .bss. Before the first call, because the trap vector
     * dereferences tp on every kernel trap from here on. */
    SETUP_HART_POINTER t0, t1

    /* Jump to C kernel entry point */
    call kernel_main

    /* Hang if kernel_main returns or for secondary harts */
3:
.Lsecondary_hart_loop:
    wfi
    j 3b

/* --- Trap CSR names, by privilege mode ---
 * The two builds differ only in which CSR set they use, never in structure:
 * Rule 0 (plan/phase5_distributed_design.md §5.1) applied to the trap path. */
#if defined(CONFIG_MODE_S)
#define CSR_SCRATCH sscratch
#define CSR_EPC     sepc
#define CSR_STATUS  sstatus
#define CSR_CAUSE   scause
#define CSR_TVAL    stval
#else
#define CSR_SCRATCH mscratch
#define CSR_EPC     mepc
#define CSR_STATUS  mstatus
#define CSR_CAUSE   mcause
#define CSR_TVAL    mtval
#endif

#define TRAP_FRAME_SIZE (36 * REGBYTES)

/* Interrupt / Exception vector entry point.
 *
 * B3: the trap path now switches stacks, which U-mode requires -- a trap from
 * user code must not keep running on the user's stack, or a task could hand
 * the kernel a bogus (or hostile) sp. Before B3 this vector simply reused the
 * interrupted sp, which is fine while only the kernel ever traps.
 *
 * The invariant is one line, and everything here follows from it:
 *
 *     CSR_SCRATCH holds a pointer to THIS HART'S RECORD (hart_t, see
 *     kernel/include/kernel/hart.h) while U-mode is running, and ZERO while
 *     the kernel is running.
 *
 * So a single csrrw both switches stacks and reveals where the trap came
 * from: if the swapped-in value is non-zero we were in U-mode and now hold
 * the hart record (whose first word is the kernel stack to switch onto); if
 * it is zero we were already in the kernel and the swap is simply undone. No
 * separate "am I in user mode" flag exists to get out of sync with reality.
 *
 * S0 (plan/phase22_smp_locking_foundation.md §6.2) changed *what* the CSR
 * points at -- the kernel sp used to sit in the register itself and now sits
 * in the record's first word -- and nothing about the shape of the test. The
 * substance got slightly stronger on the way past: the non-zero value being
 * branched on is now a .bss address fixed at link time rather than a stack
 * pointer, so "a task whose sp is somehow zero looks like a kernel trap" is
 * no longer expressible.
 *
 * The other half of S0 is `tp`. While the kernel executes, tp holds this
 * hart's record; while U-mode executes, tp belongs to the user program. The
 * vector below is where the swap happens in each direction, which is why the
 * frame's tp slot is written in the two paths separately rather than in the
 * common save block: on the way in from U-mode the slot has to receive the
 * *user's* tp, taken before the kernel's is installed over it. */
.align 4
trap_vector_entry:
    csrrw sp, CSR_SCRATCH, sp
    bnez sp, .Ltrap_from_user

    /* Zero came back: this was a kernel trap, so CSR_SCRATCH was 0 and now
     * holds the interrupted sp. Undo the swap to restore both. tp already
     * holds this hart's record -- the kernel never stops holding it -- so the
     * frame's tp slot just receives it back unchanged. */
    csrrw sp, CSR_SCRATCH, sp
    addi sp, sp, -TRAP_FRAME_SIZE
    REG_S tp,  3*REGBYTES(sp)
    j .Ltrap_save

.Ltrap_from_user:
    /* sp holds this hart's record, CSR_SCRATCH holds the user's sp, and tp is
     * still whatever the user program had in it.
     *
     * There is no stack to spill onto yet -- the user's is not trustworthy
     * and the kernel's is what we are about to go and fetch -- so one
     * register is spilled into the record itself (hart_t::tmp exists for
     * exactly these eight instructions and nothing else). */
    REG_S t0,  HART_OFF_TMP(sp)
    mv t0, sp                          /* t0 = this hart's record */
    REG_L sp,  HART_OFF_KERNEL_SP(t0)  /* onto the task's kernel stack */
    addi sp, sp, -TRAP_FRAME_SIZE
    REG_S tp,  3*REGBYTES(sp)          /* the USER's tp, before it is replaced */
    mv tp, t0                          /* kernel invariant: tp = hart record */
    REG_L t0,  HART_OFF_TMP(tp)        /* and the user's t0 back, for the frame */

.Ltrap_save:
    REG_S ra,  0*REGBYTES(sp)
    REG_S gp,  2*REGBYTES(sp)
    REG_S t0,  4*REGBYTES(sp)
    REG_S t1,  5*REGBYTES(sp)
    REG_S t2,  6*REGBYTES(sp)
    REG_S s0,  7*REGBYTES(sp)
    REG_S s1,  8*REGBYTES(sp)
    REG_S a0,  9*REGBYTES(sp)
    REG_S a1, 10*REGBYTES(sp)
    REG_S a2, 11*REGBYTES(sp)
    REG_S a3, 12*REGBYTES(sp)
    REG_S a4, 13*REGBYTES(sp)
    REG_S a5, 14*REGBYTES(sp)
    REG_S a6, 15*REGBYTES(sp)
    REG_S a7, 16*REGBYTES(sp)
    REG_S s2, 17*REGBYTES(sp)
    REG_S s3, 18*REGBYTES(sp)
    REG_S s4, 19*REGBYTES(sp)
    REG_S s5, 20*REGBYTES(sp)
    REG_S s6, 21*REGBYTES(sp)
    REG_S s7, 22*REGBYTES(sp)
    REG_S s8, 23*REGBYTES(sp)
    REG_S s9, 24*REGBYTES(sp)
    REG_S s10, 25*REGBYTES(sp)
    REG_S s11, 26*REGBYTES(sp)
    REG_S t3, 27*REGBYTES(sp)
    REG_S t4, 28*REGBYTES(sp)
    REG_S t5, 29*REGBYTES(sp)
    REG_S t6, 30*REGBYTES(sp)

    /* The interrupted sp. For a user trap CSR_SCRATCH holds it (the csrrw
     * above parked it there); for a kernel trap CSR_SCRATCH is zero and the
     * interrupted sp is simply this frame's base plus its size.
     *
     * Filling this slot correctly also fixes a long-standing inaccuracy: the
     * pre-B3 vector stored the *already-decremented* sp here, so every
     * "[Trap Register Dump] ... sp=" line ever printed reported the trap
     * frame's own address rather than the sp that was interrupted. t0's
     * original value is already saved above, so it is free as a temp. */
    csrr t0, CSR_SCRATCH
    bnez t0, .Ltrap_sp_known
    addi t0, sp, TRAP_FRAME_SIZE
.Ltrap_sp_known:
    REG_S t0, 1*REGBYTES(sp)

    /* Re-establish the kernel invariant for the duration of the handler. */
    csrw CSR_SCRATCH, zero

    /* Save EPC, status, cause, tval */
    csrr t0, CSR_EPC
    csrr t1, CSR_STATUS
    csrr t2, CSR_CAUSE
    csrr t3, CSR_TVAL

    REG_S t0, 31*REGBYTES(sp)
    REG_S t1, 32*REGBYTES(sp)
    REG_S t2, 33*REGBYTES(sp)
    REG_S t3, 34*REGBYTES(sp)

    /* Pass trap_frame pointer (sp) to C handler */
    mv a0, sp
    call trap_handler

    /* Restore EPC and status */
    REG_L t0, 31*REGBYTES(sp)
    REG_L t1, 32*REGBYTES(sp)
    csrw CSR_EPC, t0
    csrw CSR_STATUS, t1

    /* If this return goes to U-mode, park the kernel stack pointer in this
     * hart's record and point CSR_SCRATCH at the record, so the *next* trap
     * can switch back. t1 still holds the status word and t0/t1 are restored
     * below, so both are free as temps; tp still holds the record, because it
     * is not restored from the frame until the block below.
     *
     * Previous privilege lives in different bits per mode: SPP is bit 8 of
     * sstatus, MPP is bits 12:11 of mstatus. Non-zero means we are returning
     * to a privileged mode and the invariant stays at zero. */
#if defined(CONFIG_MODE_S)
    li t0, (1 << 8)
#else
    li t0, (3 << 11)
#endif
    and t0, t1, t0
    bnez t0, .Ltrap_exit_kernel
    addi t0, sp, TRAP_FRAME_SIZE
    REG_S t0, HART_OFF_KERNEL_SP(tp)
    csrw CSR_SCRATCH, tp
.Ltrap_exit_kernel:

    /* Restore general registers */
    REG_L ra,  0*REGBYTES(sp)
    REG_L gp,  2*REGBYTES(sp)
    REG_L tp,  3*REGBYTES(sp)
    REG_L t0,  4*REGBYTES(sp)
    REG_L t1,  5*REGBYTES(sp)
    REG_L t2,  6*REGBYTES(sp)
    REG_L s0,  7*REGBYTES(sp)
    REG_L s1,  8*REGBYTES(sp)
    REG_L a0,  9*REGBYTES(sp)
    REG_L a1, 10*REGBYTES(sp)
    REG_L a2, 11*REGBYTES(sp)
    REG_L a3, 12*REGBYTES(sp)
    REG_L a4, 13*REGBYTES(sp)
    REG_L a5, 14*REGBYTES(sp)
    REG_L a6, 15*REGBYTES(sp)
    REG_L a7, 16*REGBYTES(sp)
    REG_L s2, 17*REGBYTES(sp)
    REG_L s3, 18*REGBYTES(sp)
    REG_L s4, 19*REGBYTES(sp)
    REG_L s5, 20*REGBYTES(sp)
    REG_L s6, 21*REGBYTES(sp)
    REG_L s7, 22*REGBYTES(sp)
    REG_L s8, 23*REGBYTES(sp)
    REG_L s9, 24*REGBYTES(sp)
    REG_L s10, 25*REGBYTES(sp)
    REG_L s11, 26*REGBYTES(sp)
    REG_L t3, 27*REGBYTES(sp)
    REG_L t4, 28*REGBYTES(sp)
    REG_L t5, 29*REGBYTES(sp)
    REG_L t6, 30*REGBYTES(sp)

    /* Unconditional, and correct for both kernel and user returns because the
     * slot always holds the true interrupted sp (see the entry path). Must be
     * the last frame access: it overwrites the base register. */
    REG_L sp, 1*REGBYTES(sp)

#if defined(CONFIG_MODE_S)
    sret
#else
    mret
#endif
