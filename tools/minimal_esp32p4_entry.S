/*
 * Entry stub for tools/minimal_esp32p4.c -- E1,
 * plan/phase27_esp32p4_bringup.md.
 *
 * The boot ROM's download-mode loader places our segments in L2MEM and jumps
 * straight to this label. Nothing has run before us in our own address space,
 * so there is no C runtime, no stack pointer we may trust, and no guarantee
 * about the contents of .bss.
 *
 * Three instructions of setup, and no more than that on purpose: everything
 * this file does is something that would otherwise be a silent
 * misattribution later. A missing stack presents as a hang at the first
 * function call; uncleared .bss presents as a program that works until the
 * second time it is loaded.
 */

    .section .entry, "ax"
    .globl _start
    .type _start, @function

_start:
    /* No interrupts until someone asks for them. The ROM leaves the CLIC in
     * whatever state its own loader wanted, and this program installs no
     * trap vector at all -- so an interrupt arriving here would vector into
     * whatever mtvec still points at, which is the ROM's business and not
     * ours. E3 is where interrupts become our problem. */
    csrci   mstatus, 0x8            /* clear mstatus.MIE */

    la      sp, _stack_top

    /* Clear .bss: [_bss_start, _bss_end), word at a time. Both symbols are
     * 4-byte aligned by the linker script, so no tail handling is needed. */
    la      t0, _bss_start
    la      t1, _bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:

    call    minimal_main

    /* minimal_main() does not return. If it ever does, stop here rather than
     * executing whatever follows in memory. */
3:  wfi
    j       3b

    .size _start, . - _start
