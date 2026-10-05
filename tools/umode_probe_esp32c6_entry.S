/*
 * Entry, trap vector and the M->U->M trampoline for tools/umode_probe_esp32c6.c
 * -- 45.3, plan/phase45_esp32c6.md.
 *
 * run_user(entry, user_sp, arg) saves the M-mode context, drops to U-mode at
 * `entry` with sp=user_sp and a0=arg, and *returns to its caller* when U-mode
 * traps for any reason, with a0 = mcause. The trap handler records the cause,
 * mepc, mtval and U's a0 in g_trap[] and unwinds to the saved M context, so
 * a faulting U-mode test is just a function that returned a number.
 */
    .section .entry, "ax"
    .globl _start
_start:
    csrci   mstatus, 0x8
    la      sp, _stack_top
    la      t0, _bss_start
    la      t1, _bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:  call    probe_main
3:  wfi
    j       3b

    .text
    .globl run_user
    .type run_user, @function
run_user:
    addi    sp, sp, -64
    sw      ra,  0(sp)
    sw      s0,  4(sp)
    sw      s1,  8(sp)
    sw      s2, 12(sp)
    sw      s3, 16(sp)
    sw      s4, 20(sp)
    sw      s5, 24(sp)
    sw      s6, 28(sp)
    sw      s7, 32(sp)
    sw      s8, 36(sp)
    sw      s9, 40(sp)
    sw      s10, 44(sp)
    sw      s11, 48(sp)
    csrw    mscratch, sp            /* the M stack the trap handler will use */
    csrw    mepc, a0
    li      t0, 0x1880              /* MPP (bits 12:11) and MPIE (bit 7) */
    csrc    mstatus, t0             /* MPP = U, MPIE = 0: U runs with interrupts off */
    mv      sp, a1
    mv      a0, a2
    li      a1, 0
    li      a2, 0
    mret

    .balign 256                     /* mtvec's BASE may ignore its low bits on this core: be aligned */
    .globl trap_entry
trap_entry:
    csrrw   sp, mscratch, sp        /* sp = M stack, mscratch = U's sp */
    addi    sp, sp, -16
    sw      t0, 0(sp)
    sw      t1, 4(sp)
    /* Fast path, for measuring the floor cost of a U->M->U round trip: an
     * ecall from U-mode with a7 == 0x77 does nothing but step mepc and mret.
     * A real kernel syscall does strictly more than this. */
    csrr    t0, mcause
    li      t1, 8
    bne     t0, t1, 1f
    li      t1, 0x77
    bne     a7, t1, 1f
    csrr    t0, mepc
    addi    t0, t0, 4
    csrw    mepc, t0
    lw      t0, 0(sp)
    lw      t1, 4(sp)
    addi    sp, sp, 16
    csrrw   sp, mscratch, sp
    mret
1:
    la      t0, g_trap
    csrr    t1, mcause
    sw      t1, 0(t0)
    csrr    t1, mepc
    sw      t1, 4(t0)
    csrr    t1, mtval
    sw      t1, 8(t0)
    sw      a0, 12(t0)              /* U's a0: how a test reports a value via ecall */
    csrr    t1, mscratch
    sw      t1, 16(t0)              /* U's sp at the trap */
    lw      t0, 0(sp)
    lw      t1, 4(sp)
    addi    sp, sp, 16
    lw      ra,  0(sp)
    lw      s0,  4(sp)
    lw      s1,  8(sp)
    lw      s2, 12(sp)
    lw      s3, 16(sp)
    lw      s4, 20(sp)
    lw      s5, 24(sp)
    lw      s6, 28(sp)
    lw      s7, 32(sp)
    lw      s8, 36(sp)
    lw      s9, 40(sp)
    lw      s10, 44(sp)
    lw      s11, 48(sp)
    addi    sp, sp, 64
    la      a0, g_trap
    lw      a0, 0(a0)
    ret
    .size run_user, . - run_user
