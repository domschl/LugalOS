/* Linker script for large, multi-page LugalOS U-mode programs (e.g. mpy.elf).
 *
 * Unlike linker/user.ld (which caps .text + .rodata at 4080 bytes for the
 * kernel-planted exit stub), programs linked here use user/libc/crt0.S to
 * switch to a dedicated .bss stack and invoke SYS_UEXIT directly.
 *
 * W^X separation: .text + .rodata are page-aligned before .data + .bss so
 * arch/riscv/common/elf.c maps them with distinct R|X and R|W segments.
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

    /* Place .data + .bss on a 128 KB (32-page) self-aligned boundary and pad
     * NOLOAD .bss to 128 KB so the entire R|W segment maps with a single
     * PMP NAPOT region (zero ELF file-size overhead for NOLOAD .bss). */
    . = ALIGN(0x20000);
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
        . = _udata_start + 0x20000;
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
