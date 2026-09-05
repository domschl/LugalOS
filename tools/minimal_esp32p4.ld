/*
 * Linker script for tools/minimal_esp32p4.c -- E1,
 * plan/phase27_esp32p4_bringup.md.
 *
 * **Deliberately standalone**, and not the beginning of linker/esp32p4.ld.
 * The plan asks E1 for "no kernel, no linker script sharing" precisely so
 * that when the kernel's own script is written in E2 it can be designed for
 * flash XIP, a heap and U-mode regions, rather than grown out of a script
 * whose only job was to place 500 bytes of echo loop in RAM.
 *
 * ## The one constraint that matters
 *
 * This image is delivered by `esptool load-ram` over the download protocol
 * and never touches flash. That means the boot ROM's *download-mode* code is
 * live in L2MEM while we are being loaded, and its buffers start at
 * 0x4ff296b8 (see the map quoted in tools/minimal_esp32p4.c). Overwriting
 * them corrupts the loader that is in the middle of loading us -- a failure
 * that would look like a bad image rather than like what it is.
 *
 * So everything here lives in [0x4ff00000, 0x4ff28000): 160 KB, ending a
 * comfortable 6 KB below the ROM's buffers. The program needs about one
 * page of that; the margin is free and the alternative is a debugging
 * session.
 */

MEMORY
{
    RAM (rwx) : ORIGIN = 0x4ff00000, LENGTH = 160K
}

ENTRY(_start)

SECTIONS
{
    .text : ALIGN(4) {
        KEEP(*(.entry))      /* _start, which the linker would otherwise gc */
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(4) {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > RAM

    .data : ALIGN(4) {
        *(.data .data.* .sdata .sdata.*)
    } > RAM

    /* Cleared by _start rather than assumed zero: esptool loads the segments
     * an image declares, and a .bss that is nowhere in the file is memory
     * nobody has written since the last program that ran here. */
    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM

    /* Stack at the top of the region, growing down toward .bss. */
    . = ALIGN(16);
    _stack_bottom = .;
    . = ORIGIN(RAM) + LENGTH(RAM);
    _stack_top = .;

    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame*) }
}
