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
    /* 20-byte RP2350 Boot Header (binary_info) at Flash offset 0 */
    .boot_header : {
        KEEP(*(.boot_header))
        . = 20;
    } > FLASH

    /* Core Code (Execute-In-Place out of QSPI Flash) */
    .text : {
        . = ALIGN(4);
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(4);
    } > FLASH

    /* Read-Only Data & Binary Info Array (Stored in QSPI Flash) */
    .rodata : {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
        __binary_info_start = .;
        KEEP(*(.binary_info*))
        __binary_info_end = .;
        . = ALIGN(4);
    } > FLASH

    /* Read-Write Initialized Data (Copied from FLASH to SRAM at boot) */
    . = ALIGN(4);
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        _data_end = .;
    } > SRAM AT> FLASH

    __flash_binary_end = _sidata + (_data_end - _data_start);

    /* Uninitialized Data (Zeroed in SRAM at boot) */
    .bss (NOLOAD) : {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
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
