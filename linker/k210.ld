/* K210 (Kendryte, on the Maix Bit): the whole kernel lives in the 6 MB
 * block of SRAM at 0x8000_0000 -- there is no MMU, no PMP, and no
 * execute-from-flash: the ISP loads this image flat and the BROM jumps to
 * 0x8000_0000 with the 64 KiB-window trap contract of 65.1's probe
 * (mepc's software-writable bits are 15:0; normal mret pops the hardware
 * latch and is unaffected). Derived from linker/qemu-rv32.ld, whose shape
 * this copies exactly: one flat run from the bottom of RAM, U-mode
 * single-page grants included, because kernel/umode.c is built for every
 * target and asks for these symbols. */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 6M
}

SECTIONS
{
    .text : {
        _ktext_lo = .;
        . = ALIGN(4);
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(4);
        _ktext_hi = .;
    } > RAM

    .rodata : {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
    } > RAM

    /* U-mode callable trampoline text, exactly one page, power-of-two and
     * self-aligned (see linker/qemu-rv32.ld for the long reason). */
    .utext : ALIGN(4096) {
        _utext_start = .;
        *(.utext .utext.*)
        . = _utext_start + 4096;
    } > RAM

    .data : ALIGN(4096) {
        . = ALIGN(8);
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(8);
        _data_end = .;
    } > RAM

    _sidata = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss .bss.*)
        /* .sbss must sit inside the _bss_start.._bss_end window the
         * crt clears: left to the linker's default placement it landed
         * after _bss_end and every zero-initialized small-data invariant
         * (spinlock words first) started boot as SRAM garbage. */
        *(.sbss .sbss.*)
        *(SCOMMON)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > RAM

    .stack (NOLOAD) : {
        . = ALIGN(4096);
        _stack_bottom = .;
        . += 0x10000;
        _stack_top = .;
    } > RAM

    . = ALIGN(4096);
    _kernel_end = .;

    /* Upper bound of the page allocator's heap (kernel/palloc.c). */
    _heap_end = ORIGIN(RAM) + LENGTH(RAM);

    _ram_start = ORIGIN(RAM);
    _ram_end   = ORIGIN(RAM) + LENGTH(RAM);

    /* Same trio as linker/qemu-rv32.ld: image span for /proc/meminfo, and
       the window the fatal-trap handler may read instructions back from. */
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
