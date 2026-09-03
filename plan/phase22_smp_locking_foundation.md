# Phase 22 — One real lock, before a second core ever needs one

**Status: planned 2026-08-29. Amended 2026-09-03 (§6) — one
correction (S6's rule was inverted) and a new first milestone (S0).
S0 implemented and verified 2026-09-03 on QEMU and on RP2350 silicon
(see its DONE note in §3); S1-S6 not started. All of it lives on branch
`feature/multicore`.**
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

**DONE on QEMU 2026-09-03; hardware pass pending a board.**
`arch/riscv/include/arch/atomic.h` (the §6.3 seam),
`kernel/include/kernel/lock.h` + `kernel/lock.c`, `lockselftest` in the
shell, and one new suite test per arch. **QEMU 320/320** (318 + 2), six
checks passing on both RV32/M-mode and RV64/S-mode.

`spinlock_t` and `ylock_t` are as specified. The atomic sits behind
`arch_lock_try_acquire()`/`arch_lock_release()` rather than an inline
`amoswap`, and the hook is shaped as "take/release this lock word" rather
than "swap this integer" precisely so a SIO-hardware-spinlock
implementation can express itself in it — a bare swap hook could not.

Two notes on what the selftest does and does not establish, because the
difference matters more than the pass:

- **The interrupt-masking check is real.** The tick counter must be frozen
  across an `irqsave`-held spinlock and moving when it is not held —
  `kernel/random.c`'s standard, a measured effect rather than the
  implementation restating itself. Confirmed by deleting `irq_save()` from
  the acquire and watching it fail (ticks 239 → 242 during the "held"
  window). Both directions are asserted; "it did not move" alone would
  also pass on a board whose timer never fires.
- **Nothing here proves atomicity.** Executed serially on one hart, a
  plain `if (!held) held = true` gives the same answers as `amoswap`, so
  the gate check pins down the *contract* (exactly one acquire per
  release, and that the release actually releases) and not the property
  the instruction was chosen for. Atomicity is first observable at phase
  23's X1, where two harts race the same word. An earlier draft of that
  comment claimed the test distinguished the two; it does not, and saying
  so would have been the kind of unearned confidence §5 warns about.

**Hardware pass done 2026-09-03: `LOCK_SELFTEST_OK (6/6)` on a Pico 2 W
(`rp2350-clock`).** This was not a formality — §6.3 exists because "the
ISA defines `amoswap`" is not a statement about Hazard3, and 12 AMO
instructions reach the linked image. `amoswap.w.aq` and `amoswap.w.rl`
execute correctly on real silicon: no illegal-instruction trap, no fault
against SRAM. The interrupt-masking measurement passes there too, which
matters given how often QEMU has hidden a Hazard3 divergence before.

Precisely what that settles: the instructions exist, execute, and behave
correctly **on one hart**. It says nothing about whether Hazard3's AMOs
are atomic *between* the two cores — that is unobservable until phase
23's X1 puts a second hart on the same word, and it stays the open
question §6.3 named. What is now closed is the cheaper failure it was
also guarding against, which would have taken the board down on the first
lock acquisition.

`tests/hw/test_rp2350.py`: 24/24, three runs of four. The fourth returned
22/24 (C6/C7 and B3) on the first run after a fresh flash, while the WLAN
supervisor was still retrying its join; three settled runs afterwards were
clean. C6/C7 asserts exact heap equality across a compile, which
background allocation on a networked persona can disturb — plausible but
not proven, and recorded in `plan/open_issues.md` rather than fixed by
loosening a check that exists to catch arena leaks.

Cost: **+22 bytes** of static RAM.

**S2 — convert `fs/p9_link.c`'s `p9_lock_t`** to `ylock_t`, deleting the
local type.
*Verify:* the existing two-node 9P test suite, unchanged — this is the
exact regression check that caught the original race in production, and
it stays the regression check here.

**DONE 2026-09-03.** `p9_lock_t`, `p9_lock()` and `p9_unlock()` deleted;
`g_pump_lock` and `g_client_lock` are `ylock_t`, 12 call sites converted,
and `kernel/irq.h` dropped from the file's includes because the lock was
its only user. Every documented property is unchanged — re-entrant for the
owning task, yields rather than spins, separate locks for pump and client
so a client waiting for a reply cannot block the pump that delivers it.
What changed is that the test-and-set is now a real atomic instead of a
plain read-then-write that was indivisible only because interrupt masking
kept a *second access from the same hart* out.

*Verified:* QEMU **320/320**, run five times — the intermittency the
original race showed (roughly one run in three) means a single green run
is not evidence. All four multi-node 9P tests pass, plus the recursive
`/self/self/` mount case that is why re-entrancy is required at all. On
RP2350 (chess persona): **24/24**, including the three tests that drive
`p9_link.c` over real hardware — `link_usb_cdc`'s 9P read, its
garbage-resync path, and T3's QEMU-guest-to-board bridge.

Static RAM: **+0**. That figure took a correction worth recording. Written
first as `static ylock_t g_pump_lock = YLOCK_INIT;`, `sizecheck` reported
a 24-byte *saving* for a like-for-like swap of two 12-byte objects —
because on RP2350 an initialised static lands in `.data`, which
`linker/rp2350.ld` puts inside an executable PT_LOAD, so `nm` types it `t`
and `tools/sizereport.py` (which counts `b` and `d`) never sees it. The
memory had not gone anywhere; it had become invisible. `ylock_t` now
defines all-zero as its free state — `depth` is the sole authority and
`ylock_owner()` reports -1 below 1 — so the statics stay in `.bss` and
stay counted. The tool's blind spot is its own entry in
`plan/open_issues.md`: on RP2350 `.bss` and the heap are the same memory,
which is the entire reason that guard exists.

**S3 — convert `kernel/chan.c`'s endpoint busy flag** to `spinlock_t`.
*Verify:* `chanechotest` and the rest of the IPC-heavy suite, unchanged.

**DONE 2026-09-03, with a correction to this milestone's own wording.**
"Convert the busy flag to `spinlock_t`" read as *hold a spinlock for the
call*, and that would have been wrong twice over — `busy` is held across
`chan_call_task()`, which calls `task_block()`, and a `spinlock_t` held
across a block is the deadlock its own header warns about; and contention
here must **refuse** (the caller gets -1) rather than wait, because on the
re-entrant path a waiter would be waiting on itself.

So the two jobs got separated rather than merged. `busy` stays a plain
claim flag with a long lifetime; a new per-endpoint `spinlock_t` makes
*reading-and-taking* that flag indivisible, and is held for three
instructions. The release goes through the same lock, which is what gives
the next hart to claim the endpoint release ordering over everything the
call wrote into its buffers.

*Verified:* QEMU **320/320**, three runs, including `chanechotest` (the
milestone's named check) and the whole U-mode channel set. The load-bearing
one is "Recursive Local Mount Is Refused, Not Fatal" — it exercises the
refusal path directly, and had the lock been held across the call it would
**hang** rather than fail, so its passing is evidence about the shape of
the conversion and not just its correctness. RP2350 chess persona
**24/24**, with all five driver-task tests (uart, blk, i2c, st7735,
tm1638) — each of which counts `chan_call()`s actually served — passing on
silicon. Static RAM **+64**: 16 endpoints x 4 bytes, in `.bss` and counted.

**Found for S5:** `chan_call_task()` holds `irq_save()` across
`task_block()` (`kernel/chan.c`), relying on the scheduler's
flags-travel-with-the-task hand-off. That is correct per-hart today and is
exactly the kind of site S5 has to reach a written verdict on rather than
leave implied.

**S4 — convert `kernel/palloc.c`'s bitmap critical section** to
`spinlock_t`.
*Verify:* palloc's existing stress/fragmentation coverage, unchanged;
`sizecheck` shows the expected small growth (a lock is a handful of
bytes) and nothing more.

**DONE 2026-09-03.** Both critical sections — the scan-then-claim in
`palloc_pages()` and the bit-clearing loop in `palloc_free()` — now take a
single `g_palloc_lock`. `kernel/irq.h` leaves the file's includes; they
were its only users.

The straightforward conversion S3 could not take, and worth saying why it
is straightforward here: these regions call nothing at all. No printk, no
yield, nothing that can block — the one expensive thing an allocation does,
zeroing the pages it just claimed, was already deliberately outside the
critical section before this phase existed. So `spinlock_t` is right on
its own terms rather than by elimination.

One lock for the whole allocator, not one per region: there is a single
bitmap and a single pair of counters, so there is nothing to divide.

*Verified:* QEMU **320/320**, four runs. Palloc's direct coverage is
"Task Stacks Are Reclaimed On Exit, Via The Reaper (B2/B6)", which
asserts `free before=N after=N` — exact page equality across a
create-and-exit cycle, so a claim that leaked or a free that missed shows
as a number rather than a crash — plus the buddy allocator's
round/align/coalesce pair on the arena palloc hands it, and the C6/C7
compiler-arena reclaim. RP2350 chess persona **24/24**, including "Buddy
arena: board-sized, and not reserved until first use" and the heap
headroom margins. `lockselftest` still 6/6 on silicon.

Static RAM **+4** — one `spinlock_t`, which is the milestone's own
prediction met exactly.

*Observed, not fixed:* `palloc_stats()` reads `g_used_pages` and scans the
bitmap outside the lock. It is a diagnostic, and the worst a racing reader
gets is a figure that was true a moment ago — but it is an unsynchronised
read of lock-protected state, and S5 owes it a written verdict rather than
this parenthesis.

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

**DONE 2026-09-03.** The headline: §1 counted **three** independent
reinventions of the same lock. There are **six**.

| # | Site | Shape | Disposition |
|---|------|-------|-------------|
| 1 | `fs/p9_link.c` `p9_lock_t` | re-entrant, yields | converted, S2 |
| 2 | `kernel/chan.c` `ep->busy` | test-and-set, refuses | converted, S3 |
| 3 | `kernel/palloc.c` bitmap | scan-then-claim | converted, S4 |
| 4 | `kernel/printk.c` `printk_lock()` | re-entrant, **blocks**, directed wakeup | converted, S5 |
| 5 | `drivers/enc28j60_rp2350.c` `g_busy` | test-and-set, yields | **not converted** — see below |
| 6 | `drivers/cyw43_rp2350.c` `g_bus_busy` | test-and-set, yields | **not converted** — see below |

#4 is the most developed of them — re-entrant *and* blocking *and* carrying
a single-waiter hand-off, grown under M2's pressure when `uart_putc()`
started blocking. It is kept rather than replaced by a `ylock_t`: that
hand-off is a real property of the console path, and swapping it for
yield-and-retry would change the scheduling of every `printk()` in the
system to fix something a four-byte gate fixes without touching it. What
changed is only that its check-then-set of `g_printk_owner` is now
indivisible across harts.

### Converted in S5

- **`kernel/printk.c`** — `g_printk_gate` guards owner/depth/waiter.
  Released before `task_block()`, never held across it.
- **`kernel/klog.c`** — `g_klog_lock` guards the ring and `g_total`; two
  harts writing `g_ring[g_total % SIZE]` would interleave characters and
  tear the counter. The sink fanout stays *outside* it, because a sink's
  `putc()` is a UART write that can block. Separately, `g_in_fanout`
  became **per-hart**: it guards against a sink reaching `printk()` on the
  same call stack, and a call stack belongs to a hart — shared, one hart's
  fanout would have silently suppressed the other's console output rather
  than merely nesting it. First use of S0's `hart_id()` outside a
  diagnostic.
- **`kernel/balloc.c`** — `g_balloc_lock` over the buddy tree, in both
  `balloc_alloc()` and `balloc_free()`. The lazy `balloc_reserve()` stays
  outside it (it calls `palloc` and `printk`). Converted despite having no
  runtime caller but `ballocdemo` today: it hands out self-aligned
  PMP-usable blocks, so its next caller is a driver or a loader, and it
  will not think about locking any more than the three subsystems that
  needed it retrofitted did.
- **`kernel/palloc.c` `palloc_stats()` / `palloc_extra_stats()`** — both
  scans now take the lock. The tempting argument, that a diagnostic can
  tolerate a stale figure, is right about staleness and wrong about this:
  an unlocked scan racing `bit_set()`/`bit_clear()` does not return an old
  number, it returns a number for a heap state that never existed, in
  exactly the report someone reads when already suspicious about the heap.
- **`drivers/usb_cdc.c`** — `usb_cdc_putc()`'s producer-side ring update.
  This is the one the audit would have missed by reasoning: it looks like
  driver-local state that phase 23's X2 pinning would cover, and it is
  not. The function has no `USB_UATTR`, so it is *kernel* code, reached
  from the printk path by whichever task is printing and again from
  `uart_rp2350.c`'s fallback. Its lock is deliberately in ordinary kernel
  `.bss` and **not** in `g_usb`, which lives in `.ustacks16384` — the
  region PMP-granted to the U-mode USB driver. A lock word there could be
  corrupted into one that never reads free, promoting "a broken driver
  corrupts its own ring", which phase 12's isolation contains, into "a
  broken driver hangs the kernel in a spin", which it does not. The
  kernel↔U-mode side of that ring needs no lock and did not get one: this
  side only writes `head` and reads `tail`, the consumer only writes
  `tail` and reads `head`, both aligned words — single-producer/
  single-consumer, safe by construction, and never what the `irq_save()`
  was for.

### Examined and left alone, with reasons

- **`kernel/sched.c`** (4 sites) — S6's entire subject. Not touched here.
- **`kernel/ticker.c` `g_ticks`** — `ticker_count_tick()` is called only
  from the timer ISR (`arch/riscv/common/trap.c:202`) and `g_ticks++` is
  unprotected. Genuinely racy the moment a second hart takes timer
  interrupts: two ISRs would lose increments. Left, because the fix
  depends on a decision phase 23's **X4** has to make first — whether
  RP2350 gives each core its own comparator, and therefore whether this
  should be one atomic counter or one per hart. It is a diagnostic
  counter (preemption tests, `lockselftest`'s masking check), so the cost
  of being wrong is an undercount, not a corrupt kernel. **X4 owns this**;
  it is written here so X4 cannot forget it.
- **`kernel/device.c`** (`g_devs`, `g_num_devs`, `g_wire_owner`) — all 14
  `dev_register()` call sites are in `kernel/board.c`'s boot-time probe,
  before any second hart could exist. The header's "No locking: still
  single-call-stack" is *true*, and stays true as long as registration is
  boot-only. Becomes a real gap the day anything registers a device at
  runtime (hot-plug, a loadable driver), which nothing does.
- **`drivers/uart_16550.c` / `uart_rp2350.c`** — the `irq_save()` runs are
  a deliberate *continuous* mask from a fast-path miss through
  `task_block()`, documented in both files, so that a TX interrupt landing
  in the gap cannot lose the wakeup. Correct per-hart; on two harts the
  waiter slots (`g_tx_waiter`) become a race. Left because these are
  driver-task-owned paths that phase 23's **X2** pins to core 0 — unlike
  `usb_cdc_putc()` above, which only looked like one. If X2's pinning is
  ever relaxed, these are the first sites to revisit.
- **`drivers/uart_net.c`** demux ring — same class: reached through
  `uart_16550.c`'s console path, driver-owned, X2-pinned.
- **`drivers/enc28j60_rp2350.c`, `drivers/cyw43_rp2350.c`** — instances #5
  and #6 in the table, both the `if (!busy) { busy = true; }`-under-mask
  shape. Not converted, and this is a risk judgement rather than a
  correctness one: both are single-bus claims held by their own
  driver task, which X2 pins to core 0, and neither can be exercised from
  the bench available today (`enc28j60` needs the gateway persona plus
  real Ethernet hardware). Converting network drivers that cannot be
  tested, to fix a race that pinning already prevents, is the wrong trade
  for this milestone. They are named here so X2's "pin the driver tasks"
  is understood as load-bearing rather than tidy.
- **`kernel/chan.c` `chan_info()` / `chan_endpoint_busy()`** — single-byte
  `bool` reads, which cannot tear. `chan_endpoint_busy()`'s one caller
  (`uart_rp2350.c:957`) uses it to decide whether to retry or fall back,
  and the next `chan_call()` re-checks under the lock, so a stale answer
  costs at most one extra loop.
- **`kernel/balloc.c` `balloc_stats()`** — one aligned 16-bit read. Left
  unlocked *deliberately*, and the contrast with `palloc_stats()` above is
  the point: that one composes a figure from a whole scan, this one reads
  a single word that cannot tear, so the only thing exclusion would buy is
  freshness.

Static RAM: **+17**, measured — four `spinlock_t` at 4 bytes each (printk,
klog, balloc, usb_cdc) plus one byte for `g_in_fanout` becoming
`bool[MAX_HARTS]`. Per file: usb_cdc +4, balloc +4, printk +4, klog +5.

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

> **Superseded 2026-09-03 — see §6.1.** The rule stated above ("released
> before `ctx_switch()`, never held across it") is backwards, and the
> *Verify* canary derived from it would fire on correct code. The lock is
> held across the switch and released on the incoming stack. Read §6.1
> before implementing this milestone.

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

---

## 6. Amendments (2026-09-03, review against the tree)

The plan above was written 2026-08-29 from a reading of the kernel. It was
re-checked against the tree on 2026-09-03, before any implementation, and
four things changed. One of them is a correction, not an addition: **S6's
stated design rule is the inverse of the correct one** (§6.1). The rest is
work the original plan did not know it needed.

What the re-check confirmed, so it does not have to be re-established:
the A extension is already enabled on every target
(`-march=rv32imac_zicsr_zbs` / `rv64gc`, `CMakeLists.txt:40-66`) and
nothing in the tree uses an atomic today (grep for
`amoswap|__atomic|lr.w|sc.w|__sync_` across `kernel/ arch/ drivers/ fs/
net/` returns nothing), so §2's primitive needs no toolchain change. The
48 `irq_save()` sites across 13 files are as described, and
`kernel/klog.c` really does have none.

### 6.1 S6's rule is backwards — release *after* the switch, not before

§3's S6 says the scheduler lock "must be released **before**
`ctx_switch()`, never held across it." Followed literally, that is a
stack-corruption bug, and the reasoning is short enough to be checked
here rather than discovered at phase 23's X1.

`kernel/sched.c:359-390` writes the outgoing task's stack pointer
*inside* `ctx_switch()` — `REG_S sp, 0(a0)` in
`arch/riscv/common/switch.S` — which is strictly after the point where
S6 would have released the lock:

```
unlock(sched)                            /* prev is READY and visible to hart 1 */
ctx_switch(&g_tasks[prev].sp, ...)       /* prev.sp is written HERE */
```

Between those two lines the outgoing task is advertised as runnable while
its parked `sp` still holds whatever the *previous* switch left there. A
second hart that claims it in that window resumes from a stale stack
pointer, and two harts then execute on one stack. The failure is
intermittent, load-dependent, and destroys the evidence of its own cause —
the worst possible shape for something a plan could have ruled out on
paper.

The correct rule is the opposite one: the outgoing task stays unclaimable
until its context is saved, which means the lock (or an equivalent
per-task parking flag) is **held across `ctx_switch()` and released by
whoever lands on the incoming stack** — Linux's `finish_task_switch()`
shape.

This is not a foreign idea in this file. `sched_yield()` already does
exactly this hand-off for the interrupt flags, and says so at
`kernel/sched.c:381-386`: "Interrupts stay masked across the switch itself
and are restored by whichever task resumes here, from the flags IT saved.
The incoming task does the same for us." The lock follows the pattern the
scheduler already uses; it was §3's rule, not the code, that had it
backwards.

S6's *Verify* line changes with it. The new assertion is not "the lock is
never live across a `ctx_switch()`" — that canary would now fire on
correct code. It is that no task is ever observable as READY while its
parked `sp` is stale, i.e. the release happens on the incoming stack and
nowhere else.

### 6.2 New milestone S0 — per-hart state, before anything else

Neither this phase nor phase 23 accounted for the fact that **a hart
cannot currently find out which hart it is**, and that both of the
conventional places to keep that answer are already occupied. This is the
largest unpriced item in either document, it touches the most
safety-critical assembly in the tree, and everything from S6 onward
depends on it. It becomes S0, sequenced first.

- **`mhartid` is unreadable on the RV64 target.** `entry.S` performs the
  M→S transition itself (`arch/riscv/common/entry.S:38-70`,
  `CONFIG_MODE_S`), and `mhartid` is an M-mode CSR: `csrr mhartid` from
  S-mode traps as an illegal instruction. A `hart_id()` that compiles on
  both builds cannot simply read the CSR — the value has to be captured
  at boot, while still in M-mode, and kept somewhere S-mode can reach.
- **`tp` is taken.** It is saved and restored as an ordinary task
  register in the trap frame (`arch/riscv/common/entry.S:188,277`), so
  the usual RISC-V convention of reserving `tp` for the per-hart pointer
  is not free here — it means removing `tp` from the frame and auditing
  every path that assumed it round-trips.
- **`sscratch`/`mscratch` is taken, and load-bearing.** It holds the
  current task's kernel `sp` while U-mode runs, and **zero while the
  kernel runs**, so that one `csrrw` both switches stacks and reveals
  where the trap came from (`arch/riscv/common/entry.S:163-179`). That
  invariant is what removes the need for a separate "am I in user mode"
  flag, and the file says as much.

The way out is the xv6 shape: scratch holds a pointer to a per-hart
struct whose first word is the kernel `sp`, with the "were we in U-mode"
test becoming a field in that struct rather than a zero check on the
register itself. That is a restructure of the trap entry invariant, not a
one-line addition, and it is the reason S0 exists as its own milestone
rather than a bullet inside S1.

*Verify:* the existing trap, U-mode and fault suites unchanged — S0 is
behaviour-preserving on one hart by construction, in the same way §0
describes the original `irq_save()` work being landed before preemption
could exercise it. A `hart_id()` returning 0 on every current target is
the whole observable effect.

**DONE 2026-09-03.** `kernel/include/kernel/hart.h` + `kernel/hart.c`;
`CSR_SCRATCH` now carries a `hart_t *` instead of a bare kernel `sp`, with
the sp moved into the record's first word, and `tp` holds that record for
as long as the kernel is executing. The shape of the trap vector's
discrimination is unchanged (`csrrw` + `bnez`), so the restructure is a
no-op on one hart — which is what made the existing suite a real
regression check on it.

Observable: `/proc/cpuinfo` (`fs/vfs_server.c`), reporting the hart id,
the record address, and a `consistent:` line that walks the id in the
record back to the `g_harts` slot that id names and checks `tp` actually
points there. Four new QEMU tests assert it on both arches, deliberately
including the privilege mode, because the interesting half of the claim is
that this works *in S-mode* at all.

Measured, not assumed:

- **QEMU: 310/310** (was 306/306, plus the 4 new). Every U-mode, fault,
  isolation and preemption test passes unchanged.
- **RP2350 hardware** (`rp2350-clock` persona, Pico 2 W): `/proc/cpuinfo`
  reads `hart: 0, consistent: yes, priv: M, xlen: 32` on real Hazard3
  silicon, through the boot path in `arch/riscv/rp2350/boot_header.S` that
  QEMU never executes — exactly the divergence
  `[[falsify_on_hardware_not_qemu]]` exists to catch, and the reason
  SETUP_HART_POINTER is a shared macro rather than one file's prologue.
- `tests/hw/test_rp2350.py` gives a pass/fail set **byte-identical to a
  pre-S0 build of the same persona flashed on the same board** (18/24 both
  ways; the 5 failures and 1 skip are all this persona having SPISD,
  ST7735, TM1638 and CHESS compiled out, plus K3's per-persona pin list).
  That baseline was built and flashed rather than reasoned about, because
  a suite written for the chess persona failing on the clock persona looks
  exactly like a regression until someone goes and measures it.
- `priostress` reports `total_ticks=598,598 -- FAIR` on both builds, three
  runs each. It failed once on the first S0 run; that was the report
  arriving late on a just-reflashed board, and it reproduced on neither
  build afterwards.

Two things found on the way and fixed here rather than filed:

- **The scratch CSR was never zeroed at boot.** Its reset value is
  architecturally undefined and the trap vector reads it before anything
  writes it — on RP2350 the bootrom has been running in this hart's CSRs
  for a while by then. Nothing had ever set it; the kernel was getting
  away with whatever reset happened to leave behind. SETUP_HART_POINTER
  now zeroes it, on both boot paths.
- **`arch_enter_user()` was about to leak a kernel pointer.** With `tp`
  holding a kernel .bss address, entering U-mode without clearing it would
  hand every user program that pointer in a register. It is zeroed before
  the trap return.

Cost: **+32 bytes** of static RAM (`MAX_HARTS * sizeof(hart_t)`), and
nothing else, on both RP2350 personas. A pre-existing **+192 bytes** in
`kernel/sched.c` — left by commit `12384e8` (the timed-sleep fix), which
re-baselined only the clock persona and so had `sizecheck` failing on main
for the chess one — was found while re-cutting these baselines and landed
separately on main as `0cebceb`, attributed to the commit that caused it
rather than folded in here.

### 6.3 The atomic goes behind an arch hook

§2 argues the primitive is target-independent because "RISC-V
`amoswap`/`lr`/`sc` are defined identically whether or not a second hart
is ever listening." That is a statement about the ISA, and this project's
own standing rule is that Hazard3 is not assumed to match the ISA until
it has been measured against it — `[[falsify_on_hardware_not_qemu]]`,
six divergences so far. RP2350 additionally ships 32 SIO hardware
spinlocks, which exist precisely because cross-core exclusion is
something the chip offers directly.

So S1 gains one constraint: the atomic test-and-set sits behind a single
arch-level hook, so that a RP2350 build can be switched to a SIO spinlock
without touching one call site in `fs/`, `kernel/` or `drivers/`. Whether
it needs to be is an X3-era measurement, not a decision to make now — the
point is only that finding out must not be a refactor.

### 6.4 S5's convert list is longer than S2-S4's three

`kernel/printk.c` (`:78,92,99,108`) and `kernel/console.c`
(`:111,118,129,204`) both guard shared static buffers with
`irq_save()`/`irq_restore()`. They are not in the "no protection at all"
bucket S5 names (`balloc.c`, `ticker.c`, `klog.c`), so the original text
folded them into the general audit — but they are in fact the same
convert-to-`spinlock_t` job as S3 and S4, and they are the *first* thing
that visibly breaks once two harts run, well before any test asserts
anything, because both harts print. They are called out here so the audit
does not have to rediscover them, and so that S5's "converted / genuinely
safe, and why" list starts from 15 known sites rather than 3.
