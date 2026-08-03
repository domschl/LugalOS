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
    /* Jump to C kernel entry point */
    call kernel_main

    /* Hang if kernel_main returns or for secondary harts */
3:
.Lsecondary_hart_loop:
    wfi
    j 3b

/* Interrupt / Exception vector entry point */
.align 4
trap_vector_entry:
    /* Allocate stack frame for registers (36 * REGBYTES) */
    addi sp, sp, -(36 * REGBYTES)

    REG_S ra,  0*REGBYTES(sp)
    REG_S gp,  2*REGBYTES(sp)
    REG_S tp,  3*REGBYTES(sp)
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

    /* Save EPC, status, cause, tval */
#if defined(CONFIG_MODE_S)
    csrr t0, sepc
    csrr t1, sstatus
    csrr t2, scause
    csrr t3, stval
#else
    csrr t0, mepc
    csrr t1, mstatus
    csrr t2, mcause
    csrr t3, mtval
#endif

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
#if defined(CONFIG_MODE_S)
    csrw sepc, t0
    csrw sstatus, t1
#else
    csrw mepc, t0
    csrw mstatus, t1
#endif

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

    addi sp, sp, (36 * REGBYTES)

#if defined(CONFIG_MODE_S)
    sret
#else
    mret
#endif
