/* U-mode startup for multi-page programs (linker/user_large.ld).
 * Switches sp to a dedicated 32 KB stack in .bss, calls main(argc, argv),
 * and exits cleanly via SYS_UEXIT (20).
 */

    .section .text.start, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    la      sp, _ustack_top
    call    main
    mv      a1, a0
    li      a0, 20          /* SYS_UEXIT */
    ecall
1:  j       1b

    .section .bss.stack, "aw", @nobits
    .align 4
_ustack_bottom:
    .space 32768
_ustack_top:
