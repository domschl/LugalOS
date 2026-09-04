OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        . = ALIGN(8);
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(8);
    } > RAM

    .rodata : {
        . = ALIGN(8);
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(8);
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
    . = ALIGN(8);
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
        . = ALIGN(8);
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(8);
        _data_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > RAM

    /* 64KB Boot Stack (placed BEFORE _kernel_end so VMM heap starts safely above it) */
    .stack (NOLOAD) : {
        . = ALIGN(4096);
        _stack_bottom = .;
        . += 0x10000;
        _stack_top = .;

        /* Secondary hart boot stack (X1,
         * plan/phase23_multicore_scheduling.md §2).
         *
         * 16 KB, and this is a **guess, not a measurement** -- §2 asks for
         * the latter and X1 cannot yet supply it, because nothing has run
         * deeply on a second hart to measure. Said plainly rather than
         * dressed up: the number is generous on purpose and owes a real
         * figure.
         *
         * The reasoning behind "generous" is that a secondary's own call
         * chain is shallow by construction -- secondary_main() ->
         * sched_yield() -> ctx_switch(), plus a timer trap frame -- but a
         * secondary does not only run its own chain. The moment it pulls a
         * task off the shared queue it executes whatever that task does, on
         * that task's palloc'd stack rather than this one; only the idle
         * context lives here. 16 KB is therefore expected to be mostly
         * unused, and the way to find out is the paint-and-scan
         * kernel/meminfo.c already does for the primary, pointed at
         * _stack_secondary_bottom..top. That is a follow-up, and it is
         * written down as one.
         *
         * Allocated unconditionally, not behind CONFIG_ENABLE_SMP: a linker
         * script cannot see the generated config header, and 16 KB of RAM on
         * a 128 MB QEMU machine is not worth a second script to avoid. On
         * RP2350, where it would matter, secondaries are not woken at all
         * yet -- that is X3. */
        . = ALIGN(4096);
        _stack_secondary_bottom = .;
        . += 0x4000;
        _stack_secondary_top = .;
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
}
