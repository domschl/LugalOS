/* K210 loader link map: one flat load image at 0x80000000, the address
 * the BROM enters after reading flash offset 0. The whole image — text
 * through the end of .bss — is what the provisioning tool copies into
 * the flash header's length, and _loader_start.._loader_end is the
 * window the running image relocates by. */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 6M
}

SECTIONS
{
    . = 0x80000000;
    _loader_start = .;
    .text : {
        *(.text.entry)
        *(.text .text.*)
    } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data : { *(.data .data.*) *(.sdata .sdata.*) } > RAM
    .bss (NOLOAD) : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > RAM
    _loader_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.riscv.attributes) }
}
