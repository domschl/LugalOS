/*
 * Entry stub for tools/minimal_esp32c6.c -- 45.1, plan/phase45_esp32c6.md.
 *
 * The ROM's download-mode loader places our segments in HP SRAM and jumps to
 * this label. Nothing has run in our address space: no stack we may trust, no
 * guarantee about .bss. Three steps and no more, as on the P4
 * (tools/minimal_esp32p4_entry.S).
 */

    .section .entry, "ax"
    .globl _start
    .type _start, @function

_start:
    csrci   mstatus, 0x8            /* clear mstatus.MIE */

    la      sp, _stack_top

    la      t0, _bss_start
    la      t1, _bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:

    call    minimal_main

3:  wfi
    j       3b

    .size _start, . - _start
