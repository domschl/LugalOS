/* The K210 loader's entry (arch/riscv/k210/loader.c): the BROM enters
 * at 0x80000000 — the image's first byte — with its own stack still
 * set. Take over the stack immediately: a window at the very top of
 * SRAM that neither the loader's self-copy nor the kernel blob ever
 * reaches, so it survives until the kernel's own start code replaces
 * it. Then clear .bss (the BROM loaded, it did not zero) and run C. */
    .section .text.entry, "ax"
    .global _start
_start:
    /* The ISP stub hands off with its interrupt machinery live (uart
     * sources enabled at the PLIC while its own mtvec served them).
     * Take it quiet: own vector, no enables. Flash-boot carries no
     * pending state, but the same image must be ISP-bootable too. */
    csrw mie, zero
    csrw mip, zero
    li  t0, (1 << 3) | (1 << 7)
    csrc mstatus, t0
    la  t0, 9f
    csrw mtvec, t0
    li  sp, 0x80600000 - 16
    la  t0, _bss_start
    la  t1, _bss_end
1:  bgeu t0, t1, 2f
    sd  zero, 0(t0)
    addi t0, t0, 8
    j   1b
2:  call loader_main
3:  j   3b
9:  li  t2, 0x38000000          /* mtvec sink announces itself with
   li  t3, 'T'                    * 'T's -- a silent loader could be
10: lw  t4, 0(t2)                * wedged anywhere; this says "a trap
   and t5, t4, 0x80000000         * arrived here" and paces it so the
   bnez t4, 10b                   * 'T's stay legible. */
   sw  t3, 0(t2)
   li  t6, 200000
11: addi t6, t6, -1
   bnez t6, 11b
   j   9b
