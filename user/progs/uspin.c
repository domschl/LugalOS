#include "usys.h"

/* Does a timer interrupt actually reach user code?
 *
 * `preempttest` already proves the timer preempts a task that never yields,
 * but that task is kernel code. This asks the narrower question about the
 * code that is least entitled to cooperate: a program loaded off the
 * filesystem, running in U-mode under a memory domain.
 *
 * Proving it is harder than it sounds, because almost everything a program
 * can observe is a syscall, and a syscall enters the kernel anyway -- so "the
 * kernel ran" is not evidence that the *timer* interrupted user code.
 *
 * The tick counter is evidence. Only the timer interrupt handler advances it
 * (arch/riscv/common/trap.c), so:
 *
 *     read the count -> spin, making no syscall at all -> read it again
 *
 * A count that moved means a timer interrupt was taken while this program was
 * executing in U-mode, and that the program was resumed correctly afterwards
 * -- it is still running to print the result.
 *
 * ## This is not true by construction
 *
 * It looks like it might be: the privileged spec says interrupts for a higher
 * privilege level are always globally enabled while the hart runs at a lower
 * one, which would make U-mode preemptible regardless of what the kernel does
 * with mstatus.MIE. QEMU behaves that way and the test passes there either
 * way.
 *
 * Real RP2350 silicon does not. With the enable bit cleared before entering
 * U-mode, this exact program reports zero ticks across a 0.4-second spin; with
 * it set, 41. That is the whole reason this program exists, and why the
 * hardware suite runs it too -- the QEMU result alone would have justified
 * deleting the line in arch/riscv/common/umode.S that makes it work.
 */

/* Long enough to cross several 100 Hz ticks under emulation, and no longer.
 * The window cannot be made self-timing -- the whole point is that it contains
 * no syscalls -- so the count is calibrated instead: 4 million was measured at
 * zero ticks (the loop finished inside one 10 ms period and the test could not
 * have detected preemption either way), 40 million at seven. This sits in
 * between with margin on the side that matters. */
#define SPIN_ITERATIONS 20000000L

static volatile long sink;

int _start(void) {
    uprint("USPIN_START\n");

    long before = uticks();
    for (long i = 0; i < SPIN_ITERATIONS; i++) {
        sink += i;   /* volatile: the loop must survive the optimiser */
    }
    long after = uticks();

    uprint("USPIN_TICKS ");
    uputnum(after - before);
    uputchar('\n');

    if (after > before) {
        uprint("USPIN_PREEMPTED\n");
    } else {
        uprint("USPIN_NOT_PREEMPTED\n");
    }

    /* M5, plan/phase12_microkernel_migration.md: exercises SYS_YIELD/
     * SYS_TIME_MS from U-mode -- the two syscalls a long-lived U-mode driver
     * task's own poll loop needs (this program otherwise has no reason to
     * touch either). Cheap, QEMU-side proof that both round-trip correctly
     * before the RP2350-specific heartbeat-to-U-mode conversion that
     * actually depends on them touches real hardware. Not asserting a
     * nonzero delta -- 50 yields can legitimately complete inside one
     * millisecond -- just that both calls return normally rather than
     * faulting or hanging. */
    long t0 = utime_ms();
    for (int i = 0; i < 50; i++) uyield();
    long t1 = utime_ms();
    uprint("USPIN_YIELD_OK ");
    uputnum(t1 - t0);
    uputchar('\n');

    return 0;
}
