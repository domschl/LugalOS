#include "arch/trap.h"
#include "arch/csr.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/ipc.h"
#include "kernel/chan.h"
#include "kernel/sched.h"
#include "kernel/uaccess.h"
#include "kernel/ticker.h"
#include <stdbool.h>
#include "fs/vfs.h"

#if defined(CONFIG_BOARD_RP2350)
#include "drivers/usb_cdc.h"
#define REG(addr) (*(volatile uint32_t *)(addr))
#endif

void trap_init(void) {
#if defined(CONFIG_BOARD_RP2350)
    /* Enable M-mode External Interrupts (MEIE bit 11 in mie) */
    uintptr_t mie_val;
    __asm__ __volatile__("csrr %0, mie" : "=r"(mie_val));
    mie_val |= (1u << 11);
    __asm__ __volatile__("csrw mie, %0" :: "r"(mie_val));

    /* Enable M-mode Global Interrupts (MIE bit 3 in mstatus) */
    uintptr_t mstatus_val;
    __asm__ __volatile__("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val |= (1u << 3);
    __asm__ __volatile__("csrw mstatus, %0" :: "r"(mstatus_val));
#endif
}

/* See arch/trap.h. Not a general exception-handling mechanism -- deliberately
 * narrow, so it cannot accidentally mask a real fault. */
static volatile bool g_probe_active;
static volatile bool g_probe_faulted;

void arch_probe_begin(void) {
    g_probe_faulted = false;
    g_probe_active = true;
}

bool arch_probe_faulted(void) {
    g_probe_active = false;
    return g_probe_faulted;
}

/* Cause code of the most recent environment call. The hardware picks it by
 * the privilege level the ecall came FROM -- 8 = U-mode, 9 = S-mode,
 * 11 = M-mode -- so it is direct evidence of the level, not an inference from
 * something the kernel set itself.
 *
 * That distinction is the whole reason this exists: a U-mode task making
 * syscalls produces output identical to a kernel task making the same
 * syscalls, so "it printed" proves nothing about the mode transition. The
 * cause code does. */
static uintptr_t g_last_ecall_cause = 0;

uintptr_t arch_last_ecall_cause(void) { return g_last_ecall_cause; }

void trap_handler(trap_frame_t *frame) {
    uintptr_t cause = frame->cause;
    uintptr_t is_interrupt = cause & ((uintptr_t)1 << (__riscv_xlen - 1));
    uintptr_t code = cause & ~((uintptr_t)1 << (__riscv_xlen - 1));

    if (is_interrupt) {
        /* Timer: 7 is machine-mode, 5 is supervisor-mode. This is the
         * preemption tick. */
        if (code == 7 || code == 5) {
            ticker_count_tick();
            /* Rearm FIRST. A RISC-V timer interrupt is level-triggered off
             * mtime >= mtimecmp, so it stays pending until the comparator
             * moves -- returning without rearming does not drop a tick, it
             * re-enters the handler forever. */
            ticker_next();

            /* Switch away from whatever was running. This works through the
             * ordinary cooperative ctx_switch() rather than needing a second,
             * preemption-specific path: the full register state is already
             * saved in the trap frame on this task's kernel stack, so
             * ctx_switch() only has to preserve what a C call would. When
             * something switches back, it returns here, unwinds into the trap
             * vector, and the frame restores the interrupted task exactly.
             *
             * sched_yield() is a no-op when nothing else is runnable, so an
             * idle system just takes the tick and returns. */
            sched_yield();
            return;
        }
        printk("\n[Trap] Interrupt received: code 0x%lx\n", (unsigned long)code);
    } else {
        /* If ecall (Environment Call from U-mode, S-mode, or M-mode) */
        if (code == 8 || code == 9 || code == 11) {
            g_last_ecall_cause = code;
            uintptr_t sys_nr = frame->a0;

            long ret = 0;
            switch (sys_nr) {
                case 0: /* SYS_EXIT / yield */
                    ret = 0;
                    break;
                case SYS_CHAN_CALL: {
                    /* SYS_CHAN_CALL(name, req, req_len, resp, resp_max)
                     *
                     * U-mode's route to a service (C3,
                     * plan/phase6_memory_and_processes.md). Until this existed
                     * a user program could read and write files and print, and
                     * nothing else -- so "extract a kernel subsystem into a
                     * process" had no way for the result to be reachable. That
                     * is why the old register-IPC entry points were deleted
                     * rather than merely tidied: removing four stubs would have
                     * left U-mode exactly as isolated.
                     *
                     * Every buffer crosses the boundary by copy, validated
                     * against the calling task's own domain, exactly as
                     * SYS_READ_FILE does. The endpoint is named rather than
                     * addressed by pointer or pid, so a user program cannot
                     * express a reference to something it was not given.
                     *
                     * The copies are not redundant with the ones chan_call()
                     * already performs: those protect the *endpoint* from the
                     * caller's buffer changing under it, while these are what
                     * stop the kernel dereferencing a user address at all. */
                    char kname[32];
                    static uint8_t kreq[256];
                    static uint8_t kresp[256];
                    uint32_t req_len = (uint32_t)frame->a3;
                    uint32_t resp_max = (uint32_t)frame->a5;

                    if (strncpy_from_user(kname, frame->a1, sizeof(kname)) < 0) {
                        ret = -1;
                        break;
                    }
                    if (req_len > sizeof(kreq)) { ret = -1; break; }
                    if (resp_max > sizeof(kresp)) resp_max = sizeof(kresp);
                    if (req_len && copy_from_user(kreq, frame->a2, req_len) < 0) {
                        ret = -1;
                        break;
                    }

                    chan_endpoint_t *ep = chan_lookup(kname);
                    if (!ep) { ret = -1; break; }

                    int n = chan_call(ep, kreq, req_len, kresp, resp_max);
                    if (n < 0) { ret = n; break; }
                    /* Copied out only after the call succeeded, and only as
                     * many bytes as the service actually produced. */
                    ret = (resp_max && copy_to_user(frame->a4, kresp, (uint32_t)n) < 0)
                              ? -1 : n;
                    break;
                }
                case 10: { /* SYS_PRINT(const char *) */
                    /* Copied in, never dereferenced in place. A U-mode task
                     * cannot read kernel memory, but before this it could ask
                     * the kernel to print it -- the restriction intact and
                     * entirely bypassed. */
                    char kbuf[128];
                    if (strncpy_from_user(kbuf, frame->a1, sizeof(kbuf)) < 0) {
                        ret = -1;
                        break;
                    }
                    printk("%s", kbuf);
                    ret = 0;
                    break;
                }
                case 11: /* SYS_PUTNUM */
                    /* Was printk("%ld", ...): a user program's numeric output
                     * has nothing to do with kernel diagnostics, but printk()
                     * appends everything it emits to the kernel log ring
                     * (kernel/printk.c), so a tight print loop (see
                     * tools/sd_root/prime.c, fib.c) flooded /proc/kmsg with
                     * program output instead of the log staying a kernel
                     * diagnostic stream. console_putc() writes straight to
                     * whichever wire is actually bound as the console,
                     * without touching the log ring. */
                    {
                        long val = (long)frame->a1;
                        if (val < 0) { console_putc('-'); val = -val; }
                        char digits[20];
                        int n = 0;
                        do { digits[n++] = (char)('0' + (val % 10)); val /= 10; } while (val > 0);
                        while (n > 0) console_putc(digits[--n]);
                    }
                    ret = 0;
                    break;
                case 12: /* SYS_PUTCHAR */
                    /* Takes a value, not a pointer, so it is already safe to
                     * call from U-mode. The pointer-taking syscalls below are
                     * NOT -- they still dereference user-supplied addresses
                     * directly, which is what B3's copy-in/copy-out step
                     * exists to fix. Until then, U-mode code must stick to
                     * value-only syscalls.
                     *
                     * console_putc(), not uart_putc(): the latter hard-codes
                     * the physical UART regardless of what the console is
                     * actually bound to (C8 port binding), and also bypassed
                     * the kernel log ring inconsistently with SYS_PUTNUM
                     * above. */
                    console_putc((char)frame->a1);
                    ret = 0;
                    break;
                case 21: /* SYS_TICKS: the preemption tick counter */
                    /* A value, not a pointer, and read-only -- so it is safe
                     * to expose to U-mode as it stands.
                     *
                     * It exists to make U-mode preemptibility testable from
                     * inside a user program: ticker_count_tick() is reached
                     * only from the timer interrupt handler, so a program
                     * that reads this, spins without making a single syscall,
                     * and reads it again has direct evidence of whether a
                     * timer interrupt was taken while *it* was running. With
                     * interrupts masked in U-mode the count cannot move
                     * across that window, however long the spin. */
                    ret = (long)ticker_ticks();
                    break;
                case SYS_UEXIT:
                    /* A U-mode task asking to end. task_exit() switches away
                     * and never returns, so this call does not come back here
                     * and the trap frame on this kernel stack is simply
                     * abandoned along with the task.
                     *
                     * a1 carries the program's return value. Recording it
                     * *here* rather than in task_exit() is what makes it
                     * mean something: this path is only reached by a task
                     * that asked to end, so a status recorded on it can
                     * never be confused with one from a task the fault
                     * handler killed. */
                    task_set_exit_status((long)frame->a1);
                    task_exit();
                    ret = 0; /* unreachable */
                    break;
                case 13: { /* SYS_READ_FILE: vfs_read(path, buf, max_len) */
                    char kpath[128];
                    static char kdata[512]; /* static: too big for the trap stack */
                    uint32_t want = (uint32_t)frame->a3;
                    if (want > sizeof(kdata)) want = sizeof(kdata);
                    if (strncpy_from_user(kpath, frame->a1, sizeof(kpath)) < 0) {
                        ret = -1;
                        break;
                    }
                    int n = vfs_read(kpath, kdata, want);
                    if (n < 0) { ret = n; break; }
                    /* Copied out only after the read succeeded, and only as
                     * many bytes as were actually produced. */
                    ret = (copy_to_user(frame->a2, kdata, (uint32_t)n) < 0) ? -1 : n;
                    break;
                }
                case 14: { /* SYS_WRITE_FILE: vfs_write(path, buf, len) */
                    char kpath[128];
                    static char kdata[512];
                    uint32_t len = (uint32_t)frame->a3;
                    if (len > sizeof(kdata)) { ret = -1; break; }
                    if (strncpy_from_user(kpath, frame->a1, sizeof(kpath)) < 0) {
                        ret = -1;
                        break;
                    }
                    if (copy_from_user(kdata, frame->a2, len) < 0) { ret = -1; break; }
                    ret = vfs_write(kpath, kdata, len);
                    break;
                }
                default:
                    printk("[Syscall] Unknown syscall nr %ld requested\n", (long)sys_nr);
                    ret = -1;
                    break;
            }

            frame->a0 = (uintptr_t)ret;
            /* Advance EPC past 4-byte ecall instruction */
            frame->epc += 4;
            return;
        }

        /* B3: a fault taken *from U-mode* kills that task instead of halting
         * the machine. Containment is the entire point of running a task in
         * U-mode -- a kernel that halts whenever a user task misbehaves has
         * enforcement but no benefit from it.
         *
         * The previous privilege level comes from the saved status word, where
         * the hardware recorded it: MPP is mstatus[12:11], SPP is sstatus[8].
         * Zero means the trap came from U-mode. */
#if defined(CONFIG_MODE_S)
        bool from_user = ((frame->status >> 8) & 1u) == 0;
#else
        bool from_user = ((frame->status >> 11) & 3u) == 0;
#endif
        if (from_user) {
            printk("\n[Trap] User task faulted: cause %lu, epc=0x%lx, addr=0x%lx -- "
                   "terminating the task\n",
                   (unsigned long)code, (unsigned long)frame->epc,
                   (unsigned long)frame->tval);
            task_exit(); /* switches away; never returns */
        }

        /* An illegal instruction during a deliberate hardware probe is an
         * answer, not a failure: record it and step over the instruction.
         * See arch/trap.h for why this is narrowed to cause 2 only. */
        if (g_probe_active && code == 2) {
            g_probe_faulted = true;
            frame->epc += 4; /* CSR instructions have no compressed encoding */
            return;
        }

        /* Fatal exception hang */
        uint32_t inst_val = 0;
        if (frame->epc >= 0x10000000 && frame->epc < 0x20082000) {
            const uint8_t *p = (const uint8_t *)frame->epc;
            inst_val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        printk("\n[Trap Exception] Cause: 0x%lx, epc=0x%lx, tval=0x%lx, inst=0x%08x\n",
               (unsigned long)code, (unsigned long)frame->epc, (unsigned long)frame->tval, (unsigned int)inst_val);
        printk("[Trap Register Dump] a0=0x%lx, a1=0x%lx, sp=0x%lx, ra=0x%lx\n",
               (unsigned long)frame->a0, (unsigned long)frame->a1, (unsigned long)frame->sp, (unsigned long)frame->ra);
        printk("[Fatal] System halted due to unhandled exception.\n");
        while (1) {
            __asm__ __volatile__("wfi");
        }
    }
}
