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

**X2 — driver-task affinity.** Pin existing driver tasks to core 0;
confirm phase 12's per-task PMP/Sv39 isolation guarantees hold regardless
of which hart activates a given domain (§1 says they should; this
milestone is where that claim gets checked rather than assumed).

**X3 — RP2350 core-1 wake. Hardware-only, BLOCKED until bench access
exists** — the same constraint named in `plan/phase21_identity_and_authentication.md`'s
I7, for the same reason. The SIO FIFO launch sequence, `CONFIG_ENABLE_SMP`-gated.
*Verify:* on real hardware, proof core 1 executed anything at all (e.g. a
counter core 1 increments that core 0 reads back) — cannot be verified
any other way, and is not claimed done until it is.

**X4 — per-core preemption timer**, if §2's open question about RP2350's
timer hardware requires one. May turn out to be unnecessary depending on
what X3's own hardware investigation finds; written as a separate
milestone rather than folded into X3 so a "not needed after all" outcome
is a one-line note here, not a surprise buried inside X3.

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
  strength of X1's QEMU pass alone.
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
