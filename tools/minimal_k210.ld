/* K210 silicon truth probe (65.1) — flat SRAM layout.
 *
 * The BROM loads the image into SRAM (no XIP) and starts it at
 * 0x80000000 on the "boot" command; the ELF entry point must be
 * exactly 0x80000000. One PT_LOAD covers text+rodata+data; .bss is
 * NOLOAD and zeroed by the startup code (SRAM contents after a BROM
 * boot are undefined). Stacks and the hart-1 magic live at fixed
 * addresses (see minimal_k210_entry.S) and are not described here.
 */

MEMORY
{
  ram  (wxa!ri) : ORIGIN = 0x80000000, LENGTH = 6M
}

ENTRY(_start)
OUTPUT_ARCH("riscv")

SECTIONS
{
  . = 0x80000000;

  .text : {
    KEEP(*(SORT_BY_NAME(.text.start*))) /* _start first: entry point */
    KEEP(*(.text.vectors))
    *(.text .text.*)
  } >ram

  .rodata : ALIGN(8) {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
  } >ram

  .data : ALIGN(8) {
    *(.data .data.*)
    *(.sdata .sdata.*)
  } >ram

  . = ALIGN(8);
  PROVIDE(_bss_start = .);
  .bss (NOLOAD) : ALIGN(8) {
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
  } >ram
  . = ALIGN(8);
  PROVIDE(_bss_end = .);

  /DISCARD/ : {
    *(.eh_frame .eh_frame.*)
    *(.comment)
    *(.note .note.*)
    *(.riscv.attributes)
  }
}
