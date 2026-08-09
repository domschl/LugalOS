/* Linker Script for LugalOS on Raspberry Pi Pico 2 (RP2350 RISC-V Hazard3 Core) */
OUTPUT_FORMAT(elf32-littleriscv)

MEMORY
{
    FLASH (rx)     : ORIGIN = 0x10000000, LENGTH = 4M
    RAM   (rwx)    : ORIGIN = 0x20000000, LENGTH = 512k
    SCRATCH_X (rwx): ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y (rwx): ORIGIN = 0x20081000, LENGTH = 4k
}

ENTRY(_entry_point)

SECTIONS
{
    .text : {
        KEEP (*(.binary_info_header))
        KEEP (*(.embedded_block))
        KEEP (*(.reset))
        . = ALIGN(4);
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(4);
    } > FLASH

    /* U-mode-executable code -- see the QEMU linker scripts for why it cannot
     * share pages with kernel .text. One page, power-of-two aligned. */
    .utext : ALIGN(4096) {
        _utext_start = .;
        *(.utext .utext.*)
        . = _utext_start + 4096;
    } > FLASH

    .rodata : {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
        BYTE(0);
        . = ALIGN(4);
    } > FLASH

    .binary_info : {
        KEEP (*(.binary_info.keep.*))
        *(.binary_info.*)
        . = ALIGN(4);
        BYTE(0);
        . = ALIGN(4);
    } > FLASH
    __binary_info_start = ADDR(.binary_info);
    __binary_info_end = ADDR(.binary_info) + SIZEOF(.binary_info);

    .data : {
        . = ALIGN(4);
        __data_start = .;
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
        _data_end = .;
    } > RAM AT> FLASH

    __data_load_ptr = LOADADDR(.data);
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
        _bss_end = .;
    } > RAM
    /* Boot stack in main RAM, not SCRATCH.
     *
     * It used to live at the top of SCRATCH_Y, giving 8 KB at most before it
     * ran into the page allocator's heap at 0x20080000 -- against 64 KB on the
     * QEMU targets. Survivable while nothing unexpected pushed onto it;
     * preemption adds a trap frame plus handler frames at an arbitrary point
     * in the deepest call chain in the system (the Lisp evaluator), and
     * overflowing here corrupts the heap rather than faulting. A real output
     * section so it is allocated inside RAM -- setting the location counter
     * between sections does not move a region-assigned one. */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += 16k;
        _stack_top = .;
    } > RAM

    . = ALIGN(4096);
    _kernel_end = .;

    .scratch_x : {
        . = ALIGN(4);
        __scratch_x_start = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end = .;
    } > SCRATCH_X AT> FLASH

    __scratch_x_load_ptr = LOADADDR(.scratch_x);

    .scratch_y : {
        . = ALIGN(4);
        __scratch_y_start = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end = .;
    } > SCRATCH_Y AT> FLASH

    __scratch_y_load_ptr = LOADADDR(.scratch_y);

    __flash_binary_end = LOADADDR(.scratch_y) + SIZEOF(.scratch_y);

    /* Upper bound of the page allocator's heap (B2, kernel/palloc.c): stops
     * at the start of SCRATCH_X, so the heap can never grow into the scratch
     * regions or the boot stack that lives at the top of SCRATCH_Y. The bump
     * allocator this replaced had no upper bound and would have run straight
     * through both. */
    _heap_end = 0x20080000;
}
