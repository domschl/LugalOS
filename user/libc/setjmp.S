/* RISC-V setjmp / longjmp for RV32 (ilp32) and RV64 (lp64 / lp64d). */

    .text
    .globl setjmp
    .type setjmp, @function
setjmp:
#if __riscv_xlen == 64
    sd  ra,   0(a0)
    sd  sp,   8(a0)
    sd  s0,  16(a0)
    sd  s1,  24(a0)
    sd  s2,  32(a0)
    sd  s3,  40(a0)
    sd  s4,  48(a0)
    sd  s5,  56(a0)
    sd  s6,  64(a0)
    sd  s7,  72(a0)
    sd  s8,  80(a0)
    sd  s9,  88(a0)
    sd  s10, 96(a0)
    sd  s11, 104(a0)
#else
    sw  ra,   0(a0)
    sw  sp,   4(a0)
    sw  s0,   8(a0)
    sw  s1,  12(a0)
    sw  s2,  16(a0)
    sw  s3,  20(a0)
    sw  s4,  24(a0)
    sw  s5,  28(a0)
    sw  s6,  32(a0)
    sw  s7,  36(a0)
    sw  s8,  40(a0)
    sw  s9,  44(a0)
    sw  s10, 48(a0)
    sw  s11, 52(a0)
#endif
    li  a0, 0
    ret

    .globl longjmp
    .type longjmp, @function
longjmp:
#if __riscv_xlen == 64
    ld  ra,   0(a0)
    ld  sp,   8(a0)
    ld  s0,  16(a0)
    ld  s1,  24(a0)
    ld  s2,  32(a0)
    ld  s3,  40(a0)
    ld  s4,  48(a0)
    ld  s5,  56(a0)
    ld  s6,  64(a0)
    ld  s7,  72(a0)
    ld  s8,  80(a0)
    ld  s9,  88(a0)
    ld  s10, 96(a0)
    ld  s11, 104(a0)
#else
    lw  ra,   0(a0)
    lw  sp,   4(a0)
    lw  s0,   8(a0)
    lw  s1,  12(a0)
    lw  s2,  16(a0)
    lw  s3,  20(a0)
    lw  s4,  24(a0)
    lw  s5,  28(a0)
    lw  s6,  32(a0)
    lw  s7,  36(a0)
    lw  s8,  40(a0)
    lw  s9,  44(a0)
    lw  s10, 48(a0)
    lw  s11, 52(a0)
#endif
    seqz a0, a1
    add  a0, a0, a1
    ret
