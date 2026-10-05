#ifndef LUGALOS_KERNEL_KOBJ_ABI_H
#define LUGALOS_KERNEL_KOBJ_ABI_H

/* The U-mode face of kernel/kobj_sched.h: syscall numbers and conventions
 * (45.3b, plan/phase45_esp32c6.md). Shared by the kernel's dispatcher
 * (kernel/kobj_sys.c) and by U-mode code, which therefore includes nothing but
 * this.
 *
 * Calling convention, as everywhere in this kernel: syscall number in a0,
 * arguments in a1..a5, result in a0. One number per operation, from
 * SYS_KOBJ_BASE up, so a U-mode stub is a single `ecall` with no demultiplexing
 * on the user side.
 *
 * Handles are the kh_t of kobj.h, returned as a positive value; 0 means the
 * create failed. Every other result is a KO_* code (kobj.h: KO_OK 0, KO_BLOCK
 * never escapes the kernel, KO_AGAIN -1, KO_FULL -2, KO_FAIL -3, KO_TIMEOUT -4,
 * KO_DELETED -5), except where a value is the point (an event group's bits).
 *
 * KO_FAIL is also what a handle belonging to another domain, a forged handle,
 * and a pointer outside the caller's domain all produce -- deliberately not
 * distinguishable, so a probing task learns nothing about which it hit.
 *
 * Timeouts are milliseconds; 0 polls and 0xffffffff (KOS_FOREVER) waits
 * without limit. Timer delays are microseconds.
 */

#define SYS_KOBJ_BASE 32

enum {
    KOBJ_OP_SEM_CREATE = 0,    /* (max, init)                       -> handle */
    KOBJ_OP_SEM_TAKE,          /* (h, timeout_ms)                   -> rc */
    KOBJ_OP_SEM_GIVE,          /* (h)                               -> rc */
    KOBJ_OP_SEM_DELETE,        /* (h)                               -> rc */
    KOBJ_OP_MUTEX_CREATE,      /* (recursive)                       -> handle */
    KOBJ_OP_MUTEX_LOCK,        /* (h, timeout_ms)                   -> rc */
    KOBJ_OP_MUTEX_UNLOCK,      /* (h)                               -> rc */
    KOBJ_OP_MUTEX_DELETE,      /* (h)                               -> rc */
    KOBJ_OP_Q_CREATE,          /* (len, item_size)                  -> handle */
    KOBJ_OP_Q_SEND,            /* (h, item*, front, timeout_ms)     -> rc */
    KOBJ_OP_Q_RECV,            /* (h, item*, timeout_ms)            -> rc */
    KOBJ_OP_Q_WAITING,         /* (h)                               -> items queued */
    KOBJ_OP_Q_DELETE,          /* (h)                               -> rc */
    KOBJ_OP_EV_CREATE,         /* ()                                -> handle */
    KOBJ_OP_EV_SET,            /* (h, bits)                         -> bits after (0 on a bad handle) */
    KOBJ_OP_EV_CLEAR,          /* (h, bits)                         -> bits before */
    KOBJ_OP_EV_WAIT,           /* (h, want, flags, timeout_ms, uint32_t *bits) -> rc */
    KOBJ_OP_EV_DELETE,         /* (h)                               -> rc */
    KOBJ_OP_TIMER_SETFN,       /* (key, fn, arg)                    -> rc */
    KOBJ_OP_TIMER_ARM,         /* (key, fn, arg, delay_us, periodic) -> rc */
    KOBJ_OP_TIMER_DISARM,      /* (key)                             -> rc */
    KOBJ_OP_TIMER_DONE,        /* (key)                             -> rc */
    KOBJ_OP_TIMER_WAIT,        /* (uintptr_t out[3] = key,fn,arg; timeout_ms) -> rc */
    KOBJ_OP_TIME_US,           /* (uint64_t *out)                   -> rc */
    KOBJ_OP_CRIT_ENTER,        /* ()  critical section: excludes this domain's other tasks, recursive */
    KOBJ_OP_CRIT_LEAVE,        /* () */
    KOBJ_OP_THREAD_CREATE,     /* (entry, arg, stack_base, stack_size, prio) -> pid, or KO_FAIL.
                                  A new task in the *caller's own domain*: entry must be executable and the
                                  stack readable+writable in it, both validated against the domain's regions.
                                  The entry is called as `void entry(uintptr_t arg)`; returning ends the thread.
                                  prio is a runtime's own number (the radio's pp task is 23 of 25): >= 20 is
                                  the kernel's interrupt tier, >= 10 normal, below that background. */
    KOBJ_OP_THREAD_SELF,       /* ()                                -> this task's pid */
    KOBJ_OP_RANDOM,            /* (buf*, len)                       -> rc  (kernel/random.c) */
    KOBJ_OP_MAC,               /* (type, uint8_t out[6])            -> rc  (the node's MAC; type = IDF's
                                  esp_mac_type_t: 0 STA, 1 AP, 2 BT, 3 ETH -- last octet offset by type) */
    KOBJ_OP_LOG,               /* (level, text*, len)               -> rc  one line, rendered by the runtime */
    KOBJ_OP_COUNT
};

/* KOBJ_OP_EV_WAIT flags. */
#define KOBJ_EV_ALL    1u
#define KOBJ_EV_CLEAR  2u

#define SYS_KOBJ(op) (SYS_KOBJ_BASE + (op))

#endif /* LUGALOS_KERNEL_KOBJ_ABI_H */
