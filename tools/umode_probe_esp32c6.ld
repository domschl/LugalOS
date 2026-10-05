/*
 * Linker script for tools/umode_probe_esp32c6.c -- 45.3, plan/phase45_esp32c6.md.
 * Like minimal_esp32c6.ld, plus the two page-aligned sections a U-mode PMP
 * grant needs: .utext (code the probe runs in U-mode) and .ustack (its stack
 * and scratch). Each is exactly one 4 KB NAPOT region.
 */
MEMORY { RAM (rwx) : ORIGIN = 0x40800000, LENGTH = 64K }
ENTRY(_start)
SECTIONS
{
    .text : ALIGN(4) { KEEP(*(.entry)) *(.text .text.*) } > RAM
    .rodata : ALIGN(4) { *(.rodata .rodata.* .srodata .srodata.*) } > RAM

    .utext : ALIGN(4096) { _utext_start = .; KEEP(*(.utext)) *(.utext.*) . = ALIGN(4096); _utext_end = .; } > RAM
    .ustack : ALIGN(4096) { _ustack_start = .; . += 4096; _ustack_end = .; } > RAM

    .data : ALIGN(4) { *(.data .data.* .sdata .sdata.*) } > RAM
    .bss (NOLOAD) : ALIGN(4) { _bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4); _bss_end = .; } > RAM

    . = ALIGN(16);
    _stack_bottom = .;
    . = ORIGIN(RAM) + LENGTH(RAM);
    _stack_top = .;
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame*) }
}
