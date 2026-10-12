# ADR 002: Scheduler Lock Hand-Off Across Context Switches

## Status
Accepted (2026-08-25)

## Context
In a multicore SMP environment (e.g. dual-core RP2350 or ESP32-P4), when a core switches tasks, it acquires the scheduler lock `g_sched_lock`, selects the next runnable task, and calls `ctx_switch(old_sp, new_sp)`.

If `g_sched_lock` is released *before* calling `ctx_switch()`, there is an intermittent, load-dependent race condition:
1. Core 0 marks Task A as runnable and releases `g_sched_lock`.
2. Core 1 acquires `g_sched_lock`, claims Task A from the ready queue, and prepares to run it.
3. However, Core 0 is still executing inside `ctx_switch()`, where `REG_S sp, 0(a0)` saves its stack pointer.
4. Core 1 reads the old, stale stack pointer of Task A. Two CPU cores resume execution on the **exact same stack**, corrupting local variables and stack frames without an easily identifiable root cause.

## Decision
The scheduler lock `g_sched_lock` is held **across** `ctx_switch()`:
1. The outgoing task acquires `g_sched_lock` and disables local interrupts before switching.
2. `ctx_switch()` is called with the lock still held.
3. The outgoing task parks its stack pointer inside `ctx_switch()` while protected by the lock.
4. **The lock is released on the incoming stack by whichever task lands there**:
   ```c
   ctx_switch(&prev->sp, next->sp);
   /* Resumed on the incoming stack */
   spin_unlock_irqrestore(&g_sched_lock, flags);
   ```

## Consequences
* **Positive**: 100% elimination of the multi-hart stack collision race condition.
* **Positive**: Mirrors the proven architectural model of Linux's `finish_task_switch()`.
* **Negative**: The scheduler lock must travel with the task's context, requiring discipline when analyzing locking invariants.
