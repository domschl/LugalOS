/* Linker script for MicroPython U-mode ELF (mpy.elf).
 *
 * W^X separation with a 256 KB (64-page) R|W segment so .data + .bss
 * (including MicroPython's GC heap and ulibc heap/stack) map with a single
 * PMP NAPOT region.
 */

OUTPUT_ARCH("riscv")
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* R | X */
    data PT_LOAD FLAGS(6); /* R | W */
}

SECTIONS
{
    . = 0x0;

    .text : {
        *(.text.start)
        *(.text .text.*)
        . = ALIGN(8);
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4096);
    } :text

    . = ALIGN(0x40000);
    _udata_start = .;

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(8);
    } :data

    .bss (NOLOAD) : {
        _ubss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = 0x40000 - SIZEOF(.data);
        _ubss_end = .;
    } :data

    _uimage_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
    }
}
