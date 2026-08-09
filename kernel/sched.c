#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "kernel/mem_domain.h"
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
    if (index >= MAX_TASKS) return false;
    if (g_tasks[index].state == TASK_UNUSED) return false;
    if (pid)   *pid = g_tasks[index].pid;
    if (state) *state = g_tasks[index].state;
    if (name)  *name = g_tasks[index].name;
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

/* First-run entry for every task, reached from task_trampoline. */
void task_start(void (*entry)(void *), void *arg) {
    /* A first run arrives here instead of returning from ctx_switch(), so it
     * never passes the irq_restore() that every later resume goes through.
     * Enabling interrupts here is what makes a new task preemptible. */
    irq_restore(IRQ_ENABLE_BIT);
    entry(arg);
    task_exit();
}

int task_create(const char *name, void (*entry)(void *), void *arg) {
    if (!entry) return -1;

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

    void *stack = palloc_pages(TASK_STACK_PAGES);
    if (!stack) {
        printk("[Sched] Out of memory for '%s' stack (%d pages)\n",
               name ? name : "?", TASK_STACK_PAGES);
        g_tasks[slot].state = TASK_UNUSED; /* release the reservation */
        return -1;
    }

    task_t *t = &g_tasks[slot];
    t->pid = slot;
    t->name = name ? name : "unnamed";
    t->stack_base = stack;
    t->stack_pages = TASK_STACK_PAGES;
    t->domain = NULL; /* unrestricted until a caller attaches one */

    /* Prime the stack so the first ctx_switch() into this task "returns" to
     * task_trampoline with s0 = entry and s1 = arg. The frame layout must
     * match arch/riscv/common/switch.S exactly: slot 0 is ra, slots 1..12
     * are s0..s11, and the frame is 16 registers wide for ABI alignment. */
    uintptr_t top = (uintptr_t)stack + (uintptr_t)TASK_STACK_PAGES * PAGE_SIZE;
    top &= ~(uintptr_t)15; /* 16-byte aligned, per the RISC-V ABI */
    uintptr_t *frame = (uintptr_t *)top - 16;
    for (int i = 0; i < 16; i++) frame[i] = 0;
    frame[0] = (uintptr_t)task_trampoline; /* ra  */
    frame[1] = (uintptr_t)entry;           /* s0  */
    frame[2] = (uintptr_t)arg;             /* s1  */

    t->sp = (uintptr_t)frame;
    t->state = TASK_READY;

    printk("[Sched] Created task #%d '%s' (stack %p, %d KB)\n",
           slot, t->name, stack, (TASK_STACK_PAGES * PAGE_SIZE) / 1024);
    return slot;
}

/* Next runnable task after `from`, or -1 if none. The current task counts as
 * runnable only if it is still RUNNING -- so a task that just blocked or
 * exited is never picked again here. */
static int next_runnable(int from) {
    for (int step = 1; step <= MAX_TASKS; step++) {
        int i = (from + step) % MAX_TASKS;
        if (g_tasks[i].state == TASK_READY) return i;
    }
    return -1;
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
     * still executing on it. */
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
