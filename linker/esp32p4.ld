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
 * ## Why the kernel's own text was the expensive part here, and stopped being
 *
 * E2 wrote this section as a prediction: on RP2350 the .text lives in flash
 * and costs no RAM at all, while here it was RAM-resident because the image
 * is RAM-*loaded*, and booting from flash instead would give most of it back.
 * Phase 28 is what forced the issue -- adding Ethernet took the heap to
 * exactly its 128 KB floor -- and phase 32 is where it was done.
 *
 * So this is no longer a prediction. **.text and .rodata live in the XIP
 * region below**, linked into the flash window at 0x40000000 and fetched
 * through the cache; only .utext and .data remain RAM-resident of the
 * loadable sections. That moved 249 KB out of L2MEM and took the heap from
 * 128.0 KB to 376.0 KB.
 *
 * The consequence for anyone reading this script: an image it produces
 * **cannot be started by `esptool load-ram` alone**. load-ram writes segments
 * to their link addresses and jumps to the entry point, and both are now in
 * a window that is flash-mapped rather than RAM. A stub maps it first; see
 * plan/phase32_esp32p4_execute_in_place.md, and note that tools/p4run.py
 * refuses such an image with an explanation rather than hanging.
 */

OUTPUT_FORMAT(elf32-littleriscv)

MEMORY
{
    /* NOLOAD SECTIONS ONLY. See the header comment: this range overlaps the
     * ROM's download-mode buffers, so anything the loader has to write into
     * it would corrupt the loader that is doing the writing. ld enforces the
     * *size*; the ASSERTs at the bottom enforce the rule.
     *
     * 252K, not 256K, and the missing 4 KB is the whole point (E6).
     *
     * E2 read the ROM's map out of IDF's own bootloader.memory.ld.in and
     * recorded `0x4ff3fba4`-`0x4ff40000` as "ROM .bss/.data", then placed the
     * boot stack across it anyway, reasoning that NOLOAD sections are safe in
     * the ROM's area because the ROM's buffers only matter while the loader
     * is writing. That is true of the *download buffers* -- region 1 of that
     * appendix -- and false of regions 4 and 5, which it labels "shared
     * memory used in startup code **and when IDF runs**" and "the 'interface'
     * data with constant addresses".
     *
     * It cost nothing until E6 called a ROM routine. `rom_spiflash_legacy_data`
     * and `_funcs` -- the pointers every ROM flash function dereferences --
     * live at 0x4ff3ffe8 and 0x4ff3ffec on this chip revision, inside the
     * boot stack, which entry.S paints with 0xA5A5A5A5. The first call into
     * ROM flash code faulted at `epc` inside the ROM, dereferencing
     * 0xa5a5a5a5: we had painted over the ROM's own function table.
     *
     * Note the revision dependency, because it is a trap. IDF ships two ROM
     * symbol files and this project uses the `eco0_4` one (chip v1.3). The
     * flash *function* addresses are identical in both; the *data* addresses
     * differ by 0x80000 -- 0x4ff3ffe8 here against 0x4ffbffe8 for r > ECO4.
     * Checking the functions and generalising would have given the wrong
     * answer with every appearance of having been verified.
     *
     * Shortening the region is what makes this structural: nothing the linker
     * can place is able to reach the reserved bytes, rather than nothing
     * happening to. 4 KB rather than the 1116 bytes strictly needed, because
     * a page is a rounder thing to reason about and LOWRAM has room. */
    LOWRAM (rw)  : ORIGIN = 0x4ff00000, LENGTH = 252K

    /* Everything loadable, and the heap after it.
     *
     * 384K, not the 512K that reaches L2MEM's end at 0x4ffc0000, and the
     * difference is the whole of E7's memory corruption.
     *
     * The L2 cache on this part is carved out of L2MEM itself, at the top, so
     * whatever is reserved for it is not RAM. The boot ROM leaves it at 256 KB
     * -- measured, by painting the heap and watching where the paint stopped
     * surviving: everything from 0x4ff80000 up decayed to zero, with no store
     * to blame and the page allocator provably innocent. ESP-IDF's own layout
     * states the relationship, for a chip below v3 as this v1.3 part is
     * (components/esp_system/ld/esp32p4/memory.ld.in):
     *
     *     #define SRAM_HIGH_START  0x4FF40000
     *     #define SRAM_HIGH_SIZE   0x80000 - CONFIG_CACHE_L2_CACHE_SIZE
     *
     * kernel_main() calls esp32p4_l2_cache_shrink() before touching the heap,
     * which asks the ROM for a 128 KB cache -- IDF's own default -- moving the
     * boundary to 0x4ffa0000. This length and that call are one change: the
     * ASSERT below is what keeps them from drifting apart. */
    RAM    (rwx) : ORIGIN = 0x4ff40000, LENGTH = 384K

    /* Flash, mapped into the CPU's address space and executed in place.
     * U1, plan/phase32_esp32p4_execute_in_place.md.
     *
     * 0x40000000 is SOC_IROM_LOW *and* SOC_DROM_LOW (soc/soc.h) -- on this
     * part instruction and data share one window, so .text and .rodata live
     * in one region and there is no I/D split to keep consistent.
     *
     * Nothing maps this at link time. U2's stub calls the ROM's
     * Cache_FLASH_MMU_Set() to point these addresses at flash offset
     * LUGALOS_P4_OSIMAGE_BASE before any of it is fetched, which is why an
     * image built by this script cannot be started by `load-ram` alone.
     *
     * The MMU's constraint is `paddr % 64KB == vaddr % 64KB`, and both ends
     * of ours are 64 KB-aligned (0x00010000 and 0x40000000), so it holds
     * trivially. ESP-IDF's own script offsets this origin by 0x20 and
     * explains why: its .bin carries a 0x18 file header plus an 0x08 segment
     * header, so the payload starts 0x20 into the flash page and the vaddr
     * has to match. **We inherit neither the header nor the offset** -- U2
     * maps a raw binary -- and copying the 0x20 without the format that
     * needs it would be a magic number with a false explanation.
     *
     * 1 MB matches LUGALOS_P4_OSIMAGE_SIZE in
     * cmake/flash_layout_esp32p4.cmake; the ASSERT at the bottom is what
     * keeps the two from drifting. */
    XIP    (rx)  : ORIGIN = 0x40000000, LENGTH = 1M
}

ENTRY(_start)

SECTIONS
{
    /* --- the RAM-resident boot code, and why it is not in flash ---------
     *
     * U2. Everything that runs before the flash MMU is mapped has to be in
     * RAM, because until esp32p4_xip_map() returns there is nothing at
     * 0x40000000 to fetch. That is exactly two things: entry.S, which the
     * ROM jumps to, and the mapping function itself.
     *
     * Note what this replaces. The plan called for a separate "stub" with
     * its own entry point that would map flash and then jump to the kernel.
     * entry.S already *is* the boot path -- it sets mtvec, the stack, clears
     * .bss and calls kernel_main -- and it happens to need nothing from
     * flash before the point where the mapping can be inserted. So there is
     * one boot path with a new first step, rather than two boot paths that
     * have to agree with each other.
     *
     * First in the region, so _start is at ORIGIN(RAM) and the ROM's entry
     * point is a fixed, inspectable address. */
    .boot : {
        . = ALIGN(4);
        _boot_start = .;
        *(.text.entry)
        *(.boot.text .boot.text.*)
        . = ALIGN(4);
        _boot_end = .;
    } > RAM

    /* --- the flash-resident half ----------------------------------------
     *
     * .text and .rodata are fetched from flash through the cache. They are
     * the reason this phase exists: 249 KB that used to be RAM-resident
     * because the image was RAM-*loaded*, on a board where the heap and the
     * loadable image come out of the same 384 KB.
     *
     * Note that `.text.entry` is NOT here any more -- it is in .boot above,
     * which is what makes the kernel's entry point reachable before the
     * mapping exists. */
    .text : {
        . = ALIGN(4);
        _xip_start = .;
        *(.text .text.*)
        . = ALIGN(4);
    } > XIP

    /* U-mode-executable code, one page, power-of-two-sized and self-aligned
     * so a single PMP NAPOT entry can grant it -- see the QEMU scripts for
     * why it cannot share pages with the kernel's own .text, and
     * kernel/board.c's board_text_region() for who reads _utext_start.
     *
     * Nothing enters U-mode until E5. The page is here from the start
     * anyway: it costs 4 KB, and the alternative is a memory map that
     * changes shape in the middle of the phase.
     *
     * U1 left this in RAM while .text moved to flash. It is code, so it
     * looks like it belongs in the XIP window, and it does not: PMP grants
     * it to U-mode, and whether a PMP region can usefully cover
     * flash-mapped addresses is a question this phase does not have to
     * answer for the sake of 4 KB. It also means U-mode code keeps running
     * when the cache is disabled for a flash write (U3). */
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
        _xip_end = .;
    } > XIP

    /* How many 64 KB MMU pages the stub has to map. Computed here rather
     * than in the stub, because here is where the section sizes are known
     * and a stale constant in C would map too few pages and fault in
     * whatever landed past the end. */
    _xip_pages = (_xip_end - _xip_start + 0xFFFF) >> 16;

    /* No AT> anywhere in this script, so LOADADDR(.data) == ADDR(.data) and
     * _sidata == _data_start. arch/riscv/common/entry.S tests exactly that
     * and skips its copy loop -- correct here, because load-ram places .data
     * at its final address itself. The symbol still has to exist: entry.S
     * refers to it unconditionally.
     *
     * U1 kept that true. .data could have been stored in the flash image and
     * copied out at boot, which is what a conventional XIP layout does, but
     * it is 372 bytes and the copy loop is a thing that can be wrong. It
     * stays in the RAM-loaded half and arrives at its final address. */
    _sidata = LOADADDR(.data);

    .data : {
        . = ALIGN(4);
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
        _data_end = .;
    } > RAM

    /* Where the RAM-resident loadable image ends and the heap begins.
     *
     * After U1 this covers .utext and .data only -- and, from U2, the stub.
     * .text and .rodata are above in the XIP window and no longer count
     * against it, which is the whole point of the phase: this figure went
     * from 253.5 KB to under 8 KB, and the heap from 128 KB to ~376 KB. */
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

    /* Nothing may reach the ROM's constant-address data at the top of L2MEM.
     * LOWRAM's length already makes this unreachable; the ASSERT is what says
     * so if someone lengthens it again. */
    ASSERT(ORIGIN(LOWRAM) + LENGTH(LOWRAM) <= 0x4ff3f000,
           "linker/esp32p4.ld: LOWRAM reaches the ROM's reserved data at 0x4ff3fba4-0x4ff40000; the ROM flash routines dereference pointers there")

    /* The rule from the header comment, as a link error rather than as a
     * corrupt load. Every loadable section is assigned `> RAM` above, so
     * this is really a check that RAM itself has not been moved down; if a
     * future section is added to LOWRAM without NOLOAD, ld's own "section
     * .foo will not fit / LMA overlaps" is the second net. */
    /* RAM must stop where the L2 cache's storage begins. 0x4ffa0000 is the
     * boundary for the 128 KB cache esp32p4_l2_cache_shrink() configures; if
     * that call is removed or the size changed, this is the line that has to
     * change with it, and the ASSERT is here so it cannot be forgotten. Memory
     * past this point accepts stores and then loses them. */
    ASSERT(ORIGIN(RAM) + LENGTH(RAM) <= 0x4ffa0000,
           "linker/esp32p4.ld: RAM reaches into the L2 cache's own storage at the top of L2MEM; see esp32p4_l2_cache_shrink()")
    ASSERT(ORIGIN(RAM) >= 0x4ff40000,
           "loadable sections would overlap the ROM's download-mode buffers (0x4ff296b8..0x4ff40000) -- see the header comment")
    /* .text used to be checked against that boundary too. After U1 it is not
     * in RAM at all, so the check that matters is the opposite one: that it
     * really did land in the flash window and did not quietly fall back into
     * RAM if someone changes its region. A .text at 0x4ff4xxxx would boot
     * perfectly under load-ram and silently undo the whole phase. */
    /* The boot code must be in RAM and must be where the ROM's image says
     * the entry point is. A .boot that drifted into the flash window would
     * mean jumping to an address that is not mapped yet -- the one failure
     * this whole arrangement exists to avoid, and one with no console to
     * report it. */
    ASSERT(ADDR(.boot) >= 0x4ff40000 && ADDR(.boot) < 0x4ffc0000,
           "linker/esp32p4.ld: .boot is not in L2MEM -- the code that maps flash cannot itself live in flash")

    ASSERT(ADDR(.text) >= 0x40000000 && ADDR(.text) < 0x44000000,
           "linker/esp32p4.ld: .text is not in the flash XIP window -- execute-in-place has been undone; see plan/phase32_esp32p4_execute_in_place.md")
    ASSERT(ADDR(.rodata) >= 0x40000000 && ADDR(.rodata) < 0x44000000,
           "linker/esp32p4.ld: .rodata is not in the flash XIP window; see plan/phase32_esp32p4_execute_in_place.md")

    /* The MMU maps whole 64 KB pages, so a window origin that is not
     * 64 KB-aligned cannot be expressed: Cache_FLASH_MMU_Set() returns 2,
     * "vaddr or paddr is not aligned". Checked rather than commented,
     * because the failure is a stub that refuses at boot with no console. */
    ASSERT(ORIGIN(XIP) % 0x10000 == 0,
           "linker/esp32p4.ld: the XIP window must start on a 64 KB MMU page boundary")

    /* The flash half must fit the segment reserved for it in
     * cmake/flash_layout_esp32p4.cmake (LUGALOS_P4_OSIMAGE_SIZE, 1 MB). The
     * two numbers are deliberately in two files -- one is a property of the
     * link, the other of the flash map -- so this is what keeps them
     * agreeing. Overflowing it would write the OS image over /flash0. */
    ASSERT(_xip_end - _xip_start <= LENGTH(XIP),
           "linker/esp32p4.ld: .text + .rodata overflow the 1 MB OS image segment; raise LUGALOS_P4_OSIMAGE_SIZE and this region together")

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
     * answer it forced.
     *
     * E7 lowered it to 128 KB, and this is the one case the paragraph above
     * did not cover: neither of the two figures was invalidated by a change of
     * ours. The silicon simply has less memory than this script was claiming.
     * The old 256 KB was met only by counting the 128 KB at the top of L2MEM
     * that the L2 cache owns -- so the floor was being satisfied with memory
     * that does not exist, which is why it never fired while the heap was
     * quietly handing out cache storage. See the RAM region above.
     *
     * What that costs, stated rather than discovered later: the real figure is
     * 144 KB (0x4ff7c000..0x4ffa0000 as things stand), which clears the 128 KB
     * contiguous run for the largest placeable user image but is *below*
     * RP2350's 38-page (152 KB) whole-system peak with two U-mode programs
     * resident. So this board cannot host that peak, and the way to buy room
     * back is .text: 171 KB of it, against 63 KB of .rodata. */
    ASSERT(_heap_end - _kernel_end >= 128K,
           "ESP32-P4 heap has fallen below its 128 KB floor -- something grew .text or .data; see plan/phase15_memory_reclamation.md")

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

    /* G1, plan/phase30_driver_framework.md. Below the memory map because
       they are derived from it: a linker script assignment sees only symbols
       already assigned above it.

       _image_bytes -- this image is NOT one contiguous run from the bottom of
       RAM, which is what every other target's expression assumes. .bss and
       the boot stack sit in the low 256 KB of L2MEM (safe only because they
       are NOLOAD, under the ROM's download-mode buffers) while .text,
       .rodata and .data load above them, so the two halves are added. Getting
       this wrong reported a third of the image as the whole of it.

       _inst_lo/_inst_hi -- all code is RAM-resident here. */
    _image_bytes = (_bss_end - _ram_start) + (_kernel_end - _text_start);
    _inst_lo     = _ram_start;
    _inst_hi     = _ram_end;
}
