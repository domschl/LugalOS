/* Linker Script for LugalOS on Raspberry Pi Pico 2 (RP2350 RISC-V Hazard3 Core) */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 4M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 520K
}

SECTIONS
{
    /* 256-byte RP2350 Boot2 stage-2 loader block at offset 0 of Flash */
    .boot2 : {
        KEEP(*(.boot2))
        . = 256;
    } > FLASH

    /* Core Code (Execute-In-Place out of QSPI Flash) */
    .text : {
        *(.text.entry)
        *(.text .text.*)
    } > FLASH

    /* Read-Only Data (Stored in QSPI Flash) */
    .rodata : {
        *(.rodata .rodata.*)
    } > FLASH

    /* Read-Write Initialized Data (Copied from FLASH to SRAM at boot) */
    _sidata = LOADADDR(.data);
    .data : {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } > SRAM AT> FLASH

    /* Uninitialized Data (Zeroed in SRAM at boot) */
    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    } > SRAM

    /* System Stack in SRAM (32 KB) */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += 0x8000;
        _stack_top = .;
    } > SRAM

    . = ALIGN(4096);
    _kernel_end = .;
}
