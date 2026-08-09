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
 * ABI: a0 carries the call number and the return value, a1..a3 the
 * arguments. That is what arch/riscv/common/trap.c reads.
 *
 * Pointer arguments are validated by the kernel against the calling task's
 * memory domain (kernel/uaccess.c), so passing an address the program does
 * not own returns an error rather than reaching into the kernel. Handing over
 * a pointer into someone else's memory is not a way around isolation, which
 * is exactly the confused-deputy case B3 set out to close.
 */

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
