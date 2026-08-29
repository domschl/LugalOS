# Phase 22 — One real lock, before a second core ever needs one

**Status: planned 2026-08-29. Planning only — no implementation yet.**
Sequenced after phase 19's R4 (ENC28J60) and R5 (CYW43439) and phase 21's
I7 (RP2350 flash backend), all three of which are blocked on hardware
access today. Nothing in *this* phase requires hardware — see §3's Verify
lines, every one of which is QEMU/single-hart testable — but the project's
stated priority is to close out the hardware-dependent leftovers first, so
this phase does not start before they do.

**Scope.** Replace interrupt-masking-as-mutex with a real, cross-hart-safe
lock everywhere it is currently load-bearing, so that phase 23 (waking
RP2350's second core) lands on a kernel that is already safe rather than
one that becomes unsafe the moment a second hart executes.

**Out of scope:** actually waking a second core (phase 23); lock-free or
wait-free data structures; anything about PMP/Sv39 semantics themselves
(writing those CSRs is already hart-local and needs nothing from here).

---

## 0. Why this is its own phase

Every "no locking needed" comment in this kernel rests on one premise, and
that premise is written down in the one place that matters most,
`kernel/include/kernel/irq.h`, verbatim:

> "Masking interrupts is the right primitive here rather than a mutex: the
> regions are a handful of instructions, there is one hart, and a task
> that blocks while holding a scheduler lock could not be scheduled out of
> it. Multi-hart would need something else, and nothing in this kernel is
> multi-hart (entry.S parks every secondary hart at boot)."

That is the whole finding, stated by the code itself before this phase
was ever proposed. `irq_save()`/`irq_restore()` disables interrupts on
**the hart that calls it** — nothing else. It provides real mutual
exclusion against preemption and interrupt handlers on that one hart, and
it provides *nothing at all* against a second hart reading and writing
the same memory at the same time, because masking hart 0's interrupts has
no effect whatsoever on hart 1.

RP2350 already boots both of its Hazard3 cores — `arch/riscv/common/entry.S`
checks `mhartid` at the reset vector and parks anything nonzero in a `wfi`
loop (`arch/riscv/rp2350/boot_header.S` has the same parking loop for the
RP2350-specific boot path). Nothing currently wakes it: no SIO FIFO launch
vector, no stack, no task. Phase 15 already noted this in passing while
doing something else entirely: "Core 1 never takes a stack at all — it
branches straight to `wfi` and spins" (`plan/phase15_memory_reclamation.md`).

Waking that core is mechanically small — RP2350's bootrom supports
releasing a parked core over the SIO FIFO, and PMP register writes are
already hart-local CSR state, so a second core activating its own task's
memory domain is calling the *existing* `mem_domain_activate()` on itself,
not a new mechanism. The hard part is everything downstream of "now two
harts are executing kernel code at once," because every structure two
harts might touch — the scheduler's task table, the page allocator's
bitmap, IPC channel state, the one hand-rolled lock this tree already
has — is protected today by a primitive that is, by its own author's
admission, single-hart only. This phase is the part that has to happen
*first*, and the part that can be built and verified without a second
core ever running, which is exactly why it is not bundled into phase 23.

## 1. What already exists, close enough to reuse

This is not a green field. `fs/p9_link.c` already built almost the right
shape once, under real pressure — preemption (M5/B6) turned an
intermittent, roughly-one-run-in-three failure in the two-node 9P tests
into a bug report, and the fix was a hand-rolled lock:

```c
typedef struct {
    volatile bool held;
    volatile int  owner;
    volatile int  depth;
} p9_lock_t;
```

`p9_lock()` takes it under `irq_save()`, is **re-entrant for the owning
task** (a locally-mounted namespace can be walked into recursively,
`/self/self/...`, which re-enters the client path on the same task — a
plain lock would deadlock there), and **yields rather than busy-spins**
when contended, specifically because these regions can be held across a
wait (a 9P roundtrip waits for a peer's reply) and masking interrupts
across a wait would stop the very timer that lets the peer's reply be
processed.

That shape is correct. Its one flaw, for reuse across a second hart, is
the test-and-set itself:

```c
if (!l->held) { l->held = true; l->owner = me; l->depth = 1; ... }
```

is only atomic because `irq_save()` prevents a *second access from the
same hart* — nothing stops a second hart from reading `l->held == false`
at the same instant and also proceeding. Promoting this exact shape into
a kernel-wide primitive means replacing that plain read-then-write with a
real RISC-V atomic (`amoswap` or an `lr`/`sc` retry loop) and moving it
out of `fs/p9_link.c` into `kernel/`.

The same shape shows up independently at least twice more, each time
reinvented locally rather than shared — which is itself evidence this
belongs in one place:

- `kernel/chan.c`'s endpoint dispatch: `irq_save(); if (ep->busy) {
  irq_restore(flags); return -1; } ep->busy = true; irq_restore(flags);`
  — the identical test-and-set-under-mask pattern, protecting a channel
  endpoint instead of a 9P link.
- `kernel/palloc.c`'s page-allocator critical section — a scan-then-claim
  over the bitmap, wrapped in the same `irq_save()`/`irq_restore()` pair,
  with its own comment admitting the same thing: "Cooperative scheduling
  made that true for free; preemption does not."

Three independent instances of the same fix is the concrete argument for
a shared primitive, not a hypothetical one.

## 2. The primitive

Two lock types, not one — the distinction `p9_lock_t` already draws
(yield-on-contention because it can be held across a wait) matters enough
that collapsing it into a single API would just move the mistake to
whichever call site gets it wrong:

- **`spinlock_t`** — for short, non-yielding critical sections that never
  block while held (the page bitmap, a channel's busy flag): a real
  atomic acquire, `irq_save()`'d around the hold (so the *local* hart
  still can't be preempted or interrupted while holding it), busy-spin on
  contention. Meant to be held for a handful of instructions only.
- **`ylock_t`** (or similar — naming is open) — the promoted `p9_lock_t`
  shape: real atomic test-and-set, re-entrant per owning task via a depth
  counter, `sched_yield()`s rather than spins when contended, safe to
  hold across a blocking wait.

Both live in a new `kernel/lock.h`/`kernel/lock.c`, target-independent
(the atomic instruction is the only hart-count-sensitive part, and RISC-V
`amoswap`/`lr`/`sc` are defined identically whether or not a second hart
is ever listening — so this, like `kernel/sha256.c` and
`kernel/idstore.c` before it, is buildable and testable on every target
including the ones that will never see a second core).

## 3. Milestones

**S1 — the primitive and its selftest.** `kernel/lock.h`/`.c`:
`spinlock_t` and `ylock_t`, acquire/release, the re-entrant depth/owner
logic. Target-independent, no second hart involved.
*Verify:* a selftest proving lock/unlock nesting is correct, that an
`irqsave`-held spinlock actually masks interrupts across its critical
section (checked the same way `kernel/random.c`'s own tests check
hardware claims — against a real, measurable effect, not an assumption),
and that a `ylock_t`'s owning task can re-enter while a different task
attempting the same lock yields instead of proceeding.

**S2 — convert `fs/p9_link.c`'s `p9_lock_t`** to `ylock_t`, deleting the
local type.
*Verify:* the existing two-node 9P test suite, unchanged — this is the
exact regression check that caught the original race in production, and
it stays the regression check here.

**S3 — convert `kernel/chan.c`'s endpoint busy flag** to `spinlock_t`.
*Verify:* `chanechotest` and the rest of the IPC-heavy suite, unchanged.

**S4 — convert `kernel/palloc.c`'s bitmap critical section** to
`spinlock_t`.
*Verify:* palloc's existing stress/fragmentation coverage, unchanged;
`sizecheck` shows the expected small growth (a lock is a handful of
bytes) and nothing more.

**S5 — audit the rest of the tree.** `kernel/balloc.c`, `kernel/ticker.c`
and `kernel/klog.c` currently have **no** `irq_save()` protection at all
(confirmed by grep, not assumed) — each is either genuinely safe for a
reason worth writing down, or a fourth independent instance of the same
gap waiting to be found. `drivers/uart_net.c` already names itself
explicitly: "before preemption they were serialised for free by
cooperative scheduling." Every subsystem making an equivalent claim gets
found and either converted or given a written reason it doesn't need to
be.
*Verify:* this phase's own completion note lists every site examined and
its disposition (converted / genuinely safe, and why) — measured, not
assumed, the same discipline §3.1 of `plan/phase21_identity_and_authentication.md`
used for RP2350's storage layout.

**S6 — `kernel/sched.c`'s own `g_tasks`/`g_current`/`g_active`.** The
hardest one, and deliberately last. The scheduler's critical section
today both protects data *and* performs the context switch
(`ctx_switch()`) inside the masked region — a lock cannot be held across
a switch to a *different* task without that task effectively carrying the
lock away with it. The design this milestone has to land on (not defer):
each hart's notion of "which task am I currently running" stays
hart-local and lock-free by construction once each hart has its own
`g_current`; only the *shared* ready queue needs the new lock, and it
must be released **before** `ctx_switch()`, never held across it.
*Verify:* the existing scheduler and preemption test suite, unchanged,
plus a new assertion-shaped test proving the lock is never live across a
`ctx_switch()` call (a canary the switch path itself can check).

## 4. Explicitly not in this phase

- **Waking core 1.** Phase 23, entirely.
- **Lock-free or wait-free algorithms.** A spinlock is enough for two
  cores and a handful of genuinely contended structures; RP2350 is not
  going to have sixteen harts fighting over the page bitmap.
- **A second ready queue, or any per-core scheduler state.** S6 above
  only removes the *current* single-hart-only unsafety from the one
  ready queue that exists; it does not add a second one. That is phase
  23's design question, not this phase's.
- **Anything about PMP/Sv39 semantics.** Those CSR writes are already
  hart-local and correct as written; nothing here touches them.

## 5. Risks, and what each looks like

- **S6 is the one real unknown, and it is untestable here.** Getting "the
  lock is released before `ctx_switch()`, never held across it" wrong
  would deadlock the first time two harts genuinely contend for it —
  which cannot happen until phase 23 wakes a second core. QEMU cannot
  hide this the way it hid six real Hazard3 divergences before (per
  `[[falsify_on_hardware_not_qemu]]`) in the usual sense — there is
  nothing running on a second hart *to* hide anything from, on any
  target, until phase 23's own X1 milestone. State this plainly rather
  than claim confidence this phase cannot earn on its own: S6's design
  is reasoned carefully, and its first real test is phase 23 §3's X1,
  not anything in this phase.
- **A spinlock that busy-waits where a yield was needed deadlocks
  immediately**, even on one hart today — a task spinning on a lock only
  the task the scheduler just descheduled can release never gets that
  task back. This is exactly why `p9_lock_t` yields rather than spins,
  and exactly why §2 above insists on two lock types rather than one: the
  choice of which to use at each call site is a correctness decision, not
  a style preference, and S2-S5 each have to get it right per site, not
  assume one shape fits everywhere.
- **S5's audit finding more sites than expected** is itself not a
  failure — it is the point of doing an audit rather than converting only
  the three sites already known about. A shorter list than expected would
  be more suspicious than a longer one.
