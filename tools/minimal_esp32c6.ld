/*
 * Linker script for tools/minimal_esp32c6.c -- 45.1,
 * plan/phase45_esp32c6.md.
 *
 * Deliberately standalone, for the reason tools/minimal_esp32p4.ld gives: the
 * kernel's own script (45.4) should be designed for flash XIP and a heap, not
 * grown out of the one that places an echo loop.
 *
 * The image is delivered by `esptool load-ram` and never touches flash. The
 * ROM's download-mode code is live in HP SRAM while it loads us, so we stay
 * in the bottom 64 KB of the 512 KB at 0x40800000 -- IDF's bootloader keeps
 * to the top of the SRAM (bootloader.memory.ld.in, usable end 0x4087c610), and
 * the ROM's own working set is up there too.
 */

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40800000, LENGTH = 64K
}

ENTRY(_start)

SECTIONS
{
    .text : ALIGN(4) {
        KEEP(*(.entry))
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(4) {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > RAM

    .data : ALIGN(4) {
        *(.data .data.* .sdata .sdata.*)
    } > RAM

    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM

    . = ALIGN(16);
    _stack_bottom = .;
    . = ORIGIN(RAM) + LENGTH(RAM);
    _stack_top = .;

    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame*) }
}
