#ifndef LUGALOS_KERNEL_SCHED_H
#define LUGALOS_KERNEL_SCHED_H

#include <stdint.h>
#include <stdbool.h>

/* Cooperative tasks (B2, plan/phase5_distributed_design.md §5.4).
 *
 * Replaces the pre-B2 shim, which was bookkeeping only: task_create()
 * discarded its entry-point argument and sched_yield() just printed a
 * "context switch" line and rotated an index. Nothing ever ran.
 *
 * ## Cooperative first, deliberately
 *
 * Every blocking path in this tree is a busy-wait that assumes nothing else
 * can run -- uart_getc(), virtio_blk_transfer(), p9_link_cat()'s reply wait,
 * the RP2350 I2C/SPI/USB polls. With cooperative scheduling those become
 * explicit yield points and there is no preemption, so no locking has to be
 * designed at the same time as the switch itself. Timer preemption is B6,
 * behind PMP (B3), because a preempted task caught mid-update of a shared
 * global is exactly what hardware enforcement exists to contain.
 *
 * ## Rule 0 (§5.1)
 *
 * One task model, one switch implementation, both builds. task_t carries a
 * `domain` pointer that is NULL everywhere today; B3 fills it in with a PMP
 * region set on NOMMU and B5 with an Sv39 page table on MMU. The NOMMU build
 * does not get a simplified task -- that is the whole point.
 */

#define TASK_UNUSED  0
#define TASK_READY   1
#define TASK_RUNNING 2
#define TASK_BLOCKED 3
#define TASK_DEAD    4

#define MAX_TASKS 8

/* Kernel stack size per task, in pages. 8 KB: the deepest paths here are the
 * Lisp evaluator (bounded at LISP_MAX_EVAL_DEPTH) and the 9P server's
 * re-entrant VFS calls, both of which have modest frames. */
#define TASK_STACK_PAGES 2

#include "kernel/mem_domain.h" /* B3: per-task restriction set */

typedef struct task {
    int          pid;
    int          state;
    uintptr_t    sp;          /* parked stack pointer while not running */
    void        *stack_base;  /* palloc'd kernel stack, NULL for the boot task */
    uint32_t     stack_pages;
    const char  *name;
    /* B3: the task's memory domain, activated by the scheduler on every
     * switch. NULL means unrestricted (kernel tasks). Owned by whoever
     * created the task. */
    mem_domain_t *domain;
    /* How the task ended, for whoever spawned it. A U-mode program that
     * returns normally reaches SYS_UEXIT and sets both; one that faults is
     * terminated by the trap handler and sets neither, so `exit_clean` is
     * what distinguishes "the program returned 0" from "the program was
     * killed" -- statuses alone cannot, since 0 is a perfectly ordinary
     * return value. */
    long         exit_status;
    bool         exit_clean;
} task_t;

/* Turns the currently-executing boot context into task 0 so that there is
 * always a valid "current task" to switch away from. */
void sched_init(void);

/* Creates a READY task with its own kernel stack. Returns the pid, or -1 if
 * the table is full or no stack could be allocated. */
int task_create(const char *name, void (*entry)(void *), void *arg);

/* Round-robin to the next READY task. A no-op when nothing else can run, so
 * it is safe to call from any busy-wait, including before any task exists. */
void sched_yield(void);

/* Marks the calling task DEAD, frees its stack, and yields permanently.
 * Never returns. Called automatically when a task's entry function returns. */
void task_exit(void);

/* Called only by arch/riscv/common/switch.S's trampoline, on a task's first
 * run. Enables interrupts, runs the entry point, and exits the task. */
void task_start(void (*entry)(void *), void *arg);

/* Attaches a memory domain (B3). Takes effect immediately if `pid` is the
 * running task, otherwise at its next scheduling. */
int task_set_domain(int pid, mem_domain_t *domain);

/* True while any live task still references `domain` (C2). The loader uses
 * this rather than a recorded pid to decide a program has finished: pids are
 * reused when a DEAD slot is recycled, so a stale pid can name a different,
 * living task. */
bool sched_domain_in_use(const mem_domain_t *domain);

/* Records how the calling task is about to end. Called by the syscall layer
 * on SYS_UEXIT, i.e. only on a *voluntary* exit -- which is precisely what
 * makes it distinguishable from a task the trap handler killed. */
void task_set_exit_status(long status);

/* One task's state, or TASK_UNUSED for an out-of-range pid. Lets a spawning
 * task wait for its child instead of yielding a fixed number of times and
 * hoping, which is what the U-mode probes in kernel/shell.c did. */
int sched_task_state(int pid);

/* True if `pid` ended by asking to (SYS_UEXIT), writing its status to
 * `*status`. False if it is still alive, or was terminated by a fault. */
bool sched_task_exited_cleanly(int pid, long *status);

/* Blocks/unblocks by pid. A BLOCKED task is skipped by sched_yield(). */
void task_block(void);
int  task_unblock(int pid);

int         sched_current_pid(void);

/* The running task's domain, or NULL if unrestricted. The syscall boundary
 * validates user pointers against it. */
mem_domain_t *sched_current_domain(void);
const char *sched_state_name(int state);
bool        sched_task_info(uint32_t index, int *pid, int *state, const char **name);

/* True once sched_init() has run and switching is possible. Lets code that
 * runs both before and after scheduler bring-up (e.g. driver busy-waits)
 * call sched_yield() unconditionally. */
bool sched_active(void);

#endif /* LUGALOS_KERNEL_SCHED_H */
