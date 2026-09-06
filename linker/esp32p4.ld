/* Linker script for LugalOS on the ESP32-P4 (Waveshare ESP32-P4-NANO) --
 * E2, plan/phase27_esp32p4_bringup.md.
 *
 * ## The shape of L2MEM, and the one rule that follows from it
 *
 * L2MEM is 768 KB, 0x4FF00000 .. 0x4FFC0000 (SOC_DRAM_LOW/SOC_DRAM_HIGH,
 * components/soc/esp32p4/include/soc/soc.h), and it is not uniformly
 * available. This kernel is delivered by `esptool load-ram`: the download
 * protocol writes our segments into L2MEM and jumps to them, with no flash
 * write anywhere (E1). While that is happening the boot ROM's own
 * download-mode code is live in the same memory, and IDF's
 * components/bootloader/subproject/main/ld/esp32p4/bootloader.memory.ld.in
 * says where:
 *
 *     0x4ff296b8 .. 0x4ff3afc0   shared buffers (live during download mode)
 *     0x4ff3afc0 .. 0x4ff3fba4   CPU1 stack
 *     0x4ff3fba4 .. 0x4ff40000   ROM .bss/.data
 *
 * The rule is therefore about *when*, not about *where*:
 *
 *     Nothing the loader writes may land below 0x4ff40000.
 *     Anything we write ourselves, after it has handed over, may.
 *
 * Once load-ram jumps to _start the ROM's loader is finished and never runs
 * again -- its buffers, its stack and its data are dead memory. So a section
 * that is NOLOAD (nothing in the image file, written for the first time by
 * arch/riscv/common/entry.S) can live down there quite safely, while a
 * section carrying bytes cannot.
 *
 * That is exactly the split below. It is worth 149 KB of heap, which is not
 * a rounding error on a board with 512 KB of it: the first working layout
 * put everything above 0x4ff40000 and left 136 KB, under the floor asserted
 * at the bottom of this file.
 *
 * ## Why the kernel's own text is the expensive part here, and stops being
 *
 * On RP2350 the 227 KB of .text lives in flash and costs no RAM at all. Here
 * it is RAM-resident because the image is RAM-*loaded*, which is E2's whole
 * boot story. E6 boots this image from flash instead, at which point the
 * text goes back to being executed in place and this script gets most of
 * that 227 KB back. Until then the heap is smaller than the RP2350's for a
 * reason that is about the boot path rather than about the silicon.
 */

OUTPUT_FORMAT(elf32-littleriscv)

MEMORY
{
    /* NOLOAD SECTIONS ONLY. See the header comment: this range overlaps the
     * ROM's download-mode buffers, so anything the loader has to write into
     * it would corrupt the loader that is doing the writing. ld enforces the
     * *size*; the ASSERTs at the bottom enforce the rule. */
    LOWRAM (rw)  : ORIGIN = 0x4ff00000, LENGTH = 256K

    /* Everything loadable, and the heap after it. */
    RAM    (rwx) : ORIGIN = 0x4ff40000, LENGTH = 512K
}

ENTRY(_start)

SECTIONS
{
    .text : {
        . = ALIGN(4);
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(4);
    } > RAM

    /* U-mode-executable code, one page, power-of-two-sized and self-aligned
     * so a single PMP NAPOT entry can grant it -- see the QEMU scripts for
     * why it cannot share pages with the kernel's own .text, and
     * kernel/board.c's board_text_region() for who reads _utext_start.
     *
     * Nothing enters U-mode until E5. The page is here from the start
     * anyway: it costs 4 KB, and the alternative is a memory map that
     * changes shape in the middle of the phase. */
    .utext : ALIGN(4096) {
        _utext_start = .;
        *(.utext .utext.*)
        . = _utext_start + 4096;
    } > RAM

    .rodata : {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
    } > RAM

    /* No AT> anywhere in this script, so LOADADDR(.data) == ADDR(.data) and
     * _sidata == _data_start. arch/riscv/common/entry.S tests exactly that
     * and skips its copy loop -- correct here, because load-ram places .data
     * at its final address itself. The symbol still has to exist: entry.S
     * refers to it unconditionally. */
    _sidata = LOADADDR(.data);

    .data : {
        . = ALIGN(4);
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
        _data_end = .;
    } > RAM

    /* Where the loadable image ends and the heap begins. Everything above
     * this point is in the file; everything below is written at run time. */
    . = ALIGN(4096);
    _kernel_end = .;
    _heap_end = ORIGIN(RAM) + LENGTH(RAM);

    /* --- and now the half the loader must not touch --------------------- */

    /* NOLOAD, and here that is structural rather than an optimisation: this
     * section is placed *inside* the range the ROM's download-mode buffers
     * occupy, and is safe there only because no byte of it is in the image
     * file. entry.S clears it after the jump, which it must do regardless --
     * this memory holds whatever the last program to run here left behind,
     * and after a load-ram that is partly the ROM's own working set. */
    /* U-mode task stacks, E5.
     *
     * Separate from .bss because a PMP region is NAPOT: power-of-two size,
     * naturally aligned. A 512-byte stack that happens to land wherever .bss
     * put it cannot be expressed as one region at all, so the alignment is
     * the section's job rather than each object's.
     *
     * 512 rather than something smaller because this chip's PMP granularity
     * is **128 bytes** -- measured, not assumed: writing all ones to pmpaddr0
     * reads back 0x3fffffe0, so bits [4:0] are hardwired zero and address
     * bits [6:0] are not decoded. `pmpinfo` prints that readback next to the
     * conclusion. Anything below 128 would be silently rounded up by the
     * hardware into a region larger than intended, which is the direction
     * that quietly grants more than was asked for.
     *
     * NOLOAD and in LOWRAM with .bss: nothing is loaded into it, so the
     * ROM-buffer rule this script's header describes still holds. */
    .ustacks512 (NOLOAD) : ALIGN(512) {
        _ustacks_start = .;
        *(.ustacks512)
        . = ALIGN(512);
        _ustacks_end = .;
    } > LOWRAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > LOWRAM

    /* 16 KB boot stack, at the TOP of LOWRAM rather than immediately above
     * .bss.
     *
     * The size is the RP2350 figure, not the QEMU one, and both are
     * deliberate: QEMU's 64 KB is free against a 128 MB region, while here
     * the whole system lives in 768 KB. linker/rp2350.ld records the
     * measurement that settles it -- 8372 bytes peak on core 0 with the Lisp
     * evaluator running and a trap frame on top. 16 KB is a shade under 2x.
     *
     * The *placement* is the part that is not inherited. A stack grows down
     * from _stack_top, so whatever sits immediately below _stack_bottom is
     * what an overflow destroys. Below .bss that is .bss -- silent
     * corruption of live kernel state, which is precisely the failure
     * linker/rp2350.ld documents twice (core 0's 4 KB stack in SCRATCH_Y,
     * then core 1's). Putting it at the top of the region instead leaves the
     * unused remainder of LOWRAM as the thing an overflow runs into first:
     * ~70 KB of nothing, free, that turns "corrupted .bss" into "a long
     * while before anything matters". It is not a guard page -- nothing
     * faults -- but it is strictly better than the alternative and costs
     * nothing at all. */
    .stack (ORIGIN(LOWRAM) + LENGTH(LOWRAM) - 16k) (NOLOAD) : {
        _stack_bottom = .;
        . += 16k;
        _stack_top = .;
    }

    /* --- what has to stay true ------------------------------------------ */

    /* The rule from the header comment, as a link error rather than as a
     * corrupt load. Every loadable section is assigned `> RAM` above, so
     * this is really a check that RAM itself has not been moved down; if a
     * future section is added to LOWRAM without NOLOAD, ld's own "section
     * .foo will not fit / LMA overlaps" is the second net. */
    ASSERT(ORIGIN(RAM) >= 0x4ff40000,
           "loadable sections would overlap the ROM's download-mode buffers (0x4ff296b8..0x4ff40000) -- see the header comment")
    ASSERT(ADDR(.text) >= 0x4ff40000,
           ".text has been placed below the ROM's download-mode buffers -- see the header comment")

    /* .stack is given an explicit address rather than a region, because a
     * region-assigned section is packed against the previous one and there
     * is no way to say "at the top" -- setting the location counter between
     * region-assigned sections does not move them (linker/rp2350.ld learned
     * this), and AT() moves the load address, not this one, which is what
     * the first attempt here did. An explicit address does move it, and
     * takes ld's own overflow check for LOWRAM off the stack in exchange, so
     * the collision it was protecting against is checked here instead. */
    ASSERT(_bss_end <= _stack_bottom,
           ".bss has grown into the boot stack -- LOWRAM is full; see the header comment")
    ASSERT(_stack_top == ORIGIN(LOWRAM) + LENGTH(LOWRAM),
           "the boot stack is no longer at the top of LOWRAM -- see the .stack comment")

    /* The heap floor (section 6, plan/phase15_memory_reclamation.md),
     * ported to this board with its own number.
     *
     * 256 KB is RP2350's figure, and it is justified there by two
     * measurements: a 38-page (152 KB) whole-system peak with two U-mode
     * programs resident, and a 32-page (128 KB) NAPOT-aligned contiguous run
     * for the largest placeable user image. Neither has been repeated here
     * -- no U-mode program runs on this board until E5 and no persona until
     * E7 -- so the floor is inherited rather than derived, on the grounds
     * that both figures are properties of this kernel's own allocations and
     * not of the silicon under them.
     *
     * When it fires, the answer is not to lower it. It is to ask which of
     * those two figures the change invalidated, and to re-measure them here
     * -- which E7 will have the persona to do. It has already fired once,
     * usefully: it is what said that putting .bss above 0x4ff40000 with
     * everything else left 136 KB, and the two-region split above is the
     * answer it forced. */
    ASSERT(_heap_end - _kernel_end >= 256K,
           "ESP32-P4 heap has fallen below its 256 KB floor -- something grew .text or .data; see plan/phase15_memory_reclamation.md")

    /* The whole writable region, so /proc/meminfo reports the heap and the
     * stack as fractions of something real, taken from the MEMORY block
     * rather than written down again in C. The full 768 KB, because with the
     * split above this build genuinely uses all of it.
     *
     * _text_start is this target's own, and kernel/meminfo.c reads it: the
     * image here is NOT one contiguous run from the bottom of RAM, which is
     * the assumption behind the usual `_bss_end - _ram_start` figure, so
     * without it /proc/meminfo would report the .bss half and quietly omit
     * 227 KB of resident kernel text. */
    _ram_start  = ORIGIN(LOWRAM);
    _ram_end    = ORIGIN(RAM) + LENGTH(RAM);
    _text_start = ADDR(.text);
}
