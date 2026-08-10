#include "usys.h"

/* A user program that reads its own arguments (C3,
 * plan/phase6_memory_and_processes.md).
 *
 * Entered as `int _start(int argc, char **argv)` with no crt: the loader puts
 * argc in a0 and argv in a1, and the RISC-V calling convention already says
 * that is where a function's first two arguments live. Nothing had to be added
 * on this side of the boundary to make that work, which is the point of doing
 * it that way.
 *
 * Every marker is distinct so a test can tell which part failed rather than
 * only that the program did not finish:
 *
 *   UARGS_COUNT n     -- argc arrived
 *   UARGS_ARG i:s     -- argv[i] was readable and is a NUL-terminated string
 *   UARGS_NULL_OK     -- argv[argc] is NULL, as a C program is entitled to expect
 *   UARGS_DONE        -- it got to the end
 *
 * Reading argv at all exercises the thing worth testing: the vector and the
 * strings live in this program's *own* stack page, so a loader that passed a
 * kernel address instead would fault here rather than silently working.
 */
int _start(int argc, char **argv) {
    uprint("UARGS_COUNT ");
    uputnum(argc);
    uprint("\n");

    for (int i = 0; i < argc; i++) {
        uprint("UARGS_ARG ");
        uputnum(i);
        uprint(":");
        /* Through the pointer, deliberately -- a vector of addresses this
         * program cannot dereference is not a vector it has been given. */
        uprint(argv[i]);
        uprint("\n");
    }

    if (argv[argc] == 0) {
        uprint("UARGS_NULL_OK\n");
    }

    uprint("UARGS_DONE\n");
    /* A distinguishable status, so the exit-status path can be tested with a
     * value that is not 0 and not the 7 uhello returns. */
    return 42;
}
