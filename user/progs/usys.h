#ifndef LUGALOS_USER_USYS_H
#define LUGALOS_USER_USYS_H

/* The system call interface, as seen from a U-mode program.
 *
 * This header is compiled into user programs only -- it shares nothing with
 * the kernel except the numbers, which is the point: a user program has no
 * kernel headers, no kernel symbols and no kernel code, so nothing it does
 * can accidentally depend on being inside the kernel image. Compare the
 * probes in kernel/shell.c, which are kernel code pretending not to be.
 *
 * ABI: a0 carries the call number and the return value, a1..a5 the
 * arguments. That is what arch/riscv/common/trap.c reads.
 *
 * Pointer arguments are validated by the kernel against the calling task's
 * memory domain (kernel/uaccess.c), so passing an address the program does
 * not own returns an error rather than reaching into the kernel. Handing over
 * a pointer into someone else's memory is not a way around isolation, which
 * is exactly the confused-deputy case B3 set out to close.
 */

/* 1-4 were the register-IPC entry points and are permanently retired -- see
 * kernel/ipc.h. A program built against the old ABI gets a clean "unknown
 * syscall" rather than whatever took the number. */
#define SYS_CHAN_CALL   5

#define SYS_PRINT      10
#define SYS_PUTNUM     11
#define SYS_PUTCHAR    12
#define SYS_READ_FILE  13
#define SYS_WRITE_FILE 14
#define SYS_UEXIT      20
#define SYS_TICKS      21

static inline long usyscall(long nr, long a1, long a2, long a3) {
    register long r_a0 __asm__("a0") = nr;
    register long r_a1 __asm__("a1") = a1;
    register long r_a2 __asm__("a2") = a2;
    register long r_a3 __asm__("a3") = a3;
    __asm__ __volatile__("ecall"
                         : "+r"(r_a0)
                         : "r"(r_a1), "r"(r_a2), "r"(r_a3)
                         : "memory");
    return r_a0;
}

/* Five-argument form, for calls that need it. Kept separate rather than
 * widening usyscall(): every existing call site would otherwise have to pass
 * two more zeroes, and the three-argument form is what almost all of them
 * want. */
static inline long usyscall5(long nr, long a1, long a2, long a3, long a4, long a5) {
    register long r_a0 __asm__("a0") = nr;
    register long r_a1 __asm__("a1") = a1;
    register long r_a2 __asm__("a2") = a2;
    register long r_a3 __asm__("a3") = a3;
    register long r_a4 __asm__("a4") = a4;
    register long r_a5 __asm__("a5") = a5;
    __asm__ __volatile__("ecall"
                         : "+r"(r_a0)
                         : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a5)
                         : "memory");
    return r_a0;
}

/* Send a request to a named service and receive its reply, over the same
 * copy-always channels the kernel's own servers use (C3). Returns the number
 * of reply bytes, or negative on error -- including when no service of that
 * name is registered.
 *
 * The service is named, not addressed: there is no pointer or pid a program
 * could invent to reach something it was not given. Both buffers are validated
 * against this program's own memory domain and copied by the kernel, so
 * passing an address the program does not own fails rather than reaching. */
static inline long uchan_call(const char *name, const void *req, long req_len,
                              void *resp, long resp_max) {
    return usyscall5(SYS_CHAN_CALL, (long)name, (long)req, req_len,
                     (long)resp, resp_max);
}

static inline void uprint(const char *s) {
    (void)usyscall(SYS_PRINT, (long)s, 0, 0);
}

static inline void uputnum(long n) {
    (void)usyscall(SYS_PUTNUM, n, 0, 0);
}

static inline void uputchar(char c) {
    (void)usyscall(SYS_PUTCHAR, (long)c, 0, 0);
}

static inline long uread_file(const char *path, void *buf, long max_len) {
    return usyscall(SYS_READ_FILE, (long)path, (long)buf, max_len);
}

/* The kernel's preemption tick count. Only the timer interrupt handler
 * advances it, so a change across a stretch of pure user code means a timer
 * interrupt was taken while this program was running. */
static inline long uticks(void) {
    return usyscall(SYS_TICKS, 0, 0, 0);
}

/* Ends the program with `status`. A program may equally just return from
 * _start: the loader plants an exit stub at the return address that performs
 * exactly this call, carrying the return value across. */
static inline void uexit(long status) {
    (void)usyscall(SYS_UEXIT, status, 0, 0);
    for (;;) { } /* not reached; the kernel does not return from SYS_UEXIT */
}

#endif /* LUGALOS_USER_USYS_H */
