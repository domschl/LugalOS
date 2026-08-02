#ifndef LUGALOS_KERNEL_SCHED_H
#define LUGALOS_KERNEL_SCHED_H

#include <stdint.h>

#define TASK_READY   0
#define TASK_RUNNING 1
#define TASK_DEAD    2

typedef struct task {
    int pid;
    int state;
    uintptr_t sp;
    const char *name;
} task_t;

void sched_init(void);
void sched_yield(void);
int task_create(const char *name, void (*entry)(void));

#endif /* LUGALOS_KERNEL_SCHED_H */
