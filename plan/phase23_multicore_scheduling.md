# Phase 23 — Waking the second core

**Status: planned 2026-08-29. Planning only — no implementation yet.**
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
  question this phase answers, not an assumption it makes.

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
argument. This is also where phase 22's S6 (the scheduler lock never
held across `ctx_switch()`) gets its first genuine test.

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
