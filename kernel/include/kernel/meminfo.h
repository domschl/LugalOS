#ifndef LUGALOS_KERNEL_MEMINFO_H
#define LUGALOS_KERNEL_MEMINFO_H

/* Where the memory actually went.
 *
 * Section sizes are exact in the ELF and a running kernel cannot improve on
 * them, so this is deliberately not a re-implementation of `size`. It reports
 * the three things the ELF genuinely cannot know:
 *
 *   - how deep the boot stack has ever been (stack_used_bytes)
 *   - how many pages the heap has ever held at once (palloc_extra_stats)
 *   - how much of the board's RAM is left over after all of it
 *
 * The stack figure is the one with teeth. linker/rp2350.ld records why the
 * boot stack was moved out of SCRATCH_Y: preemption can push a trap frame at
 * an arbitrary point in the deepest call chain in the system, and overflowing
 * the stack there corrupts whatever sits below it rather than faulting. That
 * hazard was reasoned about but never measured. Painting the stack at boot
 * and scanning for the paint turns it into a number.
 */

/* Written over the whole unused boot stack by arch/riscv/common/entry.S
 * before it calls kernel_main(), then scanned back by stack_used_bytes().
 *
 * A byte-uniform pattern on purpose: entry.S stores it register-wide (4 bytes
 * on RV32, 8 on RV64) while the scan below reads uintptr_t, and a pattern
 * whose every byte is identical is the same value either way. It also has to
 * be a value the stack is unlikely to hold legitimately -- 0xA5A5A5A5 is
 * neither a plausible pointer on any target here (RAM is at 0x20000000 or
 * 0x80000000) nor a plausible small integer.
 *
 * Shared with entry.S, which is why this sits outside the __ASSEMBLER__
 * guard: one definition, so the paint and the scan cannot drift apart. */
#define STACK_POISON 0xA5A5A5A5

#ifdef __ASSEMBLER__

/* Paints [_stack_bottom, sp) with STACK_POISON.
 *
 * A macro, and living here next to the pattern, because **this kernel has two
 * boot paths and they do not share one**. The QEMU targets enter at `_start`
 * (arch/riscv/common/entry.S); RP2350 enters at `_reset_handler`
 * (arch/riscv/rp2350/boot_header.S) via the bootrom, and never executes
 * `_start` at all. Painting in one of them silently instruments only half the
 * targets -- which is exactly what happened when this was first written, and
 * the hardware read 100% stack usage because no poison had ever been written.
 * One macro, invoked from both, so the next thing added to a boot path is
 * visibly added to a boot path.
 *
 * Call with sp already pointing at the top of the boot stack and with all
 * three registers dead: they are clobbered. Everything in [_stack_bottom, sp)
 * is by definition not live, so this cannot destroy anything a caller needs.
 */
.macro PAINT_BOOT_STACK ta, tb, tc
    li \tc, STACK_POISON
#if __riscv_xlen == 64
    /* A positive 32-bit constant leaves the top half zero, which would paint
     * every other word with zeroes and stop the scan at the first of them.
     * \tb is still free here; it becomes sp below. */
    slli \tb, \tc, 32
    or   \tc, \tc, \tb
#endif
    la \ta, _stack_bottom
    mv \tb, sp
8:
    bgeu \ta, \tb, 9f
#if __riscv_xlen == 64
    sd \tc, 0(\ta)
    addi \ta, \ta, 8
#else
    sw \tc, 0(\ta)
    addi \ta, \ta, 4
#endif
    j 8b
9:
.endm

#else /* !__ASSEMBLER__ */

#include <stdint.h>
#include <stdbool.h>

/* High-water mark of the boot stack, in bytes: the deepest it has been since
 * boot, not how deep it is now.
 *
 * Found by scanning up from _stack_bottom for the first word that is no
 * longer STACK_POISON; everything above that has been written at some point.
 * O(stack size) and called only when /proc/meminfo is read.
 *
 * Two honest limitations. It cannot see a frame that was pushed but never
 * written to (an allocated-and-untouched local), so it is a lower bound --
 * which is the safe direction for a headroom figure. And a task that happened
 * to store the poison value itself and then unwound would be read as
 * untouched; see the note on the pattern above for why that is not a real
 * risk here.
 *
 * If no poison survives at all this returns the full stack size, which reads
 * as "100% used". That is the correct alarm for the case it most likely
 * means -- the stack really did reach the bottom -- and it is deliberately
 * not distinguished from the other case that produces it, an entry path that
 * never painted at all. Both need looking at, and inventing a separate
 * sentinel would only make the alarming one easier to explain away. */
uint32_t stack_used_bytes(void);

/* Total size of the boot stack region, from the linker script. */
uint32_t stack_size_bytes(void);

/* The board's RAM map, all figures in bytes.
 *
 * `image_bytes` is everything from the start of RAM through the end of .bss:
 * the RAM the loaded image occupies before a single allocation happens. It is
 * measured that way, rather than as .data + .bss, so that it stays honest on
 * both memory models -- on the QEMU targets .text and .rodata live in the
 * same RAM region and belong in the figure, and on RP2350 the alignment gap
 * between .data and .bss is RAM that is just as unavailable as either.
 * RP2350's flash-resident .text/.rodata are reported by meminfo_flash().
 *
 * `stack_bytes` and `heap_bytes` are the regions reserved for each. The three
 * do not necessarily sum to `total_bytes` -- inter-section padding is in
 * neither -- so a caller wanting "free" should subtract, not add. */
typedef struct {
    uintptr_t ram_start;
    uintptr_t ram_end;
    uint32_t  total_bytes;
    uint32_t  image_bytes;
    uint32_t  stack_bytes;
    uint32_t  heap_bytes;
} mem_ram_map_t;

void meminfo_ram_map(mem_ram_map_t *out);

/* Flash image size and the size of the flash region, in bytes. Returns false
 * on targets that execute from RAM and have no separate flash to report (the
 * QEMU builds), leaving *used and *total untouched. */
bool meminfo_flash(uint32_t *used, uint32_t *total);

#endif /* __ASSEMBLER__ */

#endif /* LUGALOS_KERNEL_MEMINFO_H */
