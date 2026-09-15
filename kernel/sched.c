#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "kernel/meminfo.h"
#include "kernel/mem_domain.h"
#include "kernel/chan.h"
#include "kernel/irq.h"
#include "kernel/lock.h"
#include "kernel/hart.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

/* See kernel/include/kernel/sched.h for the rationale. */

extern void ctx_switch(uintptr_t *old_sp, uintptr_t new_sp);

static void sched_reap(void); /* defined below; used by sched_yield() above it */
static int task_create_full(const char *name, void (*entry)(void *), void *arg,
                            uint32_t stack_pages, int hart); /* defined below */
extern void task_trampoline(void);

static task_t  g_tasks[MAX_TASKS];

/* Which task each hart is running, as an index into g_tasks.
 *
 * S6 (plan/phase22_smp_locking_foundation.md): per-hart and lock-free by
 * construction. "Which task am I running" is not shared state -- only the
 * hart itself ever writes its own slot, and it does so while holding the
 * scheduler lock anyway. Making this an array is what lets the *shared*
 * part shrink to the ready queue, which is the only thing g_sched_lock has
 * to cover. */
static int     g_current[MAX_HARTS];
static bool    g_active;

/* Guards the task table -- the ready queue, task states, and the reap slot.
 *
 * ## The hand-off, which is the whole of this milestone
 *
 * This lock is acquired before a switch is chosen and released **on the
 * incoming stack, by whoever lands there**. It is deliberately held
 * *across* ctx_switch(), which is the opposite of what §3's S6 originally
 * said, and the reason is worth stating where the code is rather than only
 * in the plan:
 *
 * ctx_switch() (arch/riscv/common/switch.S) parks the outgoing task's stack
 * pointer with `REG_S sp, 0(a0)` -- *inside* the call. Release the lock
 * before making that call and there is a window in which the outgoing task
 * is already marked READY, and so claimable by another hart, while its
 * parked sp still holds whatever the previous switch left there. The hart
 * that claims it resumes on a stale stack pointer and two harts then run on
 * one stack. Intermittent, load-dependent, and it destroys the evidence of
 * its own cause.
 *
 * So the outgoing task stays unclaimable until its context is saved. That
 * is Linux's finish_task_switch() shape, and it is not a foreign idea in
 * this file: sched_yield() has always handed the *interrupt flags* across a
 * switch exactly this way -- "restored by whichever task resumes here, from
 * the flags IT saved". The lock now travels the same route as the flags it
 * is paired with.
 *
 * Three places a hart can land, and each must release exactly once:
 *   - sched_yield(), after its ctx_switch() returns
 *   - task_start(), for a task's first run -- it never returns from a
 *     ctx_switch() at all, so it releases with interrupts enabled rather
 *     than from saved flags
 *   - nowhere in task_exit(), which switches away and never comes back;
 *     its successor does the releasing
 *
 * Miss any one of them and the kernel stops at the next acquire. */
static spinlock_t g_sched_lock;

/* Did a resume ever arrive without the lock it should have been handed?
 *
 * The canary for the invariant above, and note it is the *inverse* of the
 * one §3 originally specified: "the lock is never live across ctx_switch()"
 * would now fire on correct code. What must hold is that a hart which has
 * just been resumed is still holding the lock its predecessor took -- which
 * is checkable on one hart today, unlike the race it protects against. */
static uint32_t g_handoff_faults;

/* This hart's current task index. */
static inline int cur(void) { return g_current[hart_id()]; }
static inline void set_cur(int t) { g_current[hart_id()] = t; }

/* Called on every arrival from a switch. */
static inline void handoff_check(void) {
    if (!spin_is_locked(&g_sched_lock)) g_handoff_faults++;
}

uint32_t sched_handoff_faults(void) { return g_handoff_faults; }

/* Deliberately no "currently switching" guard.
 *
 * An earlier version of this file had one, and it silently broke cooperative
 * scheduling: a freshly created task enters at task_trampoline and never
 * returns from ctx_switch(), so it never reached the line that cleared the
 * flag -- its own sched_yield() then saw the flag still set and returned
 * immediately, making the task run to completion instead of yielding. The
 * `taskdemo` interleaving check is what caught it (output was A1 A2 A3 B1 B2
 * B3 rather than A1 B1 A2 B2 A3 B3), which is exactly why that test asserts
 * on ordering rather than on output merely appearing.
 *
 * No guard is needed: between the state updates below and ctx_switch() there
 * is no call that could re-enter sched_yield(), so there is no window to
 * protect. Preemption (B6) changes that and will need real critical
 * sections, not a flag. */

void sched_init(void) {
    memset(g_tasks, 0, sizeof(g_tasks));
    for (int i = 0; i < MAX_TASKS; i++) {
        g_tasks[i].pid = i;
        g_tasks[i].state = TASK_UNUSED;
        g_tasks[i].name = "(unused)";
        g_tasks[i].hart_affinity = -1;   /* any hart, unless pinned */
    }

    /* The boot context becomes task 0. Its stack is the linker-provided boot
     * stack, not a palloc'd one, so stack_base stays NULL and task_exit()
     * knows not to free it. Its sp is filled in by the first ctx_switch(). */
    g_tasks[0].state = TASK_RUNNING;
    g_tasks[0].name = "kernel";
    g_tasks[0].stack_base = NULL;
    /* Pinned to the primary: this task runs on the linker's boot stack, so
     * another hart resuming it would execute on hart 0's stack (X1). */
    g_tasks[0].hart_affinity = 0;
    g_tasks[0].priority = TASK_PRIO_NORMAL;
    /* Only the primary has a task here. Every other hart owns none until its
     * own sched_secondary_init() -- and says so, rather than defaulting to 0
     * and impersonating the boot task on a hart that is not running it. See
     * sched_current_pid()'s comment in sched.h for what that cost. */
    g_current[0] = 0;
    for (int h = 1; h < MAX_HARTS; h++) g_current[h] = TASK_NO_PID;
    spinlock_init(&g_sched_lock);
    g_active = true;

    printk("[Sched] Round-robin scheduler online (max %d tasks).\n", MAX_TASKS);
}

bool sched_active(void) { return g_active; }

int sched_current_pid(void) {
    if (!g_active) return TASK_NO_PID;
    int c = cur();
    return (c >= 0) ? g_tasks[c].pid : TASK_NO_PID;
}

bool sched_has_task(void) { return g_active && cur() >= 0; }

int sched_context_id(void) {
    int pid = sched_current_pid();
    /* Never -1, and never colliding with a pid: see sched.h. The hart id is
     * valid from SETUP_HART_POINTER onward, which is before any C runs. */
    return (pid >= 0) ? pid : (int)MAX_TASKS + (int)hart_id();
}

/* Whether there is a scheduler to block against yet. Boot runs a long way
 * before sched_init(): drivers brought up in that window must not call
 * task_block(), because nothing would ever wake them -- see
 * drivers/uart_rp2350.c's uart_hw_putc(), which hung the whole machine
 * exactly that way on the first printk long enough to fill a 32-byte FIFO. */
bool sched_is_active(void) { return g_active; }

mem_domain_t *sched_current_domain(void) {
    if (!g_active) return NULL;
    int c = cur();
    return (c >= 0) ? g_tasks[c].domain : NULL;
}

const char *sched_state_name(int state) {
    switch (state) {
        case TASK_UNUSED:  return "UNUSED";
        case TASK_READY:   return "READY";
        case TASK_RUNNING: return "RUNNING";
        case TASK_BLOCKED: return "BLOCKED";
        case TASK_DEAD:    return "DEAD";
    }
    return "?";
}

bool sched_task_info(uint32_t index, int *pid, int *state, const char **name) {
    return sched_task_info_ex(index, pid, state, name, NULL, NULL, NULL);
}

/* As above, plus how the task ended (C3).
 *
 * `exited_clean` distinguishes a task that asked to end from one the fault
 * handler killed, which an exit status alone cannot: 0 is a perfectly ordinary
 * return value and also what an uninitialised field holds. Both are only
 * meaningful once the task is DEAD; for a live task the status is reported as
 * 0 and `exited_clean` as false. `has_domain` is M6's own addition: whether
 * this task ever had task_set_domain() called on it, i.e. whether it runs
 * under real hardware-enforced isolation rather than unrestricted kernel
 * privilege -- see this function's own header comment for the fuller
 * reasoning. */
bool sched_task_info_ex(uint32_t index, int *pid, int *state, const char **name,
                        long *exit_status, bool *exited_clean, bool *has_domain) {
    if (index >= MAX_TASKS) return false;
    if (g_tasks[index].state == TASK_UNUSED) return false;
    if (pid)   *pid = g_tasks[index].pid;
    if (state) *state = g_tasks[index].state;
    if (name)  *name = g_tasks[index].name;
    bool dead = (g_tasks[index].state == TASK_DEAD);
    if (exit_status) *exit_status = dead ? g_tasks[index].exit_status : 0;
    if (exited_clean) *exited_clean = dead && g_tasks[index].exit_clean;
    if (has_domain) *has_domain = (g_tasks[index].domain != NULL);
    return true;
}

/* Attaches a memory domain to a task. Separate from task_create() because a
 * task's regions usually depend on resources allocated after it exists (its
 * own user stack, for one). */
int task_set_domain(int pid, mem_domain_t *domain) {
    if (pid < 0 || pid >= MAX_TASKS) return -1;
    if (g_tasks[pid].state == TASK_UNUSED) return -1;
    g_tasks[pid].domain = domain;
    /* If the task is the one running, the change takes effect now rather than
     * at the next switch. */
    if (pid == cur()) return mem_domain_activate(domain);
    return 0;
}

/* Sets a task's scheduling tier (M3). Separate from task_create() for the
 * same reason task_set_domain() is: a caller usually only knows what a task
 * *is* -- a driver, a background server -- once it exists, and this can be
 * called any time after, not just once at birth. Takes effect at the next
 * call to next_runnable(); no immediate re-yield is forced, since the
 * caller may be raising or lowering its *own* priority and forcing a
 * self-switch here would be a surprise side effect of a setter. */
int task_affinity(int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return -1;
    if (g_tasks[pid].state == TASK_UNUSED) return -1;
    return g_tasks[pid].hart_affinity;
}

int task_create_driver(const char *name, void (*entry)(void *), void *arg,
                       uint32_t stack_pages) {
    /* Hart 0, not "the creating hart": drivers are brought up from
     * kernel_main() on the primary, and that is also where their interrupts
     * were enabled (arch_irq_enable() writes the calling hart's PLIC
     * context). Pinning to whoever happened to create the task would be the
     * same answer today and a different one the moment anything is started
     * from elsewhere. */
    return task_create_full(name, entry, arg, stack_pages, 0);
}

int task_set_affinity(int pid, int hart) {
    if (pid < 0 || pid >= MAX_TASKS) return -1;
    if (g_tasks[pid].state == TASK_UNUSED) return -1;
    if (hart >= (int)MAX_HARTS) return -1;
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);
    g_tasks[pid].hart_affinity = hart;
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return 0;
}

/* A secondary hart's boot context becomes a task, the same way sched_init()
 * does it for the primary -- there must always be something to switch away
 * from, and set_cur() must name a real slot before this hart calls
 * sched_yield().
 *
 * TASK_PRIO_IDLE so that any real work outranks it, and pinned to this hart
 * because it runs on .stack_secondary. */
int sched_secondary_init(void) {
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return -1;
    }
    g_tasks[slot].state = TASK_RUNNING;
    g_tasks[slot].name = "idle";
    g_tasks[slot].stack_base = NULL;    /* the linker's, not palloc'd */
    g_tasks[slot].stack_pages = 0;
    g_tasks[slot].priority = TASK_PRIO_IDLE;
    g_tasks[slot].hart_affinity = (int)hart_id();
    g_tasks[slot].domain = NULL;
    set_cur(slot);
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return slot;
}

int task_set_priority(int pid, int priority) {
    if (pid < 0 || pid >= MAX_TASKS) return -1;
    if (g_tasks[pid].state == TASK_UNUSED) return -1;
    if (priority < TASK_PRIO_IDLE || priority > TASK_PRIO_INTERRUPT) return -1;
    g_tasks[pid].priority = priority;
    return 0;
}

/* Is any live task still using this domain? (C2)
 *
 * The loader needs to know when a program has finished so it can return the
 * program's pages, and asking "is that pid dead?" is not sound: task_create()
 * reuses DEAD slots, so a pid recorded earlier may by then belong to an
 * entirely different task that is very much alive. The symptom is a slot that
 * never reaps, which showed up as a steady three-page climb across repeated
 * loads on the MMU build.
 *
 * A domain pointer identifies the slot that owns it and is never recycled
 * while that slot is in use, so it answers the question the loader is
 * actually asking. */
bool sched_domain_in_use(const mem_domain_t *domain) {
    if (!domain) return false;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED || g_tasks[i].state == TASK_DEAD) continue;
        if (g_tasks[i].domain == domain) return true;
    }
    return false;
}

void task_set_exit_status(long status) {
    if (!g_active || !sched_has_task()) return;
    g_tasks[cur()].exit_status = status;
    g_tasks[cur()].exit_clean = true;
}

int sched_task_state(int pid) {
    if (!g_active || pid < 0 || pid >= MAX_TASKS) return TASK_UNUSED;
    return g_tasks[pid].state;
}

bool sched_task_exited_cleanly(int pid, long *status) {
    if (!g_active || pid < 0 || pid >= MAX_TASKS) return false;
    if (g_tasks[pid].state != TASK_DEAD) return false;
    if (!g_tasks[pid].exit_clean) return false;
    if (status) *status = g_tasks[pid].exit_status;
    return true;
}

/* First-run entry for every task, reached from task_trampoline. */
void task_start(void (*entry)(void *), void *arg) {
    /* A first run arrives here instead of returning from ctx_switch(), so it
     * never passes the release that every later resume goes through -- and
     * it is holding the scheduler lock, handed to it by whoever switched to
     * it. Releasing here is not tidiness: miss it and the very next acquire
     * anywhere in the kernel spins forever.
     *
     * With IRQ_ENABLE_BIT rather than saved flags, because a task running
     * for the first time has none. Enabling interrupts here is also what
     * makes a new task preemptible, which is what this line did before S6. */
    handoff_check();
    spin_unlock_irqrestore(&g_sched_lock, IRQ_ENABLE_BIT);
    /* ...nor does a first run pass sched_yield()'s own leading sched_reap()
     * call, which is where every *later* resume frees whatever task_exit()
     * handed off right before switching to it. If task_exit() picks a task
     * that has never run before as `next`, and that task then exits itself
     * before ever reaching its own first sched_yield(), the stack it was
     * handed off is never reclaimed here. Reaping explicitly here closes
     * that gap the same way sched_yield() already does for every other
     * resume. */
    sched_reap();
    entry(arg);

    /* Returning from the body IS a clean exit, and until X5 it was reported
     * as a kill. exit_clean was set only by task_set_exit_status(), which a
     * U-mode program reaches through usys_exit but a kernel task that simply
     * returns never calls -- so every such task showed "killed" in `ps`,
     * next to the tasks the fault handler really did kill. That column is
     * what the isolation suite reads to tell a contained fault from an
     * ordinary exit, so a value it prints for both is worth less than it
     * appears. The status is left at 0: a void body has no result, and 0 is
     * what a task that returns nothing should report.
     *
     * A task killed from the trap handler cannot reach this line -- that
     * task_exit() is called from the fault path and never returns -- so the
     * distinction stays exactly where it belongs.
     *
     * Guarded, so a body that already reported a status through
     * task_set_exit_status() keeps it: overwriting a deliberate 42 with a
     * default 0 on the way out would break exactly the check that reads it
     * (`uargs` in phase 12's C3 suite). */
    if (!g_tasks[cur()].exit_clean) task_set_exit_status(0);
    task_exit();
}

/* The one creation path. `hart` is -1 for "any hart", or the hart to pin to.
 *
 * Pinning is threaded through here rather than applied by the caller
 * afterwards because the affinity has to be in place *before* the task
 * becomes READY. X2's task_create_driver() did apply it afterwards, and
 * sched.h's own comment on that function already described why that is
 * wrong -- "creating and pinning are one act; a window in which a driver
 * task is briefly migratable is exactly the sort of thing that works until
 * it doesn't". The window was real: with a second hart spinning in
 * sched_yield(), it can claim the new task between task_create_sized()
 * returning and task_set_affinity() landing. Found in X5, by pinning a
 * probe to hart 0 and watching it run on hart 1. */
static int task_create_full(const char *name, void (*entry)(void *), void *arg,
                            uint32_t stack_pages, int hart) {
    if (!entry || stack_pages == 0) return -1;
    if (hart >= (int)MAX_HARTS) return -1;

    /* Claiming a slot must be atomic with respect to anything else that scans
     * the table, or two creators could pick the same one -- and since S6
     * "anything else" includes another hart, which irq_save() never covered. */
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) { /* slot 0 is always the boot task */
        if (g_tasks[i].state == TASK_UNUSED || g_tasks[i].state == TASK_DEAD) {
            slot = i;
            g_tasks[i].state = TASK_BLOCKED; /* reserve it before releasing */
            break;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);

    if (slot < 0) {
        /* Ordinary printk(), here and everywhere else in this file that is
         * not a fatal dump.
         *
         * This used to be a careful exception -- these messages run in the
         * caller's task context, outside every lock, so blocking there is a
         * wait rather than a hang -- against a rule that read "nothing
         * printk()s while it is mid-switch, mid-exit, holding g_sched_lock,
         * or in interrupt context".
         *
         * That rule is retired (Y5c, plan/phase31_concurrency_hierarchy.md).
         * printk() appends a record and returns from any context, so no
         * exception is needed and none of these is special. What survives is
         * about *delivery*: the fatal paths below still use printk_critical()
         * because they are followed by a halt, and a record nobody drains is
         * a record nobody reads. */
        printk("[Sched] Task table full; '%s' not created\n", name ? name : "?");
        return -1;
    }

    void *stack = palloc_pages(stack_pages);
    if (!stack) {
        printk("[Sched] Out of memory for '%s' stack (%u pages)\n",
               name ? name : "?", stack_pages);
        g_tasks[slot].state = TASK_UNUSED; /* release the reservation */
        return -1;
    }

    task_t *t = &g_tasks[slot];
    t->pid = slot;
    t->name = name ? name : "unnamed";
    t->stack_base = stack;
    t->stack_pages = stack_pages;
    t->domain = NULL; /* unrestricted until a caller attaches one */
    /* Cleared explicitly: a slot is reused from TASK_DEAD, so a previous
     * occupant's clean exit would otherwise be reported as this task's. */
    t->exit_status = 0;
    /* Same reasoning, and X1 gives it teeth: a slot last used by a task
     * pinned to a hart would silently pin this one too. A new task is
     * hart-agnostic unless this creation asked otherwise. */
    t->hart_affinity = hart;
    t->exit_clean = false;
    t->priority = TASK_PRIO_NORMAL; /* M3: raised/lowered via task_set_priority() */
    t->wake_at_ms = 0;

    /* Paint it before priming, so a high-water mark can be recovered later
     * (§6, plan/phase15_memory_reclamation.md).
     *
     * The boot stack has had this since entry.S started painting it, and
     * reading it is what found that the RP2350 boot path had been running on
     * the wrong stack entirely. Task stacks had no equivalent, so every
     * per-task size in this tree -- TASK_STACK_PAGES, and the 1-page choice
     * each driver task makes -- was a judgement with no way to check it.
     * Now `cat /proc/ps` reports what each actually touched.
     *
     * palloc_pages() already zeroed this; the second pass is what makes
     * "never written" distinguishable from "written, then zeroed". */
    for (uint32_t i = 0; i < stack_pages * (uint32_t)PAGE_SIZE / sizeof(uintptr_t); i++) {
        ((uintptr_t *)stack)[i] = STACK_POISON_WORD;
    }

    /* Prime the stack so the first ctx_switch() into this task "returns" to
     * task_trampoline with s0 = entry and s1 = arg. The frame layout must
     * match arch/riscv/common/switch.S exactly: slot 0 is ra, slots 1..12
     * are s0..s11, and the frame is 16 registers wide for ABI alignment. */
    uintptr_t top = (uintptr_t)stack + (uintptr_t)stack_pages * PAGE_SIZE;
    top &= ~(uintptr_t)15; /* 16-byte aligned, per the RISC-V ABI */
    uintptr_t *frame = (uintptr_t *)top - 16;
    for (int i = 0; i < 16; i++) frame[i] = 0;
    frame[0] = (uintptr_t)task_trampoline; /* ra  */
    frame[1] = (uintptr_t)entry;           /* s0  */
    frame[2] = (uintptr_t)arg;             /* s1  */

    t->sp = (uintptr_t)frame;

    /* Publishing the task, under the lock: the store that makes it READY is
     * what another hart's scan is looking for, so everything that scan will
     * then read -- sp, the domain, and above all hart_affinity -- has to be
     * visible first. The lock's release does that ordering; a plain store
     * here would leave a second hart free to observe READY with a stale
     * affinity, which is the very race this path was restructured to close. */
    uintptr_t pub = spin_lock_irqsave(&g_sched_lock);
    t->state = TASK_READY;
    spin_unlock_irqrestore(&g_sched_lock, pub);

    /* Y5a, plan/phase31_concurrency_hierarchy.md: the stack *address* used to
     * ride along here too, ten times over on a full RP2350 persona. It is
     * derivable and better placed -- /proc/meminfo reports the heap base
     * these are allocated from, in order -- so it goes.
     *
     * The size stays. It is not decoration: tests/runner.py's M0 check reads
     * it to prove task_create_sized() honoured a non-default page count, and
     * the task in question has already exited by the time /proc/ps could be
     * asked. What cannot be recovered afterwards is the *sequence*, so the
     * line itself stays too: if boot stops, the last one of these names the
     * task that was being created. */
    printk("[Sched] Created task #%d '%s' (%u KB)\n",
           slot, t->name, (stack_pages * (uint32_t)PAGE_SIZE) / 1024);
    return slot;
}

int task_create_sized(const char *name, void (*entry)(void *), void *arg,
                      uint32_t stack_pages) {
    return task_create_full(name, entry, arg, stack_pages, -1);
}

int task_create(const char *name, void (*entry)(void *), void *arg) {
    return task_create_full(name, entry, arg, TASK_STACK_PAGES, -1);
}

int task_create_pinned(const char *name, void (*entry)(void *), void *arg,
                       int hart) {
    return task_create_full(name, entry, arg, TASK_STACK_PAGES, hart);
}

/* Highest-priority runnable task, or -1 if none (M3,
 * plan/phase12_microkernel_migration.md). The current task counts as
 * runnable only if it is still RUNNING -- so a task that just blocked or
 * exited is never picked again here.
 *
 * One linear scan from `from`, tracking the best candidate seen so far and
 * replacing it only on a *strictly* higher priority -- which is what makes
 * this collapse to exactly the pre-M3 round-robin scan whenever every ready
 * task shares one tier (equal priority never replaces the earlier find, so
 * the first READY task found after `from` wins, same as before). */
static int next_runnable(int from) {
    int chosen = -1;
    int chosen_prio = -1;
    uint64_t now = 0;
    bool have_now = false;
    for (int step = 1; step <= MAX_TASKS; step++) {
        int i = (from + step) % MAX_TASKS;
        /* A sleeper whose time has come is made ready here, in the same scan
         * that would otherwise skip it. The clock is read at most once per
         * scan and only when some task is actually sleeping, so a system with
         * no sleepers pays nothing. */
        if (g_tasks[i].state == TASK_BLOCKED && g_tasks[i].wake_at_ms) {
            if (!have_now) { now = time_get_ms(); have_now = true; }
            if (now >= g_tasks[i].wake_at_ms) {
                g_tasks[i].wake_at_ms = 0;
                g_tasks[i].state = TASK_READY;
            }
        }
        if (g_tasks[i].state != TASK_READY) continue;
        /* X1: a task pinned elsewhere is not runnable here. */
        if (g_tasks[i].hart_affinity >= 0 &&
            g_tasks[i].hart_affinity != (int)hart_id()) continue;
        if (g_tasks[i].priority > chosen_prio) {
            chosen = i;
            chosen_prio = g_tasks[i].priority;
        }
    }
    return chosen;
}

/* Is there anything this hart could run, without taking the lock to ask?
 *
 * Deliberately racy, and safe because of what it is used for: an idle hart
 * deciding whether to bother calling sched_yield(). A false negative costs
 * one more turn round the idle loop; a false positive costs one sched_yield()
 * that finds nothing. Neither can corrupt anything, because the real decision
 * is still made under g_sched_lock inside sched_yield().
 *
 * It exists because the honest version was worse. A secondary hart idling as
 * `for(;;) sched_yield();` takes g_sched_lock on every iteration, and the
 * primary needs that same lock for every context switch, task_block() and
 * task_unblock(). On QEMU the two harts are host threads that the OS
 * timeslices, so nothing shows; on RP2350 they share one bus, the primary
 * loses, and the machine stops. Reading plain words instead of taking an
 * atomic is what makes an idle secondary cheap enough to not starve the
 * hart doing the work. */
bool sched_peek_runnable(void) {
    if (!g_active) return false;
    int h = (int)hart_id();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state != TASK_READY) continue;
        int aff = g_tasks[i].hart_affinity;
        if (aff >= 0 && aff != h) continue;
        return true;
    }
    return false;
}

uint32_t sched_stack_used(int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return 0;
    task_t *t = &g_tasks[pid];
    if (t->state == TASK_UNUSED || !t->stack_base) return 0;

    /* Scan up from the base: the first word still carrying the pattern is the
     * deepest point never written, so everything above it has been used. Same
     * direction and reasoning as meminfo.c's boot-stack scan. */
    const uintptr_t *p = (const uintptr_t *)t->stack_base;
    const uintptr_t *top = p + (t->stack_pages * (uint32_t)PAGE_SIZE) / sizeof(uintptr_t);
    while (p < top && *p == STACK_POISON_WORD) p++;
    return (uint32_t)((uintptr_t)top - (uintptr_t)p);
}

/* Has this task filled its stack completely?
 *
 * The high-water scan cannot tell "used every byte" from "used every byte and
 * kept going" -- the poison below the base belongs to whatever is there. So
 * the honest report is: the deepest word is no longer poison, therefore this
 * stack is full and anything under it is suspect.
 *
 * Worth its own function because the alternative is a reader noticing that
 * two numbers in a `ps` column happen to be equal. One did not, for hours,
 * while a task with an overflowing stack presented as broken hardware. */
bool sched_stack_full(int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return false;
    task_t *t = &g_tasks[pid];
    if (t->state == TASK_UNUSED || !t->stack_base) return false;
    return *(const uintptr_t *)t->stack_base != STACK_POISON_WORD;
}

uint32_t sched_stack_size(int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return 0;
    task_t *t = &g_tasks[pid];
    if (t->state == TASK_UNUSED || !t->stack_base) return 0;
    return t->stack_pages * (uint32_t)PAGE_SIZE;
}

/* One slot **per hart**, declared early for sched_check_incoming().
 *
 * Per-hart rather than global, and that is what makes the slot's invariant
 * structural instead of defended (phase 31 Y1/F2 -- see the fuller argument
 * at task_exit()'s claim). A dead task's successor always runs on the same
 * hart the task died on, and every path that starts running a task on a hart
 * reaps first: sched_yield() calls sched_reap() before it picks anything, and
 * again after ctx_switch() returns, and task_start() calls it on a first run.
 * So this hart's slot is always empty by the time a task running on this hart
 * can reach task_exit(). A single global slot had no such property -- a task
 * exiting on hart 1 could find hart 0's hand-off still pending. */
static void    *g_reap_stack[MAX_HARTS];
static uint32_t g_reap_pages[MAX_HARTS];

/* Debug guard for phase 27 E4's priostress fault: does the task we are about
 * to resume have a parked sp that could possibly be a context frame?
 *
 * ctx_switch() restores ra from the first word at the incoming sp. A task
 * that has ever been switched away from has a return address into this
 * file's .text there; a task that has never run has task_trampoline. Zero is
 * neither, and jumping to it produces a fatal trap with every scheduler
 * register already destroyed. Halting here instead keeps the table intact
 * and says who was switching to whom. */
/* Runs with g_sched_lock HELD, which is why every line below is
 * printk_critical() -- though the reason changed under it (Y5c,
 * plan/phase31_concurrency_hierarchy.md).
 *
 * It used to be that printk() here would block while holding the lock the
 * task it waits for needs: a deadlock rather than a slow dump. printk() no
 * longer blocks anywhere, so that is gone. What remains is delivery: this is
 * a fatal dump, the next thing that happens is a halt, and a record in the
 * log ring is read by klogd on a board that is not going to run klogd again.
 * printk_critical() is on the wire before the halt; printk() is not.
 *
 * Since phase 31 Y2 a *cprintf()* here would also be caught -- it reaches the
 * console through chan_call(), which refuses outright when a spinlock is
 * held. That turns this from a rule someone has to have read into one the
 * kernel states at the moment it is broken, which matters here more than
 * most: this function exists to explain a crash, and the version of it that
 * deadlocks explains nothing. */
/* Defined below, and used by the guard above it. Not in a header on purpose:
 * it is a fatal-path dump, and the two callers outside this file
 * (arch/riscv/common/trap.c) already declare it locally for the same reason. */
void sched_dump_table(void);

static void sched_check_incoming(int prev, int next) {
    /* The kernel's own .text, and nothing else -- see the block that defines
     * these in each of the four linker scripts. _ram_start/_ram_end bound
     * where a stack can possibly live, on all four. */
    extern char _ktext_lo[];
    extern char _ktext_hi[];
    extern char _ram_start[];
    extern char _ram_end[];
    uintptr_t sp = g_tasks[next].sp;

    /* Validate the pointer before following it. A guard that faults while
     * checking for a corrupt frame is worse than no guard: the second fault
     * lands in the handler reporting the first, and the dump this exists to
     * produce is what gets lost. The old code dereferenced any non-zero sp,
     * which is safe only for the failure it was written for -- sp intact, ra
     * zeroed -- and not for the one where sp itself is the garbage. */
    if (sp < (uintptr_t)_ram_start || sp + sizeof(uintptr_t) > (uintptr_t)_ram_end) {
        printk_critical("\n[Sched BUG] switching %d '%s' -> %d '%s': parked sp=0x%lx"
                        " is not in RAM (0x%lx..0x%lx); not dereferencing it\n",
               prev, g_tasks[prev].name, next, g_tasks[next].name, (unsigned long)sp,
               (unsigned long)(uintptr_t)_ram_start, (unsigned long)(uintptr_t)_ram_end);
        sched_dump_table();
        printk_critical("[Sched BUG] halting with the table intact.\n");
        for (;;) { __asm__ __volatile__("wfi"); }
    }

    uintptr_t ra = *(const uintptr_t *)sp;
    /* A full text-range test, which this used to say was "worth doing on its
     * own, not as a side effect of a debug guard". It has now been done on its
     * own, because testing for zero specifically was not enough.
     *
     * Zero was the failure phase 27 E4 left behind, so zero was what this
     * checked. The failure that outlived it is a parked ra pointing into
     * .rodata: non-zero, so the guard waved it through, and the hart then ran
     * .rodata as instructions until something decoded as illegal. What
     * reached the log was a fatal trap whose epc was ASCII, several frames
     * after the scheduler state that explained it -- and taken on the
     * handed-off g_sched_lock, so the machine was wedged as well as
     * confused. (Which object it landed in was never established; see
     * plan/open_issues.md for why the obvious answer was the wrong one.)
     *
     * The window is exact rather than defensive: every caller of ctx_switch()
     * is in this file, task_trampoline is in switch.S's .text, and the linker
     * scripts ASSERT all three are inside it. So a parked ra outside .text is
     * not "suspicious", it is impossible -- and if it happens anyway, the
     * frame being restored is not a frame. Halting here costs a wild jump its
     * one chance to destroy the evidence of where it came from. */
    if (ra >= (uintptr_t)_ktext_lo && ra < (uintptr_t)_ktext_hi) return;

    printk_critical("\n[Sched BUG] switching %d '%s' -> %d '%s': parked sp=0x%lx has ra=0x%lx"
                    " (outside .text 0x%lx..0x%lx)\n",
           prev, g_tasks[prev].name, next, g_tasks[next].name,
           (unsigned long)sp, (unsigned long)ra,
           (unsigned long)(uintptr_t)_ktext_lo, (unsigned long)(uintptr_t)_ktext_hi);
    printk_critical("[Sched BUG] next: state=%s stack=0x%lx pages=%u prio=%d aff=%d\n",
           sched_state_name(g_tasks[next].state),
           (unsigned long)(uintptr_t)g_tasks[next].stack_base,
           (unsigned)g_tasks[next].stack_pages,
           g_tasks[next].priority, g_tasks[next].hart_affinity);
    printk_critical("[Sched BUG] prev: state=%s stack=0x%lx pages=%u   reap=0x%lx pages=%u\n",
           sched_state_name(g_tasks[prev].state),
           (unsigned long)(uintptr_t)g_tasks[prev].stack_base,
           (unsigned)g_tasks[prev].stack_pages,
           (unsigned long)(uintptr_t)g_reap_stack[hart_id()],
           (unsigned)g_reap_pages[hart_id()]);
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        printk_critical("[Sched BUG]   #%d '%s' %s sp=0x%lx stack=0x%lx+%uP\n",
               g_tasks[i].pid, g_tasks[i].name, sched_state_name(g_tasks[i].state),
               (unsigned long)g_tasks[i].sp,
               (unsigned long)(uintptr_t)g_tasks[i].stack_base,
               (unsigned)g_tasks[i].stack_pages);
    }
    /* The frame ctx_switch() was about to restore from. Sixteen words at the
     * parked sp: slot 0 is ra, 1..12 are s0..s11. Zeros here mean the frame
     * was overwritten after it was parked; plausible-looking garbage means
     * the sp itself is wrong. */
    if (sp) {
        const uintptr_t *w = (const uintptr_t *)sp;
        for (int r = 0; r < 4; r++) {
            printk_critical("[Sched BUG]   frame 0x%lx: %08lx %08lx %08lx %08lx\n",
                            (unsigned long)(sp + (uintptr_t)r * 4 * sizeof(uintptr_t)),
                            (unsigned long)w[r*4+0], (unsigned long)w[r*4+1],
                            (unsigned long)w[r*4+2], (unsigned long)w[r*4+3]);
        }
    }
    /* And how much stack each task has actually used, because "the frame is
     * gone" and "the stack was too small" produce the same dump otherwise.
     * sched_stack_full() had existed since phase 15 with no callers at all,
     * while its own comment recorded that an overflowing stack had once
     * "presented as broken hardware" -- exactly the shape of E7's hunt, which
     * is why it is wired up here now.
     *
     * Read FULL with care, which E7 learned the hard way: it only means the
     * deepest word is no longer poison, and a word that was *zeroed* satisfies
     * that as well as one that was pushed. It reported the console task as
     * having filled 4096 of 4096 bytes when the task was 460 bytes deep and
     * the rest was corruption. */
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED || !g_tasks[i].stack_base) continue;
        printk_critical("[Sched BUG]   #%d '%s' used %u of %u%s\n",
                        g_tasks[i].pid, g_tasks[i].name,
                        sched_stack_used(g_tasks[i].pid), sched_stack_size(g_tasks[i].pid),
                        sched_stack_full(g_tasks[i].pid) ? "  <-- FULL" : "");
    }
    printk_critical("[Sched BUG] halting with the table intact.\n");
    for (;;) { __asm__ __volatile__("wfi"); }
}

/* The whole table, for a fatal path that has already lost the registers.
 * Phase 27 E4 debug aid; see sched_check_incoming() above. */
void sched_dump_table(void) {
    printk_critical("[Sched Table] reap[hart %u]=0x%lx pages=%u handoff_faults=%u\n",
           (unsigned)hart_id(),
           (unsigned long)(uintptr_t)g_reap_stack[hart_id()],
           (unsigned)g_reap_pages[hart_id()],
           (unsigned)g_handoff_faults);
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        printk_critical("[Sched Table]   #%d '%s' %s sp=0x%lx stack=0x%lx+%uP prio=%d\n",
               g_tasks[i].pid, g_tasks[i].name, sched_state_name(g_tasks[i].state),
               (unsigned long)g_tasks[i].sp,
               (unsigned long)(uintptr_t)g_tasks[i].stack_base,
               (unsigned)g_tasks[i].stack_pages, g_tasks[i].priority);
    }
}

void sched_yield(void) {
    if (!g_active) return;

    /* Nothing to switch away from on a hart that owns no task -- and, before
     * the identity fix, `prev` would have been 0 here and this hart would
     * have parked the boot task's stack pointer over the boot task's own.
     * A bring-up hart that yields simply keeps going. */
    if (!sched_has_task()) return;

    /* X7: the point at which core 1 notices it must get out of the XIP
     * window before core 0 turns it off. One load of a .bss word that is
     * zero except during a flash write, and a no-op on every target but an
     * SMP RP2350. Here rather than in the idle loop because a task that
     * computes without yielding still arrives via the preemption tick. */
    smp_flash_park_check();

    /* Free any stack left by a task that exited before we were resumed. Safe
     * here and nowhere earlier: we are demonstrably not running on it. */
    sched_reap();

    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);

    int prev = cur();
    int next = next_runnable(prev);
    if (next < 0) { spin_unlock_irqrestore(&g_sched_lock, flags); return; }

    /* next_runnable() can hand back the *calling* task, and switching to
     * ourselves would be a silent corruption rather than a no-op.
     *
     * That scan does two jobs: it finds a READY task, and on the way it wakes
     * any sleeper whose wake_at_ms has passed. The caller of sched_yield() is
     * usually RUNNING and therefore skipped -- but a task inside
     * task_sleep_ms() reaches here BLOCKED with a deadline set, so if that
     * deadline has expired the scan wakes it, finds it READY, and returns it.
     *
     * ctx_switch(&X.sp, X.sp) is not a no-op: `next`'s sp is read into a1
     * before the call, then the call stores the *current* sp over X.sp and
     * restores registers from the value a1 held -- an older, stale frame of
     * this same task. The result is a return into a dead stack, which shows
     * up as an instruction access fault with a nonsense `ra`.
     *
     * Pre-existing and not an SMP bug: on one hart it needs a sleeping task
     * whose deadline expired while nothing else was READY, which the shell
     * and p9srv normally prevent by being runnable. Phase 23's X1 made it
     * routine, because a second hart runs out of work far more often. Found
     * 2026-09-04 by a double-schedule detector reporting a task as already
     * running on the very hart that was about to switch to it. */
    if (next == prev) {
        g_tasks[prev].state = TASK_RUNNING;   /* the scan had marked us READY */
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return;
    }

    if (g_tasks[prev].state == TASK_RUNNING) g_tasks[prev].state = TASK_READY;
    g_tasks[next].state = TASK_RUNNING;
    set_cur(next);

    /* B3: install the incoming task's memory domain. Done here rather than in
     * the U-mode entry path because a task's restrictions must be re-applied
     * every time it is *resumed*, not only when it first enters U-mode --
     * otherwise a task would run under whatever domain the previously running
     * task left behind. NULL (kernel tasks) clears all restriction. */
    (void)mem_domain_activate(g_tasks[next].domain);

    /* Returns once something switches back to `prev` -- i.e. to us.
     *
     * Interrupts stay masked across the switch itself and are restored by
     * whichever task resumes here, from the flags IT saved. The incoming task
     * does the same for us. Since S6 the scheduler lock travels the same
     * route: we are still holding it as we make this call, the task we
     * switch to releases it once it is on its own stack, and whoever
     * eventually resumes *us* has handed it back. That is what keeps `prev`
     * unclaimable until ctx_switch() has finished parking its sp. */
    sched_check_incoming(prev, next);
    ctx_switch(&g_tasks[prev].sp, g_tasks[next].sp);

    /* Resumed. We are on our own stack again and should have been handed the
     * lock; releasing it here is the other half of the hand-off. */
    handoff_check();
    spin_unlock_irqrestore(&g_sched_lock, flags);
    sched_reap();
}

void task_block(void) {
    if (!g_active) return;

    /* A hart with no task cannot block. Before phase 23's identity fix this
     * did not return -- cur() answered 0 on a bring-up hart, so this marked
     * the *boot task* BLOCKED, on a hart that was not running it, and nothing
     * would ever wake it. That is a wedged shell, produced by a secondary
     * hart printing one line too early.
     *
     * Returning is the honest answer rather than a panic: the caller wanted
     * to wait, and a hart with no task waits by spinning. Every caller in
     * this tree that can reach here from bring-up polls when it cannot
     * block. */
    if (!sched_has_task()) return;

    /* Y2: a spinlock_t held here is the cross-mechanism deadlock phase 31
     * exists to catch -- the holder cannot run to release it, and on one hart
     * that is fatal immediately. Reported, not refused: a caller that wanted
     * to wait has nothing useful to do with a refusal, and the diagnostic is
     * what turns the hang into a named one. */
    (void)lock_check_may_block("blocked in task_block()");

    /* Marked BLOCKED under the lock, then released before yielding --
     * sched_yield() takes it again, and spinlock_t is not re-entrant.
     *
     * The gap between the two is harmless, and worth saying why rather than
     * leaving a reader to wonder: BLOCKED means "do not pick me", so a hart
     * scanning the table in that window declines to schedule a task that is
     * still running here. The state this sets is the *absence* of a claim,
     * which is the one transition that cannot race into a double-schedule. */
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);
    g_tasks[cur()].state = TASK_BLOCKED;
    spin_unlock_irqrestore(&g_sched_lock, flags);
    sched_yield();
}

int task_unblock(int pid) {
    if (!g_active || pid < 0 || pid >= MAX_TASKS) return -1;
    /* Test and transition under one lock: the check that it is BLOCKED and
     * the store that makes it READY are the read-then-act pair a second
     * hart can invalidate between, which would wake a task twice or wake
     * one that had already gone. */
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);
    if (g_tasks[pid].state != TASK_BLOCKED) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return -1;
    }
    g_tasks[pid].wake_at_ms = 0;   /* an explicit wake outranks a deadline */
    g_tasks[pid].state = TASK_READY;
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return 0;
}

void task_sleep_ms(uint32_t ms) {
    uint64_t end = time_get_ms() + ms;
    /* Same for a hart with no task as for no scheduler at all: there is no
     * wake_at_ms slot to write and nothing to block, so spin out the time. */
    if (!g_active || !sched_has_task()) {
        while (time_get_ms() < end) { }
        return;
    }
    /* Re-blocks around the deadline test rather than trusting one wake: if
     * nothing else is runnable, task_block() returns immediately and the loop
     * simply spins out the remaining time, which is what should happen when
     * the CPU has nothing else to do. */
    while (time_get_ms() < end) {
        uintptr_t f = spin_lock_irqsave(&g_sched_lock);
        g_tasks[cur()].wake_at_ms = end;
        spin_unlock_irqrestore(&g_sched_lock, f);
        task_block();
    }
    {
        uintptr_t f = spin_lock_irqsave(&g_sched_lock);
        g_tasks[cur()].wake_at_ms = 0;
        spin_unlock_irqrestore(&g_sched_lock, f);
    }
}

/* A dead task's stack, waiting to be freed by whoever runs next.
 *
 * task_exit() used to free its own stack and then switch away, which was safe
 * only because cooperative scheduling meant nothing could allocate and reuse
 * those pages in the window between the free and the ctx_switch -- a window in
 * which the task is still executing on them. Preemption makes that window
 * real: a timer interrupt would push a trap frame onto a stack that has
 * already been handed back to the allocator.
 *
 * One slot suffices, but not for the reason this comment used to give.
 *
 * It said "a task can only exit while running, and the next task reaps before
 * anything else can exit". The second clause is false (phase 31 Y1):
 * task_exit() runs with interrupts *enabled* until it takes g_sched_lock, and
 * printk_critical() sits in that window -- a bounded spin on the UART FIFO,
 * which at 115200 baud is milliseconds for one line, far longer than a 10 ms
 * tick needs to land. Two tasks really can be mid-exit at once.
 *
 * What actually makes one slot enough is maintained rather than assumed:
 * task_exit() frees whatever it finds here before claiming the slot. Two
 * exiting tasks serialise on g_sched_lock, and the one that arrives second is
 * provably not running on the first's stack -- the first reached ctx_switch()
 * before the lock was released, so whoever released it is not the first. */

/* Does a range about to be handed back to the allocator overlap a stack some
 * live task is standing on? Phase 27 E4/E7 guard.
 *
 * A free of the wrong range is invisible at the moment it happens --
 * palloc_free() only clears bitmap bits, it does not touch the memory -- and
 * only becomes a fault later, when palloc_pages() hands those pages to
 * someone else and **zeroes them** under a running task's feet. By then the
 * evidence is a context frame full of zeros and no trace of who freed what.
 *
 * Both free sites call this, which is the point: E4 put the check only in
 * sched_reap() and left task_exit()'s own inline free unguarded, so exactly
 * half the ways a stack can be freed were covered. */
static void sched_check_free_range(void *stack, uint32_t pages, const char *who) {
    if (!stack) return;
    uintptr_t lo = (uintptr_t)stack;
    uintptr_t hi = lo + (uintptr_t)pages * PAGE_SIZE;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED || g_tasks[i].state == TASK_DEAD) continue;
        if (!g_tasks[i].stack_base) continue;
        uintptr_t tlo = (uintptr_t)g_tasks[i].stack_base;
        uintptr_t thi = tlo + (uintptr_t)g_tasks[i].stack_pages * PAGE_SIZE;
        if (lo < thi && tlo < hi) {
            printk_critical("\n[Sched BUG] %s: freeing 0x%lx..0x%lx overlaps live "
                            "#%d '%s' stack 0x%lx..0x%lx (state=%s)\n",
                            who, (unsigned long)lo, (unsigned long)hi,
                            g_tasks[i].pid, g_tasks[i].name,
                            (unsigned long)tlo, (unsigned long)thi,
                            sched_state_name(g_tasks[i].state));
            printk_critical("[Sched BUG] halting before the free.\n");
            for (;;) { __asm__ __volatile__("wfi"); }
        }
    }
}

/* The allocation-side counterpart, declared in kernel/include/kernel/palloc.h
 * (which carries the reasoning). Called by palloc_pages() with the range it is
 * about to zero, before it zeroes it.
 *
 * Not static, and not under the scheduler lock. The read is a scan of fields
 * that only the scheduler writes, and this path ends in a halt rather than a
 * recovery, so a torn read costs nothing that matters.
 *
 * It used to say something stronger -- that taking g_sched_lock here would
 * create the palloc->sched ordering sched_reap() avoids -- and that stopped
 * being true with phase 31's F2. There is no sched->palloc ordering left
 * anywhere: task_exit() no longer frees under the lock, and task_create()
 * calls palloc_pages() having already released it. So this *could* now take
 * g_sched_lock and scan the table properly, which would make a diagnostic
 * that currently admits to racing exact instead.
 *
 * Deliberately not done here. It is a change to a guard that is working, it
 * belongs with whatever else wants that ordering settled, and the argument
 * for it should be made where lock levels are decided rather than smuggled in
 * beside an unrelated fix. Recorded so the option is not lost. */
void palloc_report_alloc(void *p, uint32_t pages, void *caller_ra) {
    if (!p) return;
    uintptr_t lo = (uintptr_t)p;
    uintptr_t hi = lo + (uintptr_t)pages * PAGE_SIZE;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED || g_tasks[i].state == TASK_DEAD) continue;
        if (!g_tasks[i].stack_base) continue;
        uintptr_t tlo = (uintptr_t)g_tasks[i].stack_base;
        uintptr_t thi = tlo + (uintptr_t)g_tasks[i].stack_pages * PAGE_SIZE;
        if (lo < thi && tlo < hi) {
            printk_critical("\n[PAlloc BUG] handing out 0x%lx..0x%lx (%u pages) "
                            "which overlaps live #%d '%s' stack 0x%lx..0x%lx "
                            "(state=%s sp=0x%lx)\n",
                            (unsigned long)lo, (unsigned long)hi, pages,
                            g_tasks[i].pid, g_tasks[i].name,
                            (unsigned long)tlo, (unsigned long)thi,
                            sched_state_name(g_tasks[i].state),
                            (unsigned long)g_tasks[i].sp);
            printk_critical("[PAlloc BUG] requested from ra=0x%lx\n",
                            (unsigned long)(uintptr_t)caller_ra);
            sched_dump_table();
            printk_critical("[PAlloc BUG] halting before the zeroing.\n");
            for (;;) { __asm__ __volatile__("wfi"); }
        }
    }
}

static void sched_reap(void) {
    /* The slot is claimed under the scheduler lock and freed outside it.
     * That split is not stylistic: palloc_free() takes palloc's own lock
     * (S4), and nesting the two would create a lock ordering this kernel
     * has no reason to have. Taking the pointer out first makes the free a
     * purely local operation. */
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);
    uint32_t h = hart_id();
    void *stack = g_reap_stack[h];
    uint32_t pages = g_reap_pages[h];
    g_reap_stack[h] = NULL;
    g_reap_pages[h] = 0;
    spin_unlock_irqrestore(&g_sched_lock, flags);

    if (!stack) return;
    sched_check_free_range(stack, pages, "reap");

    palloc_free(stack, pages);
}

void task_exit(void) {
    /* A hart with no task has nothing to exit; parking is all that is left,
     * and is what the !g_active case has always done. */
    if (!g_active || !sched_has_task()) { for (;;) { } }

    task_t *t = &g_tasks[cur()];

    /* printk(), and this line is a small monument to Y5c
     * (plan/phase31_concurrency_hierarchy.md).
     *
     * It was printk_critical() for a concrete reason: printk() reached the
     * console through uart_flush() -> chan_call() -> task_block(), so a task
     * announcing its own death switched away in the middle of dying, and a
     * second task could enter task_exit() behind it -- breaking the one-slot
     * reaper's assumption two comments below, and faulting. Found on the
     * ESP32-P4 the day preemption started working there (phase 27 E4).
     * Nothing here may block until the switch below has happened, and that
     * has not changed.
     *
     * What changed is printk(). It appends a record to the log ring under a
     * leaf spinlock and returns; it reaches no blocking primitive from any
     * context. So the workaround is no longer needed, and dropping it fixes
     * something in its own right: printk_critical() writes the UART directly,
     * outside every ordering the console has, and this message spliced itself
     * into the middle of a U-mode program's output. The QEMU suite's C3 check
     * caught exactly that. */
    printk("[Sched] Task #%d '%s' exited\n", t->pid, t->name);

    /* M5 Phase 2: if this task owned a chan endpoint with a request
     * pending, its caller would otherwise block forever waiting for a
     * reply nothing will ever send -- see chan_owner_exited()'s own
     * comment for why this stopped being a theoretical gap. Before
     * next_runnable() below, so an unblocked caller is eligible to be
     * picked as the very next task to run.
     *
     * **Outside the lock, and that is the whole point** (phase 31 Y1). This
     * call used to sit under g_sched_lock, where it deadlocked the kernel:
     * chan_owner_exited() calls task_unblock(), task_unblock() takes
     * g_sched_lock, and spinlock_t is not re-entrant -- so the hart spun
     * forever on a lock only it held, with interrupts already off. The path
     * written to stop a caller hanging hung the whole machine instead.
     *
     * Moving it out loses nothing, because the lock was never protecting it:
     * kernel/chan.c does not take g_sched_lock at all, and endpoint state is
     * guarded by ep->lock. What the placement is for is *ordering* -- the
     * caller has to be READY before next_runnable() looks -- and doing the
     * work earlier satisfies that exactly as well as doing it inside.
     *
     * A tick can now land between here and the acquire below. That window
     * already existed and was already longer (printk_critical() above spins
     * on the UART FIFO), and what must not happen in it is *blocking*, which
     * neither of these two calls does. */
    chan_owner_exited(t->pid);

    /* Taken here and never released on this path: this task switches away
     * and nothing ever switches back to it, so the successor picked below
     * inherits the lock and does the releasing. That is the hand-off in its
     * starkest form -- see g_sched_lock's comment. The two calls above are
     * outside it because neither may block, which is a stronger requirement
     * than "not while holding this". */
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);

    int next = next_runnable(cur());
    if (next < 0) {
        /* No other runnable task. Nothing can reap this stack or resume us,
         * so keep it and park forever rather than returning into a caller
         * that no longer exists. The lock IS released here, unlike the
         * normal path below: there is no successor to hand it to, and
         * parking forever while holding it would wedge every other hart. */
        spin_unlock_irqrestore(&g_sched_lock, flags);
        /* Still mid-exit, and about to park forever: must not block. */
        printk_critical("[Sched] No runnable task remains after #%d exited; halting task\n", t->pid);
        for (;;) { }
    }

    /* Hand the stack to the reaper rather than freeing it here: this code is
     * still executing on it.
     *
     * **This hart's slot is empty, and that is a property of the code rather
     * than a hope** (phase 31 Y1/F2). The successor of a dead task runs on
     * the hart the task died on, and every path that starts running a task on
     * a hart reaps first -- sched_yield() calls sched_reap() before it picks
     * anything and again after ctx_switch() returns, and task_start() calls
     * it on a first run. So a task can only reach here after its own hart's
     * slot has been drained.
     *
     * That is why there is no free here any more. This code used to free
     * whatever it found in a single global slot, which was a real
     * g_sched_lock -> g_palloc_lock nesting: sched_reap() forty lines up goes
     * out of its way to avoid exactly that, taking the pointer out under the
     * lock and freeing it outside. Making the slot per-hart removed the need
     * for the free rather than relocating it -- the global slot's only
     * problem was that a task exiting on one hart could find another hart's
     * hand-off still pending, and per-hart slots cannot collide.
     *
     * An occupied slot now means the reap-before-run property has been
     * broken. Halting says so at the moment it happens; overwriting would
     * leak a stack silently, and freeing would put back the nesting this
     * milestone removed. Same argument, and same treatment, as
     * sched_check_free_range() above. */
    uint32_t h = hart_id();
    if (g_reap_stack[h]) {
        printk_critical("\n[Sched BUG] hart %u reap slot still holds 0x%lx (%u pages) "
                        "as #%d '%s' exits -- a task ran on this hart without "
                        "reaping first.\n", (unsigned)h,
                        (unsigned long)(uintptr_t)g_reap_stack[h],
                        (unsigned)g_reap_pages[h], t->pid, t->name);
        sched_dump_table();
        printk_critical("[Sched BUG] halting rather than leaking it.\n");
        for (;;) { __asm__ __volatile__("wfi"); }
    }
    g_reap_stack[h] = t->stack_base;
    g_reap_pages[h] = t->stack_pages;

    t->state = TASK_DEAD;
    t->stack_base = NULL;
    t->stack_pages = 0;

    g_tasks[next].state = TASK_RUNNING;
    int prev = cur();
    set_cur(next);
    (void)mem_domain_activate(g_tasks[next].domain);

    /* Parks the dead task's sp into a slot nobody will read again. The
     * incoming task reaps as soon as it resumes, off this stack. */
    sched_check_incoming(prev, next);
    ctx_switch(&g_tasks[prev].sp, g_tasks[next].sp);

    for (;;) { } /* unreachable: nothing ever switches back to a DEAD task */
}
