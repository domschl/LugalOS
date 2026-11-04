# LugalOS Driver Framework & Hardware Isolation

A driver in LugalOS owns its hardware through a task and is reached through a channel endpoint, so no other code touches its registers while it runs. Whether that task is also confined to a memory domain depends on the board: on the RP2350 the drivers run in U-mode, each in its own PMP domain; on the ESP32-P4, the ESP32-C6 and QEMU they are kernel-mode tasks. A preset can also fold a kernel-mode driver, so that it has no task at all.

The contract, with a worked example, is the header comment of `drivers/include/drivers/driver_task.h`. This page is the overview.

---

## 1. The Four Parts of a Driver

1. **The register half.** Which bits in which register on this chip. Never shared between chips.
2. **The task half.** A `driver_task_t` and a `serve()` callback: the driver's wire protocol, one request at a time. The framework (`drivers/driver_task.c`) owns the loop, the endpoint, the reply and the count of calls served; `driver_task_start()` creates it.
3. **The facade.** The functions everyone else calls (`uart_putc()`, `blk_read()`, `i2c_xfer()`). Each asks `driver_task_alive()` and either calls the endpoint (`driver_task_call()`) or, when no task owns the hardware (during boot, or if the task never started), touches the hardware itself.
4. **A U-mode domain**, on the RP2350 only (section 3).

Three invariants hold inside `serve()`, and the first two are checked at run time (`kernel/lock.h`): no console output (`printk()` is fine, `cprintf()` is not); no call back into anything that could call this same endpoint (`chan_call()` refuses a call that would close a wait cycle); and only the driver touches its hardware while it is alive.

---

## 2. Where Drivers Run, per Board

| Board | Drivers | How |
| :--- | :--- | :--- |
| RP2350 | UART, USB CDC, SPI SD, I2C, ST7735, TM1638, LCD terminal, USB keyboard, clock display | U-mode tasks, each in its own PMP domain (`driver_umode_enter()`) |
| ESP32-P4 | UART, SDMMC (`sdblk`), I2C | kernel-mode driver tasks (`driver_task_start()`) |
| ESP32-C6 | I2C | a kernel-mode driver task; folded in `esp32c6-release` (section 4) |
| QEMU | UART (16550), virtio block (`blk`), I2C | kernel-mode driver tasks; all three folded in `rv32-fold` |

The ESP32-C6's vendor Wi-Fi code is not a driver in this sense, but it runs in U-mode too, in a domain of its own (`kernel/radio_c6.c`, [ADR 007](../../history/adr/007_c6_peripheral_confinement.md)). `ps` shows each task's isolation: `PMP` or `Sv39` for a confined task, `-` for a kernel-mode one.

---

## 3. U-Mode Domains (RP2350)

A driver that confines itself calls `driver_umode_enter()` from its own task, with a `driver_umode_spec_t`: its stack, the register windows it needs and any shared data region. The framework builds the `mem_domain_t` (`mem_domain_add()` per region, always including the shared `.utext` page the task executes from), attaches it with `task_set_domain()` and drops to U-mode. From then on a store outside the grants is an access fault. Up to `MEM_DOMAIN_MAX_REGIONS` regions fit a domain: 5, or 14 on the C6, whose radio domain needs them.

If the domain cannot be enforced, or no domain slot is left, the driver says so and keeps running without the confinement; it never claims isolation it does not have. On every other board `driver_umode_enter()` refuses at once, and its domain table is not built.

A dev image can prove the confinement on the hardware: each confined driver has an isolation test (`uartisotest`, `blkisotest`, `i2cisotest`, `usbisotest`, ...) that makes the driver store outside its grant and checks that the store faulted and that a canary in kernel memory is untouched.

---

## 4. Folded Endpoints (per Preset)

A preset may **fold** kernel-mode endpoints: `LUGALOS_FOLD` in `CMakePresets.json`, a comma-separated list of endpoint names. `driver_task_start()` then registers `serve()` as an inline channel handler and creates no task. A caller goes through the same claim, FIFO and copies as before, but `serve()` runs on the caller's stack and at the caller's priority, so the driver costs no task and no stack. Callers and drivers do not change: folding is a property of the preset, never of the code.

* **Which endpoints may fold**: a `serve()` that returns in bounded time (or blocks only whoever calls it), fits its callers' stacks, needs no priority of its own, is not pinned to a hart and runs in kernel mode. The rule and its reasons: [ADR 008](../../history/adr/008_folded_endpoints.md).
* **Guardrails**: `tools/servecheck.py`, run by `tests/runner.py`, refuses any direct call to a serve callback, so every call goes through the framework. Every folded preset has an unfolded twin that runs the same suite: `rv32-fold` and `rv32` on QEMU, `esp32c6-release` and `esp32c6` on the C6-Zero.
* **Visible at run time**: `/proc/config` lists the folded endpoints (`FOLD=`), the boot log says `Driver folded`, and a folded endpoint has no task in `ps`.

---

## 5. Interrupts

A peripheral's interrupt handler is attached with `devirq_attach()` (`kernel/devirq.h`). `arch/riscv/common/trap.c` resolves which line fired (the PLIC, the RP2350's `meinext`, the P4's CLIC) and calls `devirq_dispatch()`. The handler acknowledges the hardware and wakes whoever is waiting with `task_unblock()`. A driver that waits records the waiting task when it begins to wait (`sched_current_pid()`), not when it starts, so the same code works when the driver is folded and the caller is the one waiting. Interrupt-shared state is guarded with `spin_lock_irqsave()` on both sides, and the task is woken only after the lock is released ([Concurrency](concurrency.md)).

---

## 6. Calling a Driver

An endpoint serves one request at a time. A caller that finds it busy waits its turn with
`chan_call_wait(ep, req, len, resp, max, wait_ms)` (`kernel/chan.h`), or with
`driver_task_call()` / `driver_task_call_wait()` for a `driver_task_t`:
* Waiting callers are served in arrival order. The caller releasing the endpoint hands it
  straight to the next one, so a client issuing requests back to back cannot keep it from a
  client that was already waiting.
* `CHAN_WAIT_MS` (50 ms) is the usual bound; block devices use 500 ms, the local 9P link 1 s.
* A context re-entering an endpoint it holds is refused at once rather than queued behind
  itself. So is a caller holding a spinlock, or one in an interrupt handler.
* Plain `chan_call()` never waits. It is for the few callers that must not queue, such as a
  UART writer that would otherwise wait behind a READ waiting for a key.

**A facade touches the hardware itself only when the task is not running**
(`driver_task_alive()` false: during boot, or if the task never started). If the call fails
while the task is alive, the facade reports an error (or drops a display update). The task may
still be driving the hardware, and a second access beside it collides on the bus.

Timed waits on hardware read the clock *before* the condition, so the last test always comes
after the deadline (`time_poll_until_us()`, `kernel/time.h`). In the other order, a task
preempted past the deadline gives up on a reading taken before the preemption.

`chanqtest` checks the queue under real tasks; `i2cstats` counts I2C failures by cause.
