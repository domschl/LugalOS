/* The ESP32-P4 loader (arch/riscv/esp32p4/loader.c): one region of L2MEM that
 * the kernel's RAM half never loads into, where the boot ROM puts it.
 * P4_LOADER_BASE is the silicon revision's (linker/esp32p4_memory_rev*.ld,
 * copied to esp32p4_memory.ld in the build directory). The loader refuses a
 * segment that would reach its own region. */

OUTPUT_FORMAT(elf32-littleriscv)
ENTRY(_start)

INCLUDE esp32p4_memory.ld

MEMORY
{
    LRAM (rwx) : ORIGIN = P4_LOADER_BASE, LENGTH = 24K
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
        . = ALIGN(16);
        _bss_end = .;
        . += 4K;
        _loader_stack_top = .;
        _loader_end = .;
    } > LRAM
    /DISCARD/ : { *(.comment) *(.note*) *(.riscv.attributes) *(.eh_frame*) }
}
