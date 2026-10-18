# Phase 63 — Concurrency Review: Shared State, Ownership, Locks

**Status: in progress (63.1 done, 63.2 under way). Written 2026-10-18.** Scheduled before phase 62 Part B (audio).
Milestone scheme: `63.1`, `63.2`, …

---

## 0. Why Now

Phase 62 Part A found three concurrency bugs. Each surfaced by chance on hardware, and each
was older than the code that exposed it:

| Bug | Class | How it showed |
| :--- | :--- | :--- |
| `ksnprintf()` kept its output buffer in one global context | shared mutable state | Three terminals plus a once-a-second formatter in the menu bar: one task's digits landed in another task's dead stack slot, and the kernel faulted in `console_pump()` on a pointer reading "100" (fixed in `7d3b846`). |
| `i2cdiag` drove the I2C controller from the shell task while the bus task ran transfers | driver-task bypass | One "I2C error" in the touch poller per hardware-suite run, once the battery monitor was sampled every second (fixed in 62.4). |
| `i2c_task_call()` gave up after eight yields when the bus task was busy | starvation | About 30 touch errors per BMI270 microcode upload, and earlier "transient" scan failures (fixed in 62.5). |

Part B adds DMA interrupts, rings shared between an audio task, the mixer and several
streams, and capture fan-out, which is where these classes cost the most. Cleaning up the
base first means a fault seen during Part B points at audio, not at an old latent bug.

**First scan of the tree** (kernel, drivers, fs, net, user, arch; 2026-10-18):

| Pattern | Count |
| :--- | :--- |
| function-local `static` buffers (`static char/uint8_t x[...]` inside a function) | 38 |
| file-scope `static` byte buffers | 96 |
| `static volatile` globals | 136 |
| `chan_call()` call sites outside `kernel/chan.c` | 85 |
| interrupt handlers attached (`devirq_attach`) | 11 |
| files using spinlocks / ylocks | 16 / 17 |

Most of these are fine: a buffer used by only one task needs no lock. The work is telling
the fine ones from the rest, and writing the answer down next to each one.

### What exists already

- **The lock checker** (`kernel/lock.h`, `lockcheck`) reports blocking while holding a
  spinlock, and console writes from inside a driver's serve callback (Y3/Y5).
  Report-and-continue.
- **`chan_call()` refuses re-entry** into a busy endpoint. It is the caller's job to wait,
  which is what 62.5 got wrong once.
- **`rv64-smp`** runs the scheduler on two harts in QEMU.

None of these see the classes in the table above: data shared without a lock, or hardware
touched outside its owner.

---

## 1. Scope: The Classes

1. **Shared mutable state.** Static or global buffers, contexts and "last result" values
   reached by functions that more than one task (or a task and an interrupt) can call.
   Examples: formatters, parsers, error-text buffers returned as pointers, scratch frames.
   Already known: `drivers/bmi270.c`'s `static char why[32]` (written in 62.5, returned to a
   caller); `kernel/ea.c`'s `m_buf`/`s_buf`; `kernel/llp_node.c`'s static frames;
   `drivers/uart_net.c`'s SLIP buffers.
2. **Owner bypass.** Code that touches a peripheral's registers outside the task that owns
   it: diagnostics, boot-time helpers called later, fallback paths taken "when the task is
   not running". `i2cdiag` was one; the Tab5 expander and touch-firmware helpers were
   close to it.
3. **Busy endpoints and retry loops.** Every `chan_call()` user:
   - Is the wait bounded in time, not in attempts?
   - Can one client starve another?
   - What does the caller do on -1?
4. **Interrupt-shared state.**
   - Variables written in handlers and read in tasks (and the reverse).
   - Whether `volatile` is enough or the value needs a single atomic access.
   - Multi-word updates that a handler can observe half-done (frame-buffer front/back
     index, flip pending, ring head/tail pairs).
5. **Read-modify-write without a lock.** Shared registers or words updated as read, change,
   write from more than one task. The PI4IOE expanders were one (fixed in 62.4). GPIO
   output registers, clock gates and shared configuration words are candidates.
6. **Lock discipline the checker does not cover.**
   - ylocks taken in inconsistent order.
   - Locks held across `task_sleep_ms()` where that starves others.
   - Long critical sections under `console_lock()`.
   - Spin-free waits that only yield, and so starve lower tiers.

Out of scope: performance tuning, and the U-mode isolation model (its own tests cover it),
except where a finding touches them.

---

## 2. Milestones

### 63.1 Inventory scanner

`tools/racescan.py` (Python, `uv`, typed for `pyrefly`) produces candidate lists, not
verdicts:
- **Static storage in functions and at file scope**, from the sources and from the ELF
  (`nm -S`: writable `b`/`d` symbols with their size). The two are matched so nothing is
  missed through a macro.
- **Who can reach each one.** A coarse call graph from the compiler
  (`-fcallgraph-info=su` or `-fdump-ipa-cgraph` on the P4 and RV64 builds) and the task entry
  points (`task_create*`, `driver_task` specs, interrupt handlers). A symbol reachable from
  two or more entry points is a candidate.
- **Register accesses** (`REG(`, `*(volatile` on peripheral bases) grouped by file, set
  against the owning driver task, for class 2.
- **`chan_call` sites** with their retry shape, for class 3.

Output goes to `build/racescan/candidates.md`: one line per candidate with file:line, its
class, and the entry points that reach it. The script's own tests use a small fixture tree
with a known answer.

**Done (2026-10-18):**
- `tools/racescan.py` uses libclang over a build's `compile_commands.json`, about 35 s per
  build.
- Contexts:
  - `kernel` (kernel_main);
  - `task:<name>` (task_create* and driver_task_spec_t .serve);
  - `chan:<name>` (chan_register);
  - `isr:<handler>` (devirq_attach).
- Function addresses in ops tables and callbacks count as calls from whoever takes them, so
  the list is a deliberate over-approximation.
- Writes follow the lvalue path. An address passed to a `const` parameter is a read, an array
  passed to a non-const parameter is a write, and `REG(base + off) = v` does not write
  `base`.
- Groups:
  - **A**: written in two or more contexts.
  - **S**: written only by shells.
  - **B**: one writer, readers elsewhere.
- Static storage with no write after its initializer is dropped.
- The "all writers lock" column sees only a lock in the writing function itself.
- The fixture test passes: `tests/racescan/test_racescan.py` with `fixture.c`.
- First counts (unmarked shared statics):

| build | statics | A | S | B | MMIO files, 2+ contexts | chan_call sites |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| esp32p4-tab5 | 820 | 199 | 195 | 66 | 13 | 11 |
| rp2350-terminal | 744 | 140 | 183 | 62 | 15 | 14 |
| esp32c6 | 601 | 179 | 167 | 43 | 13 | 8 |

### 63.2 Triage and the ownership record

Every candidate gets one of four verdicts, recorded in the code next to the symbol as a
one-line comment:
- `single task: <task>`
- `guarded by: <lock>`
- `interrupt-shared: <how>`
- **finding**

The comment rule goes into AGENTS.md §8 (what to include in comments: "which lock guards
what" is already there). `racescan.py` then reports only symbols without such a line. A new
unmarked static becomes visible in review, and the scanner's count can only shrink.

**Interrupt-shared entries done (2026-10-18).** Every candidate an interrupt handler reaches,
in all five scanned builds (Tab5, RP2350 terminal, C6, rv32, rv64-smp), has a verdict in the
code. One comment covers the run of declarations below it, up to a blank line.

Scanner changes made during the triage:
- `--why CONTEXT FUNC` prints the shortest call path, which is how each verdict was checked.
- Element writes to static arrays were invisible: the base reaches the subscript through an
  array-to-pointer decay. Fixed; the counts rose with it (Tab5 now 480 unmarked: A 209,
  S 207, B 64; 45 marked).
- Section 4 lists handlers that can reach a blocking call (`sched_yield`, `task_block*`,
  `task_sleep_ms`, `ylock_acquire_at`, `chan_call`). A handler's context stops there, so
  the scheduler behind it no longer counts as reached from the interrupt. Zero in all five
  builds now.
- `devirq_in_handler()` (new, per-hart depth kept by `devirq_dispatch()`) marks a path as
  not taken from a handler: calls under `if (!devirq_in_handler())`, or after
  `if (devirq_in_handler()) return;`, are not followed for interrupt contexts.

Findings, all fixed on this branch:

| # | where | class | what | fix |
| :--- | :--- | :--- | :--- | :--- |
| F1 | kernel/printk.c | 1 | `printk_critical()` built its klog copy in one shared `g_crit_buf`/`g_crit_len`: two reporters at once (two tasks, or a task and a lock fault in a handler) mixed their text | the copy on the caller's stack, as `printk()`'s record buffer |
| F2 | drivers/dsi_esp32p4.c | 4 | `dsi_lcd_flip(true)` and `term_sync_post()` called `gdma_isr()` from a task with interrupts on: the handler proper could run between its status read and clear and finish a frame twice (a second restart into a running transfer) | `gdma_poll()`, interrupts masked; `gdma_wait_flip()` |
| F3 | drivers/dsi_esp32p4.c | 4 | the terminal drew into the back buffer while a flip was pending, i.e. into the next front: a half-drawn frame at the frame end, dirty bookkeeping on the wrong buffer | `term_convert()` takes the pending flip back (one store) and arms it again when done; no wait, so a character costs what it did (Tab5 3.7 ms, LCD-7B 1.8 ms) |
| F4 | drivers/dsi_esp32p4.c | 4 | `lcd db off` switched the front index and the DMA item in two steps and freed buffer 1 under the frame still reading it | both with interrupts masked; wait one frame before the free |
| F5 | kernel/klog.c, drivers/uart_16550.c | 6 | the 16550 handler's overrun `printk()` reached klog's boot-time inline drain, which takes `console_lock` and can block, in the window before klogd | the inline drain (both `klog_emit` and `klog_emit_tok`) skips in a handler; the record stays in the ring |
| F6 | drivers/uart_{16550,esp32p4,rp2350}.c | 4 | the TX/RX waiter slot was taken by "free? then take" under `irq_save()`, atomic on one hart only: `uart_tx_send()`'s fallback lets any printer wait, and two on two harts could both take it, the overwritten one never woken | a claim word per direction (`arch_lock_try_acquire`), held for the whole wait; a loser polls |
| F7 | drivers/uart_rp2350.c | 4 | the RP2350 TX wait blocked once without `task_prepare_block()` and wrote into the FIFO without re-checking: a wake from the other core before the block was lost | prepared and looped on the FIFO, as the 16550 one |

Open, for 63.4: the Tab5 touch poller counts one I2C error on some boots, during the
BMI270 microcode upload (seen on `main` as well, 1 of 2 boots): one client starving another
on the i2c task.

Tested: QEMU 512/512; Tab5 49/50 (the touch error above), NANO 50/50, LCD-7B 50/50. The
RP2350 change (F6, F7) is built but untested on hardware: no RP2350 board attached.

Next: the rest of group A (no handler involved), then B, then S.

### 63.3 Fix shared mutable state (class 1)

Each finding gets the smallest fix that removes the sharing:
- a per-call buffer passed in, the way `ksnprintf` now carries its context;
- per-task storage;
- a ylock where the buffer must stay shared.

Pointers returned to static text (`why[32]`) become caller-supplied buffers or constant
strings. Each fix gets a regression test where a test can show the bug (QEMU, with two tasks
hammering the function).

### 63.4 Owner bypass and starvation (classes 2 and 3)

- Diagnostics and helpers that touch hardware go through the owning task, or take the
  owner's hardware lock (the pattern `g_esp_hw_lock` set in 62.4).
- One `chan_call` waiting helper, with a time bound and fairness, used by every client
  instead of hand-written loops. `i2c_task_call()`'s 50 ms version is the model. Clients
  that issue long sequences pause between calls, as the BMI270 upload does.
- **Decide whether `chan_call` should queue callers.** A FIFO of waiting tasks would remove
  the starvation class entirely. Evaluated against the cost in `kernel/chan.c`, with a
  recommendation for the owner before anything changes.

### 63.5 Interrupt-shared state (class 4)

- Each handler's shared variables listed with the access width and order they need.
- Multi-word states (frame-buffer flip and index; ring head and tail; DMA descriptors) get a
  stated protocol, written once in the code: who writes, who reads, when it is consistent.
  Where a handler can see a half-done update, the update is made safe: a single word, or
  interrupts masked around it.
- The SDIO/LLP, USB host, DSI scanout and GDMA paths are read first. Part B adds the
  next ones.

### 63.6 Read-modify-write and lock discipline (classes 5 and 6)

- Shared hardware words updated under a lock or through a single owner.
- **A lock-order record.** Each ylock gets a rank. `ylock_acquire()` checks the rank in
  debug builds and reports an inversion the way the checker reports Y3/Y5 (report and
  continue).
- Critical sections under `console_lock()` that sleep or run long (the rotation's full
  redraw is one) are measured with a hold-time histogram in `lockcheck`. Anything over a
  threshold gets a reason in a comment or is shortened.

### 63.7 Stress harness

Load that makes collisions likely instead of lucky:
- **QEMU `rv64-smp` and `rv32`:** N tasks calling formatters, the VFS, 9P and `chan_call`
  endpoints in tight loops while shells type and the log ring fills. Run for a fixed time
  with the checker's counters and a canary per stack.
- **Hardware, all three P4 boards:**
  - three to four terminals, the sensor hub at a short period, touch, 9P traffic over the
    network, and `power log`;
  - on the Tab5, auto-orientation turning the display on a timer (a test hook, not the IMU).
  - Pass: an hour with no trap, no checker report, and no I2C or SDIO error counted.
- A **stack-canary report** in `ps` (lowest untouched word per task stack) so overflow is
  measured, not guessed. Part A sized two stacks by argument.

### 63.8 Close

- `doc/dev/kernel/` gains a concurrency page: the ownership comment rule, the `chan_call`
  wait helper, the interrupt-shared protocols, the lock ranks, and how to run
  `racescan.py` and the stress harness.
- AGENTS.md: the rules in short, next to the existing comment standard.
- `racescan.py` runs in `tests/runner.py` and fails on an unmarked new static.

---

## 3. Exit Criteria

- Every candidate from 63.1 has a verdict in the code, and `racescan.py` reports zero
  unmarked statics.
- Every finding is fixed or recorded as accepted with its reason (e.g. single-hart
  boot-only code).
- All `chan_call` clients use the shared wait helper.
- The stress harness runs clean for an hour on each P4 board and for its fixed time on both
  QEMU targets.
- The QEMU suite and the three P4 suites pass.

## 4. Decisions for the Owner

1. Whether `chan_call` should queue waiting callers (63.4), once the cost is known.
2. Whether the lock-rank check stays on in release builds or is debug-only.
3. Whether `racescan.py` failing the runner is acceptable from 63.8 on, which makes new
   statics need their ownership comment before merge.
