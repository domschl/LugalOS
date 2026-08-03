OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        . = ALIGN(8);
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(8);
    } > RAM

    .rodata : {
        . = ALIGN(8);
        *(.rodata .rodata.*)
        . = ALIGN(8);
    } > RAM

    . = ALIGN(8);
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(8);
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(8);
        _data_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
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
}
