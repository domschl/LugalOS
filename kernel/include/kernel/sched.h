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

/* M3, plan/phase12_microkernel_migration.md: a handful of static tiers, not
 * a numeric range -- "a handful of tiers... covers everything in the tree
 * today" was the milestone's own scope, and a wider range would be
 * precision this scheduler has no way to use yet (no priority inheritance,
 * no decay, one hart). Higher wins. Assigned once, normally at creation via
 * task_set_priority() (mirroring task_set_domain()'s "attach after create"
 * shape) -- not boosted transiently on wake, so a task's tier describes its
 * *role* (a driver task is always latency-sensitive) rather than its
 * momentary history.
 *
 * TASK_PRIO_NORMAL is every task's default until told otherwise, which is
 * what keeps every pre-M3 caller of task_create()/task_create_sized()
 * unaffected: with nothing yet assigned a different tier, next_runnable()'s
 * tie-break (see kernel/sched.c) reduces to exactly the round-robin order
 * this tree already tests (taskdemo's A1 B1 A2 B2 A3 B3 interleaving). */
#define TASK_PRIO_IDLE       0
#define TASK_PRIO_BACKGROUND 1
#define TASK_PRIO_NORMAL     2
#define TASK_PRIO_INTERRUPT  3

/* M0, plan/phase12_microkernel_migration.md: raised from 8 so that a fully
 * decomposed board persona (one task per driver, per Rule 4) doesn't hit the
 * slot ceiling on its own drivers alone -- chess's persona reached 8 using
 * only its own devices plus the shell, before any of RTC/EEPROM/net existed
 * as tasks. Cost is a static array: (24-8) * sizeof(task_t), well under 1 KB. */
#define MAX_TASKS 24

/* Default kernel stack size per task, in pages, for task_create()'s callers
 * that don't ask for a specific size. 8 KB: the deepest paths here are the
 * Lisp evaluator (bounded at LISP_MAX_EVAL_DEPTH) and the 9P server's
 * re-entrant VFS calls, both of which have modest frames.
 *
 * Not every task needs this much -- a driver task that only scans a keypad
 * or reads an RTC register has nowhere near this call depth. task_create_sized()
 * (M0) lets such a caller ask for fewer pages; task_create() keeps this
 * default so every pre-M0 caller is unaffected. */
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
    /* M3: TASK_PRIO_NORMAL by default (task_create_sized() sets it), raised
     * or lowered only via task_set_priority(). */
    int          priority;
} task_t;

/* Turns the currently-executing boot context into task 0 so that there is
 * always a valid "current task" to switch away from. */
void sched_init(void);

/* Creates a READY task with its own kernel stack of `stack_pages` pages.
 * Returns the pid, or -1 if the table is full, `stack_pages` is 0, or no
 * stack could be allocated. The primitive M4 (plan/phase12_microkernel_migration.md)
 * needs: a driver task that never gets near TASK_STACK_PAGES's depth
 * shouldn't have to pay for it. */
int task_create_sized(const char *name, void (*entry)(void *), void *arg,
                      uint32_t stack_pages);

/* Creates a READY task with the default (TASK_STACK_PAGES) kernel stack.
 * Thin wrapper over task_create_sized() -- every caller from before M0
 * keeps this and is unaffected by it existing. */
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

/* Sets a task's scheduling tier to one of the TASK_PRIO_* constants above
 * (M3). Takes effect at the next call to next_runnable() -- immediately if
 * `pid` is not currently RUNNING, at the next preemption or yield if it is.
 * Returns -1 for an out-of-range pid, an unused slot, or a value outside
 * [TASK_PRIO_IDLE, TASK_PRIO_INTERRUPT]. */
int task_set_priority(int pid, int priority);

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

/* As above, plus how the task ended: its exit status and whether it asked to
 * end rather than being killed by the fault handler (C3). An exit status alone
 * cannot tell those apart -- 0 is both an ordinary return value and what a
 * killed task leaves behind. Both are only meaningful once the task is DEAD. */
bool        sched_task_info_ex(uint32_t index, int *pid, int *state, const char **name,
                               long *exit_status, bool *exited_clean);

/* True once sched_init() has run and switching is possible. Lets code that
 * runs both before and after scheduler bring-up (e.g. driver busy-waits)
 * call sched_yield() unconditionally. */
bool sched_active(void);

#endif /* LUGALOS_KERNEL_SCHED_H */
