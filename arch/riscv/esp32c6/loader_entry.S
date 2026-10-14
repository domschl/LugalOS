/* The ESP32-C6 loader's entry (arch/riscv/esp32c6/loader.c): the boot ROM
 * jumps here with its own stack still set up, which the loader keeps -- it is
 * the region IDF's bootloader also runs on (0x4087c610..0x4087e610). */
    .section .text.entry
    .global _start
_start:
    la t0, _bss_start
    la t1, _bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call loader_main
3:  j 3b
