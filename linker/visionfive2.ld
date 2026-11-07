OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    /* Stock OpenSBI v1.0 reserves 0x40000000..0x4007FFFF. Loading at 0x40200000
     * matches the standard 2 MB superpage alignment expected by U-Boot's booti. */
    RAM (rwx) : ORIGIN = 0x40200000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        _ktext_lo = .;
        . = ALIGN(8);
        *(.text.entry)
        *(EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj) .text EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj) .text.*)
        . = ALIGN(8);
        _ktext_hi = .;
    } > RAM

    .rodata : {
        . = ALIGN(8);
        *(EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj) .rodata EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj) .rodata.*)
        *(EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj) .srodata EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj) .srodata.*)
        . = ALIGN(8);
        PROVIDE(__lisp_Primitives_start = .);
        KEEP(*(.lisp_primitives))
        PROVIDE(__lisp_Primitives_end = .);
    } > RAM

    . = ALIGN(8);
    _sidata = LOADADDR(.data);

    .utext : ALIGN(4096) {
        _utext_start = .;
        *(.utext .utext.*)
        . = _utext_start + 4096;
    } > RAM

    .utext_kobj : ALIGN(16384) {
        _utext_kobj_start = .;
        *(.utext_kobj .utext_kobj.*)
        *(.utext_kobj_ro .utext_kobj_ro.*)
        *osi_impl.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *uprintf.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *nvs_ram.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *uheap.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        . = _utext_kobj_start + 16384;
    } > RAM

    .udata_kobj (NOLOAD) : ALIGN(8192) {
        _udata_kobj_start = .;
        *(.udata_kobj .udata_kobj.*)
        . = _udata_kobj_start + 8192;
        _udata_kobj_end = .;
    } > RAM

    .data : ALIGN(4096) {
        . = ALIGN(8);
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(8);
        _data_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > RAM

    .stack (NOLOAD) : {
        . = ALIGN(4096);
        _stack_bottom = .;
        . += 0x10000;
        _stack_top = .;

        . = ALIGN(4096);
        _stack_secondary_bottom = .;
        . += 0x10000;
        _stack_secondary_top = .;
    } > RAM

    . = ALIGN(4096);
    _kernel_end = .;

    _heap_end = ORIGIN(RAM) + LENGTH(RAM);

    _ram_start = ORIGIN(RAM);
    _ram_end   = ORIGIN(RAM) + LENGTH(RAM);

    _image_bytes = _bss_end - _ram_start;
    _inst_lo     = _ram_start;
    _inst_hi     = _ram_end;

    PROVIDE(task_trampoline = _ktext_lo);
    PROVIDE(sched_yield     = _ktext_lo);
    PROVIDE(task_exit       = _ktext_lo);
    ASSERT(task_trampoline >= _ktext_lo && task_trampoline < _ktext_hi,
           "task_trampoline outside .text: sched_check_incoming()'s ra window is wrong")
    ASSERT(sched_yield >= _ktext_lo && sched_yield < _ktext_hi,
           "sched_yield outside .text: sched_check_incoming()'s ra window is wrong")
    ASSERT(task_exit >= _ktext_lo && task_exit < _ktext_hi,
           "task_exit outside .text: sched_check_incoming()'s ra window is wrong")
}
