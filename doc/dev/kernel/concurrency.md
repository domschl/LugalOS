# Concurrency: Shared State, Locks and Waits

LugalOS runs many tasks on one or two harts, with preemption, interrupt handlers and
drivers living in their own tasks. Most shared state in the tree is fine because only one
task ever touches it. The rules on this page are how the rest stays correct, and how a
reviewer can tell which case a piece of code is in.

---

## 1. Every Shared Static Says Who Owns It

A writable `static` or global reached from more than one task or handler carries a verdict
comment, on its line or in the comment just above it. One comment covers a run of
declarations up to the next blank line (a blank line inside a struct ends it too):

| Verdict | Meaning |
| :--- | :--- |
| `single task: <who>` | Only that task (or the boot path before the scheduler) touches it. |
| `guarded by: <lock>` | Every access holds that lock. |
| `interrupt-shared: <protocol>` | A handler and tasks share it; the comment states who writes, who reads, and when it is consistent. |
| `accepted: <why>` | Shared without a lock on purpose, e.g. a selftest's state (one run at a time), a cache whose readers tolerate an old value, a setting written by a single store from a shell. |

```c
/* guarded by: g_dev_lock (below), which init and every measurement take:
 * the hub's sampler, `sensor` and Lisp may all measure. */
static struct { ... } g;
static ylock_t g_dev_lock;
```

`tools/racescan.py --build build/<preset>` lists the statics that are shared and unmarked
(`build/racescan/candidates.md`). The count is zero in every configured build, and
`tests/runner.py` keeps it there: it runs `racescan.py --check` on rv32, rv64-smp, the Tab5,
the RP2350 terminal and the C6, and fails on a shared static without a verdict (or an
interrupt handler that can reach a blocking call), naming the variable, where it is, which
contexts reach it and what to write.

- Groups: **A** written from two or more contexts, **S** written only by shells, **B** one
  writer with readers elsewhere.
- `--why CONTEXT FUNC` prints the shortest call path from a context (`task:<name>`,
  `isr:<handler>`, `chan:<name>`, `kernel`) to a function, which is how a verdict is checked.
- The list over-approximates on purpose: a function whose address goes into an ops table
  counts as called by whoever takes the table.

---

## 2. Locks

| Primitive | Header | Use |
| :--- | :--- | :--- |
| `spinlock_t` | kernel/lock.h | Short sections, and anything a handler shares. `spin_lock_irqsave()` masks interrupts on the calling hart *and* excludes the other hart. A spinlock is a leaf: no blocking, no other lock, no wake-ups while it is held. |
| `ylock_t` | kernel/lock.h | A yielding, re-entrant task lock for longer sections (a device's transfer, a volume). Zero is a free ylock: no lazy init. |
| `session_claim_t` | kernel/lock.h | A tool whose state is one set (chess, `e`, `ed`, the pager): a second terminal is told the tool is in use instead of waiting out a whole session. |
| `sysreg_update()` | kernel/sysreg.h | Read-modify-write of a register word that holds other drivers' bits (the P4's HP_SYS_CLKRST and LP_CLKRST gates and resets). On the RP2350, use the atomic set/clear aliases (+0x2000 / +0x3000) instead. |

### `irq_save()` is not a lock

`irq_save()` keeps the handler out only on the calling hart. A word that a handler and a
task (or tasks on two harts) both read-modify-write takes a spinlock on both sides,
`spin_lock_irqsave()` in the handler too. The handler wakes tasks only after it releases
the lock. Boards run one hart until `smpstart`, `rv64-smp` always runs two, so a protocol
that only works on one hart passes on hardware and fails in QEMU.

### Shared hardware engines

Some hardware is one resource for the whole chip, however many drivers use it.
The P4's cache maintenance is the one that cost data: `Cache_WriteBack_Addr` and
`Cache_Invalidate_Addr` program the cache controller's single set of sync registers, so two
at once (the panel's frame flush and the SD driver's bounce buffer) overwrite each other's
setup and one range is silently not done. A lost invalidate hands back the previous DMA
transfer's bytes; a lost writeback sends them. Use `esp32p4_dcache_*()` and
`esp32p4_extmem_*()` (arch/riscv/common/trap.c), which serialise every operation under one
spinlock in 32 KB chunks, as IDF's `esp_cache_msync()` does; never call the ROM routines
directly after boot.

### Lock order

ylocks are taken in one order. The kernel learns it: when a context holding A takes B, the
pair A->B is recorded, and a later B->A is reported once (report and continue, in every
build). On the P4 panel, `g_frame_lock` is always taken after `console_lock`
(`frame_begin()` takes both).

`lockcheck [pairs]` shows the learned order, any inversions, and a histogram of
`console_lock` hold times with the last holds of 100 ms or more. A hold that long needs a
reason in a comment at the call site.

### The shell side

Every Lisp evaluation, and every shell command that becomes a Lisp form, holds
`g_lisp_lock`. Native shell commands do not, so two terminals can run them at once: a
native tool whose state is one set takes a `session_claim_t`.

---

## 3. Calling a Driver Task

A driver task owns its hardware. Callers reach it with
`chan_call_wait(ep, req, len, reply, cap, wait_ms)` (kernel/chan.h):

- A busy endpoint queues the caller (FIFO, 8 places) until its turn or its deadline. The
  releasing caller hands the endpoint straight to the oldest waiter, so a client calling
  back to back cannot barge ahead.
- `CHAN_WAIT_MS` (50) is the common bound; block devices wait 500 ms (an SD write can stay
  busy 250 ms), local 9P 1 s.
- It refuses at once where waiting is impossible: in a handler, with a spinlock held, with
  no task, or from the context that holds the endpoint (recursion).
- Never write a retry loop around plain `chan_call()`.

A facade touches the hardware itself only when `driver_task_alive()` is false (boot, before
the task exists). A failed call while the task is alive is an error, not a reason to go
around the task. A client that issues long sequences (the BMI270's microcode upload) pauses
between calls so others get a turn.

The VFS handle table works the same way: `vfs_open()` with every handle in use waits up to
500 ms for one to close. `stress` reports the table's peak and how often opens waited or
found none.

---

## 4. Waiting on Hardware

A timed poll reads the clock *before* it tests the condition (`time_poll_until_us()`,
kernel/time.h). Read the other way round, a task preempted past its deadline fails on a
status word read before the preemption, though the transfer finished meanwhile. That was
the Tab5's touch I2C error.

```c
if (!time_poll_until_us(!(REG(SD_STATUS) & STATUS_DATA_BUSY), time_get_us() + 1000000u))
    return -1;
```

A path a handler can reach must not block. Cut it off with
`if (devirq_in_handler()) return;` (or call under `if (!devirq_in_handler())`). racescan
lists handler paths that reach a blocking call, and stops following a path at those
guards.

---

## 5. The Stress Harness

`stress [s] [norot] [only=PATHS]` (kernel/stress.c) runs worker tasks that pick paths at
random: formatting, files on `/ram0` and `/sd0`, the local 9P server, `chan_call_wait()`
into a busy endpoint, log bursts, shell commands under capture, and on the Tab5 a panel
rotation every 7 s. Every result is checked. A failed read-back logs what came back instead
(`[stress] ...: first difference at N`).

The report adds what happened meanwhile: lock faults, lock-order inversions, I2C failures,
log gaps, full stacks, stuck workers and VFS handle shortage, then `STRESS_OK` or
`STRESS_FAIL`.

| Where | How |
| :--- | :--- |
| QEMU | `tests/runner.py` runs `stress 30` on rv64 and rv32, `stress 60` on rv64-smp. |
| P4 boards | `uv run tools/stress.py --board nano\|lcd7b\|tab5 --minutes 60` adds two terminals, typing, 9P over TCP and sensor sampling every 2 s, then checks `dmesg`, `lockcheck`, `i2cstats` and `ts`. |

`ps` shows each task's stack depth: the deeper of the poison scan and the stack pointer seen at
every switch and every trap. A frame with a large local array can jump the poison; the stack
pointer cannot. A task whose stack pointer goes below its stack's base halts the board with a
dump of every task's stack; within 512 bytes of the base it is reported once. The stress report
gives the deepest worker stack.

---

## 6. Checklist for New Code

1. A new writable static reached from two contexts gets a verdict comment.
2. Hardware is touched by its driver task, or under the owner's hardware lock.
3. Waits go through `chan_call_wait()` or `time_poll_until_us()`, never hand-written loops.
4. Handler-shared words: a spinlock on both sides, or a single-store protocol written down.
5. Shared clock and reset words: `sysreg_update()`.
6. Run `racescan.py` and a `stress` run on the targets the change touches.
