# Phase 31 — One wait-for graph, and no cycles in it

**Status: Y0 done 2026-09-10 — the inventory, plus five findings including a
reproduced whole-kernel hang (F1). Y1-Y4 not started. Runs before phase 30,
which runs before phase 28.** The ordering is in §0.4, and there is a standing rule in
§0.5 that can pull this phase forward on its own.

**Milestone letter: `Y`.** A–G, H–N and P–T, V–X are spoken for across
`plan/`; `O`, `U` and `Z` remain free after this.

## 0. Why this phase exists

### 0.1 The rule this project started with, and what happened to it

LugalOS began with one concurrency hazard and one rule for it. `chan_call()`
is synchronous — the caller blocks until the owning task replies — so a cycle
of channel calls is a deadlock, and the rule was that circular calls are not
allowed.

That rule is not a comment. `kernel/chan.c` **enforces** it:

```c
if (would_cycle(me, ep->owner_pid)) {
    printk("[Chan] Refusing '%s': task %d -> %d would close a circular wait\n", ...);
    return -1;
}
```

`would_cycle()` walks `g_wait_for[]`, a task→task wait-for graph, and refuses
the call before anything is written to the endpoint. Every caller in the tree
already handles the `-1`. This is the shape the rest of this phase is trying
to reach: an invariant that is checked rather than remembered.

Then locks arrived — phase 22's `spinlock_t` and `ylock_t`, phase 23's
per-hart scheduling — and the rule did not grow with them.

### 0.2 The state of things, counted

| | enforced? |
|---|---|
| **Channels** — 81 `chan_call()` sites, 29 `chan_serve_wait()` | **Yes.** `would_cycle()` refuses a circular wait at run time. |
| **Locks** — 16 objects, 54 `spin_lock_irqsave()` sites, 11 `ylock_acquire()` | **No.** No ordering, no levels, no check. `ylock_t` records an `owner` and nothing ever builds a graph from it. |
| **Blocking waits** — 43 `task_block()` sites | **No.** Nothing says which contexts may block at all. |

The locks, and what each protects:

```
kernel/palloc.c    g_palloc_lock     the page bitmap
kernel/balloc.c    g_balloc_lock     the buddy arena
kernel/klog.c      g_klog_lock       the log ring
kernel/printk.c    g_printk_gate     the ownership protocol's own state
kernel/sched.c     g_sched_lock      the task table, ready queue, reap slot
kernel/smp.c       g_smp_lock, g_load_lock, g_smptest_lock
drivers/uart_*.c   g_tx_batch_lock   (three separate instances, one per driver)
drivers/usb_cdc.c  g_usb_tx_lock
fs/p9_link.c       g_pump_lock, g_client_lock      (ylock_t)
net/mqtt.c         g_client_lock                   (ylock_t)
kernel/lock.c      g_test_ylock                    (selftest only)
```

Plus one that is a lock without being either type: **printk's ownership
protocol** (`g_printk_owner` / `g_printk_depth`), which is a blocking,
re-entrant, task-owned lock built on `task_block()` and guarded by
`g_printk_gate`. It behaves like a `ylock_t` and is not one, which is worth
knowing before reasoning about it.

### 0.3 The gap that matters: the two graphs do not compose

`would_cycle()` sees **only task→task channel edges**. It knows nothing about
locks. So this cycle is invisible to it:

> task A holds lock L → `chan_call()` to task B → task B tries to take L

A is blocked waiting for B; B is spinning or yielding for a lock only A can
release. Nothing in the tree detects it, and nothing prevents it.

This is not hypothetical, and the tree knows it. `kernel/printk.c` documents
exactly this cycle and avoids it by ordering alone:

> The tempting fix was to flush while still holding ownership. It trades this
> bug for a worse one: `uart_flush()` blocks in `chan_call()` waiting for the
> uart task, so printk ownership would be held across that wait, and anything
> the uart task needs printk for deadlocks against it.

And `drivers/uart_16550.c`'s task body carries the other half of the same
cycle as a standing instruction:

> Must never `printk()` from inside this loop — a caller can be blocked on
> this very endpoint while holding `printk_lock()`, and taking that lock here
> would deadlock against it.

There are roughly twenty such comments across `kernel/`, `drivers/`, `fs/` and
`arch/`. Each is correct. Collectively they are an invariant maintained by
whoever remembers to read them.

### 0.4 What that costs, measured on one afternoon

Phase 27's E4 hit this class **three times in a single session**, and the
third was in the fix for the second:

1. **`task_exit()` announced its own death with `printk()`**, which blocks
   through `uart_flush()` → `chan_call()` → `task_block()`. A task switched
   away in the middle of dying and a second task entered `task_exit()` behind
   it. Deterministic crash.
2. **The obvious fix — announce from `sched_reap()` — hung the board**, because
   `sched_reap()` is called from `sched_yield()`, which the timer interrupt
   calls. A blocking IPC call from interrupt context.
3. **The diagnostic written to investigate (1) had the same bug**:
   `sched_check_incoming()` runs holding `g_sched_lock`, and a `printk()`
   there blocks while holding the lock the task it waits for needs.

Each was found by running into it. The fix (`printk_critical()`, phase 27 E4)
is correct and is a *special case* of a general rule nobody has written down:
**some contexts may not block at all.** That a special case had to be
discovered by crashing is the argument for this phase.

And one violation is **still open**: `task_exit()` calls `palloc_free()` while
holding `g_sched_lock`, which `sched_reap()`'s own comment says must not
happen —

> palloc_free() takes palloc's own lock (S4), and nesting the two would create
> a lock ordering this kernel has no reason to have.

It has one now. Nothing detected it; it was found by reading the file while
looking for something else. **It is this phase's first work item.**

### 0.5 Sequencing, and a standing rule

**Phase 31 → phase 30 → phase 28.**

Before phase 30 for two reasons. The open violation in §0.4 should not wait
behind a refactor. And phase 30 *moves code between contexts* — extracting
driver task bodies and U-mode domain construction into shared code — which is
precisely the operation that silently creates a new cycle. A checker in place
first makes that refactor verified rather than hopeful.

**The standing rule, agreed 2026-09-06:** *if phase 27 gets stuck again on
this class of problem, phase 27 stops and this phase runs first.* Three
instances in one session is enough evidence that working around the fourth
would be the wrong call. "This class" means: a deadlock, a hang, or a
corruption traced to a lock held across a block, a blocking call from
interrupt context, or a cycle spanning locks and channels.

**The rule was tested on 2026-09-08, and correctly did not fire.** Phase 27's
E7 hit exactly the presentation this phase exists to catch — task context
frames zeroed under a live task, a console write that blocked forever, a
scheduler halting mid-switch — and it was on its way here as a work item. It
does not belong here. The cause was `linker/esp32p4.ld` handing out the top
256 KB of L2MEM as heap when the hardware had reserved it as the L2 cache's own
storage (see phase 27 §E7): memory that accepts a store, returns it on an
immediate read, and decays to zero afterwards. Nothing raced, no lock was held
across a block, and no channel was involved.

Recorded because the near-miss is the useful part. The scoping clause above —
*traced to* one of three named mechanisms, rather than "looks like a
concurrency bug" — is what kept a memory-map bug out of this phase's backlog,
and the discipline that satisfied it was refusing to accept a plausible cause
without a measurement that excluded the others. Two of the instruments built to
do that excluding are worth keeping in mind here, because a real cycle would
need the same treatment: an allocation-side overlap guard that stayed silent,
and a hardware store watchpoint that proved no store was happening at all.

## 1. What this phase defines

### 1.1 One graph, not two

The rule is not "channels must not form cycles, and separately locks must be
ordered". It is:

> **Every blocking primitive is an edge in one wait-for graph, and that graph
> must be acyclic.**

`chan_call()`, `task_block()`, `ylock_acquire()` and `spin_lock_irqsave()` are
all edges. A cycle is a deadlock regardless of which mix of mechanisms it
passes through, and the existing `would_cycle()` covers only one kind of edge.

### 1.2 A strict level per lock

Every lock gets a level. A holder of level *n* may only acquire locks of level
strictly greater than *n*. Acquiring at or below the current level is a bug,
caught rather than reasoned about.

Provisional assignment, to be settled by G1's inventory rather than asserted
here — the leaves are the allocators and the log ring, because everything may
need them and they need nothing:

```
  0  (nothing held)
  1  g_palloc_lock, g_balloc_lock, g_klog_lock, g_tx_batch_lock, g_usb_tx_lock
  2  g_printk_gate
  3  g_sched_lock
  4  g_smp_lock, g_load_lock
```

`task_exit()`'s `palloc_free()` under `g_sched_lock` is 3 → 1, which this
ordering rejects, which is the point.

### 1.3 A context rule

Orthogonal to levels, and the thing phase 27 E4 discovered the hard way:

* **Interrupt context** may not block: no `chan_call()`, no `task_block()`, no
  `ylock_acquire()`, no `printk()`.
* **Mid-switch and mid-exit** may not block, for the same reason with a
  different mechanism: the task is not in a state where being descheduled is
  safe.
* **Holding any lock**, no `chan_call()` and no `task_block()` — a blocking
  wait with a lock held is the cross-mechanism cycle of §0.3, and forbidding
  it outright is simpler than ordering against it.

`printk_critical()` already exists as the escape hatch for the first two.

## 2. Milestones

### Y0 — The inventory

Every lock, its level, what it protects, and which contexts may take it.
Every blocking primitive and the contexts it is legal in. Written where a
person about to add a lock will meet it — `kernel/include/kernel/lock.h`'s
header comment is the natural home, since it already argues about
`spinlock_t` vs `ylock_t` as a correctness decision.

Done when: every lock in §0.2 has a level, and every level is justified by
what would break if two were swapped.

#### Y0 done — the inventory, and what reading it found — 2026-09-10

**The count in §0.2 is wrong: there are 18 kinds of lock, not 16, and up to
33 instances.** Missing were `drivers/flash_esp32p4.c`'s `g_flash_ylock`
(arrived with phase 27 E6, after §0.2 was written) and — more interesting —
`chan_endpoint_t::lock`, a `spinlock_t` **per endpoint**, sixteen of them
(`CHAN_MAX_ENDPOINTS`). The channel layer was counted as "the enforced one"
and its own locks were not counted at all.

##### The actual hierarchy, from the code rather than from intuition

The evidence is that **every `spinlock_t` in this tree is a leaf**. Each
critical section calls only pure helpers — `bit_get`/`bit_set`, `hart_id`,
`memcpy`, `node_parent` — and the two exceptions are both in `task_exit()`
and are both bugs (below). Where a section looked like it called something
blocking, it turned out to release and re-take around it, with a comment
saying why: `uart_putc()` around `uart_flush()`, `printk_lock()` around
`task_block()`. That discipline is real and it holds everywhere.

`ylock_t` is the opposite by design, and the header says so: *"sections that
can be held across a wait (a 9P round trip waits for a peer's reply)"*. So:

| level | what | may acquire |
|---|---|---|
| 1 | `ylock_t`: `g_pump_lock`, `g_client_lock` (p9), `g_client_lock` (mqtt), `g_flash_ylock` | anything above |
| 2 | printk ownership (`g_printk_owner`/`g_printk_depth`) | level 3 only |
| 3 | every `spinlock_t`: `g_palloc_lock`, `g_balloc_lock`, `g_klog_lock`, `g_printk_gate`, `g_sched_lock`, `g_smp_lock`, `g_load_lock`, `g_smptest_lock`, `g_tx_batch_lock` ×3, `g_usb_tx_lock`, `ep->lock` ×16 | **nothing** |

Read with §1.2's rule (*a holder of level n may only acquire strictly greater
than n*) this says one thing: **a `spinlock_t` is a leaf, full stop.** That is
a stronger and simpler invariant than a graduated ordering, it is what the
code already does, and it is checkable with one comparison.

**§1.2's provisional table is inverted and should be replaced by the above.**
It put the allocators at level 1 and `g_sched_lock` at 3, which permits a
holder of `g_palloc_lock` to acquire `g_sched_lock` — the direction that must
never happen. It rejects `task_exit()`'s violation as claimed, but by
accident: 3 → 1 is rejected because 1 is not greater than 3, not because the
allocator is a leaf.

##### F1 — `task_exit()` deadlocks the whole kernel, and the deadlock is the recovery path

**Confirmed by reproduction, rv32 QEMU, one hart, 2026-09-10.**

`task_exit()` takes `g_sched_lock` (`kernel/sched.c:988`) and then calls
`chan_owner_exited()` (`:996`) — which calls `task_unblock()`, which takes
`g_sched_lock` again (`:827`). `spinlock_t` is not re-entrant and interrupts
are already off, so the hart spins forever on a lock only it holds.

It fires whenever a task that owns a channel endpoint dies **with a request
pending** — which is precisely the case `chan_owner_exited()` was written for
(M5 Phase 2, commit f36ceaa, "real U-mode isolation for a driver with an IPC
endpoint"). Its own comment: *"its caller would otherwise block forever
waiting for a reply nothing will ever send."* The caller no longer blocks
forever; the whole board does.

Reachable in production, not only in theory: `arch/riscv/common/trap.c:1163`
calls `task_exit()` for **any U-mode task that faults**, and on RP2350 the
i2c, uart, tm1638, st7735 and spisd driver tasks are U-mode tasks that own
endpoints. A driver faulting mid-request hangs the machine instead of
returning `-1` to its caller.

The reproducer is twenty lines: register an endpoint to a task, have that
task return from `chan_serve_wait()` without replying, and `chan_call()` it
from the shell. Observed: `[Sched] Task #4 'dxowner' exited` prints, and then
nothing at all — no return from `chan_call()`, no later command, no `halt`.

The QEMU suite does not catch it because the isolation tests fault an
*intruder* task, never the endpoint's owner while a request is in flight.

##### F2 — `palloc_free()` under `g_sched_lock` is real, and is latent rather than live

The violation §0.4 names is still there (`kernel/sched.c:1024`), and it is
worth being precise about its severity: **it is not a deadlock today.**
`g_palloc_lock`'s critical sections call only `bit_get`/`bit_set`/`bit_clear`,
so no path takes `g_palloc_lock` and then `g_sched_lock`; the reverse edge
that would close the cycle does not exist.

What makes it worth fixing anyway is that `sched_reap()` — sixty lines
earlier in the same file — deliberately releases `g_sched_lock` *before*
calling `palloc_free()`, and says why. Two paths to the same allocator, one
correct and one not, is how the reverse edge eventually gets added by someone
reading the wrong one.

##### F3 — `g_sched_lock` is handed off, not released, and that defeats analysis

On `task_exit()`'s normal path the lock is taken and **never released**: the
successor inherits it and releases it in `task_start()` or on return from
`ctx_switch()`. So its critical section is not a lexical region, and any tool
that pairs an acquire with the next release in the same function — which is
the obvious way to write one — sees no section at all. That is exactly why a
first pass over this tree missed both F1 and F2 while reporting several
correct drop-and-retake idioms as violations.

**Y2's checker must model the hand-off explicitly**, not infer scope from
brace structure. `handoff_check()` already exists and is the hook.

##### F4 — §1.3's third bullet is too strong and must be per-type

> *"Holding any lock, no `chan_call()` and no `task_block()`."*

That forbids what `ylock_t` was built for. `fs/p9_link.c`'s `p9_link_pump()`
holds `g_pump_lock` across `link->poll()`, `recv_frame()` and a whole
`p9_route_frame()` — which includes `printk()` and can include blocking IPC —
and `net/mqtt.c` holds `g_client_lock` across `write_all()` and
`mqtt_service()`. Both are the documented intent, not oversights.

The rule has to split:

* **`spinlock_t`** — never held across anything that can block. Already the
  header's contract; F1 and F2 are its only violations.
* **`ylock_t`** — *may* be held across a block. It therefore needs
  **ordering**, not prohibition: every ylock sits above everything it can
  wait on, and no task holding one may be waited on by a task it waits for.

##### F5 — the one enforced invariant is enforced with an unsynchronised read

`would_cycle()` walks `g_wait_for[]` (`kernel/chan.c:65`) **before**
`chan_call_task()`'s `irq_save()` and under no lock at all, while another
hart can be writing `g_wait_for[me]` at `:193`. The consequence is a missed
refusal — a cycle that should have been rejected is admitted — rather than
corruption, and the window is a few instructions. It is worth recording that
the one place this tree does check a wait-for graph is the one place the
check is not itself synchronised on a two-hart kernel.

##### What Y1 now has to decide

F1 is a kernel hang and should be fixed before F2, which is latent. The
obvious shape — have `task_exit()` do its `chan_owner_exited()` work before
taking `g_sched_lock`, or give `task_unblock()` a lock-already-held variant —
is a real design choice with a race to argue about either way, and belongs in
Y1 rather than being picked here.

### Y1 — The open violation

`task_exit()` calls `palloc_free()` while holding `g_sched_lock`. Fix it, and
in doing so decide whether the one-slot reap design is still right now that
`task_exit()` no longer blocks (phase 27 E4 removed the printk that made
concurrent exits possible, so the slot's stated assumption may hold again —
"may" is not "does", and this milestone is where that gets established).

Done when: no lock is held across `palloc_free()` anywhere, and the reap
slot's comment states an invariant that is true rather than one that was.

### Y2 — The checker

A per-context held-lock level, asserted on acquire. `spin_lock_irqsave()` and
`ylock_acquire()` refuse — loudly, with both lock names — an acquisition at or
below the current level. `chan_call()` and `task_block()` refuse if any lock
is held.

This is the milestone that turns twenty comments into twenty checks, and it is
the same move this tree has made before: `handoff_check()` for the scheduler
lock hand-off, the stack poison scan for depth, `rx_wakes` for interrupt-driven
RX. An invariant that is only documented is one nobody can test.

Cost to keep it honest: a counter per hart and a compare per acquire. If that
turns out to matter it can be compiled out per preset, but it should not be
compiled out by default — the QEMU suite is where it will earn its keep.

Done when: the checker is on in every build, the QEMU suite is 359/359 with it
armed, and deliberately taking two locks out of order produces a named
diagnostic rather than a hang.

### Y3 — Fold the channel graph in

`would_cycle()` becomes one case of the general check rather than a separate
mechanism: a channel call is an edge like any other. Either extend it to know
about lock ownership, or express both through the same structure.

Done when: the cycle in §0.3 — lock held → `chan_call` → same lock — is
refused, and there is a test that constructs it deliberately.

### Y4 — The audit

Walk the ~20 existing hazard comments. Each one is either (a) now enforced by
Y2/Y3 and the comment can point at the check instead of restating the rule,
or (b) not covered, which means the rule has a gap and the gap is the finding.

Done when: every comment in that set is in category (a), or the exceptions are
listed here with reasons.

## 3. How it is tested

* **The QEMU suite is the regression net**, exactly as it was for phase 22's
  locking: 359 tests that exercise the scheduler, the channels, the drivers
  and two harts. A checker that is wrong will not be subtle.
* **A deliberate violation test**, in the `lockselftest` family: take two locks
  out of order, call `chan_call()` with a lock held, and assert that each is
  refused with a named diagnostic.
* **Hardware**, for the RP2350 personas and the ESP32-P4, because interrupt
  context is where the rule matters most and QEMU's timing hides it — phase 27
  E4's bug passed every QEMU test.

## 4. Explicitly not in this phase

* **No new locking primitives.** `spinlock_t` and `ylock_t` are sufficient;
  this is about how the existing ones compose.
* **No lock-free rewrites.** Removing a lock to avoid ordering it is a way of
  not answering the question.
* **No change to `chan_call()`'s semantics.** Synchronous, copy-always, refuse
  on cycle — all unchanged. Y3 widens what counts as a cycle, nothing else.
* **Nothing about phase 27's remaining milestones**, except the standing rule
  in §0.5 that can interrupt them.

## 5. Risks

* **The inventory turns up a genuine two-order requirement** — two locks that
  must be takeable in either order. Then the answer is to restructure those
  two, not to weaken the rule to accommodate them. A hierarchy with an
  exception is not a hierarchy, and the exception is where the next deadlock
  will live. Watch for this in Y0 and treat it as a design finding rather than
  a checker limitation.
* **The checker costs more than expected on the hot path.** `sched_yield()`
  takes `g_sched_lock` on every switch, and preemption makes that frequent.
  Measure before deciding; the phase-27 E4 tick measurement is the method.
* **The checker is itself in the scheduler**, so it must obey its own rules:
  it may not `printk()` from a context that cannot block. `printk_critical()`
  exists for exactly this, and Y2 will be its first heavy user.
* **False confidence.** A passing checker proves no cycle was *taken*, not
  that none exists. Static ordering is what makes the absence structural; the
  runtime check is how the ordering is kept honest.

## 6. Budget

Small, and front-loaded onto thinking rather than typing. Y0 is reading and a
table. Y1 is a bug fix. Y2 is the bulk — a counter, two assertions, and the
argument about where to put them. Y3 and Y4 are consolidation.

The measure of success is that the twenty comments become redundant, and that
the next instance of §0.4's failure class is a named refusal at the moment of
the mistake rather than a crash three milestones later.
