# ADR 008: Folded Endpoints, a Driver Served in Its Callers' Context

## Status
Accepted (2026-11-04)

## Context
A kernel-mode driver here is a task that owns its hardware and serves requests over a channel endpoint (`drivers/include/drivers/driver_task.h`). That task costs at least a page of stack, and on the smallest boards pages are what runs out: the ESP32-C6-Zero has 74. Some drivers do almost nothing with their task. The C6's I2C driver answers one transfer at a time, polls the controller to completion, and has no state between requests.

The channel layer already has a second kind of endpoint: an inline handler that runs in the caller's context (`chan_register()`, used by `p9` and `console`). A caller of either kind goes through the same claim and FIFO, and its request and response are copied in and out the same way (`kernel/chan.c`, `chan_call_impl()`). So a driver's `serve()` can be registered as an inline handler instead of being run by a task, and no caller and no driver can tell the difference.

The microkernel stays the lead. A build that folds an endpoint is an exception derived from the unfolded build, not a second design.

## Decision
1. **Folding is a property of a preset, never of a driver.** `LUGALOS_FOLD` (a comma-separated list of endpoint names) is set in `CMakePresets.json`. It becomes `CONFIG_FOLD`, and `driver_task_start()` (`drivers/driver_task.c`) registers a folded endpoint with `serve()` as its handler and creates no task. Driver code is the same in both builds.
2. **`serve()` is reached only through the framework.** `tools/servecheck.py` fails on any direct call to a serve callback, and on any `.serve(` call outside `drivers/driver_task.c`. It runs in `tests/runner.py`.
3. **Every folded preset has an unfolded twin that runs the same suite.** `rv32-fold` folds the QEMU target's `uart`, `blk` and `i2c` and runs the core QEMU suite; `rv32` is its twin. On hardware, `esp32c6-release` folds `i2c` and `esp32c6` is its unfolded dev twin.
4. **A folded endpoint is visible at run time.** `/proc/config` lists it (`FOLD=`), the boot log says `Driver folded`, and the endpoint has no task in `/proc/ps`.
5. **An endpoint may fold only if its `serve()`:**
   * returns in bounded time, or blocks only through primitives that block whoever is calling (`sched_current_pid()` at wait time, as the UARTs do), never by waking a pid recorded at start;
   * fits the stacks of all its callers (the RAM ledger of 66.1 shows each stack's high-water mark);
   * does not need its own priority. A UART task runs at `TASK_PRIO_INTERRUPT`; folded, it runs at the caller's;
   * is not pinned to a hart;
   * runs in kernel mode. A U-mode driver's task is its memory domain, and folding it would drop the isolation (66.8, not done).

## Consequences
* **Positive**: A folded endpoint costs no task and no stack. On the C6-Zero, folding `i2c` frees one page.
* **Positive**: Callers, drivers and the channel API are unchanged, so an endpoint is folded or unfolded by editing one preset, and the twin catches a driver that behaves differently.
* **Negative**: The caller's stack absorbs `serve()`'s depth, and its priority applies. A slow `serve()` now delays its caller's other work directly, not through the scheduler.
* **Negative**: A folded `serve()` runs under whatever ylocks its caller holds (`chan_call()` already refuses a caller holding a spinlock), so lock orders that were two tasks' are now one task's. `lockcheck` learns and checks them on the folded build as on its twin; a folded preset is checked with it under load before it is used.
* **Neutral**: Interrupt handlers and code running before the scheduler get -1 from a folded endpoint, exactly as from a task endpoint, so the facades' fallback paths are the same in both builds.
