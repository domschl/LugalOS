/* Linker script for LugalOS U-mode user programs.
 *
 * A user program is not a small kernel. It is linked on its own, against
 * nothing, into an image the kernel loads at a base it chooses -- so the
 * layout here is a contract with arch/riscv/common/elf.c, and the ASSERTs
 * below are that contract being enforced at link time.
 *
 *     0x0000  .text + .rodata     ->  loaded into page 0, granted R|X
 *     0x1000  .data + .bss        ->  loaded into page 1, granted R|W
 *
 * ## Why this exists at all
 *
 * The U-mode probes in kernel/shell.c live in a `.utext` section inside the
 * kernel image, and have to avoid *by discipline* anything the compiler might
 * emit into kernel .text or .rodata: string literals are built one character
 * at a time, helpers are marked always_inline because -Os turned them into
 * real calls into kernel .text, UBSan is switched off per function, and a
 * `volatile` is needed to stop gcc re-coalescing those character stores back
 * into a .rodata copy. Each of those was added after a fault that looked like
 * a hardware problem.
 *
 * None of it is needed here. A separately linked program cannot accidentally
 * reference the kernel's .rodata, because at the time it is linked the kernel
 * does not exist. Self-containment stops being a rule someone has to remember
 * and becomes a property of how the binary is produced -- and the two ASSERTs
 * below turn the remaining limits (one page of text, one of data) from a
 * mystery fault at run time into a link error naming the section.
 *
 * ## Position independence
 *
 * The program is linked at 0 but loaded wherever the page allocator had room,
 * so it must contain no absolute addresses. -mcmodel=medany makes every code
 * and data reference PC-relative, and the build additionally passes
 * -mno-relax (linker relaxation would rewrite those into gp-relative
 * accesses, and nothing sets gp for a user program) and -fno-jump-tables (a
 * switch table is a block of absolute addresses in .rodata). See the
 * user-program section of CMakeLists.txt.
 */

OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
    . = 0x0;

    .text : {
        *(.text.start)      /* _start first, so the entry sits at a low offset */
        *(.text .text.*)
        . = ALIGN(4);
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
    }
    _utext_end = .;

    /* 4080, not 4096: the loader plants a 16-byte exit stub at the top of the
     * text page, which is where a program that simply returns from main ends
     * up. See install_exit_stub() in arch/riscv/common/elf.c. */
    /* The exit stub sits in the last 16 bytes of the first page, which is
     * where a program that simply returns from _start lands. That much is
     * still a link-time contract, because it is about the *first page*
     * specifically and not about how big the image is. */
    ASSERT(_utext_end <= 4080,
           "user program: .text + .rodata must fit in 4080 bytes (one page minus the exit stub)")

    . = 0x1000;

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
    }

    .bss (NOLOAD) : {
        _ubss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4);
        _ubss_end = .;
    }
    _udata_end = .;

    /* No ASSERT on the total size any more (C4).
     *
     * It used to be `_udata_end <= 0x2000`: two pages, checked here because
     * the loader could not allocate anything else. The loader now sizes the
     * image from the program headers and rounds up to a power-of-two page run,
     * so the real limit is what the heap can *place* -- a k-page image needs a
     * k-page-aligned run, and on a small heap the alignment binds long before
     * the free page count does.
     *
     * That is not a number a linker script can know, so it is discovered at
     * load time and reported there, naming placement rather than "out of
     * memory". Asserting a stale figure here would only fail the build for
     * programs that would have run. */
    _uimage_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
    }
}
