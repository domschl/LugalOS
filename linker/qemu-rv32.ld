OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        *(.text.entry)
        *(.text .text.*)
    } > RAM

    .rodata : {
        *(.rodata .rodata.*)
    } > RAM

    _sidata = LOADADDR(.data);
    .data : {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } > RAM


    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
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
