/* The ESP32-P4 loader's entry (arch/riscv/esp32p4/loader.c): the boot ROM
 * jumps here. The loader takes its own stack, inside its own region, rather
 * than rely on where the ROM left sp. */
    .section .text.entry
    .global _start
_start:
    la sp, _loader_stack_top
    la t0, _bss_start
    la t1, _bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call loader_main
3:  j 3b
