# Phase 23 — Waking the second core

**Status: COMPLETE 2026-09-04 — X1 through X7 all done, on branch
`feature/multicore`.** Two harts share one ready queue and one scheduler
lock (X1); driver tasks are pinned, and what that does *not* cover is
written down (X2); RP2350's core 1 provably executes our code, launched
over the SIO FIFO (X3), and needs no separate preemption timer (X4); the
isolation and fault suite has been re-run with both harts demonstrably
mid-task at the instant of each fault (X5); and a hart in bring-up no
longer impersonates the boot task (X6, added after X5).

**Core 1 schedules tasks on real RP2350 silicon** (X7): two Hazard3 cores
share one ready queue, PMP domains are enforced on the secondary, and the
whole isolation suite passes with both cores busy. The bug that stood in the
way was a tight test-and-set spin starving the other core — invisible on
QEMU, fatal on one bus.

Planned 2026-08-29, amended 2026-09-03 (§6); §1's premise that RP2350
boots both cores was falsified by X3 and corrected where it was made.
Depends entirely on `plan/phase22_smp_locking_foundation.md` (S1-S6)
being complete: every shared structure this phase's second hart could
touch must already be protected by a real cross-hart lock before that
hart is ever allowed to run kernel code, or waking it converts a latent
race into an active one on day one. Sequenced, like phase 22, after
phase 19's R4/R5 and phase 21's I7 — all three blocked on hardware access
today, none of them blocking on this phase, this phase blocking on all
of them by project priority rather than by technical dependency.

**Scope.** Actually run kernel code on RP2350's second Hazard3 core:
launch it, give it a scheduler to pull work from, and prove two tasks
run concurrently rather than merely interleaved.

**Out of scope:** more than two cores (RP2350 has exactly two); NUMA-style
memory affinity (RP2350's SRAM is uniformly reachable from both cores —
not a problem this silicon has); load balancing or work stealing; any
locking primitive work (that's all of phase 22, finished before this
starts).

---

## 0. Why this is its own phase

Once phase 22 makes locking real, "run something on the second core" is
two genuinely different kinds of work, and conflating them would make
this phase impossible to falsify honestly. One half is a **RP2350-specific
hardware launch sequence** — releasing a parked Hazard3 core over the SIO
FIFO is real silicon behavior this project's own history says cannot be
trusted without measuring it on the actual chip
(`[[falsify_on_hardware_not_qemu]]`: "QEMU has six times hidden a Hazard3
divergence"). A second QEMU-emulated RISC-V hart is not the same claim as
a second real Hazard3 core, for exactly the reasons phase 12 already
documented once for interrupts (Xh3irq vs. PLIC, found by hand against
the Pico SDK's own register headers, not guessed).

The other half is a **generic question about the scheduler**: does two
ready tasks on two harts, sharing one lock-protected queue, actually run
concurrently without corrupting shared state? That question has nothing
RP2350-specific in it, and QEMU's own `riscv "virt"` machine already
supports it directly — `qemu-system-riscv64 -M virt -smp 2` boots two
harts on hardware this project already targets (RV64/Sv39), with no new
target, no new toolchain, and no board access required. Splitting the
phase this way means the part that can be falsified today, on QEMU,
before any hardware is available, is falsified first — and the part that
genuinely cannot be is named as blocked rather than quietly skipped or
quietly assumed to work because the QEMU half passed.

## 1. What's already true and doesn't need to change

- **PMP/Sv39 activation is already hart-local.** `arch/riscv/common/mem_domain.c`
  writes PMP `pmpaddrN`/`pmpcfg0`/`pmpcfg1` and the Sv39 `satp` CSR
  directly — ordinary CSR writes, architecturally local to whichever hart
  executes them. A second hart activating its own current task's domain
  is calling the *existing* `mem_domain_activate()` on itself; this phase
  adds no new isolation mechanism, only a second caller of the one that
  exists.
- **Interrupt controllers are already per-hart-addressable in principle**
  — RP2350's Hazard3 Xh3irq array (`meiea`/`meinext`) and QEMU's PLIC are
  both already handled in `arch/riscv/common/trap.c`, verified once
  against real register headers (phase 12). Whether the existing
  claim/complete logic generalizes correctly to a nonzero hart is an open
  question this phase answers, not an assumption it makes. **Amended
  2026-09-03:** the hardware is per-hart-addressable; the *code* is not —
  `QEMU_PLIC_CONTEXT` names hart 0 in three separate address
  computations. See §6.1.

## 2. Design questions this phase has to answer, not defer

- **One shared ready queue vs. two independent per-core queues.** Start
  with one queue, protected by phase 22's lock. This matches the
  project's existing preference for one correct mechanism over an
  optimization nothing has justified yet (the same argument
  `kernel/chan.h`'s copy-always design already made for local vs. remote
  IPC being the same code path) — two harts pulling from a table bounded
  at a couple dozen tasks is not a contention problem worth solving
  before it is measured to be one.
- **Driver task affinity.** Every RP2350 driver task today (console/UART,
  USB CDC, SD/SPI, I2C RTC/EEPROM, ST7735, TM1638, heartbeat — phase 12's
  M5 list) has only ever run on core 0, because there was only core 0.
  Recommend pinning driver tasks to core 0 explicitly for the first cut —
  a one-field addition to the task structure — so "which hart is this
  peripheral register access happening from" stays a question with one
  answer while everything else about this phase is new. Only
  user/Lisp-spawned tasks become eligible for core 1 initially.
- **Core 1's boot stack.** Phase 15 already found and wrote down that
  core 1 "never takes a stack at all — it branches straight to `wfi` and
  spins." Waking it means giving it one, sized with the same discipline
  phase 12's M5 heap-reclaim work used for every other stack in this
  tree: measured against a real worst-case call depth, not guessed.
- **Per-core preemption timer.** `kernel/ticker.c` currently assumes one
  timer driving one hart's preemption. Whether RP2350 gives each Hazard3
  core independent timer access, or one shared timer has to interrupt
  both, needs measuring against the RP2350 datasheet and the Pico SDK's
  own register headers — the same standard phase 12 already held Xh3irq
  to, not an assumption carried over from the single-core design.
  **Amended 2026-09-03:** independently of how that question resolves,
  `arch_ticker_init()` programs a chip-wide tick generator and calibrates
  it with a busy-loop, so core 1 must never re-run it. See §6.2.
- **A build-time gate.** `CONFIG_ENABLE_SMP` (phase 8's feature-flag
  mechanism), off by default, wrapping all of this. A regression in
  a second-core code path must not be able to put every other board
  persona's single-core boot at risk — the same reasoning phase 8 built
  the mechanism for in the first place.

## 3. Milestones

**X1 — generic two-hart scheduler bring-up on QEMU.** `-M virt -smp 2`,
RV64 only, no RP2350 involved. Core 1 given its own boot stack, pulling
from the one shared ready queue under phase 22's lock.
*Verify:* a QEMU-only test proving two tasks are *demonstrably* running
concurrently, not merely interleaved — two tasks each incrementing a
shared counter through the new lock, checked for lost updates under real
two-hart contention, which is the first time anything in this project's
history has been able to make that check for real rather than by
argument. This is also where phase 22's S6 (the scheduler lock's hand-off
across `ctx_switch()`) gets its first genuine test.

> **Amended 2026-09-03 — see §6.** X1 has two prerequisites this milestone
> did not originally name: phase 22's new S0 (per-hart state — core 1
> cannot identify itself today, §6.2 of that document), and §6.1 below
> (the PLIC context is hardcoded to hart 0, so core 1 would take no
> interrupts at all, including no timer). Note also that phase 22's S6
> rule was *inverted* in the original text; the lock is held across the
> switch and released on the incoming stack. Read
> `plan/phase22_smp_locking_foundation.md` §6.1 before writing X1's test,
> since the canary it originally described would fire on correct code.

**DONE 2026-09-04.** Two harts, one shared ready queue, `CONFIG_ENABLE_SMP`
off by default. `cmake --preset rv64-smp` builds it; the suite runs it as
its own target under `qemu -smp 2` and skips it when that build is absent.

```
workers=4 iters=20000  locked=80000 (want 80000)
                       unlocked=79971 (lost 29)   harts=2
```

The unlocked counter is the point of that line. Without it, "80000 of
80000" only shows that nothing went wrong; with it, 29 lost updates prove
there was real contention for the lock to have prevented. Phase 22's claim
is tested against a second hart for the first time, including S6's
hand-off canary, which on one hart could only ever catch a coding mistake.

**§1 was wrong by omission, and it cost the most time.** It listed
"PMP/Sv39 activation is already hart-local" under what does not need to
change. True of `mem_domain_activate()`, and irrelevant, because a
secondary hart was never in the kernel's *own* address space to begin
with: `satp` is per-hart, so hart 1 ran in bare mode while hart 0
translated. It started, printed, entered the scheduler, and then resumed a
task whose saved context held addresses it could not translate. The dumps
showed one symbol as both `0x80235c90` and `0xffffffff80235c90`, which is
what gave it away. `vmm_secondary_init()` fixes it.

**Two more bugs, both real, neither anticipated:**

- **`tp` was restored from the trap frame on kernel returns.** A task
  preempted on one hart and resumed on another would restore the *old*
  hart's record, so hart 1 adopted hart 0's identity, read the wrong
  `g_current[]`, and both harts re-ran the primary's init in a loop. The
  frame's copy names whichever hart took the trap; the correct action on a
  kernel return is none at all, since `tp` already holds this hart's
  record.
- **`sched_yield()` could switch a task to itself.** `next_runnable()`
  wakes any sleeper whose deadline passed *including the caller*, so a
  task inside `task_sleep_ms()` could be returned as `next == prev`.
  `ctx_switch(&X.sp, X.sp)` is not a no-op: it stores the current sp over
  `X.sp` and restores from the value already read into `a1`, an older
  frame of the same task. **Pre-existing and not an SMP bug** — on one
  hart it needs a sleeper to expire while nothing else is READY, which the
  shell and p9srv normally prevent. A second hart runs out of work far
  more often, which is how X1 found it.

**X4 is partly answered, by need.** `g_ticks` became per-hart. S5 had
recorded it as racy and left the choice between one atomic counter and one
per hart to X4; X1 forced it, because `lockselftest`'s masking check holds
a spinlock with interrupts off and asserts the tick counter is frozen —
and a *global* counter keeps advancing from the other hart's timer, failing
a kernel whose masking is perfectly correct. Per hart is also what every
reader already meant.

**A test was wrong, not the kernel.** `lockselftest` asserted that
immediately after the final `ylock_release()` the lock reads free. With a
second hart the waiter is already inside it by the next statement, so the
assertion observed an intermediate state a concurrent system may change at
any moment. Removed; the release is proven by the waiter getting in, which
was already checked.

*Verified:* QEMU **328/328** (324 + 4 SMP), five consecutive runs. RP2350
chess persona **24/24** with the gate off, `lockselftest` 7/7 on silicon.
Static RAM **+208** on RP2350 where SMP is *not* enabled: `hart_affinity`
across 24 task slots (+192, mostly alignment padding), per-hart `g_ticks`
(+8), and `g_smp_release` (+8). The `smptest` statics are inside the gate,
and on a non-SMP build the command reports SKIPPED rather than failing by
design.

**Affinity arrived early, in X1 rather than X2.** Not tuning: task 0 runs
on the linker's boot stack and each secondary's idle task on
`.stack_secondary`, so letting another hart pick one up puts two harts on
one stack. X2 extends the same field to the driver tasks.

**X2 — driver-task affinity.** Pin existing driver tasks to core 0;
confirm phase 12's per-task PMP/Sv39 isolation guarantees hold regardless
of which hart activates a given domain (§1 says they should; this
milestone is where that claim gets checked rather than assumed).

**DONE 2026-09-04.** Twelve driver tasks pinned — both UARTs, heartbeat,
usbcdc, both block drivers, i2c, st7735, tm1638, clock, wifiup, p0log —
via `task_create_driver()` rather than a `task_set_affinity()` call after
each `task_create_sized()`. Creating and pinning are one act on purpose: a
window in which a driver task is briefly migratable is the sort of thing
that works until it doesn't, and a helper is what stops the next driver
half-doing it. `ps` grew a **Hart** column, so the policy is a fact about
the running system rather than a claim about the source — the argument
phase 12's M6 made for `Isol`:

```
  0  RUNNING  kernel   -   -     0     -
  1  BLOCKED  uart     -   -     0     744/4096 B
  2  READY    idle     -   -     1     -
  3  BLOCKED  blk      -   -     0     720/4096 B
  5  READY    p9srv    -   -     any   648/12288 B
```

**The isolation claim is evidenced, not just unbroken.** "The isolation
tests still pass" would prove nothing here: with drivers pinned to hart 0,
a run can satisfy every check while never installing a restricted domain
anywhere else. `mem_domain_activate()` now counts activations per hart and
`/proc/cpuinfo` reports them, so the suite asserts both halves — an
out-of-domain store still faults on a two-hart kernel, **and**
`domains_hart1` is non-zero, meaning the isolation being checked was
installed by code running there. Measured: `domains_hart0: 5,
domains_hart1: 7` after two U-mode programs.

**X2 disproved part of S5's audit, which is what it was for.** S5 left the
UART paths unconverted on the strength of this pinning. Checking rather
than inheriting shows it covers the waiter path but **not** `g_tx_batch`:
`uart_flush()` and `uart_putc()` run in the caller's context — every
console write, and `printk_unlock()` — so two harts share that buffer.
Same shape as `usb_cdc_putc()`, missed for the same reason: the
disposition asked which file the state lived in rather than who calls it.
Both UARTs' batches now take a `spinlock_t`; phase 22's §S5 entry is
corrected in place rather than left to read well.

*Verified:* QEMU **330/330** (328 + 2 X2 checks). RP2350 chess persona
**24/24**. Static RAM **+12** — the per-hart activation counters (+8) and
one `spinlock_t` (+4); the other UART's lock and `hart_affinity` were
already counted in X1.

**X3 — RP2350 core-1 wake. Hardware-only, BLOCKED until bench access
exists** — the same constraint named in `plan/phase21_identity_and_authentication.md`'s
I7, for the same reason. The SIO FIFO launch sequence, `CONFIG_ENABLE_SMP`-gated.
*Verify:* on real hardware, proof core 1 executed anything at all (e.g. a
counter core 1 increments that core 0 reads back) — cannot be verified
any other way, and is not claimed done until it is.

**DONE 2026-09-04 — hardware-verified on a Pico 2 (chess persona).**

```
[SMP] core1 ticks before launch: 0
[SMP] launch handshake completed
[SMP] core1 ticks after 50ms: 1310682 (delta 1310682)
[SMP] CORE1_ALIVE -- a second Hazard3 core is executing our code
core1_probe: 0x51c0de02
```

Exactly this milestone's bar and no more: core 1 executes our code and
counts. It does **not** join the scheduler. Build with `cmake --preset
rp2350-smp`; `smpstart` in the shell performs the launch.

*The phase's premise was wrong.* `plan/phase22_smp_locking_foundation.md`
§1 said "RP2350 already boots both of its Hazard3 cores ... and parks
anything nonzero in a `wfi` loop", and both boot paths carried a comment
saying the same.
A three-instruction probe — a `.smpmark` write on the secondary path, into
a `(NOLOAD)` section placed where no boot path clears it (above
`__bss_end`, outside the painted stack) — read back **0**. Core 1 had
never executed a byte of this image; it waits in the bootrom, so the
`mhartid` branch in `_reset_handler` has been dead code since it was
written. Re-reading the comment could not have settled that.

*The launch protocol was transcribed from the Pico SDK, not inferred:*
the six-word sequence `{0, 0, 1, vector_table, sp, entry}` pushed through
the SIO FIFO (`SIO+0x50` ST, `+0x54` WR, `+0x58` RD) with an echo
handshake that restarts from the top on any mismatch. On RISC-V the third
word is `mtvec` where Arm uses VTOR. Hazard3's SEV equivalent is
`slt x0, x0, x1`, a hint encoding. `core1_entry` writes its marker
`0x51C0DE02` as its **first instruction**, before touching `tp`, `mtvec`
or `mstatus`, so "the core started" and "the core got through setup" stay
distinguishable.

*Core 1's stack is SCRATCH_Y* — exactly the case `linker/rp2350.ld`
already anticipated in a comment ("take it back out of the heap here"),
and unstriped, so the two cores do not contend for it the way they would
on striped SRAM0-7. Heap **90 → 89 pages**. Static RAM **+0** with SMP
off: it all compiles out.

*Two mistakes worth recording, both of which cost a BOOTSEL recovery.*
(1) A budget that is not a single counter is not a budget — the first
handshake wrapped two 100k inner waits in a 2M outer guard, a product of
4×10¹¹, i.e. no bound at all. It is now one shared budget. (2) Launching
at boot, before a console existed, wedged the board into total silence.
The launch is an explicit shell command now, because a board that boots is
a board that can be reflashed.

*Core 1 only counts, deliberately.* The first attempt sent it straight
into `secondary_main()` — `trap_init`, ticker, scheduler, `printk` — and
wedged; with everything downstream silent there was no way to tell a
handshake that never started the core from a core that started and died.
A bare `for(;;) g_core1_ticks++;` separates those two outcomes. Two
blockers for the scheduler step were recorded here for X5's successor:
`printk()` from core 1 reaches core 0's pinned UART task through a blocking
`chan_call`, and `task_block()` before `sched_secondary_init()` would block
task 0, since `g_current[1]` is still 0.

> **Corrected 2026-09-04 — the first of those two is not a blocker, and the
> second is bigger than it was written.** Measured on `rv64-smp`: a U-mode
> task pinned to hart 1 printed through the whole path and the UART task's
> own server-side counter advanced by 27 (`uartstats`, 27 → 54). A blocking
> `chan_call` from a secondary hart to a hart-0-pinned driver task *works* —
> the caller's task blocks, hart 0 serves it, the reply wakes it. That was
> assumed to be a problem without being checked.
>
> The real blocker was the second one, and it is not specific to
> `task_block()`: **a hart that owns no task reported itself as task 0**,
> because `sched_current_pid()` read `g_current[hart]`, which `sched_init()`
> set to 0 for every hart. So everything in the bring-up window acted on the
> boot task — `task_block()` blocked it, and `printk_lock()`'s ownership test
> matched a lock hart 0 was holding and took the re-entrant path, putting
> both harts inside the console's exclusive region. See §3's identity-fix
> entry below.

*Verified:* RP2350 chess persona **24/24** on the X3 build (core 1 not
launched); board stayed responsive after `smpstart`. QEMU **330/330**.
`rp2350-chess` and `rp2350-clock` still build with `CONFIG_ENABLE_SMP=0`.


**X4 — per-core preemption timer**, if §2's open question about RP2350's
timer hardware requires one. May turn out to be unnecessary depending on
what X3's own hardware investigation finds; written as a separate
milestone rather than folded into X3 so a "not needed after all" outcome
is a one-line note here, not a surprise buried inside X3.

**DONE 2026-09-04 — and it needed no code, which is the outcome this
milestone was written to be able to report.**

Two halves, both now answered:

- *Is the comparator per-core?* Yes. The SDK's own register header, from
  the datasheet: `SIO_MTIMECMP` is "core-local, i.e., each core gets a copy
  of this register, with the comparison result routed to its own interrupt
  line." So `ticker_arm_this_hart()` arming a secondary's deadline cannot
  disturb the primary's, and the split X1 made — `ticker_init()` programs
  and calibrates the shared tick generator, `ticker_arm_this_hart()` only
  sets this hart's comparator and MTIE — is correct on RP2350 as written.
- *Should the tick counter be one atomic or one per hart?* Per hart,
  decided in X1 by need rather than preference: `lockselftest`'s masking
  check holds a spinlock with interrupts off and asserts the counter is
  frozen, which a global counter breaks by advancing from the *other*
  hart's timer — failing a kernel whose masking is perfectly correct.

S5 handed both questions here; neither survives into X5.

**X5 — re-run the existing isolation/fault suite (phase 12's `isotest`
family) with tasks actually distributed across both cores**, not just
possible in principle. A driver's PMP grant faulting correctly on core 0
was proven once already; this milestone is where "and it still faults
correctly if a user task on core 1 is running at the same instant" gets
proven too, rather than assumed to follow.

**DONE 2026-09-04.** QEMU **338/338** (330 + 8 X5 checks), all on the
`rv64-smp` target under `qemu -smp 2`.

The milestone's wording is the hard part: every one of those isolation tests
*already* passed on a two-hart kernel before this milestone, and such a run
proves nothing the single-hart run did not. Driver tasks are pinned to hart
0 (X2), and a short-lived probe started from the shell lands wherever the
ready queue sends it — on a quiet machine, usually the shell's own hart. So
X5 needed two mechanisms, for two different things that could be untrue:

- **`smpload start` / `smpload stop`** (`kernel/smp.c`) runs four yielding
  tasks with a per-hart progress counter. The trap handler calls
  `smp_load_note_fault()` *before* killing a faulting task, snapshotting
  every hart's counter at that instant. `stop` then asserts that a hart
  other than the one taking the fault had made progress **before** the
  snapshot and made more **after** it. Both halves are needed: progress
  before alone would pass for a hart that had since gone idle, progress
  after alone for one that only started later. That is a claim about
  simultaneity, and no number of passing tests can establish it.
- **`isolationtest <hart>`** (and `deputytest`, `usertest`) pins the probe,
  so "a domain fault is handled correctly on a hart that is not the primary"
  stops being a lucky draw. Run in both directions, because hart 0 is the
  busy one and an unpinned probe would rarely choose it — asking for hart 0
  and getting it is what shows the pin is a constraint rather than a label.

*Falsified, not just observed.* The same kernel under `qemu -smp 1` reports
`hart 1: progress 0` and `SMPLOAD_FAIL (2 failed)` — no fault was taken
while loaded, and no other hart was mid-task. The evidence tracks the second
hart rather than the passage of time.

**Three pre-existing bugs, each found by an X5 mechanism rather than by
reading.**

1. **Creating a task and pinning it were two steps, and another hart fits
   between them.** `task_create_driver()` called `task_create_sized()` and
   *then* `task_set_affinity()` — but a task is READY the moment creation
   returns, so a second hart spinning in `sched_yield()` can claim it before
   the pin lands. `sched.h`'s own comment on that function already said this
   must not happen ("creating and pinning are one act; a window in which a
   driver task is briefly migratable is exactly the sort of thing that works
   until it doesn't"); the code did not implement it. Found by asking for
   hart 0 and being told `ran in U-mode on hart 1 -- NOT the hart it was
   pinned to`. Fixed by threading the affinity through a single
   `task_create_full()`, set before the task is published, and publishing
   under `g_sched_lock` so the release orders the affinity ahead of the
   READY another hart's scan is looking for. It could not bite before X5
   because drivers are created before `smp_release_secondaries()`.
2. **`/proc` reads were silently truncated at 512 bytes.**
   `print_proc_file()` (`user/lisp/lisp.c`) read into a fixed 512-byte
   buffer while the generator had 896 available — so `ps` stopped mid-row at
   eight tasks, with no error and no marker. It went unnoticed because the
   machine normally runs six, and appeared the moment X5's load added four.
   Now streamed through `vfs_open`/`vfs_pread`, which also makes the result
   a consistent snapshot and, as a side effect, **saves 255 bytes** of
   static RAM on every board.
3. **A kernel task that returned was reported as `killed`.** `exit_clean`
   was set only by `task_set_exit_status()`, which a U-mode program reaches
   through `usys_exit` but a kernel task that simply returns never calls.
   That column is exactly what phase 12's C3 suite reads to tell a contained
   fault from an ordinary exit — a value printed for both is worth less than
   it looks. `task_start()` now marks the return, guarded so a deliberate
   status (`uargs`' 42) survives.

The generator's own 896-byte cap is still a cap — `/proc/ps` runs out at
about thirteen tasks against `MAX_TASKS` 24 — but it now stamps
`-- truncated --` over the tail instead of ending mid-row. Enlarging it is
the wrong trade at eight handles on a 264 KB part; being honest about the
edge costs 18 bytes of text.

*What X5 does **not** cover, and why.* Phase 12's RP2350-specific half of
the isotest family (`heartbeatisotest`, `tm1638isotest`, `i2cisotest`,
`st7735isotest`, `blkisotest`, `uartisotest`, `usbisotest`,
`clockisotest`) cannot be run under this milestone's conditions today,
because RP2350's core 1 does not join the scheduler. Those tests still pass
on hardware exactly as before, on one core. Naming this is the point: X5's claim is about the generic domain
tests on QEMU's two harts, not about the driver-domain tests on RP2350.

**X6 — hart identity in the bring-up window. DONE 2026-09-04.** Added
after X5, when the question "should we unblock `printk()` from core 1
before doing anything else with multi-core?" was checked rather than
answered from the X3 note.

*The premise did not survive measurement.* A blocking `chan_call` from a
secondary hart to a hart-0-pinned driver task already works: a U-mode probe
pinned to hart 1 printed through `printk` → `uart_flush` → `chan_call` →
the pinned UART task, and that task's own server-side counter moved 27 → 54
(`uartstats`). The caller blocks, hart 0 serves, the reply wakes it. X3
recorded this as a blocker without testing it.

*The real defect was one line, and it produced both symptoms.*
`sched_init()` set `g_current[h] = 0` for every hart, so
`sched_current_pid()` answered **0** — the boot task — for any hart
executing kernel code before its own `sched_secondary_init()`. Everything
downstream then acted on a task running on a different hart at that
instant:

- `task_block()` marked the *boot task* BLOCKED, from a hart that was not
  running it. Nothing would ever wake it: a wedged shell, caused by a
  secondary hart printing one line too early.
- `printk_lock()` compared `g_printk_owner == me` and matched a lock hart 0
  was holding, took the re-entrant path, and let both harts into the region
  whose entire purpose is that only one is inside it.

The same window exists on the primary before `sched_init()`.

*The fix separates two questions that had been sharing one answer.*
`sched_current_pid()` now returns `TASK_NO_PID` (-1) when the hart owns no
task, `sched_has_task()` answers "may I block?", and `sched_context_id()`
gives a stable identity for whatever is executing — the pid where there is
one, `MAX_TASKS + hart_id()` where there is not. The split matters because
-1 is the *unowned* sentinel in every lock in this tree: a bring-up hart
identifying as -1 would read as nobody, and a lock it held would look free
to everyone including itself. Lock ownership uses the context id;
anything that blocks, wakes, or indexes a task uses the pid and checks it.

Consumers corrected, each of which had been silently using task 0's
identity: `printk_lock()` (context id, and it polls instead of blocking when
it cannot be a waiter), `chan_call()` (refuses from a task-less hart rather
than indexing `g_wait_for[-1]`), `ylock_acquire()` (context id), and the
UART TX/RX waiter slots in both drivers (poll instead of registering).
`sched_yield()`, `task_block()`, `task_exit()`, `task_sleep_ms()` and
`task_set_exit_status()` now return instead of operating on `g_tasks[-1]`
or on task 0.

*Verify, and it is deterministic.* `secondary_main()` prints from the
task-less window, with the pid in the line rather than assumed:

```
[SMP] hart 1: in the kernel, no task yet (pid -1)
```

Reverting only the `g_current[]` initialiser turns that into `pid 0`, which
is what the falsification run produced. The consequences need a race; the
cause does not, so the boot log separates fixed from broken with nothing to
reproduce. The line also establishes that `printk()` works at all from a
task-less hart — which is what makes second-core bring-up debuggable, and
is precisely what X3 lacked when it wedged the board twice and had to
retreat to a core that only counts.

*Verified:* QEMU **339/339**, three consecutive clean runs. RP2350 chess
persona **24/24** on real hardware. Static RAM **+0** on both personas.

*Not done here:* core 1 still does not join the RP2350 scheduler. That is
X7 below, which this milestone exists to make approachable — the remaining
work is a bring-up sequence, not an investigation, and it can now be done
with a console rather than blind.

**X7 — RP2350's core 1 joins the scheduler. OPEN, hardware.** The step X3
deliberately stopped short of and X6 made approachable. Everything below is
RP2350-specific: the generic two-hart scheduler is finished and proven on
`rv64-smp`, so nothing here is about scheduling *design*, only about a
second Hazard3 core and the things on this chip that are per-core or that
couple to it.

*Ordered, because the order is what keeps a failure diagnosable.*

1. **Point core 1 at `secondary_main()` instead of `core1_probe_main()`.**
   `boot_header.S`'s `core1_entry` already does everything before that: the
   `0x51C0DE02` marker as its first instruction, `tp` from `mhartid`,
   `mtvec`, `mstatus` zeroed, `SETUP_HART_POINTER`, and a stack in
   SCRATCH_Y. **Keep the counter as a selectable fallback**, not as dead
   code deleted on the way past — it is the only known-good state of this
   path, and X3's cost was learned by not having one.

   Note that `entry.S`'s secondary path stays excluded on this board
   (`#if CONFIG_ENABLE_SMP && !defined(CONFIG_BOARD_RP2350)`): core 1
   arrives through `core1_entry`, never through `_start`. Nothing from
   `_start`'s secondary path needs replicating — `.data`/`.bss` are hart 0's
   work, and the `pmpaddr0`/`pmpcfg0` grant there is `CONFIG_MODE_S`-only,
   so an M-mode RP2350 secondary has no default PMP region to install.

2. **`trap_init()` on core 1.** Its RP2350 branch sets `mie.MEIE` and
   `mstatus.MIE`, both per-hart CSRs, so calling it from core 1 configures
   core 1 and nothing else. What must be *stated* rather than discovered:
   `arch_irq_enable()` writes Hazard3's `MEIEA` (`trap.c:167`), which is
   also per-core, and every driver called it on core 0 during boot — so
   **core 1's `MEIEA` is empty and core 1 will take no device interrupts.**
   That is the correct default, because X2 pins the driver tasks to hart 0
   anyway; routing any device IRQ to core 1 is a separate policy decision,
   not a step in this bring-up.

3. **`ticker_arm_this_hart()` on core 1.** X4 already answered the hardware
   question — `SIO_MTIMECMP` is core-local — and the call sets `mie.MTIE`
   plus this core's deadline, both per-core. *Verify:* preemption actually
   fires on core 1, using the per-hart tick counters X1 already added.
   Arming a comparator and taking the interrupt are different claims.

4. **`sched_secondary_init()`, then the idle loop.** Unchanged from the
   QEMU path. X6 is what makes steps 1-3 safe to `printk` from at all.

5. **PMP domains on core 1.** `mem_domain_activate()` writes PMP CSRs,
   which are per-hart, and `sched_yield()` already calls it on every
   resume — so a task migrating to core 1 should get its domain installed
   there. "Should" is the operative word: X2 proved this on QEMU, where the
   backend is Sv39. The PMP backend has never run on a second hart.
   *Verify:* X5's own `isolationtest 1` / `deputytest 1` / `usertest 1` on
   the board, plus the `domains_hart1` counter.

6. **Park core 1 across flash writes.** The one genuinely new mechanism,
   and the one that fails by hanging rather than reporting.
   `drivers/flash_rp2350.c` turns XIP off, after which *every* instruction
   fetch from `0x10000000`-and-up faults or hangs. Its own routine is
   `.ramfunc` and runs with interrupts masked — but that protects the hart
   executing it, and with core 1 scheduling, core 1 is running
   flash-resident code at that instant. Needs an explicit handshake: core 1
   into a `.ramfunc` spin (or a `wfi` with its timer masked) before XIP goes
   down, released after. §6 already recorded this as owed by X3; it becomes
   real here, because until now core 1 ran a counter that lives in `.bss`
   and was never a problem.

7. **Leave the launch explicit.** `smpstart` stays a shell command rather
   than becoming automatic at boot until everything above is proven on
   hardware. A board that boots is a board that can be reflashed — X3 paid
   for that sentence twice.

*Verify, on real hardware, and each of these is a claim the QEMU work
cannot make:*

- `harts_online: 2` on the board, replacing X3's `CORE1_ALIVE`.
- `smptest` — lost updates through the lock, on two **Hazard3** cores.
  X1 proved the scheduler design; S1 proved `amoswap.w.aq`/`.rl` executes
  on this silicon. Neither proved cross-*core* atomicity on it, which is
  §6.3's remaining open question and this is the first test that closes it.
- `lockselftest` 7/7 with a second core genuinely running.
- X5's isolation suite under `smpload`, on the board.
- **The RP2350 driver-domain isotest family** (`heartbeatisotest`,
  `tm1638isotest`, `i2cisotest`, `st7735isotest`, `blkisotest`,
  `uartisotest`, `usbisotest`, `clockisotest`) under that load — the
  exact set X5 named as out of its reach, and the reason it was.
- A flash write (phase 21's I7 path) while core 1 is scheduling, for the
  park handshake.
- The hardware suite still 24/24, and the chess and clock personas still
  building with `CONFIG_ENABLE_SMP=0`.

*Risks, named:* the XIP failure mode is a hang with nothing left running to
report it, so step 6 is where a BOOTSEL recovery is most likely and the
counter fallback from step 1 is most valuable. And a two-core RP2350 shares
one bus and one flash cache, so any *performance* claim from QEMU is
worthless here — which is the whole reason this milestone exists separately
from X1.

**DONE 2026-09-04 — core 1 is in the scheduler on real silicon.**

```
[SMP] CORE1_SCHEDULING -- core 1 joined the scheduler as pid 9
[SMP] core 1 stack: 772 of 16384 bytes used

ps:   9  RUNNING  idle  -  -  1  -

smptest:  locked=80000 (want 80000)  unlocked=79999 (lost 1)  harts=2
          SMP_SELFTEST_OK (3/3)
```

*The bug was one thing, and it was not what the first three attempts
concluded.* **A tight test-and-set spin starves the other core.** Every
`spin_lock_irqsave()` waiter re-issued `amoswap`, which takes exclusive
ownership of the cache line on every attempt — so a waiter can keep the
*holder* from getting the line back to release with. On QEMU the harts are
host threads the OS timeslices and it never appears. On RP2350's two cores
and one bus it is a deadlock by starvation, and both cores stop. The
primitive is now test-and-**test**-and-set: wait on plain loads, back off on
a local, and only attempt the atomic once the word looks free.

The same shape appeared twice more and was fixed the same way: X1's
secondary idle loop (`for(;;) sched_yield();`) took `g_sched_lock` on every
iteration, and now asks `sched_peek_runnable()` — a deliberately racy,
lock-free question — before paying for the lock. X1's own comment had
predicted "X2 or later can make it wfi once there is a reason to" and named
the cost as power; the cost was starvation. `wfi` itself was tried first and
QEMU's X2 check rejected it within one run (`domains_hart1: 0` — a hart
asleep until the next tick never picks up a short-lived task).

*What found it was instrumentation, not reading.* Three things, in order of
how much they were worth:

1. **A deadman.** `rp2350_reboot_after_ms()` arms a bootrom reboot before the
   risky window. A bootrom reboot is not a power-on reset, so SRAM survives
   and `.smpmark` still names the last step reached. This converted "a dead
   board, a BOOTSEL press, and no evidence" into a board that comes back and
   says what happened. Every attempt after it was cheap.
2. **A marker for *both* cores.** `core1_probe` alone answers half the
   question. The half that mattered was `core0_probe: 0xc0de0007` — core 0
   died *inside a printk*, while core 1 idled fine. Until that word existed,
   "core 1 killed the board" and "the board died while core 1 was up" were
   indistinguishable, and three attempts were spent on the wrong one.
3. **Staged modes.** `smpstart stage1|stage2|stage3` runs the first N steps
   of `secondary_main()` and then falls into X3's counter, so core 0 sees the
   marker *and* whether core 1 is still executing. All three passed, which
   located the fault in the tail rather than in any step — and the tail is
   where the lock spin was.

*Two wrong conclusions, corrected by measurement.* The 4 KB stack in
SCRATCH_Y was diagnosed as the cause and moved to a 16 KB `.stack1` in RAM.
The instrumentation added at the same time then measured actual usage at
**772 bytes** — so the stack was never the bug. The move stands (4 KB with
the heap immediately below is a real hazard, and the number is now known
rather than argued), but the diagnosis was wrong. Likewise the console
interleaving was read as a failed lock; `smpstart locktest` showed `amoswap`
excluding the two cores correctly, and the actual defect was `uart_flush()`
running after `printk_unlock()` released ownership, over a single shared
`g_tx_batch`. That is fixed by per-hart batches, not by holding the lock
across a blocking flush — which would deadlock against the uart task.

*From the SDK, and this is what unblocked it.* `runtime_init_per_core_h3_irq_registers`
(`pico_crt0/crt0_riscv.S`) runs on every core, core 1 at launch included, and
clears **`meifa`** — Hazard3's per-core external-interrupt *force* array, CSR
`0xbe2`, windows 3..0. It has no guaranteed reset value, and a stale bit
means a core takes an external interrupt the instant `mstatus.MIE` goes on.
This kernel had never cleared it, which was survivable while only core 0
existed because `boot_header.S` brought that core up from reset itself. Core
1 arrives from the bootrom instead. Also from the same source: `mie` is now
written rather than read-modify-written, and the launch drains core 1's
mailbox FIFO.

*Verified on a Pico 2 (chess persona), all with core 1 scheduling:*

- `harts_online: 2`, and `ps` showing pid 9 `idle` RUNNING on hart 1.
- **`smptest`: `locked=80000 (want 80000) unlocked=79999 (lost 1) harts=2`.**
  Zero lost through the lock under real contention between two Hazard3
  cores — §6.3's open question, closed on silicon rather than by argument.
- `lockselftest` **7/7**, including S6's hand-off canary, with two cores live.
- `isolationtest 1` and `deputytest 1`: a U-mode probe **pinned to hart 1**
  faulted on hart 1, kernel memory untouched, syscall boundary held. **PMP
  domains enforced on a non-primary Hazard3 core** — X7 step 5, and the
  thing X2 could only ever prove on Sv39.
- The **whole RP2350 driver-domain isotest family** under `smpload` —
  heartbeat, i2c, uart, usb, st7735, tm1638, blk — every one ISOLATED, with
  `SMPLOAD_OK (3/3)`: 7 faults taken while loaded, another hart mid-task at
  the instant of each. This is exactly the set X5 named as out of its reach.
- `flashpark`: core 1 acknowledges the park request from a `.ramfunc` spin
  in **25 µs** (10 µs single-core).
- RP2350 hardware suite **24/24** on the non-SMP chess persona. QEMU
  **339/339**. Static RAM **+260** (the second hart's TX batch and its length
  word), re-baselined; heap 86 → 83 pages for core 1's 16 KB stack.

*Not covered, and stated rather than implied.* The park handshake is
exercised through `flashpark`, which requests and releases without turning
XIP off. The only real flash writer in the tree is phase 21's identity store,
and provisioning to test a lock would destroy the record it writes. The
XIP-off sequence itself is unchanged and proven single-core; the untested
combination is "core 1 parked **and** XIP actually down", which is narrow and
now named. Core 1 also still takes no device interrupts — its `MEIEA` is
empty because every driver enabled its IRQ on core 0 — which is the correct
default given X2's pinning, not an oversight.




*Verified:* QEMU **338/338**. RP2350 chess persona **24/24** on real
hardware, including B3/B6/C3/C4's isolation tests — no regression from the
scheduler, trap and `/proc` changes, which affect every target. On that
one-hart board `smpload` skips cleanly and `isolationtest 1` answers
`hart 1 is not online (1 hart(s) running)` rather than pinning a task
somewhere it can never run. Static RAM **-255 bytes** on both RP2350
personas, re-baselined.


## 4. Explicitly not in this phase

- **More than two cores.** RP2350 has exactly two; nothing here
  generalizes to N without more design than this silicon needs.
- **NUMA-style memory affinity or per-core allocators.** RP2350's SRAM
  has no locality difference between the two cores — not a real problem,
  so not a real milestone.
- **Load balancing or work stealing.** X1's single shared queue *is* the
  scheduling policy for this phase. Anything more adaptive is a later
  phase's problem, and only if it is ever measured to be a problem.
- **Any change to phase 22's locking primitives themselves.** They are
  finished, tested, and load-bearing before this phase's first milestone
  starts.

## 5. Risks, and what each looks like

- **X7 fails by hanging, not by reporting.** Turning XIP off with core 1
  fetching from flash leaves nothing running to say so — the same shape as
  §6's I7 note, now with a second core in it. The mitigation is ordering
  (X7's steps are sequenced so a failure names its own step) and keeping
  X3's counter as a selectable fallback, since it is the only known-good
  state of that path. Expect BOOTSEL recoveries; X3 needed two.
- **X3 cannot be verified without real hardware.** Named plainly, the
  same way `plan/phase21_identity_and_authentication.md` names I7 —
  written down as blocked, not silently skipped, not claimed done on the
  strength of X1's QEMU pass alone. *Resolved 2026-09-04: bench access
  arrived and X3 was verified on a Pico 2. The risk was real — QEMU
  could not have shown that core 1 never leaves the bootrom.*
- **X1 passing is necessary, not sufficient.** A clean two-hart QEMU
  result is real evidence the *scheduler design* is sound; it is not
  evidence about RP2350's actual Hazard3 core-launch behavior, interrupt
  routing to a nonzero hart, or anything else this project's own history
  says QEMU has hidden before. Treat X1 as clearing the way to attempt
  X3, not as a substitute for it.
- **If phase 22's S6 has a bug, X1 is where it surfaces** — on QEMU,
  before any hardware is involved, which is the entire reason phase 22
  was sequenced to finish first and phase 23 was split the way it is
  here. A failure at X1 sends the fix back to phase 22, not forward into
  a hardware debugging session.
- **Driver task affinity (X2) being wrong in a subtle way** — e.g. a
  driver's ISR expecting to run on the hart that armed it — would look
  like an intermittent, load-dependent failure exactly like the original
  `fs/p9_link.c` race phase 22 §1 describes, and for the same underlying
  reason: state one hart's code implicitly assumed was hart-local turning
  out not to be. Worth naming now so it is recognized quickly if it
  happens, rather than re-discovered from scratch.

---

## 6. Amendments (2026-09-03, review against the tree)

Re-checked against the tree on 2026-09-03, before any implementation,
alongside `plan/phase22_smp_locking_foundation.md` §6. Phase 22 gained a
correction (its S6 rule was inverted) and a new first milestone (S0,
per-hart state). This phase gains three prerequisites it did not know it
had, one hidden coupling to phase 21's I7, and an honest restatement of
what the two usage scenarios are actually worth.

Read `plan/phase22_smp_locking_foundation.md` §6.2 first: **X1 cannot
start until phase 22's new S0 lands**, because core 1 has no way to
identify itself today and the two conventional places to keep that answer
(`tp`, and `sscratch`'s zero-means-kernel invariant) are both already in
use.

What the re-check confirmed and does not need revisiting: PMP really is
hart-local and the region budget really is per-task rather than
per-system (`kernel/include/kernel/mem_domain.h:18-30`), so §1's claim
holds — a second hart activating a domain is a second caller of
`mem_domain_activate()`, not a new mechanism.

### 6.1 The PLIC context is hardcoded to hart 0

§1 says the interrupt controllers are "already per-hart-addressable in
principle." The *hardware* is; the *code* is not. `QEMU_PLIC_CONTEXT` is
a compile-time constant naming hart 0's context (0 for M-mode, 1 for
S-mode, `arch/riscv/common/trap.c:81-90`), and it is baked into three
separate register-address computations: the claim/complete address
(`:89`), the per-context priority threshold written at init (`:134`), and
the per-context enable bit set for each IRQ (`:160`).

Left as is, core 1 receives **no interrupts at all** on QEMU — including
no timer, therefore no preemption, therefore a core 1 that can only ever
run a task to completion or to a voluntary yield. This is small, purely
mechanical work, but it is a prerequisite for X1 rather than a follow-up
to it, and X1's "two tasks demonstrably running concurrently" cannot be
honestly claimed without it.

### 6.2 RP2350's ticker init programs a shared block, and measures

§2 asks whether RP2350 needs a per-core preemption timer and defers it to
X4. Two things about `kernel/ticker.c` change how that question has to be
asked.

First, `arch_ticker_init()` (`kernel/ticker.c:53-113`) is not a per-hart
routine that happens to run once. It writes `TICKS_RISCV_CTRL` and
`TICKS_RISCV_CYCLES` — a chip-wide tick generator — polls `RUNNING` on
it, and then **measures** the resulting rate against `time_get_us()` with
a 2 ms busy-loop. Core 1 must not re-run any of that: reprogramming the
shared generator mid-flight would disturb core 0's already-calibrated
`g_interval`, and the 2 ms calibration loop on a second hart measures
nothing useful. Core 1's share of ticker bring-up is exactly two steps —
set its own deadline, set its own `MTIE` — over an interval core 0 has
already established. Splitting `arch_ticker_init()` into a "program and
calibrate the block" half and a "arm this hart" half is X4 work that has
to happen whether or not the comparator turns out to be per-core.

Second, whether `SIO_MTIMECMP`/`SIO_MTIMECMPH` (`kernel/ticker.c:25-26`)
are core-local in the SIO alias, or shared between the two cores, decides
whether X4 is optional or mandatory. That is a datasheet question, to be
answered against the RP2350 datasheet and the Pico SDK's own register
headers to the same standard §2 already demands — not inferred from the
fact that each hart architecturally wants its own comparator.

### 6.3 Flash writes and core 1 are coupled — and I7 lands first

Neither document mentions this, and the stated sequencing makes it
arrive as a surprise. RP2350 flash programming runs through the bootrom
and disturbs the XIP window; a core executing from XIP while the other
programs flash is the classic RP-series hard-lock. Phase 21's I7 (the
RP2350 flash backend) is scheduled *before* this phase, so by the time
X3 wakes core 1, there will be a flash write path in the tree that
assumes it is the only thing running.

This therefore owes an explicit park-core-1 handshake around every flash
write — core 1 parked, or executing from SRAM, for the duration — and
that requirement belongs to this phase rather than to I7, since I7 is
correct as written for a single-core system. Naming it here so it is
designed rather than debugged.

> **Assigned 2026-09-04 to X7, step 6.** It was written against X3, which
> turned out not to need it: X3's core 1 runs a counter living in `.bss`
> and never fetches from the XIP window. It becomes real the moment core 1
> executes scheduler and task code, which is X7.

### 6.4 What the two usage scenarios are actually worth

The phase was written around the mechanism. Both scenarios it enables
deserve a stated expectation, so that a result can be judged against
something.

**Distributing tasks across cores.** This is X1, and it works — but the
payoff is *latency isolation, not throughput*. Most of the task table is
drivers, and X2 correctly pins those to core 0; what is left eligible for
core 1 is the shell, a loaded user program, the Lisp evaluator, chess
search, and the DCF77/NTP work. The demonstrable win is that heavy
compute on core 1 stops starving the display and keypad on core 0. Given
that this project has already shipped a fix for exactly that failure —
the clock's frame rate halving, visible as flicker, when a second
always-ready task existed (`kernel/include/kernel/sched.h`,
`task_sleep_ms()`'s header) — that is a real and testable improvement.
It is a different claim from "twice as fast," and X1's verify line should
not be read as making the second one.

**Two cores inside one application (chess).** Easier than it looks, and
easier than the general case, for one specific reason: chess is linked
*into the kernel image* as a persona (`CMakeLists.txt:538-551`), not
loaded as a U-mode ELF. So there is no thread-spawn syscall to invent and
no shared-domain PMP question — a second searcher is a plain
`task_create()` with shared memory already available. The design is
standard Lazy SMP: two searchers on the same root, sharing the
transposition table, differing in depth and move ordering.

Two costs are real and should be priced before starting:

- `user/chess/src/search.c` keeps its search state in file-statics —
  `history_table` and `killer_moves` (`:67-68`), the ply-indexed move
  pools, and a `static Position temp_pos` at roughly 8 KB (`:29`). All of
  it has to become per-searcher, threaded through a context struct. The
  change is mechanical but it is a few hundred lines through the hottest
  code in the engine, and it doubles the pool and `temp_pos` footprint
  against a 312 KB heap.
- The TT is 32 KB / 2048 entries on embedded (`user/chess/src/tt.c:26-28`),
  and Lazy SMP's gain scales with TT size. Concurrent access wants the
  lockless XOR-key scheme rather than a lock. A realistic expectation is
  roughly 1.3-1.6x node throughput on two cores and considerably less
  than that in playing strength — worth stating now, because "we added a
  core and it is not twice as fast" is otherwise a bug report.

**A loaded U-mode program using two cores** needs neither of the above:
it needs a thread-spawn syscall and two stacks inside one shared domain.
That is genuinely outside both phases and should be its own phase if it
is ever wanted — not smuggled into X1 because the mechanism happens to
be nearby.

### 6.5 The AMP alternative, named so it is a decision

Full SMP is not the only way to reach §6.4's second scenario. A
dedicated worker on core 1 driven by a mailbox — no shared ready queue,
no scheduler lock, no kernel-wide `irq_save()` audit, one lock on the
transposition table — delivers most of the chess case for a fraction of
phase 22's surface area.

It is written down here as a real option rather than dismissed, because
the argument against it is a judgement call and not a technical one: it
is a *second* concurrency mechanism alongside the scheduler, where this
project has consistently preferred one correct mechanism over a
special-cased fast path (`kernel/chan.h`'s copy-always design made the
same call for local vs. remote IPC). It also does not deliver §6.4's
first scenario at all. The recommendation stays full SMP; the point is
that this should be a decision taken with the cheaper option in view,
rather than a default inherited from the fact that these two documents
were written first.
