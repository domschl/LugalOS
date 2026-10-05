/* 45.2: placement is irrelevant to the measurement -- one RWX region so the
 * linker sizes everything. The real layout is 45.4's job. */
MEMORY { RAM (rwx) : ORIGIN = 0x40800000, LENGTH = 0x7c000 }
SECTIONS {
  .text : { *(.text .text.* ) } > RAM
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > RAM
  .data : { *(.data .data.* .sdata .sdata.*) } > RAM
  .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) } > RAM
}
