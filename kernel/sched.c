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
    }

    /* The boot context becomes task 0. Its stack is the linker-provided boot
     * stack, not a palloc'd one, so stack_base stays NULL and task_exit()
     * knows not to free it. Its sp is filled in by the first ctx_switch(). */
    g_tasks[0].state = TASK_RUNNING;
    g_tasks[0].name = "kernel";
    g_tasks[0].stack_base = NULL;
    g_tasks[0].priority = TASK_PRIO_NORMAL;
    for (int h = 0; h < MAX_HARTS; h++) g_current[h] = 0;
    spinlock_init(&g_sched_lock);
    g_active = true;

    printk("[Sched] Cooperative round-robin scheduler online (max %d tasks)\n", MAX_TASKS);
}

bool sched_active(void) { return g_active; }

int sched_current_pid(void) { return g_active ? g_tasks[cur()].pid : 0; }

/* Whether there is a scheduler to block against yet. Boot runs a long way
 * before sched_init(): drivers brought up in that window must not call
 * task_block(), because nothing would ever wake them -- see
 * drivers/uart_rp2350.c's uart_hw_putc(), which hung the whole machine
 * exactly that way on the first printk long enough to fill a 32-byte FIFO. */
bool sched_is_active(void) { return g_active; }

mem_domain_t *sched_current_domain(void) {
    return g_active ? g_tasks[cur()].domain : NULL;
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
    if (!g_active) return;
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
    task_exit();
}

int task_create_sized(const char *name, void (*entry)(void *), void *arg,
                      uint32_t stack_pages) {
    if (!entry || stack_pages == 0) return -1;

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
    t->state = TASK_READY;

    printk("[Sched] Created task #%d '%s' (stack %p, %u KB)\n",
           slot, t->name, stack, (stack_pages * (uint32_t)PAGE_SIZE) / 1024);
    return slot;
}

int task_create(const char *name, void (*entry)(void *), void *arg) {
    return task_create_sized(name, entry, arg, TASK_STACK_PAGES);
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
        if (g_tasks[i].priority > chosen_prio) {
            chosen = i;
            chosen_prio = g_tasks[i].priority;
        }
    }
    return chosen;
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

void sched_yield(void) {
    if (!g_active) return;

    /* Free any stack left by a task that exited before we were resumed. Safe
     * here and nowhere earlier: we are demonstrably not running on it. */
    sched_reap();

    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);

    int prev = cur();
    int next = next_runnable(prev);
    if (next < 0) { spin_unlock_irqrestore(&g_sched_lock, flags); return; }

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
    ctx_switch(&g_tasks[prev].sp, g_tasks[next].sp);

    /* Resumed. We are on our own stack again and should have been handed the
     * lock; releasing it here is the other half of the hand-off. */
    handoff_check();
    spin_unlock_irqrestore(&g_sched_lock, flags);
    sched_reap();
}

void task_block(void) {
    if (!g_active) return;
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
    if (!g_active) {
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
 * One slot suffices because a task can only exit while running, and the next
 * task reaps before anything else can exit. */
static void    *g_reap_stack;
static uint32_t g_reap_pages;

static void sched_reap(void) {
    /* The slot is claimed under the scheduler lock and freed outside it.
     * That split is not stylistic: palloc_free() takes palloc's own lock
     * (S4), and nesting the two would create a lock ordering this kernel
     * has no reason to have. Taking the pointer out first makes the free a
     * purely local operation. */
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);
    void *stack = g_reap_stack;
    uint32_t pages = g_reap_pages;
    g_reap_stack = NULL;
    g_reap_pages = 0;
    spin_unlock_irqrestore(&g_sched_lock, flags);

    if (stack) palloc_free(stack, pages);
}

void task_exit(void) {
    if (!g_active) { for (;;) { } }

    task_t *t = &g_tasks[cur()];
    printk("[Sched] Task #%d '%s' exited\n", t->pid, t->name);

    /* Taken here and never released on this path: this task switches away
     * and nothing ever switches back to it, so the successor picked below
     * inherits the lock and does the releasing. That is the hand-off in its
     * starkest form -- see g_sched_lock's comment. The printk() above is
     * deliberately outside it, because printk() can block. */
    uintptr_t flags = spin_lock_irqsave(&g_sched_lock);

    /* M5 Phase 2: if this task owned a chan endpoint with a request
     * pending, its caller would otherwise block forever waiting for a
     * reply nothing will ever send -- see chan_owner_exited()'s own
     * comment for why this stopped being a theoretical gap. Before
     * next_runnable() below, so an unblocked caller is eligible to be
     * picked as the very next task to run. */
    chan_owner_exited(t->pid);

    int next = next_runnable(cur());
    if (next < 0) {
        /* No other runnable task. Nothing can reap this stack or resume us,
         * so keep it and park forever rather than returning into a caller
         * that no longer exists. The lock IS released here, unlike the
         * normal path below: there is no successor to hand it to, and
         * parking forever while holding it would wedge every other hart. */
        spin_unlock_irqrestore(&g_sched_lock, flags);
        printk("[Sched] No runnable task remains after #%d exited; halting task\n", t->pid);
        for (;;) { }
    }

    /* Hand the stack to the reaper rather than freeing it here: this code is
     * still executing on it.
     *
     * "The next task reaps before anything else can exit" (see g_reap_stack's
     * comment) holds under cooperative scheduling and ordinary preemption,
     * since the printk() above is too fast for a second task to reach its
     * own task_exit() in the gap. This slot is still only one deep, but it
     * does not trust the handoff to land before it's needed again: if a
     * previous dead task's stack is somehow still sitting here unreaped,
     * free it now -- we are provably not running on it, only on our own --
     * instead of silently overwriting the only reference to it. */
    if (g_reap_stack) {
        palloc_free(g_reap_stack, g_reap_pages);
    }
    g_reap_stack = t->stack_base;
    g_reap_pages = t->stack_pages;

    t->state = TASK_DEAD;
    t->stack_base = NULL;
    t->stack_pages = 0;

    g_tasks[next].state = TASK_RUNNING;
    int prev = cur();
    set_cur(next);
    (void)mem_domain_activate(g_tasks[next].domain);

    /* Parks the dead task's sp into a slot nobody will read again. The
     * incoming task reaps as soon as it resumes, off this stack. */
    ctx_switch(&g_tasks[prev].sp, g_tasks[next].sp);

    for (;;) { } /* unreachable: nothing ever switches back to a DEAD task */
}
