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

/* "No task" -- what sched_current_pid() answers on a hart that is executing
 * kernel code but does not own a task slot yet. Distinct from every real pid
 * and, deliberately, the same value locks use for "unowned", so a caller that
 * forgets to check gets a refusal rather than a wrong task. */
#define TASK_NO_PID (-1)

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
    /* When a sleeping task becomes runnable again, in monotonic ms; 0 when it
     * is not sleeping. A BLOCKED task with this set is woken by the scheduler
     * itself rather than by another task calling task_unblock(). */
    uint64_t     wake_at_ms;
    /* Which hart may run this task: -1 for any, otherwise a hart id (X1,
     * plan/phase23_multicore_scheduling.md).
     *
     * X1 needs this for correctness, not tuning, which is why it arrives
     * before X2's driver pinning rather than with it. Almost every task here
     * is hart-agnostic -- its stack came from palloc and means the same thing
     * on either hart. The exceptions are the boot contexts: task 0 runs on
     * the linker's boot stack and each secondary's idle task runs on
     * .stack_secondary. Let another hart pick one of those up and two harts
     * are executing on one stack, which is precisely the failure S6's
     * hand-off rule exists to prevent -- arriving by a different route.
     *
     * X2 extends this to the driver tasks. The field is deliberately general
     * from the start so that doing so is a call to task_set_affinity() and
     * not a second mechanism. */
    int          hart_affinity;
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
/* A task's stack high-water mark and its total size, in bytes (§6,
 * plan/phase15_memory_reclamation.md). 0 for an unused slot or the boot task,
 * whose stack is the linker's and is reported by meminfo.c instead.
 *
 * The figure is a true high-water, not a current depth: task_create_sized()
 * paints the stack, and this returns the distance from the top down to the
 * deepest word that pattern no longer covers. That is what makes a *finished*
 * task's number meaningful, which is the case that matters -- you want to know
 * how deep a driver task got, after it got there. */
uint32_t sched_stack_used(int pid);
uint32_t sched_stack_size(int pid);

/* True when the task's stack has been filled to its deepest word -- which is
 * an overflow, not a tight fit: the scan cannot see how far past the end it
 * went. `ps` marks it, because a silent one is indistinguishable from broken
 * hardware for as long as it takes someone to notice two equal numbers. */
bool sched_stack_full(int pid);

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

/* Sleeps the calling task for `ms`, without being runnable meanwhile.
 *
 * The primitive this scheduler was missing, and its absence was not academic.
 * Watching for something in a scheduler with no timed sleep means spinning on
 * sched_yield(), and a task that is always runnable takes its full share of
 * the round robin: at TASK_PRIO_NORMAL that halved the clock's frame rate and
 * showed as a visible flicker. The workaround was to drop such watchers to
 * TASK_PRIO_BACKGROUND -- but next_runnable() picks the strictly highest
 * priority ready task, and the clock's frame loop is *always* ready, so a
 * BACKGROUND task on that persona never runs again at all.
 *
 * That combination silently disabled the WLAN supervisor: it joined at
 * NORMAL, demoted itself on success, and from that moment could neither
 * notice a dropped link nor rejoin one (found 2026-09-03, after a measurement
 * run ended at the first WLAN drop). Sleeping properly removes the reason to
 * demote anything.
 *
 * If nothing else is runnable this returns to spinning, which is the correct
 * behaviour: the CPU has nothing better to do. */
void task_sleep_ms(uint32_t ms);
int  task_unblock(int pid);

/* The pid of the task this hart is running, or -1 if it is running none.
 *
 * The -1 case is real, and used to be silently answered as 0. A secondary
 * hart executes kernel code from its reset vector until sched_secondary_init()
 * gives it a slot, and before phase 23's identity fix this function reported
 * that hart as task 0 -- the boot task, which is running on a *different*
 * hart at the same instant. Anything that then acted on the answer acted on
 * the wrong task: task_block() blocked the shell, and printk_lock()'s
 * ownership test matched a lock hart 0 was holding and let both harts into
 * the region at once. The same window exists on the primary before
 * sched_init().
 *
 * So callers that index an array with this, or store it as an owner, must
 * check for -1. Callers that only want a stable identity for the executing
 * context -- a lock owner, a re-entrancy test -- want sched_context_id()
 * instead, which is always valid. */
int         sched_current_pid(void);

/* Does this hart currently own a task slot? Equivalent to
 * sched_current_pid() >= 0, spelled out for the code whose real question is
 * "may I block?" rather than "who am I?". A hart with no task must not
 * block: there is nothing to mark BLOCKED and nothing to resume. */
bool        sched_has_task(void);

/* A stable identity for whatever is executing on this hart, valid always --
 * including before sched_init() and on a secondary hart during bring-up.
 *
 * It is the pid where there is one, and MAX_TASKS + hart_id() where there is
 * not, so the two spaces cannot collide and neither can ever be -1. That
 * matters because -1 is the "unowned" sentinel in every lock in this tree:
 * a bring-up hart identifying as -1 would read as *nobody*, and a lock it
 * held would look free to everyone including itself.
 *
 * Use this for lock ownership and re-entrancy. Use sched_current_pid() when
 * the answer must name a task that can be blocked, woken, or indexed. */
int         sched_context_id(void);

/* True once there is a task table and a scheduler running. Code that may run
 * during boot must check this before blocking: before sched_init() there is
 * nothing to block and nothing to wake it. */
bool        sched_is_active(void);

/* Whether this hart has anything runnable, answered WITHOUT the scheduler
 * lock and therefore only approximately. For an idle hart deciding whether
 * to call sched_yield() at all -- see the definition for why asking cheaply
 * matters more than asking exactly. */
bool        sched_peek_runnable(void);

/* The running task's domain, or NULL if unrestricted. The syscall boundary
 * validates user pointers against it. */
mem_domain_t *sched_current_domain(void);
const char *sched_state_name(int state);
bool        sched_task_info(uint32_t index, int *pid, int *state, const char **name);

/* As above, plus how the task ended: its exit status and whether it asked to
 * end rather than being killed by the fault handler (C3). An exit status alone
 * cannot tell those apart -- 0 is both an ordinary return value and what a
 * killed task leaves behind. Both are only meaningful once the task is DEAD.
 *
 * M6: plus whether the task has a memory domain attached at all
 * (task_set_domain() was called on it) -- true for every U-mode driver
 * task once M5 gave it a real one, and for a loaded user program while
 * it runs; false for the kernel task itself and for any task that still
 * runs kernel-mode only (M4.5's own p9srv, still unconverted). This is
 * what makes `ps`'s "Isol" column a fact about the running system rather
 * than a claim: it reads the same field mem_domain_activate() reads. */
bool        sched_task_info_ex(uint32_t index, int *pid, int *state, const char **name,
                               long *exit_status, bool *exited_clean, bool *has_domain);

/* True once sched_init() has run and switching is possible. Lets code that
 * runs both before and after scheduler bring-up (e.g. driver busy-waits)
 * call sched_yield() unconditionally. */
bool sched_active(void);

/* How many times a hart resumed from a context switch *without* holding the
 * scheduler lock its predecessor should have handed it (S6,
 * plan/phase22_smp_locking_foundation.md).
 *
 * Must be zero. It is the canary for the one rule this milestone turns on --
 * that the lock is held across ctx_switch() and released on the incoming
 * stack -- and it is deliberately the inverse of the check §3 originally
 * specified, which would have fired on correct code. Checked by
 * `lockselftest`. */
uint32_t sched_handoff_faults(void);

/* Pins `pid` to `hart` (-1 = any hart). Returns -1 for an out-of-range pid
 * or an unused slot.
 *
 * For a task that is already running. To create one already pinned, use
 * task_create_pinned() or task_create_driver() -- setting the affinity after
 * creation leaves a window in which another hart can claim the task, which
 * X5 observed happening. */
int  task_set_affinity(int pid, int hart);

/* Creates a task already pinned to `hart` (-1 = any), with the affinity in
 * place before it is ever READY. The general form of task_create_driver()
 * below, for callers that need a hart other than the primary -- X5's
 * isolation probes, which have to fault on a chosen hart rather than
 * whichever one the ready queue happens to offer them. */
int  task_create_pinned(const char *name, void (*entry)(void *), void *arg,
                        int hart);

/* A task's hart pinning: -1 for any hart, otherwise the hart it is bound to.
 * -1 for an out-of-range pid or an unused slot. Reported by `ps` so X2's
 * pinning is a fact about the running system rather than a claim about the
 * source -- the same argument phase 12's M6 made for the Isol column. */
int  task_affinity(int pid);

/* Turns the calling secondary hart's boot context into a task, so it has
 * something to switch away from, and marks it pinned here. Returns its pid,
 * or -1 if the table is full. */
int  sched_secondary_init(void);

/* Creates a task pinned to the boot hart (X2,
 * plan/phase23_multicore_scheduling.md).
 *
 * For tasks that own a piece of hardware. A driver task's state is not just
 * "shared data that happens to have one writer" -- it is device registers,
 * a bus claim, an interrupt armed on a particular hart's controller context.
 * Phase 22's S5 audit left several such sites unconverted *specifically*
 * because this pinning exists (the UART wait paths, uart_net's demux,
 * enc28j60's and cyw43's bus flags), so the pinning is load-bearing rather
 * than tidy: relaxing it means going back and converting them first.
 *
 * A helper rather than "call task_set_affinity() after task_create()" so
 * that the next driver cannot half-do it. Creating and pinning are one act;
 * a window in which a driver task is briefly migratable is exactly the sort
 * of thing that works until it doesn't. */
int  task_create_driver(const char *name, void (*entry)(void *), void *arg,
                        uint32_t stack_pages);

#endif /* LUGALOS_KERNEL_SCHED_H */
