OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
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

    /* Push writable data to a 2 MB boundary so [ORIGIN(RAM), 2 MB) contains
     * .text and .rodata and nothing else.
     *
     * That range is what a U-mode task is granted execute access to, and it
     * has to be expressible on both backends: PMP needs a power-of-two,
     * self-aligned block, Sv39 needs page alignment. Without this the
     * smallest legal block containing the code also swallowed .data and .bss
     * -- harmless under PMP, which does not restrict M-mode, but under Sv39
     * it stripped the KERNEL's own write access to its data and hung the
     * boot. The padding costs image size, not RAM. */
    . = ALIGN(4);
    _sidata = LOADADDR(.data);
    /* Code that U-mode is allowed to execute, and nothing else.
     *
     * It cannot share pages with the kernel's .text: RISC-V forbids S-mode
     * from executing a page marked U, so granting U-mode execute on the
     * kernel's code would stop the KERNEL running it. (SUM permits S-mode
     * *data* access to U pages; it does not extend to instruction fetch.)
     * That is not a PMP-vs-MMU difference either -- it is simply invisible
     * under PMP, which does not restrict M-mode at all.
     *
     * Exactly one page, so the region is power-of-two and self-aligned for
     * PMP and page-aligned for Sv39. Overflowing it is a link error rather
     * than a silent overlap. */
    .utext : ALIGN(4096) {
        _utext_start = .;
        *(.utext .utext.*)
        . = _utext_start + 4096;
    } > RAM

    .data : ALIGN(2M) {
        . = ALIGN(4);
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        _data_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM

    /* 64KB Boot Stack (placed BEFORE _kernel_end so VMM heap starts safely above it) */
    .stack (NOLOAD) : {
        . = ALIGN(4096);
        _stack_bottom = .;
        . += 0x10000;
        _stack_top = .;
    } > RAM

    . = ALIGN(4096);
    _kernel_end = .;

    /* Upper bound of the page allocator's heap (B2, kernel/palloc.c). The
     * bump allocator this replaced had no upper bound at all -- it walked
     * off the end of RAM given enough allocations. */
    _heap_end = ORIGIN(RAM) + LENGTH(RAM);

    /* The whole writable region, so /proc/meminfo can report the heap and the
     * stack as fractions of something real. Taken from the MEMORY block rather
     * than written down again in C: the point of these numbers is that they
     * come from the board's memory map, not from a constant that can drift
     * away from it. */
    _ram_start = ORIGIN(RAM);
    _ram_end   = ORIGIN(RAM) + LENGTH(RAM);

    /* G1, plan/phase30_driver_framework.md: two facts kernel/meminfo.c and
       arch/riscv/common/trap.c used to hold as per-board #if arms.

       _image_bytes -- how much RAM the resident image occupies. Computed by
       the script because only the script knows the layout; here the image is
       one contiguous run from the bottom of RAM.

       _inst_lo/_inst_hi -- the window the fatal-trap handler may read an
       instruction back from without risking a second fault inside the
       handler reporting the first. Code runs from RAM on this target.

       Below the memory map rather than next to _kernel_end, because these
       read it: a linker script assignment sees only symbols already
       assigned above it. */
    _image_bytes = _bss_end - _ram_start;
    _inst_lo     = _ram_start;
    _inst_hi     = _ram_end;

    /* _ktext_lo/_ktext_hi -- the kernel's own .text, and nothing else. Narrow
       where _inst_lo/_inst_hi are deliberately wide: that pair bounds what the
       fatal handler may *read*, this pair bounds what a parked return address
       may legitimately *be*. sched_check_incoming() tests the ra it is about
       to restore against this window, so a hand-off to a corrupt frame halts
       with the table intact instead of jumping into .rodata and destroying
       the evidence. The ASSERTs below are the premise it rests on -- every
       caller of ctx_switch(), and the first-run entry point, inside it. */
    /* The same script also links minimal_rp2350.elf and friends -- standalone
       probes with no scheduler in them at all -- where these three symbols do
       not exist and a bare reference is a link error rather than a failed
       assertion. PROVIDE defines them only when nothing else has, so the real
       kernel asserts against the real addresses and a probe asserts against a
       tautology. */
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
