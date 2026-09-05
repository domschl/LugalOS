/* Linker Script for LugalOS on Raspberry Pi Pico 2 (RP2350 RISC-V Hazard3 Core) */
OUTPUT_FORMAT(elf32-littleriscv)

MEMORY
{
    FLASH (rx)     : ORIGIN = 0x10000000, LENGTH = 4M
    RAM   (rwx)    : ORIGIN = 0x20000000, LENGTH = 512k
    SCRATCH_X (rwx): ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y (rwx): ORIGIN = 0x20081000, LENGTH = 4k
}

ENTRY(_entry_point)

SECTIONS
{
    .text : {
        KEEP (*(.binary_info_header))
        KEEP (*(.embedded_block))
        KEEP (*(.reset))
        . = ALIGN(4);
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(4);
    } > FLASH

    /* U-mode-executable code -- see the QEMU linker scripts for why it cannot
     * share pages with kernel .text. One page, power-of-two aligned. */
    .utext : ALIGN(4096) {
        _utext_start = .;
        *(.utext .utext.*)
        . = _utext_start + 4096;
    } > FLASH

    /* M5 Phase 4, plan/phase12_microkernel_migration.md: st7735's own
     * dedicated U-mode-executable page -- the shared .utext page above
     * ran out of room (font5x7[96][5] alone is 480 bytes, more than what
     * was left after heartbeat/tm1638/i2c). Same shape as .utext, a
     * second, separate region so PMP still only has to grant one RX
     * region per driver, just not always the *same* one. */
    .st7735text : ALIGN(4096) {
        _st7735text_start = .;
        *(.st7735text .st7735text.*)
        . = _st7735text_start + 4096;
    } > FLASH

    /* M5 Phase 5, plan/phase12_microkernel_migration.md: blk's own
     * dedicated U-mode-executable page, same reasoning as .st7735text
     * above -- the shared .utext page overflowed by 84 bytes once blk's
     * SD/SPI primitives (cs_select/spi_transfer/sd_send_cmd and the
     * sector read/write loops) joined heartbeat/tm1638/i2c there, found
     * by the linker itself ("cannot move location counter backwards"),
     * not predicted in advance. */
    .blktext : ALIGN(4096) {
        _blktext_start = .;
        *(.blktext .blktext.*)
        . = _blktext_start + 4096;
    } > FLASH

    /* M5 Phase 7, plan/phase12_microkernel_migration.md: usb_cdc's own
     * dedicated U-mode-executable page, same reasoning as .st7735text/
     * .blktext above -- the shared .utext page overflowed once usb_cdc's
     * dispatch (by far the largest of any driver converted so far: EP0
     * enumeration, EP2/EP4 pump/drain, the full SETUP_REQ dispatch)
     * joined heartbeat/tm1638/i2c/uart there, found by the linker itself
     * ("cannot move location counter backwards"), not predicted in
     * advance (though expected -- see this phase's own plan doc entry). */
    .usbtext : ALIGN(4096) {
        _usbtext_start = .;
        *(.usbtext .usbtext.*)
        . = _usbtext_start + 4096;
    } > FLASH

    /* Phase 17b, plan/phase17b_clock_task_split.md: the clock server's own
     * dedicated U-mode-executable page, same shape and same reasoning as
     * .st7735text/.blktext/.usbtext above. It carries the row scan, the
     * frame buffer writers, the button state machine, the glyphs and the
     * whole proportional font (drivers/pico_clock_font.c) -- the last of
     * those is why sharing .utext was never worth trying. */
    .clocktext : ALIGN(4096) {
        _clocktext_start = .;
        *(.clocktext .clocktext.*)
        . = _clocktext_start + 4096;
    } > FLASH

    .rodata : {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
        BYTE(0);
        . = ALIGN(4);
    } > FLASH

    /* READONLY, and it matters beyond tidiness. kernel/main.c declares these
     * as `const void * const`, but an explicit section attribute makes GCC
     * emit the section writable anyway -- and because the driver text pages
     * above end exactly where .rodata begins, ld folds .usbtext/.clocktext,
     * .rodata and this into a single PT_LOAD whose flags are the union: RWX.
     * ld then says so ("LOAD segment with RWX permissions"), correctly, about
     * an image that has no business carrying one. Marking the section what it
     * already is takes the W back out. */
    .binary_info (READONLY) : {
        KEEP (*(.binary_info.keep.*))
        *(.binary_info.*)
        . = ALIGN(4);
        BYTE(0);
        . = ALIGN(4);
    } > FLASH
    __binary_info_start = ADDR(.binary_info);
    __binary_info_end = ADDR(.binary_info) + SIZEOF(.binary_info);

    /* M5 heap-reclaim (plan/phase12_microkernel_migration.md): every
     * RP2350-only driver's own NAPOT-aligned static region -- U-mode
     * stacks plus usb_cdc's combined-state region -- grouped by required
     * alignment and packed contiguously, largest first, instead of
     * scattered across .bss in link order. Found on real hardware in M5
     * Phase 7: a NAPOT `aligned(N)` object costs not just its own size but
     * (empirically) up to another N-1 bytes of *preceding* linker padding
     * to reach that alignment wherever it happens to land -- across all
     * eight of these objects that padding totalled ~51 KB, more than the
     * objects themselves. Packing them here instead, first-to-last:
     *
     *   - Each tier's own total size is a whole multiple of that tier's
     *     alignment (e.g. two 2048-byte objects sum to 4096, itself
     *     2048-aligned), so the *next* tier's smaller alignment requirement
     *     is satisfied automatically at zero cost -- no per-object gap
     *     survives once objects are grouped this way.
     *   - Placed as the very first thing in RAM: ORIGIN(RAM) (0x20000000)
     *     is already 16384-aligned on its own, so even the widest tier
     *     (.ustacks16384, currently just usb_cdc's own g_usb_region) pays
     *     no leading gap either -- the one gap this scheme can't remove
     *     structurally (something has to be first) costs nothing here
     *     because what's first is already maximally aligned by construction.
     *
     * Each driver opts in with `__attribute__((section(".ustacksN")))`
     * alongside its existing `aligned(N)`, matching the array's own real
     * size -- N must still be its own PMP grant size (mem_domain_add()'s
     * own power-of-two-and-self-aligned check is unchanged and unrelated
     * to this placement). Deliberately NOT applied to kernel/shell.c's
     * g_user_stack/g_echo_ustack: those are common code, built for the
     * Sv39/QEMU targets too, where a sub-page PMP-style shrink either does
     * nothing (Sv39 can't grant finer than a page) or, worse, could
     * silently over-grant the rest of the page around a smaller region --
     * a hazard that file's own comment already names. Scoped to RP2350
     * driver files only, where PMP genuinely honors these sizes. */
    .ustacks16384 (NOLOAD) : ALIGN(16384) {
        *(.ustacks16384)
    } > RAM

    .ustacks2048 (NOLOAD) : ALIGN(2048) {
        *(.ustacks2048)
    } > RAM

    .ustacks512 (NOLOAD) : ALIGN(512) {
        *(.ustacks512)
    } > RAM

    .ustacks256 (NOLOAD) : ALIGN(256) {
        *(.ustacks256)
    } > RAM

    /* What these tiers cost, so /proc/meminfo can report it (§6): 22 KB of
     * RAM that is neither .data nor .bss, and was invisible in the image
     * figure until now.
     *
     * From ADDR()/SIZEOF() rather than by bracketing with the location
     * counter. `.` between output sections is not the address of the next
     * one: everything above here is assigned `> FLASH`, so a
     * `_ustacks_start = .;` placed before .ustacks16384 picks up a *flash*
     * address, and subtracting it from a RAM one produced a 261435 KB
     * "ustacks" line on the board. Found by reading the number, not by
     * reasoning about the script. */
    _ustacks_start = ADDR(.ustacks16384);
    _ustacks_end   = ADDR(.ustacks256) + SIZEOF(.ustacks256);

    .data : {
        . = ALIGN(4);
        __data_start = .;
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        /* Code that must not be executing from flash while flash is being
         * written (I7b, plan/phase21_identity_and_authentication.md §3.2).
         *
         * Inside .data rather than in a section of its own, for two reasons
         * that both turned out to matter. It is already copied to RAM by
         * _reset_handler's walk of boot_header.S's address_mapping_table, so
         * there is no second copy-table entry to forget -- a separate section
         * would have placed the symbols correctly and left the bytes
         * uncopied, which executes whatever SRAM happened to hold. And a
         * separate *executable* output section next to this writable one made
         * ld union them into a single RWX PT_LOAD and say so, the same
         * complaint .binary_info above is annotated READONLY to avoid.
         *
         * NOT `.scratch_x`, which earlier drafts of the plan assumed: phase 15
         * §1.2 gave both scratch banks to the page allocator and the ASSERT
         * below still forbids placing there. It would also have cost 8 KB of
         * heap rather than this code's actual size, since the heap is one
         * contiguous range and excluding SCRATCH_X strands SCRATCH_Y above it.
         * Here the code sits inside the kernel's own RAM footprint, below
         * _kernel_end, so the heap simply starts after it. Main SRAM is what
         * the Pico SDK uses for the same job (`__no_inline_not_in_flash_func`
         * lands in ordinary RAM, not scratch), so nothing bank-specific is
         * being relied on. */
        . = ALIGN(4);
        __ramfunc_start = .;
        *(.ramfunc .ramfunc.*)
        . = ALIGN(4);
        __ramfunc_end = .;
        . = ALIGN(4);
        __data_end = .;
        _data_end = .;
    } > RAM AT> FLASH

    __data_load_ptr = LOADADDR(.data);
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
        _bss_end = .;
    } > RAM
    /* Boot stack in main RAM, not SCRATCH.
     *
     * It used to live at the top of SCRATCH_Y, giving 8 KB at most before it
     * ran into the page allocator's heap at 0x20080000 -- against 64 KB on the
     * QEMU targets. Survivable while nothing unexpected pushed onto it;
     * preemption adds a trap frame plus handler frames at an arbitrary point
     * in the deepest call chain in the system (the Lisp evaluator), and
     * overflowing here corrupts the heap rather than faulting. A real output
     * section so it is allocated inside RAM -- setting the location counter
     * between sections does not move a region-assigned one. */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        _stack_bottom = .;
        . += 16k;
        _stack_top = .;
    } > RAM

    /* Core 1's stack (X7, plan/phase23_multicore_scheduling.md). Same size
     * and same region as core 0's above, for the same reason -- see the
     * note further down where SCRATCH_Y used to serve this purpose, and the
     * hardware failure that moved it here.
     *
     * Unconditional rather than behind CONFIG_ENABLE_SMP, because a linker
     * script cannot see the generated config header. It costs every RP2350
     * persona 16 KB of ~360 KB; the 256 KB floor asserted below still
     * holds. NOLOAD, so it occupies no flash. */
    .stack1 (NOLOAD) : {
        . = ALIGN(16);
        _stack1_bottom = .;
        . += 16k;
        _stack1_top = .;
    } > RAM

    /* X3 probe (plan/phase23_multicore_scheduling.md): a word no boot path
     * writes.
     *
     * Deliberately outside every region the RP2350 boot code touches --
     * boot_header.S clears __bss_start..__bss_end and
     * _ustacks_start.._ustacks_end, and PAINT_BOOT_STACK paints only
     * _stack_bottom.._stack_top. That is what makes it usable as evidence:
     * whatever is here at the point core 0 reads it was put there by
     * something other than core 0's initialisation.
     *
     * The question it answers is whether core 1 executes this image at all.
     * The comment in _reset_handler has always said "RP2350 boots both
     * cores", but on RP-series silicon a secondary normally waits inside the
     * *bootrom* for a SIO-FIFO launch sequence, in which case that mhartid
     * branch is dead code and X3 needs the launch protocol rather than a
     * stack. Guessing between those two costs a debugging session either
     * way; this costs three instructions. */
    . = ALIGN(4);
    .smpmark (NOLOAD) : {
        _smp_mark = .;
        . += 4;
        /* The primary's own progress, same trick as core 1's word beside it
         * (X7). When both cores stop, "how far did core 1 get" answers half
         * the question -- and the half that was missing turned out to be
         * where core 0 was standing when it did. */
        _smp_mark0 = .;
        . += 4;
    } > RAM

    . = ALIGN(4096);
    _kernel_end = .;

    .scratch_x : {
        . = ALIGN(4);
        __scratch_x_start = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end = .;
    } > SCRATCH_X AT> FLASH

    __scratch_x_load_ptr = LOADADDR(.scratch_x);

    .scratch_y : {
        . = ALIGN(4);
        __scratch_y_start = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end = .;
    } > SCRATCH_Y AT> FLASH

    __scratch_y_load_ptr = LOADADDR(.scratch_y);

    __flash_binary_end = LOADADDR(.scratch_y) + SIZEOF(.scratch_y);

    /* Upper bound of the page allocator's heap (B2, kernel/palloc.c): the top
     * of SCRATCH_Y, i.e. the end of every SRAM bank this chip has.
     *
     * This used to stop at ORIGIN(RAM) + LENGTH(RAM) (0x20080000) to keep the
     * heap out of the scratch banks, protecting two things that were once in
     * them: the .scratch_x/.scratch_y output sections, and the boot stack,
     * which lived at the top of SCRATCH_Y before it moved into .stack above.
     * Neither is there any more. The boot stack moved; the two sections have
     * always linked to size 0, because nothing in this tree has ever placed
     * an object into either (the only `__scratch_*` references anywhere are
     * boot_header.S's own copy table, whose scratch entries are no-ops for
     * exactly that reason: its `bltu a1, a2` sees start == end and returns).
     *
     * So the bound was protecting 8 KB of real SRAM from being used at all.
     * RP2350 has 520 KB, not 512: SRAM0-7 striped at 0x20000000, then SRAM8
     * at 0x20080000 and SRAM9 at 0x20081000 -- **contiguous with main RAM in
     * the address map**, which is what lets one page bitmap span all of it
     * with no special case in palloc.c. (§1.2,
     * plan/phase15_memory_reclamation.md.)
     *
     * The bootrom's own initial stack pointer is SRAM_END, this same address
     * (boot_header.S's IMAGE_DEF, `.word SRAM_END`), so the bootrom does push
     * into these banks before handing over -- harmless, because
     * _reset_handler's first action on core 0 is `la sp, _stack_top`, with no
     * call or push between entry and that instruction, and core 1 never
     * leaves its `wfi` loop to acquire a stack at all. Whatever the bootrom
     * left there is dead by the time palloc_init() runs. */
    /* Core 1's stack is `.stack1` in main RAM, above -- NOT SCRATCH_Y.
     *
     * X3 put it in SCRATCH_Y and argued the case well: the scratch banks are
     * unstriped, so two cores hammering them do not contend the way they
     * would on striped SRAM0-7. The argument was right and the size was
     * wrong. 4 KB was justified as "core 1's own chain is shallow (entry ->
     * secondary_main -> sched_yield -> ctx_switch, plus a trap frame)",
     * which was true of X3's core 1, whose entire program was a counter.
     *
     * X7's core 1 runs secondary_main() for real: trap_init(), printk()
     * through the console and chan layers, sched_secondary_init(),
     * sched_yield(), ctx_switch(), and a trap frame on top of all of it at
     * an arbitrary point. Core 0 measures 8372 bytes peak on that same code.
     * 4 KB is not enough, and the failure is not a fault -- SCRATCH_Y ends
     * at 0x20081000 with the page allocator's heap immediately below it, so
     * an overflow corrupts the heap silently.
     *
     * That is exactly the bug .stack above was created to fix for core 0,
     * written up two hundred lines earlier in this file, and repeated here
     * for the second core. Observed on hardware 2026-09-04: core 1's first,
     * shallow printk() came out, everything deeper after it took the board
     * down hard enough that the USB console never came back.
     *
     * So core 1 gets the same 16 KB in main RAM that core 0 has, and
     * SCRATCH_Y goes back to the heap. Net cost against X3: 3 pages. The
     * unstriped-contention argument is a real but small optimisation, and it
     * does not outrank not corrupting the heap. */
    _heap_end = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);

    /* --- The heap floor (§6, plan/phase15_memory_reclamation.md) ---
     *
     * Every heap regression this project has had was discovered the same way:
     * a board failing to do something, days or weeks after the commit that
     * caused it. S4's NODE_POOL_SIZE growth surfaced as "chess will not
     * start", months later, on a completely fresh boot. The size of this heap
     * is known at *link* time, so that is where it should be caught.
     *
     * 256 KB, chosen against measurements rather than as a round number:
     *
     *   - The whole system's observed peak, with two U-mode programs resident
     *     at once, is 38 pages (152 KB) -- tests/hw/test_rp2350.py's C2.
     *   - The largest user image this board can place needs a 32-page (128 KB)
     *     NAPOT-aligned run, which is a *contiguous* requirement on top of the
     *     above, not a share of it.
     *
     * 64 pages leaves both satisfiable together with room to spare. At the
     * time of writing the heap is 89 pages, so there are 25 pages of slack
     * before this fires: enough that ordinary work never trips it, little
     * enough that a subsystem quietly claiming 100 KB of .bss does.
     *
     * When it does fire, the answer is not to lower it reflexively. It is to
     * ask which of the two figures above the change has invalidated -- and if
     * neither, then lowering it is a deliberate, reviewable decision with the
     * reasoning right here to argue against.
     *
     * Only on this board. The QEMU targets link into 128 MB, where a floor
     * would never fire and would only be one more number to keep plausible.
     * cmake/board-rp2350-clock.cmake's persona shares this script, so it is
     * covered too. */
    ASSERT(_heap_end - _kernel_end >= 256K,
           "RP2350 heap has fallen below its 256 KB floor -- something grew .bss; see plan/phase15_memory_reclamation.md")

    /* The scratch banks are the heap's now, so nothing may be placed into
     * them -- an object here would be handed out as heap memory and silently
     * overwritten. This is the guard that keeps _heap_end above honest: it is
     * a link error naming the cause, rather than corruption on the board. If
     * a future driver genuinely wants a scratch bank (they are unstriped, so
     * they behave differently under contention), take it back out of the heap
     * here at the same time. */
    /* X3 took SCRATCH_Y for core 1's stack, so only SCRATCH_X remains
     * heap-adjacent and unclaimed; the guard narrows to it rather than
     * being dropped. */
    ASSERT(SIZEOF(.scratch_x) == 0,
           "something now places into a scratch bank, but _heap_end hands both to the page allocator -- see plan/phase15_memory_reclamation.md")

    /* The whole writable region, so /proc/meminfo can report the heap and the
     * stack as fractions of something real. Taken from the MEMORY block rather
     * than written down again in C: the point of these numbers is that they
     * come from the board's memory map, not from a constant that can drift
     * away from it. Now includes SCRATCH_X/Y, because as of §1.2
     * (plan/phase15_memory_reclamation.md) the heap does -- this used to stop
     * at the end of main RAM and describe the two scratch banks as "8 KB that
     * exist on the die but that nothing on this board currently places into",
     * which was true and was precisely the argument for giving them to the
     * page allocator instead. Reporting 512 KB while the heap ran to 520
     * would make /proc/meminfo's heap-as-a-fraction-of-RAM figure wrong.
     *
     * FLASH likewise, so the flash figure covers the whole XIP window rather
     * than just the part __flash_binary_end reaches: on this board most of the
     * image is the embedded FAT32 payload, and the two are worth telling
     * apart. */
    /* --- The three-segment flash map (I7a, §3.3 of
     * plan/phase21_identity_and_authentication.md) ---
     *
     * __flashfs_base / __identity_base and their sizes arrive as --defsym
     * from cmake/flash_layout.cmake, which is the single definition of this
     * map. They are NOT written down again here: with the FAT32 image no
     * longer inside the ELF, this script has nothing of its own to check
     * against, so a second copy of the numbers would be exactly the drift
     * §10 warns about.
     *
     * What these two ASSERTs buy is that a boundary mistake is a link error
     * naming the cause, rather than an OS image that overwrites the
     * filesystem -- or, far worse, a device's identity -- the next time it is
     * flashed. Measured 2026-09-01: an OS reflash leaves the identity sector
     * byte-identical, which is the property these keep true as the image
     * grows.
     */
    ASSERT(__flash_binary_end <= __flashfs_base,
           "the OS image has grown past its segment and would overwrite the flash-fs -- raise LUGALOS_FLASHFS_BASE in cmake/flash_layout.cmake, or shrink the image")

    ASSERT(__flashfs_base + __flashfs_size <= __identity_base,
           "the flash-fs segment overlaps the identity sector -- see cmake/flash_layout.cmake")

    ASSERT(__identity_base + __identity_size <= ORIGIN(FLASH) + LENGTH(FLASH),
           "the identity sector runs past the end of flash -- see cmake/flash_layout.cmake")

    _ram_start   = ORIGIN(RAM);
    _ram_end     = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    _flash_start = ORIGIN(FLASH);
    _flash_end   = ORIGIN(FLASH) + LENGTH(FLASH);
}
