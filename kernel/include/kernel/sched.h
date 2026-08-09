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

/* Attaches a memory domain (B3). Takes effect immediately if `pid` is the
 * running task, otherwise at its next scheduling. */
int task_set_domain(int pid, mem_domain_t *domain);

/* Blocks/unblocks by pid. A BLOCKED task is skipped by sched_yield(). */
void task_block(void);
int  task_unblock(int pid);

int         sched_current_pid(void);
const char *sched_state_name(int state);
bool        sched_task_info(uint32_t index, int *pid, int *state, const char **name);

/* True once sched_init() has run and switching is possible. Lets code that
 * runs both before and after scheduler bring-up (e.g. driver busy-waits)
 * call sched_yield() unconditionally. */
bool sched_active(void);

#endif /* LUGALOS_KERNEL_SCHED_H */
