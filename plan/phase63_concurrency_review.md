# Phase 63 — Concurrency Review: Shared State, Ownership, Locks

**Status: in progress (63.1–63.6 done). Written 2026-10-18.** Scheduled before phase 62 Part B (audio).
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
on the i2c task. (63.4: not starvation, F39.)

Tested: QEMU 512/512; Tab5 49/50 (the touch error above), NANO 50/50, LCD-7B 50/50. The
RP2350 change (F6, F7) is built but untested on hardware: no RP2350 board attached.

The RP2350 change (F6, F7) passed its suite afterwards, 34/34.

**Group A done (2026-10-18).** Every group-A candidate in the five builds has a verdict, except
the 37 per build that belong to the four open findings below. More scanner changes:
- A pointer declared on a static (`T *p = &g_x[i];`) counts as a write through it; the VFS
  handle table was invisible before.
- One comment marks a declaration group across preprocessor lines, attributes, arithmetic in
  array sizes and struct bodies, but not across a statement.

Fixed on this branch:

| # | where | what | fix |
| :--- | :--- | :--- | :--- |
| F8 | kernel/console.c | one global capture (`ea`, the editor): a second capturer made the first's output run past its buffer | a slot per capturing task, under g_cap_lock |
| F12 | fs/vfs_server.c | a handle slot was found and taken in two steps, and freed by a memset another open could overtake | g_handles_lock for both |
| F14 | arch/riscv/common/mem_domain.c | the first PMP probe from two tasks interleaved its count, and published `avail` after `start` (an activation could read -1) | probe into locals, publish `start` last behind a fence |
| F16 | kernel/random.c | the counter was incremented and re-read unlocked: two tasks could hash the same value (identical bytes from the timer source) | an atomic add; each call hashes the value it took |
| F17 | drivers/driver_task.c | U-mode domain slots taken by `count++` while driver tasks' first passes interleave; a failure's `count--` could free another's | one atomic claim; an unused slot stays unused |
| F18 | kernel/time.c, drivers/usb_cdc.c | `time_delay_us()` pumped the USB device from any waiting task beside the usbcdc driver task; its re-entry guard was test-then-set | pump only without the task; atomic exchange |
| F19 | kernel/radio_c6.c | boot's join and an LLP host's could overwrite each other's key and start the radio twice | atomic claims of the credentials and of the start |
| F20 | kernel/ota.c, drivers/flash_esp32c6.c | a lazy `ylock_init()` on first use: a second first user reset a lock already held | none needed: zero is a free ylock |
| F21 | drivers/ota_esp32c6.c | the IDF layout probe marked itself done before it was; a concurrent OTA request read "updates disabled" | g_idf_lock |
| F22 | kernel/kobj_sched.c | the object layer's lazy init could run over tables and a lock in use | a one-shot compare-and-swap; late callers wait |
| F23 | drivers/reader_view.c | closing a reader freed its surface while the compositor read it | open and close under console_lock |

Smaller ones:
- `lcd pattern` now holds console_lock, like the other scan-out changes.
- The ESP I2C bring-up takes g_esp_hw_lock.
- Locks for the clipboard's writers and for the UART and loopback 9P self-tests' static buffers.
- One BME280 measurement at a time.
- The BMI270's last reading and reload are under its lock.
- mqttd fences a new source before counting it.

Tested: QEMU 512/512; Tab5, NANO and LCD-7B 50/50; RP2350 terminal 34/34. C6 (the NANO's
C6-MINI): all pass but the DHCP lease, which a C6 bridging wlan0 for the NANO's P4 does not
take by design (`[LLP] bridging wlan0`).


The five that needed a design, fixed afterwards (2026-10-18):

| # | where | what | fix |
| :--- | :--- | :--- | :--- |
| F9 | net/stack.c, tcp.c, udp.c | one transmit frame and the TCP/UDP tables used from many tasks with no lock (DHCP, DNS and NTP from the shell or Lisp, MQTT, the stack task) | one re-entrant stack lock, `net_lock()`: the stack task holds it for each round, every public entry point takes it; released around the inline 9P and Spartan service, which can reach a remote mount's client lock (order: 9P client/pump locks before the stack's) |
| F10 | fs/9p.c | the request's connection and auth policy were globals while the server ran for several tasks at once: a TCP request could run under a local request's "no key" policy, or with another connection's fids; same-task recursion left the outer request with the inner's; one shared data buffer; unlocked fid slots | per-context request state (`sched_context_id()`), saved and restored around each request; fid and msize slots under g_fid_lock; the data buffer claimed atomically, a page for a second request |
| F11 | fs/p9_link.c | `waiter_begin()` set the slot's link before its tag | fields first, link last behind a fence |
| F13 | fs/vfs_server.c | the mount table had no lock: `/c6`, unmounted and remounted on every C6 restart, was cleared under tasks resolving through it (an all-zero entry reads as FAT32 with no volume) or holding handles on it; `format` re-initialised a volume in use | reference-counted entries: lookups and open handles hold one; an unmount marks the entry dying and the last reference clears it and closes its session; a slot is ready only once filled; `format` and a new RAM disk withdraw the volume and refuse one in use |
| F15 | drivers/sensor_hub.c | `sensor init` re-ran the hub's init under the running sampler (caches cleared, drivers re-initialised); readers saw torn caches | s_hub_lock over init, each device's conversion, `hub_derive()` and every reader; the BME280 and BMI270 inits under their drivers' locks |
| F24 | fs/p9_link.c | found on the way: the 9P client parsed replies in place in one static buffer, and its callers used the data after the client lock was released; the background-link poller read each slot twice | every public `p9_remote_*()` holds the client lock throughout; one read; registration under a spinlock |

Group A is at zero unmarked in all five builds.

Tested after the fixes: QEMU 512/512 and every host harness under ASan/UBSan; LCD-7B 50/50,
NANO 50/50 (with its C6 back on its loader: an earlier C6 suite run had loaded the kernel
past it, which the OTA check catches), RP2350 terminal 34/34, Tab5 49/50 (the touch error
during the BMI270 upload, for 63.4). The NANO's run includes the remount of /c6 after a C6
restart and a rejoin under an open 9P session.

**Groups B and S done (2026-10-18).** Every candidate in all ten configured builds has a
verdict (the five scanned so far, plus the LCD-7B, NANO, C6-MINI, rv64 and the RP2350
sensor persona): zero unmarked.

Two scanner gaps first, both hiding reachability:
- `const` tables were dropped before their initializers were read, so every function reached
  only through one (all Lisp primitives, const ops tables) looked unreachable.
- A static function defined in a header (or a table in an included file) was looked up in
  the header's own name space, not the translation unit's.
- Fixing both moved the Lisp interpreter, the C compiler and much of the shell into view
  (Tab5's group S went from 176 to 286).

What serialises the shell side, found on the way:
- Every Lisp evaluation, and every shell command that becomes a Lisp form, holds
  `g_lisp_lock`. That covers the interpreter and the C compiler (`cc` is a primitive).
- The native commands `parse_and_eval_cmd()` handles in C do not take it, and neither do
  the Lisp "sessions" that park it (chess, the Z-machine, the clock, a nested `lsh`, LLM
  chats).
- Those are where two terminals can meet. For tools whose state is one set, a session claim
  (`kernel/lock.h`'s `session_claim_t`, re-entrant): a second terminal is told the tool is in
  use rather than made to wait out a session. Used by chess, `e`, `ed`, `ea` and the pager.

Fixed:

| # | where | what | fix |
| :--- | :--- | :--- | :--- |
| F25 | user/chess | one engine state for every terminal: a session ending in one freed the tables a search in another used | session claim at the four entry points |
| F26 | `e`, `ed`, `ea` | native commands whose state is one set | session claims |
| F27 | kernel/shell.c | a pipeline's input was one global; another terminal's command read it as its own | per execution context |
| F28 | fs/9p.c | the grants cache and console key were rewritten from a shell while the server read them for an attach | g_auth_lock; readers copy out |
| F29 | kernel/time.c | the wall-clock base (two 64-bit words and the rate), set by `date`/NTP while every task reads it, tears on rv32 | a sequence lock, lock-free readers |
| F30 | fs/vfs_server.c | one static buffer for every `cp` | on the stack |
| F31 | kernel/kobj_sys.c | a domain's threads making their first critical section at once could each create a mutex: two critical sections that do not exclude each other (the C6 radio) | g_crit_lock |
| F32 | drivers/i2c_bus.c | the probe window was wait-then-set: two tasks could both open one | a ylock held for the window |
| F33 | kernel/line_editor.c | two history adds could both pass the room check and count past the array | g_hist_lock for writers |
| F34 | arch/riscv/common/elf.c | two terminals starting programs could take one slot and share the header buffer | g_elf_lock over the load |
| F35 | net/dns.c, net/ntp.c | one reply slot each: two terminals querying at once took each other's answer | a lock per query |
| F36 | various | wire claims tested then set (g_wire_lock); the pager's stack one for all terminals (session claim); the RAM screen built twice on first use (console_lock); the EMAC's diagnostics taking the rings mid-poll (under net_lock); every sensor driver's init and measurement under its own lock, as the BME280's | |

Everything else in S has an `accepted:` or `single task:` reason in the code:
- selftests and diagnostics, one run at a time;
- caches whose readers tolerate an old value;
- configuration set from a shell by single stores (`tz`, `path`, `identity`, `mqtt`
  credentials).

Tested: host harnesses (ASan/UBSan) clean; QEMU 511/512 (TCP recovery R3 on rv32, the known
flake: 6/6 alone afterwards); NANO 50/50 (its C6 on the new build); RP2350 34/34; LCD-7B 50/50
alone. In the run with all three P4 boards in parallel, the LCD-7B and the Tab5 each counted
two touch I2C errors; not reproduced on the LCD-7B alone or by replaying the tests. Recorded
with the Tab5's touch errors for 63.4.

Next: 63.4 (owner bypass and the chan_call wait helper; the touch I2C errors). Done, below.

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

**Done (2026-10-19).** The owner chose the FIFO (decision 1, §4).

`chan_call_wait(ep, …, wait_ms)` (kernel/chan.h, "Waiting for a turn"):
- A caller finding the endpoint busy joins its FIFO (8 places) and blocks until its turn or its
  deadline. The releasing caller hands the endpoint straight to the oldest live waiter, so it
  never goes free in between and a client calling back to back cannot barge.
- Refused at once, as before: the context that holds the endpoint (the recursive local 9P
  mount), no task, an interrupt handler, a spinlock held. A full queue polls at 1 ms.
- `chan_owner_exited()` also takes a dead task out of the queues and passes on a turn handed
  to it, or a claim it died holding.
- Cost: about 12 bytes per endpoint, one test under the spinlock chan_call() already took.
- `CHAN_WAIT_MS` (50) is the common bound. Block devices wait 500 ms (an SD write may stay
  busy 250 ms), local 9P 1 s. U-mode `SYS_CHAN_CALL` waits `CHAN_WAIT_MS`.
- `chanqtest` (kernel/chan_selftest.c, in the QEMU suite) checks arrival order, the hand-off
  against a back-to-back client, deadlines, plain chan_call()'s refusal, and re-entry. With
  the hand-off disabled, the first two fail.

Every hand-written retry loop is gone: driver_task_call, i2c_task_call, st7735, tm1638, spisd,
uart_rp2350, lcdterm, the clock driver, the local 9P link, loopback 9P, /srv writes. Three
uses of plain `chan_call()` stay, each with a reason in the code:
- the 16550 and P4 UART writers (`uart_tx_send`) wait only while a WRITE is in flight and
  never behind a READ that waits for a key (the RP2350's, whose ops never block, waits its
  turn in the queue: with a hand-off the endpoint stays busy while anyone is queued, and its
  old "yield while busy" loop could have starved);
- `spisd_probe()`: a busy endpoint means a card that answers;
- the lock selftest and `chanechotest` test chan_call() itself.

racescan section 3 now tells "waits" from "refuses" and counts hand-written retries (the
16550/P4 UART writer; none elsewhere). All ten builds: zero unmarked statics.

Findings, fixed:

| # | where | class | what | fix |
| :--- | :--- | :--- | :--- | :--- |
| F37 | drivers/virtio_blk.c | 2 | a request over four blocks went straight to the device while the task was alive: one descriptor chain and request header for both | chunks through the task; direct only without it |
| F38 | spisd, sdmmc, st7735, tm1638, the clock driver, the 16550/P4 `uart_getc` | 2 | a call that failed with the task alive (busy past a few yields) fell back to the hardware beside the task: the SPI bus driven by two at once (spisd has no bus lock), the LED matrix scanned by two, a second RX reader. Requests too big for the endpoint (st7735 bitmaps over 32 rows, long strings) went direct too | with the task alive a failed call is an error or a dropped draw; bitmaps go in bands, strings are cut at what the screen shows; getc waits its turn as long as it takes |
| F39 | drivers/i2c_bus.c (ESP) | 6 | `esp_i2c_run()` read the status, then the clock: preempted past its 10 ms deadline, it gave up on a status word read before the preemption and failed a finished transfer. This was the Tab5's touch I2C error and the BMI270's "incomplete" transfers (11.9 ms, INT_RAW 0x8211) | the clock first, then the status (`time_poll_until_us()`, kernel/time.h). The same order fixed in the bus clear, usbhost (5 ms channel halt, `wait_set`/`wait_clear`, port enable), sdmmc and sdio waits |

The I2C tool to find F39 stays: `i2cstats` counts failed transfers by cause (no turn, NACK,
timeout, arbitration, incomplete) with the last one's address, status and duration.

Class 2 in racescan's MMIO list: driver facades are done (above). The shell's `usb` and
`usb regs` only read registers that have no read side effects. The shell-only MMIO files
(clock, PSRAM, XIP, SMP, GPIO setup) are read-modify-write questions, for 63.6.

Tested:
- QEMU 514/514, with `chanqtest` 6/6 on rv64, rv32 and rv64-smp (second hart started); host
  harnesses clean under ASan/UBSan.
- Tab5 50/50, the first clean run since the touch error appeared. Over five boots: touch
  0 errors, `no_turn` 0, `incomplete` 0; before the F39 fix, 2 of 5 boots had incomplete
  transfers.
- LCD-7B 50/50, NANO 50/50, RP2350 terminal 34/34 (also after the RP2350 writer change).
- C6-MINI 20/21: the DHCP lease again, which a C6 bridging wlan0 does not take.
- rp2350-chess board 34/34: its st7735, tm1638 and sdblk tasks serve through
  `chan_call_wait()`. The preset had stopped linking (`prim_rgb` defined only for non-ST7735
  builds); fixed. The clock driver compiles (rp2350-clock), not run on hardware.
- LCD-7B again 50/50 on the final build.

The NANO's C6-MINI did not boot from flash: its slot A held `968.cfa5a878+`, a dirty build
from the 63.3 work, which stopped after `uart_init()` (breadcrumb 0x21) even after a power
cycle. The suites had passed only because a RAM-loaded kernel was still running. Rewritten
with `c6flash.py` (current build): it boots, joins and bridges by itself. C6 suite on the
flashed kernel 20/21 (DHCP, as above); NANO 50/50 with it.

### 63.5 Interrupt-shared state (class 4)

- Each handler's shared variables listed with the access width and order they need.
- Multi-word states (frame-buffer flip and index; ring head and tail; DMA descriptors) get a
  stated protocol, written once in the code: who writes, who reads, when it is consistent.
  Where a handler can see a half-done update, the update is made safe: a single word, or
  interrupts masked around it.
- The SDIO/LLP, USB host, DSI scanout and GDMA paths are read first. Part B adds the
  next ones.

**Done (2026-10-19).**

The handlers: nine attached through devirq, plus the timer tick:
- ESP32-P4: uart_isr (UART0 and USB-Serial/JTAG), gdma_isr, sdhost_isr.
- RP2350: uart_isr, edgecap_isr.
- QEMU: the 16550's uart_isr.
- ESP32-C6: uart0_isr, and the radio's ri_stub.
- The tick: preemption and timed wakeups, under g_sched_lock.

From racescan's data (Tab5, RP2350 terminal, C6, rv32, rv64-smp), 67 variables are shared
between a handler and tasks. Each has a verdict since 63.2. This pass checked that each
claimed protocol holds, and on how many harts.

**The finding is one class.** Several protocols relied on `irq_save()` to keep a task's
update apart from the handler, or from another task. That excludes only the task's own hart.
Every hardware target runs one hart by default (`harts_online: 1`; a second joins after
`smpstart`), so these held as shipped. rv64-smp runs two harts all the time, and a second
hart breaks them:

| # | where | what | fix |
| :--- | :--- | :--- | :--- |
| F40 | drivers/dsi_esp32p4.c | the terminal took a pending flip back with one store while `gdma_isr()` on the other hart, past its test, swapped the buffers: the next draw went into the new front (F3 again). `gdma_poll()` and `lcd db off` masked interrupts against a handler that could be running elsewhere | `g_scan_lock`, held by the frame end (the handler and `gdma_poll()`), the take-back and the index/item switch |
| F41 | drivers/uart_16550.c, uart_esp32p4.c | INT_ENA / IER read-modify-written by the handler and by the RX and TX waiters on up to three harts: one's enable could overwrite the other's, and that waiter then blocks with its interrupt off | `g_ier_lock` / `g_ena_lock` over every enable update and the waiter slot that goes with it; the handler wakes after unlocking (a spinlock is a leaf) |
| F42 | drivers/uart_rp2350.c | the same for IMSC | the RP2350's atomic set/clear aliases (+0x2000/+0x3000): one store each, no lock. `fence w,o` between the waiter slot and the enable; the handler reads the slot once |
| F43 | drivers/uart_net.c | the p9share demux (console ring, SLIP frame) protected against its two callers by masking, "there is one hart" | `g_demux_lock` |

Checked, and left as they are:
- `sdhost_isr`: it clears the card bit in SD_INTMASK while the LLP pump (any hart, under the
  host lock) sets it. Only that bit is ever set, so a lost update leaves the bit clear or set
  with `g_irq_seen` pending, and the next poll handles both. The protocol is now written at
  `sdhost_sdio_irq()`.
- klog, the lock checker, the scheduler and the clock: real locks, or per-hart slots (63.2).
- The C6's handlers: one core.
- The USB host: polled, no handler.

Tested: QEMU 514/514 (rv64-smp runs the 16550 change on two harts); Tab5, LCD-7B, NANO 50/50;
RP2350 terminal 34/34. Tab5 with `smpstart join` (two harts online): panel terminal bench,
console, LLP up, no trap or lock report. That is a smoke test; whether each racing path
actually ran on hart 1 is for 63.7's harness.

### 63.6 Read-modify-write and lock discipline (classes 5 and 6)

- Shared hardware words updated under a lock or through a single owner.
- **A lock-order record.** Each ylock gets a rank. `ylock_acquire()` checks the rank in
  debug builds and reports an inversion the way the checker reports Y3/Y5 (report and
  continue).
- Critical sections under `console_lock()` that sleep or run long (the rotation's full
  redraw is one) are measured with a hold-time histogram in `lockcheck`. Anything over a
  threshold gets a reason in a comment or is shortened.

**Done (2026-10-19).**

*Shared hardware words (class 5).* Clock gates and reset lines are the words several drivers
read-modify-write. I resolved every compound register update in the P4 build to its address.
Seven words in HP_SYS_CLKRST are written from more than one file:
- SOC_CLK_CTRL0/1/2, REF_CLK_CTRL2, ANA_PLL_CTRL0, HP_RST_EN0/1;
- EMAC's helper adds PERI_CLK_CTRL00/01 and the SDMMC/EMAC reset word.

Several of those drivers come up at run time (the panel on the first `lcd`, the LLP task's SD
host, `smpstart`, I2C's re-bring-up), so two updates could lose one: a gate closed again, a
reset left asserted. Fixed:
- `sysreg_update()` (kernel/sysreg.h), under one spinlock, for every read-modify-write in the
  HP_SYS_CLKRST and LP_CLKRST blocks. The drivers' field helpers route there by address;
  `sysreg_lock()` covers the multi-statement updates.
- The RP2350's GPIO_NSMASK0 (seven drivers) is written through its atomic set alias, which
  needs no password. RESETS already used the aliases.
- The C6's PCR is one word per peripheral.
- One CPU frequency switch at a time (`g_freq_lock`): `cpufreq` from two terminals
  interleaved the mux, divider and PLL steps.

*Lock order (owner's decisions: learned, on in every build).*
- When a context holding ylock A takes ylock B, the pair A->B is recorded once. A later B->A
  is reported once, report and continue.
- Locks are told apart by address. A try-acquire adds no pair into itself.
- 6-deep held stacks per context and 96 pairs: about 2.8 KB on rv32.
- `lockselftest` checks learning, reporting, re-entry and try-acquire.
- After every suite: the P4 boards 44–46 pairs, the RP2350 23, **0 inversions** anywhere.

*`console_lock` hold times.*
- `console_lock()` passes its caller's name. `lockcheck [pairs]` shows the histogram, the
  longest hold and the last holds of 100 ms or more.
- Found: on the Tab5 the panel's flush task held it 64–256 ms on every redraw that scrolled
  (the rotated blit), about 175 times per suite run, and every terminal waited behind it.
- Owner's decision: split the lock, and schedule the blit's speed-up (63.6b).

| # | where | what | fix |
| :--- | :--- | :--- | :--- |
| F44 | P4 clock/reset words, RP2350 NSMASK0 | read-modify-write from several drivers, some at run time | `sysreg_update()`; the atomic set alias |
| F45 | arch/riscv/common/clk_esp32p4.c | two `cpufreq` at once interleave a multi-step switch | `g_freq_lock` |
| F46 | drivers/dsi_esp32p4.c | the conversion ran entirely under console_lock | split: `term_snapshot()` (console_lock + `g_frame_lock`: the rows into the shadow, and a snapshot of the reader and canvas rectangles, their backings and a copy of the visible cells) and `term_draw()` (`g_frame_lock` alone); every frame-side change takes both (`frame_begin()`), and readers and canvases free their pixels under the frame lock (`console_frame_lock()`) |

After the split, on the Tab5: holds of 64–256 ms went from about 175 to 21 per suite run, and
the flush task is no longer among the long holds. What remains has a reason in the code:
- `term_sync_post` (~170 ms): a synchronous full redraw for animations and rotation.
- `term_colours` (~100 ms): a command.
- `term_bench` (seconds): a benchmark of the console path itself.
- `lcdterm_canvas` (745–836 ms): one canvas operation from Lisp in the RGB565 canvas test. Not
  explained yet (the fill itself is a plain loop); measured in 63.6b.

A screenshot now converts what is pending and waits for the flip before it reads the front
buffer. Before, it caught whatever the paced conversion had last put on the glass, so a
command's own echo was there or not depending on timing.

LCD-7B, lcdterm_shots terminal scenario, before and after the split, 1-bit compare: identical
but for the status bar and one 16-row band per shot (the echo above). The tool itself only
reads 1-bit PBM; screenshots have been P5/P6 since phase 55, so it needs updating (63.6b).

Tested: QEMU 514/514; Tab5, LCD-7B, NANO 50/50; RP2350 terminal 34/34.

### 63.6b The panel's conversion cost (scheduled by the owner)

- The Tab5's rotated blit, now 100–250 ms for a full redraw: word-wide 8x8 transposes instead
  of per-bit work, aiming at the LCD-7B's 8–16 ms.
- `term_sync_post`'s full redraw falls with it.
- Measure the 745–836 ms `lcdterm_canvas` hold (an RGB565 canvas opened and filled from
  Lisp).
- `tests/hw/lcdterm_shots.py --compare`: read P5/P6, and run on the Tab5.

**Done (2026-10-19).**

*The 800 ms "canvas operation"* was not a drawing primitive. Ops '<' and '>' are Cmd+Left and
Cmd+Right, and they run the ribbon's slide (`screen_animate_ribbon()`): six full frames
under console_lock, each a full rotated conversion.

*What a conversion costs, measured.* PSRAM through the cache does 43 MB/s writing (`psram
bench`, which now also times `memset` and `memcpy`: 43 and 27 MB/s) and 55 MB/s reading.
The conversion runs at that rate. Per full redraw on the Tab5, before:
- 55–110 ms for the 1-bpp blit;
- 50–130 ms for the anti-aliased cells.

Both wrote along the rotated frame's columns: a cache line filled per 16 or 64 bytes. And
both wrote the text areas, the second over the first.

Changes (drivers/dsi_esp32p4.c):
- The rotated 1-bpp blit walks the frame row by row (each row front to back once) with a
  64-bit 8x8 transpose. A host test compared it with the old loop over 300 random frames in
  both rotations: identical.
- With the anti-aliased pass drawing the text, the 1-bpp pass leaves the visible text areas
  alone. They lie on the 8x8 grid, so a unit is inside or out.
- The anti-aliased cells, rotated, are drawn column by column: each frame row front to back
  once.
- The cell cache is per window (four slots, in PSRAM, sized for 160 x 45) and is dropped
  when its window's geometry changes. It was one array shared by all windows: a cell of a
  second window could match the first's and not be drawn, which the 1-bpp layer underneath
  had been hiding (a non-anti-aliased glyph showing through).

| Tab5, rotated | before | after |
| :--- | ---: | ---: |
| full redraw, 1-bpp pass | 55–110 ms | 1–10 ms |
| full redraw, cells | 50–130 ms | 40–80 ms |
| full redraw at boot, total | ~127 ms | ~63 ms |
| one slide frame | ~135 ms | ~87 ms (render 12, convert 70, flip 5) |
| slide (Cmd+Left/Right) | 784–822 ms | 646–666 ms |
| `lcd termbench` | 2.6 s | 1.65 s |

The LCD-7B's forced redraw (not rotated) is about 30 ms for a full frame: 1.2 MB of RGB565 at
the measured bandwidth (half in GRAY8). The rest of a colour change is clearing both buffers
when the format switches (gray <-> RGB565). That stays: without it the new buffer shows stale
PSRAM for a frame. So both panels' redraws are now near the bandwidth. What would shorten
them further is a faster PSRAM path. At hex DDR 200 MHz, 43 MB/s is a fraction of the raw
rate, and the cache configuration is the same as IDF's default (128 KB L2, 64-byte lines).

*`lcdterm_shots.py`:*
- It reads P4, P5 and P6 and compares them as RGB, noting a format change.
- It masks the status bar's right end, clock and link icons, scaled by the cell height.
- `--board tab5` turns the portrait frame back to landscape and fetches over 9P on TCP with
  a one-time key: `p9share` does not demultiplex the Tab5's USB-Serial/JTAG. The fetch needs
  the Tab5's networking, which does not transmit yet (LLP up, 16 frames sent against 2,304
  received).
- LCD-7B, before and after this work: all 8 shots identical.

The lock-order record caught a mistake on the way: a debugging printk in `term_draw()`,
before klogd, drained the log inline and took console_lock under `g_frame_lock`, the wrong
way round (66 inversions in one run). `term_draw()` prints nothing; that is in its comment.

Tested: Tab5, LCD-7B, NANO 50/50; 0 inversions.

*The PSRAM path (owner: investigate now).* An IDF build of the same benchmark (bootloader,
partition table and app written below LugalOS's filesystem on the NANO, LugalOS restored
afterwards; a RAM-loaded IDF app does not start on the P4) measured:
- IDF: word write + write-back 82 MB/s, word read 102, `memset` 84, `memcpy` 56;
- LugalOS on the NANO: 79, 101, 80, 50.

So the PSRAM setup is at parity. Every MSPI, clock, divider, mode-register and cache setting
was compared with IDF's and matches.

The Tab5's half rate (43 MB/s `memset`) is its panel. RGB565 scan-out at 1280 x 720 and
58 Hz reads 107 MB/s from PSRAM, and every transaction pays a fixed cost. The LCD-7B shows the
same in between: 75 MB/s with GRAY8 scan-out, 65 with RGB565. A longer AXI read burst for the
scan-out (32 beats instead of IDF's 16) gave the Tab5's CPU 57–60 MB/s (+35%), with no DMA
error, no underrun and an exact frame readback, but the panel went dark. Found by the owner
looking at it; reverted to 16. No automatic check sees what the panel shows: the suite's
readback reads the frame from memory.

What remains is the scan-out itself: a lower refresh rate, or a smaller pixel format where
the bridge takes one (the Tab5's v1.3 bridge is used in RGB565), would each return bandwidth.
Those change what the panel shows and are for the owner. The NANO's flash keeps IDF's
partition table and NVS in the unused tail of LugalOS's boot region (`0x8000`–`0x10000`);
nothing reads it. NANO 50/50 afterwards.

*63.6 closed (2026-10-19), owner's decision:* the refresh rate and the pixel formats stay as
they are; the panel's share of the PSRAM bandwidth is accepted.

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

*Status (2026-10-19, end of day): harness done, found bugs fixed, hour runs not yet passed.*

Done:
- **`stress [s] [norot] [only=fmt,ram,sd,p9,chan,log,shell]` / `stress bg ...`**
  (kernel/stress.c): mixed worker tasks, each picking a path at random every iteration
  (formatter, files on `/ram0` and `/sd0`, the local 9P server, `chan_call_wait()` into an
  endpoint that holds its turn 0-2 ms, log bursts, shell commands under capture; on the Tab5 a
  task of its own turns the panel every 7 s unless `norot`). Every result is checked; at the end
  the lock faults, order inversions, I2C failures, klog gaps, full stacks (`sched_stack_full`,
  the canary half of this item; `ps` shows the high water) and stuck workers since the start.
  A worker per path did not fit: the boards run 16-18 tasks of their own out of 24, and with
  three terminals the LCD-7B has three slots left (the minimum).
- **`tools/stress.py --board B --minutes N [--norot] [--only ...] [--bare]`**: around
  `stress bg`, two more terminals, focus moved and commands typed (focus back on terminal 1
  for the last 30 s: the serial console shows the focused terminal, and the report goes to the
  one that started the run), `sensor period 2` (new), 9P over TCP from the host with a
  one-time `p9key`, then `dmesg`, `lockcheck`, `i2cstats`, `ts` checked.
- **runner.py:** `stress 30` on rv64 and rv32, `stress 60` on rv64-smp (517/517 before the
  last round of changes).
- **Found by it, fixed:**
  - `p9_link_cat()` sessions on one link ran concurrently and shared fids 1/2 and Tversion: the
    whole session now holds `g_client_lock`.
  - `sched_current_pid()` / `cur()` and `devirq_in_handler()` read `g_current[hart_id()]` with
    interrupts on: a task preempted between the two reads and resumed on the other hart read the
    other hart's task (a 9P fid filed under no connection, about once in two minutes on
    rv64-smp). Read under `irq_save()` now; four 300 s rv64-smp runs clean afterwards.
  - `p9_link_cat()`'s two message buffers came from the fast page zone, which three terminals
    and the workers exhausted on the LCD-7B (every session failed): from the bulk zone now.
  - `stress`, `chanqtest`, `lockselftest`, `lockcheck` fell through to the Lisp evaluator after
    running ("Unbound function").
  - **Tab5 rotation overflowed the terminal shadows** (owner saw stale text in the lower half
    after a turn): each terminal's shadow share was sized for the boot orientation (78 x 21) and
    `vtterm_resize()` let a portrait window grow to 38 rows inside it -- rows past the 21st went
    into the next terminal's share, the last terminal's past the array; and a whole-screen scroll
    region kept its old bottom row. Now a backend that `rotates` gets shares sized for both
    orientations (`SCREEN_BYTES_ROT`, `screen_init_rot()`), a window is never taller than its
    shadow (`shadow_rows`), a whole-screen region follows the screen, and `screen_reorient()`
    resizes hidden terminals through `vtterm_resize()` too. vtselftest checks it; the owner
    confirmed all four orientations.

Hour runs (2026-10-19, 60 min each, with terminals, typing, 9P/TCP, sensors at 2 s):
- **NANO:** STRESS_FAIL -- `ram` 2 failed writes to `/ram0` in 388,907 (first: "write");
  everything else clean (sd 96,521, no errors).
- **LCD-7B:** STRESS_FAIL -- only 1 worker (task table full: something held more slots than in
  the 3-minute trials) and 1 local 9P read failure in 38,361; sd 12,778 clean.
- **Tab5:** stopped after ~14 min: the panel was black (see below). A 20 min run without
  rotation: `sd` **5 read-back mismatches** in 17,874 (no driver error logged); with only the sd
  and p9 paths (+ 9P/TCP, rotation) 60,213 sd cycles clean -- the mismatch needs another path
  at the same time.

*2026-10-20.* The open items, worked through:

1. **Tab5 black panel at boot.** A TSL2591 on the Grove port (bus 1), placed against the
   glass, reads the panel: ~180 lux lit, ~2.6 lux black with the backlight on, ~0.9 lux backlight
   off. `tools/panelboot.py --board tab5 --boots N` resets the board N times and counts lit and
   black boots from `sensor lux` with the backlight on and off (new: the TSL2591 driver searches
   every bus and has a device lock; `sensor lux` reads on demand). Back-to-back USB resets make it
   frequent: 11 black of 30 on the old code (the owner had seen ~1 in 10).
   `lcd reinit video|panel|phy|link` (new) redoes part of the bring-up on a running board. In the
   black state `video` and `panel` (the panel's reset and init table) do not bring the picture
   back; `phy` (the DSI PHY and timing, the panel left alone) does. **The panel is fine every
   time; on some boots the P4's DSI PHY comes up wrong, and a second setup is right.**
   **Not fixed.** The rate drifts over the day with the same code (11, 10 and 3 of 30 at
   different times), so a 30-60 boot run cannot show a small effect, and four changes that each
   looked like the fix in one run did not hold:
   - host and PHY powered down before setup (IDF's teardown order): 29/30 lit;
   - plus 5 ms for the PHY's LDO3 to settle: 60/60, then 10/30 in the afternoon;
   - plus the PHY's test interface cleared (TESTCLR) before the PLL is programmed, as Linux's
     dw-mipi-dsi does: 30/30 and 58/60; control without it in the same hour 27/30;
   - plus a second PHY setup at boot after the panel's init: 59/60.
   - M5Stack's own sequence (M5GFX.cpp, Tab5): the panel reset once, before the PHY, LCD_RST
     released by its pull-up rather than driven high, the touch side waited for, then the
     PHY, then 80 ms before the first command: 54/60. Kept: it is the vendor's sequence.
   M5Stack's Tab5 demo (M5Tab5-UserDemo) has no ST7121 bring-up of its own (its BSP drives the
   older ILI9881C panel); its SD setup (slot 0, 4-bit, 40 MHz, pads on LDO_VO4 at 3.3 V) and
   esp-hosted's SDIO settings (slot 1, 4-bit, 40 MHz, same pins, C6 reset on GPIO15) match
   ours except the card's 20 MHz here. Next for the panel: the factory firmware through the
   same reset series, counted by eye -- if it goes black too, the cause is not the bring-up.
   The first three stay (harmless; the order IDF and Linux use); the fourth was removed. A
   second setup at run time (`lcd reinit phy`) has brought the picture back every time (5 of 5).
   No boot-log line and no event time differs between lit and black boots (153 lit, 27 black
   compared).
   The ST7121 always answers 0 ms after its boot-time reset (the early expander setup has
   already reset it cold), lit or black; M5GFX's timing is ours already (its 70 MHz pixel
   clock is 240/3 = 80 MHz through IDF's rounding too).
2. **NANO `/ram0` write failures: found.** The VFS has 8 file handles for the whole system;
   with four workers, the shell, the 9P server and the power log the table filled and
   `vfs_open()` failed at once. Now an open with every handle in use waits up to 500 ms for one
   to close, and the table's peak, waits and give-ups are counted (`vfs_handle_stats()`, in the
   stress report). NANO 10 min: peak 8 of 8, 1 open waited, 0 failed, STRESS_OK. LCD-7B 10 min:
   peak 5, STRESS_OK, 5718 9P/TCP reads clean. `VFS_MAX_HANDLES` stays 8 (each handle is
   1.1 KB, mostly its /proc buffer): a decision for the owner if waits become common.
3. **Tab5 SD corruption: found and fixed -- concurrent cache maintenance.**
   The path there (all of it kept, each piece cheap):
   - A read-back check in the SD facade: a checksum of every sector written, compared with
     every later read of it (`sdinfo`: count, and per mismatch the sector, the first bytes,
     the callers and times of the expected and of the matching write). It caught it: 20
     mismatches in a 20-minute run, all one sector, the root directory's second (lba 30497),
     which read back the bytes `fat_set_entry()` had written to FAT sector 202 thirteen
     milliseconds before -- and those were the directory sector itself. So in one operation a
     read of FAT sector 202 returned the directory sector read just before it (the FAT cache
     filled with directory data, then written into the FAT), and the next write of the
     directory sector sent the old bytes (a deleted entry came back).
   - Both are what a lost cache operation on the SD bounce buffer does: a lost invalidate
     hands back the previous transfer's bytes, a lost writeback sends them. The P4's ROM
     `Cache_WriteBack_Addr` / `Cache_Invalidate_Addr` program one set of cache-controller
     registers; our wrappers called them with no lock, so the panel's flush task writing back
     frame buffers and the SD driver invalidating its buffer could overwrite each other's
     setup. IDF wraps every such call in a spinlock critical section (esp_cache_msync()).
     It needed heavy panel output at the same time as SD traffic: the `log` path's bursts
     drawn on the Tab5's rotated terminal. The NANO has no panel; the LCD-7B's flushes are
     smaller and fewer.
   - Fix: one spinlock, interrupts masked, around every ROM cache operation after boot, large
     ranges in 32 KB chunks (arch/riscv/common/trap.c). Same 20-minute setup: 0 mismatches in
     389,216 reads (20 in 484,833 before). Full `stress.py` run (terminals, typing, 9P/TCP, power log on): STRESS_OK, sd 23,416 iterations, 0 errors -- the first Tab5 pass.
   Ruled out on the way, each by a run that still failed: the C6 link (LLP off), the power log,
   a reply reaching the wrong `sdblk` caller (request tags, 0), lock release by a non-holder
   (now a lock fault, 0), mount references, VFS handles, stack depth, the card (`sdbench`),
   and the 40 MHz high-speed mode (CMD6 switch, kept: reads 46 % faster). Also fixed on the
   way: SD/SDIO reads completed on DATA_OVER before the IDMAC released the buffer (the
   descriptor's OWN bit is now waited for), and FAT32 ignored device errors.
   Found on the way, not fixed: the stress `log` path outruns the Tab5's USB-Serial/JTAG
   console; with no reader the console driver spins on a full FIFO and slows everything, and
   a few times the USB console stopped answering until the cable was replugged (the board ran
   on; the keyboard worked).
4. `tools/stress.py` refuses a board that does not answer (one left in download mode by a
   flash ran 10 minutes against nothing).

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

1. ~~Whether `chan_call` should queue waiting callers (63.4), once the cost is known.~~
   Decided 2026-10-19: a FIFO, opt-in through `chan_call_wait()`.
2. ~~Whether the lock-rank check stays on in release builds or is debug-only.~~ Decided
   2026-10-19: a learned order rather than ranks, on in every build.
3. ~~Whether `racescan.py` failing the runner is acceptable from 63.8 on.~~ Decided
   2026-10-20: yes, with a message that explains the system to someone who does not know it
   (`racescan.py --check`, five builds, ~100 s in parallel).
4. ~~Task stacks from PSRAM for the LCD-7B's page shortage.~~ Decided 2026-10-20: stacks stay
   in SRAM (grown where needed). The VFS handle's /proc buffer moved out of the handle (a page
   taken at open from the bulk zone, else SRAM): a handle is ~250 B instead of 1.1 KB, which
   fixed /proc/ps's cut-off at 13 tasks; the handle count (8) stays until the owner raises it.
