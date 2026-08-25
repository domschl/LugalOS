#include "usys.h"

/* W^X, from the program's own point of view (C4,
 * plan/phase6_memory_and_processes.md).
 *
 * Worth its own program because C4 changed where the answer comes from. The
 * loader used to grant page 0 as R|X and page 1 as R|W by position -- it knew
 * the layout because linker/user.ld asserted it. Permissions are now derived
 * from each segment's ELF p_flags, so "the text page is not writable" stopped
 * being a property of the loader's assumptions and became a property of
 * reading the headers correctly. A loader that mixed up the flags, or granted
 * a NAPOT piece with the wrong permissions, would still run every other test
 * here perfectly.
 *
 * The program stores into its own text. It should not survive:
 *
 *   UWX_ALIVE          -- printed before the attempt, so a missing marker
 *                         means it died too early rather than passing
 *   UWX_NOT_ENFORCED   -- printed only if the store succeeded, which is the
 *                         failure this exists to catch. A loud line rather
 *                         than an absent one.
 */
int _start(void) {
    uprint("UWX_ALIVE\n");

    /* Its own entry point: unambiguously inside the executable segment, and
     * an address the program is entitled to *read*. Volatile so the store
     * cannot be optimised away as dead.
     *
     * Via `unsigned long` rather than `(void *)`: ISO C has no conversion
     * from a function pointer to an object pointer, and -Wpedantic says so.
     * An integer of pointer width is the spelling that means the same thing
     * without the extension. */
    volatile unsigned int *own_text =
        (volatile unsigned int *)(unsigned long)_start;
    *own_text = 0;

    uprint("UWX_NOT_ENFORCED\n");
    return 1;
}
