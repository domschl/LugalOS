/* The ESP32-C6 loader (arch/riscv/esp32c6/loader.c): one region of SRAM that
 * the kernel's RAM half never loads into, where the boot ROM puts it.
 *
 * 0x40870000..0x4087a000 lies inside the space IDF's own second-stage
 * bootloader occupies on this chip (0x4086b910..0x4087a610, IDF
 * components/bootloader/subproject/main/ld/esp32c6/bootloader.memory.ld.in):
 * clear of the ROM's stack and data above it, and far above the kernel's
 * loaded sections at 0x40800000. The loader refuses a segment that would
 * reach it. */

OUTPUT_FORMAT(elf32-littleriscv)
ENTRY(_start)

MEMORY
{
    LRAM (rwx) : ORIGIN = 0x40870000, LENGTH = 40K
}

SECTIONS
{
    .text : {
        _loader_start = .;
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.* .srodata .srodata.*)
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
    } > LRAM
    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
        _loader_end = .;
    } > LRAM
    /DISCARD/ : { *(.comment) *(.note*) *(.riscv.attributes) *(.eh_frame*) }
}
