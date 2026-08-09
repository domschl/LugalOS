OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        . = ALIGN(4);
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(4);
    } > RAM

    .rodata : {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > RAM

    . = ALIGN(4);
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        _data_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM

    /* 64KB Boot Stack (placed BEFORE _kernel_end so VMM heap starts safely above it) */
    .stack (NOLOAD) : {
        . = ALIGN(4096);
        . += 0x10000;
        _stack_top = .;
    } > RAM

    . = ALIGN(4096);
    _kernel_end = .;

    /* Upper bound of the page allocator's heap (B2, kernel/palloc.c). The
     * bump allocator this replaced had no upper bound at all -- it walked
     * off the end of RAM given enough allocations. */
    _heap_end = ORIGIN(RAM) + LENGTH(RAM);
}
