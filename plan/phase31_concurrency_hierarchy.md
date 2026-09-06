# Phase 31 — One wait-for graph, and no cycles in it

**Status: planned, not started, 2026-09-06. Runs before phase 30, which runs
before phase 28.** The ordering is in §0.4, and there is a standing rule in
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
