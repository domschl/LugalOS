# LugalOS Scheduler & Multicore SMP Architecture

The LugalOS kernel implements a preemptive, priority-tiered scheduler capable of running across single-core microcontrollers and dual-core symmetric multiprocessing (SMP) SoCs (such as the RP2350 and ESP32-P4).

---

## 1. Task Lifecycle & Priority Tiers

Tasks are tracked in a fixed-size table `g_tasks[MAX_TASKS]`. Each task has its own private kernel stack, an optional user-mode stack, and a memory domain (`mem_domain_t`).

### Static Priority Tiers
Rather than dynamic priority decay, LugalOS uses static roles:
* `TASK_PRIO_INTERRUPT` (3): Critical driver tasks (e.g. UART, display scanout, radio supervisor). Always chosen first when runnable.
* `TASK_PRIO_NORMAL` (2): Default tier for interactive applications, shell (`lsh`), and user ELFs.
* `TASK_PRIO_BACKGROUND` (1): Asynchronous workers, deferred log flushers (`klogd`), and file caches.
* `TASK_PRIO_IDLE` (0): The idle loop running `wfi` (Wait For Interrupt) to conserve power.

Within the same tier, tasks are scheduled in fair round-robin order.

---

## 2. Timer Preemption

Preemption is driven by hardware timers (RISC-V `mtime` / `mtimecmp` or silicon timer peripherals):
* Every timer tick fires an interrupt, updating system uptime.
* If the currently executing task has consumed its time slice and another task in the same or higher priority tier is `TASK_READY`, the timer interrupt handler triggers `sched_yield()`.
* Tasks yielding cooperatively hand off the CPU immediately without waiting for a timer tick.

---

## 3. Multicore SMP & Shared Ready Queue

On dual-core platforms (RP2350 Hazard3 and ESP32-P4):
* **Shared Ready Queue**: Both CPU cores (harts) pull tasks from a single unified ready queue.
* **Per-Hart Execution Slots**: `g_current[MAX_HARTS]` tracks which task each hart is currently executing. A hart only modifies its own slot, avoiding cross-hart cache line bouncing.
* **Driver Task Pinning**: Peripheral driver tasks can be pinned to core 0 if required by hardware interrupt routing constraints.

---

## 4. The Critical Scheduler Lock Hand-Off Across `ctx_switch()`

A central architectural triumph of the LugalOS SMP scheduler is the **lock hand-off**:

### The Problem
During context switching, `ctx_switch(old_sp, new_sp)` must save the callee-saved registers and park the outgoing task's stack pointer (`sp`).
If the scheduler lock (`g_sched_lock`) were released *before* calling `ctx_switch()`, there would be a fatal race window:
1. Core 0 marks Task A as `TASK_READY` and releases the lock.
2. Core 1 immediately claims Task A from the ready queue and switches to it.
3. However, Core 0 has not yet finished executing the `REG_S sp, 0(a0)` store instruction inside `ctx_switch()`.
4. Core 1 resumes on a stale stack pointer, and two CPU cores execute on the **same stack simultaneously**, corrupting kernel memory.

### The Solution: Cross-Stack Lock Hand-off
To eliminate this race condition completely:
1. Core 0 acquires `g_sched_lock` and disables local interrupts.
2. Core 0 selects the next runnable task and calls `ctx_switch()` **while still holding `g_sched_lock`**.
3. `ctx_switch()` parks the old `sp` safely on the outgoing stack and loads the new task's `sp`.
4. **The lock is released on the incoming stack by whichever task resumes there**:
   ```c
   /* In sched_yield() */
   ctx_switch(&prev->sp, next->sp);
   /* Resumed on the new task's stack: release lock and restore interrupt flags */
   spin_unlock_irqrestore(&g_sched_lock, flags);
   ```
This design guarantees that an outgoing task cannot be claimed by another hart until its stack pointer and register context are 100% saved.

---

## 5. Synchronization Primitives

LugalOS provides two kernel synchronization primitives:
1. `spinlock_t`: Built using RISC-V `amoswap.w.aqrl` with local interrupt masking (`spin_lock_irqsave()`). Used for short critical sections (scheduler, page allocator, klog ring).
2. `ylock_t`: A yielding lock that spins briefly and then calls `sched_yield()` if contention persists. Re-entrant by the same task, ideal for long console operations and block transfers.
