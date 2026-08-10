#include "usys.h"

/* A user program that reaches a *service* rather than a syscall (C3,
 * plan/phase6_memory_and_processes.md).
 *
 * This is the capability the phase exists to establish. Before SYS_CHAN_CALL a
 * U-mode program could print, read files and write files, and nothing else --
 * so "move a kernel subsystem out into a process" had no way for anything to
 * reach the result. A program that can name a service and exchange messages
 * with it over the same copy-always channels the kernel's own servers use is
 * what makes C6 and C7 worth doing.
 *
 * /srv/console is the service used here because it is the one that can be
 * observed from outside without a second program: whatever is sent to it
 * appears on the terminal. The marker is emitted *by the console server*, not
 * by this program's own SYS_PRINT, so seeing it proves the message crossed the
 * channel rather than taking the direct path.
 *
 * Markers:
 *   UCHAN_VIA_SERVICE  -- emitted by the console server on this program's behalf
 *   UCHAN_SENT n       -- the call returned, with the reply length
 *   UCHAN_NOSUCH_OK    -- an unregistered name is refused rather than reaching
 *   UCHAN_FOREIGN_OK   -- a buffer this program does not own is refused
 *   UCHAN_DONE
 */
int _start(void) {
    static const char msg[] = "UCHAN_VIA_SERVICE\n";
    char reply[8];

    long n = uchan_call("console", msg, sizeof(msg) - 1, reply, sizeof(reply));
    uprint("UCHAN_SENT ");
    uputnum(n);
    uprint("\n");

    /* A name nothing has registered must fail. Without this the test above
     * would pass for a kernel that accepted every name and quietly did
     * nothing -- the reply length would just be zero. */
    if (uchan_call("definitely_not_a_service", msg, 4, reply, sizeof(reply)) < 0) {
        uprint("UCHAN_NOSUCH_OK\n");
    }

    /* The confused-deputy case, at the channel boundary this time. The kernel
     * must validate the request pointer against *this* program's domain, not
     * merely copy from wherever it points -- otherwise a program that cannot
     * read kernel memory itself could still ask a service to read it.
     * 0x100 is not in any of this program's three pages. */
    if (uchan_call("console", (const void *)0x100, 8, reply, sizeof(reply)) < 0) {
        uprint("UCHAN_FOREIGN_OK\n");
    }

    /* The retired register-IPC numbers must be gone, not repurposed. A binary
     * built against the old ABI should get a clean refusal; if some later
     * syscall took number 1, this call would instead invoke it with arguments
     * meant for something else. */
    if (usyscall(1, 0, 0, 0) < 0) {
        uprint("UCHAN_RETIRED_OK\n");
    }

    uprint("UCHAN_DONE\n");
    return 0;
}
