#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "kernel/meminfo.h"
#include "kernel/mem_domain.h"
#include "kernel/chan.h"
#include "kernel/irq.h"
#include "kernel/printk.h"
#include <string.h>

/* See kernel/include/kernel/sched.h for the rationale. */

extern void ctx_switch(uintptr_t *old_sp, uintptr_t new_sp);

static void sched_reap(void); /* defined below; used by sched_yield() above it */
extern void task_trampoline(void);

static task_t  g_tasks[MAX_TASKS];
static int     g_current;      /* index into g_tasks */
static bool    g_active;

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
    g_current = 0;
    g_active = true;

    printk("[Sched] Cooperative round-robin scheduler online (max %d tasks)\n", MAX_TASKS);
}

bool sched_active(void) { return g_active; }

int sched_current_pid(void) { return g_active ? g_tasks[g_current].pid : 0; }

mem_domain_t *sched_current_domain(void) {
    return g_active ? g_tasks[g_current].domain : NULL;
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
    if (pid == g_current) return mem_domain_activate(domain);
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
    g_tasks[g_current].exit_status = status;
    g_tasks[g_current].exit_clean = true;
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
     * never passes the irq_restore() that every later resume goes through.
     * Enabling interrupts here is what makes a new task preemptible. */
    irq_restore(IRQ_ENABLE_BIT);
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
     * the table, or two creators could pick the same one. */
    uintptr_t flags = irq_save();
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) { /* slot 0 is always the boot task */
        if (g_tasks[i].state == TASK_UNUSED || g_tasks[i].state == TASK_DEAD) {
            slot = i;
            g_tasks[i].state = TASK_BLOCKED; /* reserve it before releasing */
            break;
        }
    }
    irq_restore(flags);

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
    for (int step = 1; step <= MAX_TASKS; step++) {
        int i = (from + step) % MAX_TASKS;
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

    uintptr_t flags = irq_save();

    int prev = g_current;
    int next = next_runnable(prev);
    if (next < 0) { irq_restore(flags); return; } /* nothing else can run */

    if (g_tasks[prev].state == TASK_RUNNING) g_tasks[prev].state = TASK_READY;
    g_tasks[next].state = TASK_RUNNING;
    g_current = next;

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
     * does the same for us. */
    ctx_switch(&g_tasks[prev].sp, g_tasks[next].sp);

    irq_restore(flags);
    sched_reap();
}

void task_block(void) {
    if (!g_active) return;
    g_tasks[g_current].state = TASK_BLOCKED;
    sched_yield();
}

int task_unblock(int pid) {
    if (!g_active || pid < 0 || pid >= MAX_TASKS) return -1;
    if (g_tasks[pid].state != TASK_BLOCKED) return -1;
    g_tasks[pid].state = TASK_READY;
    return 0;
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
    uintptr_t flags = irq_save();
    void *stack = g_reap_stack;
    uint32_t pages = g_reap_pages;
    g_reap_stack = NULL;
    g_reap_pages = 0;
    irq_restore(flags);

    if (stack) palloc_free(stack, pages);
}

void task_exit(void) {
    if (!g_active) { for (;;) { } }

    task_t *t = &g_tasks[g_current];
    printk("[Sched] Task #%d '%s' exited\n", t->pid, t->name);

    uintptr_t flags = irq_save();

    /* M5 Phase 2: if this task owned a chan endpoint with a request
     * pending, its caller would otherwise block forever waiting for a
     * reply nothing will ever send -- see chan_owner_exited()'s own
     * comment for why this stopped being a theoretical gap. Before
     * next_runnable() below, so an unblocked caller is eligible to be
     * picked as the very next task to run. */
    chan_owner_exited(t->pid);

    int next = next_runnable(g_current);
    if (next < 0) {
        /* No other runnable task. Nothing can reap this stack or resume us,
         * so keep it and park forever rather than returning into a caller
         * that no longer exists. */
        irq_restore(flags);
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
    int prev = g_current;
    g_current = next;
    (void)mem_domain_activate(g_tasks[next].domain);

    /* Parks the dead task's sp into a slot nobody will read again. The
     * incoming task reaps as soon as it resumes, off this stack. */
    ctx_switch(&g_tasks[prev].sp, g_tasks[next].sp);

    for (;;) { } /* unreachable: nothing ever switches back to a DEAD task */
}
