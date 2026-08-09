#include "usys.h"

/* The isolation probe, as a real user program.
 *
 * kernel/shell.c already has `usertest-isolation`, but it proves something
 * narrower than it looks: the probe is kernel code in a kernel section,
 * granted execute by a region the kernel constructed around its own image. It
 * shows the domain mechanism works. It cannot show that a program which
 * arrived from the filesystem, was parsed by the loader, and was placed in
 * allocator pages is confined -- because until this milestone such a program
 * did not run in U-mode at all. It was called, from the kernel, at kernel
 * privilege.
 *
 * So this exists to make the loader path itself the thing under test.
 *
 * The address is deliberate, and it is a peripheral rather than a stray
 * kernel pointer: 0x40000000 is the base of RP2350's APB peripheral window,
 * and it is outside every region of the task's domain on all three targets --
 * for a *different reason* on each, which is the useful part.
 *
 *   QEMU RV32   nothing matches it, and PMP denies U-mode by default
 *   QEMU RV64   it falls in the identity-mapped MMIO gigapage, which carries
 *               no PTE_U
 *   RP2350      Hazard3 hardwires U-mode RWX over 0x40000000..0x5fffffff
 *               (datasheet §3.8.8.1), so this is only denied because
 *               mem_domain_activate() installs a dynamic region that revokes
 *               it. Before that fix this store succeeded on real silicon
 *               while faulting under emulation -- a confined program with
 *               write access to every peripheral in the chip.
 *
 * One probe, three mechanisms, same required outcome.
 *
 * Expected result: the store faults, the trap handler terminates the task,
 * and `exec` reports that the program was terminated before it could exit.
 * Reaching the line after the store is a failure, and says so -- silence
 * would be indistinguishable from a task that was correctly stopped.
 */

int _start(void) {
    uprint("UISO_ALIVE probing outside the domain\n");

    *(volatile long *)0x40000000 = 0xdead;

    uprint("UISO_NOT_ISOLATED the store was allowed\n");
    return 1;
}
