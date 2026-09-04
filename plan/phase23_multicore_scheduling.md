# Phase 23 — Waking the second core

**Status: planned 2026-08-29. Amended 2026-09-03 (§6) — three
prerequisites X1 did not name, one coupling to phase 21's I7, and a
stated expectation for each usage scenario. Planning only, no
implementation yet.**
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
blockers for the scheduler step were found there and are **not** solved
here, recorded for X5's successor: `printk()` from core 1 reaches core 0's
pinned UART task through a blocking `chan_call`, and `task_block()` before
`sched_secondary_init()` would block task 0, since `g_current[1]` is
still 0.

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

X3 therefore owes an explicit park-core-1 handshake around every flash
write — core 1 parked, or executing from SRAM, for the duration — and
that requirement belongs to X3 rather than to I7, since I7 is correct as
written for a single-core system. Naming it here so it is designed rather
than debugged.

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
