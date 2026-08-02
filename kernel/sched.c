#include "kernel/sched.h"
#include "kernel/printk.h"

#define MAX_TASKS 8

static task_t task_table[MAX_TASKS];
static int current_task_idx = 0;
static int num_tasks = 0;

void sched_init(void) {
    num_tasks = 0;
    current_task_idx = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_table[i].state = TASK_DEAD;
        task_table[i].pid = i;
        task_table[i].name = "(unused)";
    }
    printk("[Sched] Round-Robin Process Scheduler initialized (Max Tasks: %d)\n", MAX_TASKS);
}

int task_create(const char *name, void (*entry)(void)) {
    if (num_tasks >= MAX_TASKS) {
        printk("[Sched Error] Task table full\n");
        return -1;
    }

    int slot = num_tasks;
    task_table[slot].pid = slot;
    task_table[slot].name = name;
    task_table[slot].state = TASK_READY;
    num_tasks++;

    printk("[Sched] Created Task #%d: '%s' (entry at 0x%lx)\n",
           slot, name, (unsigned long)entry);
    return slot;
}

void sched_yield(void) {
    if (num_tasks <= 1) return;

    int prev = current_task_idx;
    current_task_idx = (current_task_idx + 1) % num_tasks;

    if (prev != current_task_idx) {
        printk("[Sched] Context Switch: Task #%d ('%s') -> Task #%d ('%s')\n",
               prev, task_table[prev].name,
               current_task_idx, task_table[current_task_idx].name);
    }
}
