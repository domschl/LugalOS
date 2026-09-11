# Phase 31 — One wait-for graph, and no cycles in it

**Status: Y0-Y4 done 2026-09-11; REOPENED the same day for Y5.** One wait-for
graph fed by channels, ylocks and printk ownership; a leaf rule for
`spinlock_t` checked at every acquire; two real bugs found and fixed (a
reproduced whole-kernel hang in `task_exit()`, and a lock-ordering inversion
beside it). Two exceptions are listed under Y4 rather than closed: interrupt
context is still convention, and two hand-rolled yielding locks in the RP2350
drivers are outside the graph.

**Y5 asks the better question about the third of those resources: why is
printk ownership an edge at all?** Kernel logging becomes append-to-a-ring and
return — deadlock-free by construction rather than by checking — and the graph
drops to two contributors. It slices in ahead of phase 30's G3, because G3 and
G4 would otherwise migrate fourteen drivers under a constraint Y5 deletes.
Phase 30 is paused at G2; phase 28 still follows. The ordering is in §0.4, and
there is a standing rule in §0.5 that can pull this phase forward on its own.

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

#### Y1 analysis — the options for F1, and what settles it — 2026-09-10

**What `g_sched_lock` was actually doing at that call, which decides the
fix.** `kernel/chan.c` never takes `g_sched_lock` — endpoint state is
protected by `ep->lock`, and only for the `busy` flag. So holding the
scheduler lock across `chan_owner_exited()` provided **no mutual exclusion
whatsoever**. Its only purpose is the one its own comment gives: the caller
must be READY *before* `next_runnable()` runs, so it is eligible to be picked
as the successor.

That is an ordering requirement, not an atomicity one, and it is satisfied by
doing the work *earlier* just as well as by doing it *inside*.

##### Option A — move `chan_owner_exited()` above the lock (recommended)

```c
chan_owner_exited(t->pid);                              /* unblocks the caller */
uintptr_t flags = spin_lock_irqsave(&g_sched_lock);     /* then claim the table */
int next = next_runnable(cur());
```

Two lines moved. `task_unblock()` keeps its own locking, `chan.c` learns
nothing about the scheduler's internals, and the ordering the comment asks
for still holds: the caller is READY before `next_runnable()` looks.

*Verified*: with this applied the F1 reproducer prints `chan_call returned -1`
and the shell keeps answering, where before the board stopped dead. The QEMU
suite is 363/363, six presets build clean.

*The window it opens, and why it is acceptable.* `chan_owner_exited()` now
runs with interrupts enabled, so a tick can preempt mid-exit between it and
the lock. That window already exists and is already longer — `printk_critical()`
sits in it and is a bounded spin on the UART FIFO, milliseconds at 115200 —
and the code below already defends against what it allows (see the reap slot,
next). What the exiting task must not do in that window is *block*, which is
E4's rule and which neither call does.

##### Option B — a lock-already-held variant of `task_unblock()`

Add `task_unblock_locked()` and a `chan_owner_exited_locked()`, keeping the
unblock and `next_runnable()` under one continuous lock.

Strictly more atomic, and rejected: it buys atomicity nothing needs. The
transition it would make atomic is `caller → READY` *plus* `pick successor`,
and no invariant depends on those two being indivisible — a caller woken a
moment early simply returns `-1` from `chan_call()` and carries on, which is
the correct outcome by any timing. The cost is real: `chan.c` would have to
know that it is called with a lock it never takes, which is precisely the
kind of implicit contract this phase exists to remove.

##### Option C — handle owner death in `sched_reap()` instead

Rejected outright. `sched_reap()` is called from `sched_yield()`, which the
timer interrupt calls — E4's finding #2, the fix that hung the board. Moving
work there is moving it into interrupt context.

##### Option D — make `g_sched_lock` re-entrant

Rejected. §4 rules out new primitives, and it would be the wrong answer
anyway: re-entrancy would hide the ordering question rather than answer it,
and every other user of that lock would pay for it.

##### The second half of Y1: the reap slot's invariant is false, and the code already knows

> *"One slot suffices because a task can only exit while running, and the
> next task reaps before anything else can exit."*

The second clause is not true. `task_exit()` runs with interrupts **enabled**
from entry until `spin_lock_irqsave()`, and `printk_critical()` sits in that
window — a bounded spin on the UART FIFO, which at 115200 baud is milliseconds
for one line, far longer than a 10 ms tick needs to land. A second task can
and does reach its own `task_exit()` while the first is mid-exit.

The code does not rely on the false clause: it frees any stack it finds in the
slot before claiming it, and says so. **The recommendation is to keep one slot
and fix the sentence**, because the slot is sufficient for a reason that is
maintained rather than assumed:

> At most one stack is ever pending, because `task_exit()` frees whatever it
> finds before claiming the slot. Two tasks mid-exit serialise on
> `g_sched_lock`, and the one that arrives second is provably not running on
> the first's stack — the first reached `ctx_switch()` before releasing, so
> the successor that released is not it.

##### What this hands to F2, and why the order matters

F2 wants `palloc_free()` out from under `g_sched_lock`. In `task_exit()`
there is no "after the lock" to move it to — the lock is handed off, never
released — so the free must move *before* the acquire, and reading and
clearing `g_reap_stack` outside the lock is a double-free on two harts.

Two shapes are worth weighing when F2 is taken up, and both are cleaner than
moving the call:

* **Call `sched_reap()` at the top of `task_exit()`.** It already claims under
  the lock and frees outside it, correctly, so the slot is empty on arrival
  and the defensive branch becomes unreachable rather than merely unused. The
  residual is that another hart can refill the slot between that call and the
  acquire.
* **Make the slot as deep as the hart count.** Then "occupied" is bounded by
  the number of tasks that can be mid-exit at once, the reaper drains all
  entries, and nothing ever needs to free under the lock. This removes the
  residual above rather than shrinking it.

There is also a third position that should be argued rather than assumed:
`g_palloc_lock` is a leaf, so `g_sched_lock → g_palloc_lock` **cannot cycle**,
and F2 could legitimately be closed by declaring leaf-only nesting legal and
correcting `sched_reap()`'s comment instead of the code. Y0's table would then
read "a spinlock holder may acquire only a leaf spinlock" rather than
"nothing". That is a weaker invariant and a cheaper one; whether the extra
freedom is worth giving up the one-comparison check is F2's call.

#### F2 done — the slot went per-hart, and the free stopped being needed — 2026-09-10

**The third position above was rejected, and the reason is worth keeping.**
Blessing `g_sched_lock → g_palloc_lock` would permanently forbid the reverse,
and the tree already shows pressure in that direction:
`palloc_report_alloc()` scans the scheduler's task table and **wants** the
scheduler lock — its own comment says it may not take it precisely because
sched → palloc exists, and settles for admitting the read can tear. Keeping
the nesting would have made that concession permanent.

**The fix was not to move the free but to remove the need for it.** The
defensive `palloc_free()` under the lock existed to handle a slot that was
already occupied, and a *global* slot could be occupied for one reason only:
a task exiting on one hart finding another hart's hand-off still pending.
`g_reap_stack` and `g_reap_pages` are now arrays indexed by hart.

That makes the slot's invariant structural rather than defended. A dead task's
successor always runs on the hart the task died on, and **every path that
starts running a task on a hart reaps first** — `sched_yield()` calls
`sched_reap()` before it picks anything *and* again after `ctx_switch()`
returns, and `task_start()` calls it on a first run. So a task can only reach
`task_exit()` after its own hart's slot has been drained. Per-hart slots
cannot collide; there is nothing left to free.

An occupied slot now means that property has been broken, so `task_exit()`
halts and says which hart and which stack, the same treatment
`sched_check_free_range()` already gives its own impossible case. Overwriting
would leak a stack silently and freeing would put back the nesting this
milestone removed.

**Y1's done-condition is now met in full**: no lock is held across
`palloc_free()` anywhere — the only call left in `kernel/sched.c` is
`sched_reap()`'s, after the lock is released — and the reap slot's comment
states the invariant the code maintains rather than one it assumed.

Verified: 363/363 QEMU including the whole SMP arm, which is where a per-hart
slot is the change that matters; six presets warning-clean; 12/12 on the
ESP32-P4 hardware suite.

**What this unblocks, recorded rather than taken.** With no `sched → palloc`
ordering left anywhere, `palloc_report_alloc()` *could* now take
`g_sched_lock` and scan the table exactly instead of racily. That is a change
to a guard that is working, and the argument for it belongs where lock levels
are decided — Y2 — rather than beside this fix. `kernel/sched.c` carries the
note so the option is not lost.

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

#### Y2 done — the leaf check, and two bugs in the checker itself — 2026-09-11

**One comparison, not a level table.** Y0 found that every `spinlock_t` in the
tree is a leaf, so the rule the checker enforces is *"while a `spinlock_t` is
held, nothing may be acquired and nothing may block"* — stronger than §1.2's
graduated levels, true of the code as it stands, and needing no table that has
to be kept right. §1.2's provisional table is superseded; the argument now
lives in `kernel/include/kernel/lock.h`, which is where Y0 said it belonged.

**Per-hart state, which is what makes the hand-off a non-problem.** F3 warned
that `g_sched_lock` is acquired by `task_exit()` and released by a different
task, so its section is not lexical. Per-hart counting does not care: a
hand-off crosses `ctx_switch()`, which stays on one hart, so the increment and
the decrement land on the same counter whoever performs them. It also cannot
be per-*task*, because a task may migrate harts — but only across a block, and
a spinlock is never held across one.

**The name comes from the call site, not from the lock.** `spin_lock_irqsave`
and `ylock_acquire` are now macros that capture `#l` and `__func__`, so a
diagnostic reads `took &inner (in lock_selftest()) while holding &outer, taken
in lock_selftest()` without adding a field to every `spinlock_t`. The cost is
`.rodata`, which on RP2350 is flash rather than the heap `.bss` competes with.

**Reports, does not halt, and reports *before* the acquisition that would
hang** — so F1's failure mode (a board that stops dead after one line) becomes
a named diagnostic followed by the same hang. `chan_call()` is the exception
that refuses outright, because refusal is already its contract and every
caller handles `-1` by falling back to direct access.

##### The two bugs were both in the checker, and both were the same mistake

Nothing in the tree tripped the checker — zero violations across the whole
QEMU suite. What it caught immediately was itself, twice, and the pattern is
worth recording because any future instrumentation here will meet it:

* **It counted its own diagnostic.** `printk_critical()` reaches the console
  through the UART driver's spinlock, so every acquire the *report* makes
  arrives back in the checker with a lock held. The recursion guard
  suppressed the message but sat *below* `g_lock_faults++`, so one deliberate
  violation reported "fault 55".
* **It destroyed the state it was about to print.** The held lock's name was
  read from the globals inline, on the third line of a message whose first two
  lines had already taken and released the console's lock and overwritten it.
  Every violation reported as `while holding &g_klog_lock` — wrong, and
  plausible enough to be believed. Both were found by the deliberate-violation
  test in the first run.

**The instrument must be inert in its own measurement.** That is the general
form, and it applies to Y3's work as much as to this.

##### Cost, measured rather than assumed

§5 warned the checker might cost too much on a path `sched_yield()` takes on
every switch. Measured like-for-like, same machine, minutes apart: **182.8 s
without, 185.6 s with** — about 1.5%, against a four-run baseline whose own
spread is 3.4 s (179.4–182.8). An earlier run at 203.1 s was machine
contention, not the checker, and is recorded because reporting only the
convenient number would be the same error as the two above. The honest
statement is that this measurement cannot resolve a cost much below 2%; the
per-preset compile-out §5 allows remains available and unused.

Verified: 363/363 QEMU with it armed, ten presets warning-clean, `lockselftest`
9/9 on QEMU **and on ESP32-P4 silicon**, with exactly the two deliberate
faults and no others.

### Y3 — Fold the channel graph in

`would_cycle()` becomes one case of the general check rather than a separate
mechanism: a channel call is an edge like any other. Either extend it to know
about lock ownership, or express both through the same structure.

Done when: the cycle in §0.3 — lock held → `chan_call` → same lock — is
refused, and there is a test that constructs it deliberately.

#### Y3 done — one graph, and §0.3's cycle refused — 2026-09-11

**Expressed through one structure rather than two mechanisms**, which was the
second of the options this milestone offered and the one that leaves nothing
to keep in sync. `g_wait_for[]` moved out of `kernel/chan.c` into
`kernel/lock.c`, and **every blocking primitive that waits on a task now
contributes an edge**:

* `chan_call()` — the caller waits for the endpoint's owner (as before);
* `ylock_acquire()` — the acquirer waits for the task holding the ylock (new,
  and the half that was missing).

That is the whole of why §0.3's cycle used to be invisible: the B→A half of it
was a *lock* edge, and the graph had never heard of locks.

**Spinlocks are deliberately not edges.** A `spinlock_t` may not be held
across a block at all — Y2 enforces that outright — so it can never be half of
a cycle. A stronger guarantee than being counted in a graph, and free.

**F5 is closed as part of it.** `waitfor_enter()` tests for a cycle *and* adds
the edge under one lock. The previous shape walked the array with no lock at
all and then wrote the edge separately, so on two harts the answer could be
stale before it was acted on — the one invariant this tree actually enforced
was enforced with an unsynchronised read.

##### The test builds §0.3 in the order that lets it be refused

Both halves of the cycle can detect it, but only one half can *do* anything:
`chan_call()` has a refusal in its contract and every caller handles `-1`,
while `ylock_acquire()` has no failure to return. So the test assembles it as
*the lock holder calls the lock waiter*:

1. the selftest task takes a ylock;
2. a second task tries to take it, and yields — recording the edge;
3. the selftest registers an endpoint to that task and calls it.

```
[Chan] Refusing 'lockcycle': task 0 -> 4 would close a circular wait
```

A working checker never blocks here, and the precondition — that the edge was
actually recorded — is asserted before the call is made, so a regression fails
the assertion instead of hanging a 363-test suite on a task that is not
serving. Where the cycle closes at `ylock_acquire()` instead, it is reported
and the wait continues: a livelock anyone can diagnose rather than one nobody
can.

Verified: `lockselftest` 12/12 on QEMU **and on ESP32-P4 silicon**, 363/363
QEMU (184.1 s, within the band measured in Y2), ten presets warning-clean.

### Y4 — The audit

Walk the ~20 existing hazard comments. Each one is either (a) now enforced by
Y2/Y3 and the comment can point at the check instead of restating the rule,
or (b) not covered, which means the rule has a gap and the gap is the finding.

Done when: every comment in that set is in category (a), or the exceptions are
listed here with reasons.

#### Y4 done — the audit, one more gap closed, and two real exceptions — 2026-09-11

33 hazard comments across `kernel/`, `drivers/`, `fs/` and `net/`. Most are
now category (a) and say so, pointing at the check rather than restating the
rule. Two classes are not, and they are the finding.

##### The gap the audit itself closed: printk ownership

`drivers/uart_16550.c`'s task body carried the sharpest statement of it:

> *"chan.c's wait-for cycle guard covers `chan_call()` itself, but
> `printk_lock()` is a **different** blocking resource it cannot see."*

That was still true after Y3. §0.2 had flagged it — *"a lock without being
either type"* — and Y3 folded in ylocks and left it. **printk ownership is now
an edge in the same graph**, which buys two different things depending on
which half of the cycle closes last: a `chan_call()` that would close it is
*refused outright*, and a cycle closing inside `printk_lock()` is *named*
instead of hanging in silence.

Named rather than prevented, deliberately: `printk_unlock()` clears ownership
unconditionally, so "proceed without owning it" is not representable without
adding state to the most safety-critical path in the kernel, for a case that
should never happen. The same bargain Y2 struck for spinlocks.

That one change moved four comments from (b) to (a) — the three UART drivers'
task bodies and `drivers/i2c_rtc.c`'s.

##### Exception 1: interrupt context, mid-switch and mid-exit are still convention

§1.3 names three contexts that may not block. Only *"while holding a lock"* is
machine-checked. `drivers/include/drivers/uart.h` states the other two and now
says explicitly that they are unchecked.

The reason is structural rather than an omission: **this kernel has no
in-interrupt flag and cannot trivially grow one.** Preemption works by calling
`sched_yield()` *inside* the timer handler, so a naive depth counter would
mark every legitimate preemptive context switch as interrupt context and
refuse the scheduling it exists to perform. A correct flag has to distinguish
"in a handler" from "in a handler, having deliberately entered the scheduler",
and that is a design, not a counter.

`printk_critical()` already exists as the sanctioned escape for these
contexts, and E4's three failures were all fixed by using it — so the cost of
the gap today is that the rule is documented rather than enforced, not that it
is unmet.

##### Exception 2: two hand-rolled yielding locks are outside the graph

`drivers/cyw43_rp2350.c`'s `g_bus_busy` and `drivers/enc28j60_rp2350.c`'s
`g_busy` are `ylock_t` in everything but type: a flag, and a `sched_yield()`
loop waiting for it. They record no owner, so they contribute no edge, so a
cycle through one is invisible exactly as the channel-only graph made lock
cycles invisible.

This is not theoretical. `net/stack.c`'s comment records what it already cost:
a re-entrant call through `net_set_address()`'s gratuitous ARP took the bus
lock a frame below already held it, and *"the board deadlocked on `wifi probe`
with the console gone. It cost a physical BOOTSEL to recover."*

**The fix is to make them `ylock_t`, not to widen the graph.** A `ylock_t` is
re-entrant for its owner, which is precisely the failure above, and it is in
the graph for free. Not done here: it changes two drivers on a board that is
not currently attached, and phase 30 is the milestone that opens those files
anyway. Recorded in `plan/open_issues.md`.

##### Left alone, and why

A comment that explains a *choice* is not a rule waiting to be enforced.
`net/ntp.c`'s "blocking here would deadlock against the task that has to run"
justifies polling instead of blocking; `drivers/usb_cdc.c`'s re-entrancy note
justifies dropping instead of blocking. Both describe code that cannot be half
of a wait-for cycle because it never waits on a task. Rewriting them to
mention a checker would make them longer and less true.

##### The measure of success, honestly

§6 said the measure was that the twenty comments become redundant. They have
not become redundant, and on reflection they should not: each still says *what
the rule is*, which a diagnostic printed at 3 a.m. does not. What changed is
that none of them is now the only thing standing between the tree and the
bug — every rule in that set is either checked, or listed above as not.

Verified: 363/363 QEMU, `lockselftest` 12/12, ten presets warning-clean.

### Y5 — Kernel logging that cannot block

Reopened 2026-09-11, after Y0–Y4 had closed. Proposed by the user, in these
terms:

> *"All kernel-logs are guaranteed async and go into a pre-defined finite
> ring-buffer. This ring-buffer is the lowest level in the task hierarchy. In
> case of ring-buffer overflow, there is no blocking, but logging information
> is discarded (but the fact that overflow happened will be part of the
> kernel-log). Different consumers (e.g. UART driver) can empty the
> kernel-log-ringbuffer for output, and their operation will never block the
> actual print."*

Y4 folded printk ownership into the graph, which made a cycle through it
*named* instead of silent. Y5 is the next question, and the better one: why is
it an edge at all?

#### 5.1 What is already built, and where the blocking actually enters

`kernel/klog.c` is three-quarters of the proposal already:

* a 4096-byte ring that retains **every** byte `printk()` emits, whether or
  not any sink is attached;
* an absolute, monotonic byte coordinate space — `klog_total()`,
  `klog_oldest()`, `klog_read(abs_offset, ...)`;
* a sink registry, with the console sink registered at `kernel/main.c:144`;
* a drop policy that is already the right one: the ring **overwrites the
  oldest** byte, so a producer never has to decide whether to wait.

The ring is guarded by `g_klog_lock`, a leaf `spinlock_t` held across a single
byte store. It does not block and cannot.

**The blocking is entirely in the fan-out.** `klog_putc()` appends to the ring
and then, *on the producer's own stack*, calls every attached sink — and the
console sink ends in `uart_putc()`, which batches and `chan_call()`s the uart
task once the batch fills. `kernel/klog.c:33` already names this as the reason
the two halves of that function are protected by different things.

So Y5 is not "write a logging framework". It is **cut the producer's call to
the sinks, and give the consumer a read cursor** — which the coordinate space
above already supports, with no new state. A consumer whose cursor has fallen
below `klog_oldest()` knows exactly how many bytes it lost, which is the
user's "the fact that overflow happened will be part of the kernel-log",
computable rather than recorded.

#### 5.2 The prize: one fewer blocking resource, not one more mechanism

`printk_lock()` exists for one reason: `vprintk_to()` emits character by
character, and the fan-out underneath it can block, so a whole message needs a
lock held across the whole emission or two tasks interleave mid-word.

Format into a bounded stack buffer instead, and append the finished record to
the ring in one `g_klog_lock` critical section, and **message atomicity comes
from the single append**. `printk_lock()` then has nothing left to protect,
and with it go `g_printk_owner`, `g_printk_depth`, `g_printk_waiter`, the
polling fallback for task-less harts, and Y4's graph edge.

The wait-for graph goes from three blocking resources to two: channels and
ylocks. §4's test — *can the existing primitives express it cleanly?* — is
satisfied by **removing** a hand-rolled one, which is the strongest form of
the preference stated there.

It also retires machinery added one commit earlier, and that is worth saying
plainly rather than defending: phase 30's G2 added
`lock_noprintk_enter()/leave()` so that a `printk()` from inside a driver
serve callback is reported. Under Y5 that call is safe, because it appends to
a ring and returns. See §5.6 — the check is **narrowed**, not deleted, because
`cprintf()` from a serve callback stays a genuine cycle.

#### 5.3 The measured constraint: the ring is too small to be a transport

As history, 4 KB is adequate. As the transport, it is not, and this is
measured rather than estimated.

On the live RP2350, right now:

```
cat /proc/kmsg  →  4109 bytes, beginning mid-word: ", usbnet, usbcon"
                   earliest surviving timestamp 0.461s; the boot banner is gone
```

The ring has **already wrapped before the board finishes booting**. rv32's
minimal boot to a shell prompt is 3916 bytes against the same 4096. With a
consumer that cannot start until after `sched_init()`, the default outcome of
a naive async switch is that the boot log — the part you need most — is
precisely the part discarded.

Two fixes, both agreed, and both are needed:

* **`KLOG_RING_SIZE` 4096 → 8192.** One extra page. The RP2350 heap is 87
  pages of 4096 (348 KB), so the cost is 1 of 87, and the sizing is then
  ~2× the measured boot volume rather than 0.96× of it.
* **Terser boot output**, as its own pass. Reducing what boot prints is worth
  more than any ring size, because it is the one burst that is guaranteed to
  happen on every power-up and is guaranteed to have no consumer running for
  the first part of it.

#### 5.4 The rule that makes early boot safe without an exception

A producer may drain inline **only while no scheduler exists**.

This is not a compromise, it is a proof about when the hazard exists at all.
Before `sched_init()` there are no tasks, so `uart_putc()`'s
`driver_task_alive()` test is false and it writes the hardware directly — the
`chan_call()` path is not reachable, and there is nothing for a deadlock to
form between. The moment tasks can run, the producer stops draining, forever.

So the transition is a single predicate, not a mode flag anyone has to
maintain: *is there a registered consumer?* Before there is one, print goes
out inline exactly as it does today. After, the producer only ever appends.

And if the consumer **dies or is starved**, the system goes quiet rather than
blocking. That is the user's instruction applied literally — *prevent blocking
at all cost, even at the cost of losing older messages* — and the loss is
recoverable: `/proc/kmsg` still holds the ring, and `printk_critical()` still
reaches the wire synchronously. A silent console with an intact log is a
better failure than a deadlocked kernel.

#### 5.5 What stays synchronous, and why each one is a different answer

* **`printk_critical()` stays synchronous.** *"The ship is already sinking in
  that case anyway"* (user). It is the instrument this phase exists because of
  — F1 was a dead machine after one `printk()` — and its guarantee is that the
  line reached the wire *before* the halt. It already bypasses the sinks
  (`critical_putc_both()` → `klog_record()` + `uart_critical_putc()`); Y5
  leaves that path alone and adds a synchronous ring flush to the fatal path,
  so a panic empties what the consumer had not yet drained.
* **`cprintf()` stays synchronous.** Application output is a different stream
  with a different contract: dropping bytes from `ls` is a bug, not a
  trade-off, and a shell that outruns its console *should* be made to wait.
  What changes is only the primitive underneath it — from the bespoke printk
  ownership to a plain `ylock_t`, which is an existing primitive already in
  the graph and is exactly the shape it needs (a lock held across a block).
* **`printk_debug()` stays direct.** It writes the physical UART register and
  spins on THR-empty. That is bounded, not a lock, and its whole purpose is to
  bypass the machinery.

#### 5.6 The two consequences worth designing for, not discovering

**Interleaving.** Today `printk()` and `cprintf()` share `printk_lock()`, and
that shared lock is the only thing stopping a kernel log line landing in the
middle of a shell line. Split the streams and that protection is gone.

The resolution needs no new mechanism, and it follows from the one asymmetry
Y5 is built on: **the consumer may block; the producer may not.** The drain
task takes the same console `ylock_t` that `cprintf()` takes, and writes whole
records under it. Two writers, one lock, line-granular output — and the drain
task blocking on that lock is harmless precisely because nothing is waiting on
the drain task.

**The G2 check is narrowed, not removed.** *"Never `printk()` from a serve
callback"* stops being a rule. *"Never `cprintf()` from a serve callback"*
remains one: `cprintf()` → console ylock → `console_putc()` → `uart_putc()` →
`chan_call("uart")`, which from inside another driver's callback can close a
cycle. So `lock_noprintk_enter()` is retargeted from `printk_lock()` to the
console path and renamed for what it actually guards.

#### 5.7 Sequencing

Five steps, each independently testable, in this order. Phase 30 pauses at G2
— G3 migrates seven U-mode drivers and G4 the UART console family, all under a
constraint Y5 deletes, and migrating them twice is the avoidable mistake.

* **Y5a — Size and volume.** `KLOG_RING_SIZE` to 8192; the terser-boot pass.
  No behaviour change, so the suite is a pure regression check.
  *Done when:* the RP2350's `/proc/kmsg` still contains its own boot banner at
  the shell prompt — the measurement in §5.3, inverted.

  **Y5a done — 2026-09-11.** `KLOG_RING_SIZE` 8192, and rv32's boot cut from
  3916 to 2645 bytes (-32%): the banner from three lines to one (102 of its
  170 bytes were `=`), `[Arch]`/`[Mode]`/`[Priv]` from three lines to one,
  eleven verbose subsystem lines shortened, six "device absent" lines that the
  `[Dev] Registry:` line already summarises, the stack *address* off the
  `[Sched] Created task` line, and `/ram0`'s four-line "probe failed,
  formatted, mounted, mounted" narration down to the one line that is news
  (`fat32_init_quiet()`/`fat32_format_quiet()`, used only by
  `vfs_mount_ramdisk()`, where a blank volume is the expected state — on a
  real SD card the message stays).

  Headroom against the ring went from 1.05x to 3.1x. On the board:
  `/proc/kmsg` now begins at `[    0.000]` with its own banner, where it
  previously began mid-word at `[    0.461]`, and still holds the banner after
  2.5 minutes of uptime including the CYW43 join and the mqttd retry ladder.

  One test changed, and deliberately: the M0 check read the stack size out of
  the creation line as `stack \w+, 4 KB`. The size is load-bearing — it proves
  `task_create_sized()` honoured a non-default page count, and `sized1` has
  exited before `/proc/ps` could be asked — so the size stayed and only the
  address went. The regex now reads `'sized1' \(4 KB\)`, asserting the same
  claim. Found by the suite, which is what it is for: the first grep for
  affected test strings missed this one because it matches no literal the
  source contains.

  Stopped here rather than continuing, per §5.9: with tokenisation on the
  roadmap, literal length stops being a storage cost.

* **Y5b — Record append.** `vprintk_to()` formats into a bounded stack buffer;
  `klog_append(buf, len)` writes the whole record under `g_klog_lock`.
  `printk()` stops taking `printk_lock()`. **No consumer yet** — the producer
  still drains inline, so output is byte-identical and this step is verifiable
  on its own.

  **The record header carries the timestamp as 4 binary bytes**, rendered at
  drain rather than at emit. This step introduces the framing anyway, and the
  rendered `[    0.010] ` prefix is 12 bytes on every line — 456 of rv32's
  2645-byte boot, 17% of the ring, spent on a number that fits in four. No
  call site changes and `/proc/kmsg` looks identical. It is also the first
  piece of §5.9's tokenisation, taken early because it is free here and
  awkward later.

  *Done when:* QEMU 363/363 with no test changed, two harts printing
  concurrently produce no interleaved record (the X7 splice, asserted), and
  the boot burst is ~300 bytes smaller with output byte-identical.

  **Y5b done — 2026-09-11.** The ring stores records: a 6-byte header (4 bytes
  of milliseconds or `KLOG_NO_TS`, 2 of length) followed by the payload.
  `printk()` formats into a `KLOG_REC_MAX`-byte buffer on its own stack and
  appends one record under the existing leaf spinlock; the timestamp is
  rendered only on the way out. `klog_total()`/`klog_oldest()`/`klog_read()`
  keep their meaning on the **rendered** stream, which is why `/proc/kmsg`
  needed no change at all.

  **The storage estimate above was wrong and the measured figure is 138 bytes,
  not ~300.** 49 boot records read back as 2411 bytes and store as 2273: the
  binary timestamp saves 8 bytes on each of the 36 stamped records, and the
  6-byte header costs 6 on all 49. I wrote the estimate counting only the
  first half. The framing is still worth having — it is what Y5d and Y5f are
  built on — but it is a prerequisite, not the optimisation, and Y5f is where
  the 6x actually lives.

  **An unlooked-for improvement:** the log can no longer begin mid-word. Under
  eviction the readback now starts at `[    0.009] [PAlloc] ...`, a whole
  line, because `ring_make_room()` evicts whole records — where the
  byte-stream ring produced §5.3's `, usbnet, usbcon`. A half-overwritten
  record cannot be rendered at all, so framing forced the fix that the old
  ring merely tolerated the absence of.

  `vprintk_to()` gained a context parameter, which is what makes formatting
  into a caller-owned buffer safe: `printk()` is genuinely re-entered (an ISR
  printing inside an outer `printk()`), and a preempted task may resume on the
  other hart, so neither a static buffer nor a per-hart one is correct. This
  also retires `ksnprintf()`'s documented non-reentrancy.

  `printk_critical()` keeps writing the UART character by character as it
  formats and stores its ring copy at the end — buffering it first would mean
  a fault partway through emits nothing, and that path's whole guarantee is
  that what you read reached the wire before the halt.

  **`printk_lock()` is still taken**, and the plan text above was wrong to say
  otherwise. Its scope shrank — formatting is out from under it and the append
  is atomic on its own — but the fan-out is still synchronous and still has to
  be serialised against the other hart's and against `cprintf()`. Removing it
  belongs with Y5c (which gives the fan-out to a consumer) and Y5d (which
  moves `cprintf()` to a `ylock_t`), not here.

  Verified: ten presets clean, QEMU 363/363 with no test changed, console
  output byte-identical against the Y5a build, `/proc/kmsg` renders
  identically, eviction leaves 8145 of 8192 bytes in 225 whole records, and
  `lockselftest` is 15/15 with two new checks — a record stored and read back
  whole with its timestamp rebuilt, and an over-long message truncated with a
  visible `...` and counted.

* **Y5c — The consumer.** A drain task with a cursor in `klog_total()`'s
  coordinate space, taking the console ylock, writing whole records. The
  producer's inline drain becomes conditional on *no consumer registered*
  (§5.4). Overflow is reported as a synthesized record naming the byte count
  from `klog_oldest() - cursor`.
  *Done when:* `printk()` from inside a driver serve callback neither blocks
  nor faults, and its message appears; a forced burst larger than the ring
  produces a gap record naming a byte count, and no hang.

  **Y5c done — 2026-09-11.** `kernel/klogd.c`, and `printk()` takes no lock at
  all: it formats into a stack buffer, appends a record under the ring's leaf
  spinlock, and returns. The wake is `task_unblock()` -- a non-blocking signal,
  §4's own example -- skipped when a spinlock is held, because that call takes
  `g_sched_lock` and nesting it is what Y2 refuses; klogd's bounded sleep is
  what gets those records out instead.

  The ring lock became `spin_lock_irqsave_bottom()`, a declared bottom of the
  lock order. Without it every `printk()` under a lock reported `took
  &g_klog_lock`, drowning the checker in the one case Y5 exists to make safe.
  The asymmetry is self-enforcing: nesting the ring lock inside another is not
  reported, taking any other lock while holding it still is.

  **This milestone was mostly a hunt for things that had been leaning on
  `printk()` being synchronous**, and each was a real defect rather than a
  test to adjust:

  * `printk_debug()` writes the UART registers directly and still took
    `printk_lock()`; once `printk()` did not, boot output spliced mid-word.
    The inline pre-consumer fan-out keeps that lock.
  * `printk_critical()` writes the console live *and* stores to the ring, so a
    consumer replayed every fatal line twice. The record now carries an
    "already on console" bit in its length word.
  * Two drainers -- klogd, and the console's own sync point -- straddled each
    other and emitted records twice. `klog_take()` now renders and advances the
    cursor in one critical section.
  * klogd at `TASK_PRIO_INTERRUPT` ran on every `sched_yield()` and stopped
    `taskdemo`'s tasks interleaving. A logger that perturbs what it observes is
    worse than a late one; it runs at `TASK_PRIO_NORMAL`.
  * The drain point belongs at whole-write granularity, not per character.
    Per character it inserted the backlog between the `O` and the `K` of
    `UMODE_OK`; gating on a line boundary fixed the splice and left the worse
    half, which is that draining can *block* -- and a task that blocks
    mid-string may resume on the other hart, stranding half its output in the
    first hart's TX batch.
  * **The one that cost the most: `printk_unlock()` was the only thing that
    ever flushed the UART's per-hart TX batch.** Taking `printk()` off that
    lock left the console path with no flush at all, so a U-mode program
    writing character by character through `SYS_PUTCHAR` had its output sit in
    hart 1's batch indefinitely -- while on hart 0 the shell's next
    `cprintf()` happened to flush it. That asymmetry made it look like a
    second-hart bug for several rounds. `SYS_PUTCHAR`/`SYS_PUTNUM` and
    `console_puts()` now flush at their own boundary.
  * `SYS_PRINT` moved to the console stream, the argument `SYS_PUTNUM` already
    made. Measured rather than argued: on `printk()` the suite scores 353/363,
    on `console_puts()` 361/363.

  And one thing became *possible* rather than broken: `task_exit()` is back on
  plain `printk()`. It used `printk_critical()` because printk reached the
  console through `chan_call()` and a task could switch away mid-death (phase
  27 E4). That reason is gone, and dropping the workaround also stopped the
  message splicing itself into a U-mode program's output.

  Verified: ten presets clean, QEMU **363/363**, `lockselftest` 16/16 with two
  new checks -- `printk()` from a serve callback is silent where `cprintf()`
  from one still reports, and a 400-record burst issued with a spinlock held
  drops the oldest and says so (`26533 bytes of log were dropped`).

* **Y5d — Delete the primitive.** `cprintf()` and `printk_debug()` move to the
  console `ylock_t`; `printk_lock()`/`printk_unlock()` and the Y4 graph edge
  are removed; G2's check is retargeted per §5.6.
  *Done when:* `kernel/printk.c` has no ownership state, the wait-for graph
  has two contributors instead of three, and `lockselftest` asserts a
  `printk()` from a serve callback is *not* a fault while a `cprintf()` from
  one still is.

  **Y5d done — 2026-09-11.** All three, and the milestone is mostly deletion:
  `printk_lock()`/`printk_unlock()` are gone, with `g_printk_owner`,
  `g_printk_depth`, `g_printk_waiter`, `g_printk_gate`, the polling fallback
  for a task-less hart and Y4's hand-maintained graph edge. The console's lock
  is `console_lock()` -- a plain `ylock_t` -- and `waitfor_enter()` is now
  called from exactly two places in the tree: `kernel/chan.c` and
  `ylock_acquire()`. Channels and ylocks. Two.

  Nothing clever made that possible; Y5b and Y5c did. The lock existed to hold
  a whole message together across a char-at-a-time emission that could block.
  A message is one record appended under a leaf spinlock now, and the part
  that blocks belongs to klogd. What was left to serialise is *the wire* --
  `cprintf()`, `printk_debug()` and the drain all reach the same UART -- and
  that is `console.c`'s business, not `printk.c`'s.

  `console_flush()` came with it, the other half of Y5c's most expensive bug:
  `printk_unlock()` was the only thing in the kernel that flushed the UART's
  per-hart TX batch, so the console stream had been borrowing printk's flush
  without anyone noticing until printk stopped taking that path.

  **`printk_critical()`'s rationale was rewritten rather than kept.** It listed
  three reasons -- scheduler teardown, interrupt context, anything holding
  `g_sched_lock` -- and `printk()` is safe in all three now. One reason
  survives and it is the whole reason: *printk() is delivered eventually, by
  klogd; this is delivered now.* Anything whose value is that it arrived
  before the machine stopped belongs there.

  One test was made robust rather than adjusted: Y3's "a ylock wait is an
  edge" check sampled a window -- the edge exists only while the waiter is
  inside its yield -- with 200 bare `sched_yield()`s. That was enough until
  klogd joined the rotation, then failed about one run in three on two harts.
  It now polls with `task_sleep_ms()`, because a yield hands off within one
  hart's ready set and what is needed is for the waiter on the *other* hart to
  be scheduled. The property under test did not change.

  **Three things the suites caught, all mine, all worth recording:**

  * **Flushing per write undid M4.** `console_flush()` went into
    `console_puts()`, which looked like "the end of a string" and is in fact
    called once per *literal run* inside `cprintf()`'s format engine. One
    `help` became 260 `chan_call()`s -- the per-character IPC batching exists
    to prevent. The flush belongs at real boundaries: a whole `cprintf()`, a
    newline from `SYS_PUTCHAR`, and `SYS_UEXIT` (before `task_exit()`, where
    blocking is still allowed) for a program like `user_probe` that prints
    "UMODE_OK" and exits without a newline. Back to 140 calls for a `help`,
    against a historical 142.
  * **Drain outside the lock is a race.** `cprintf()` drained *before*
    acquiring, so another writer could interpose between the drain and the
    write -- and a log line from seconds earlier landed inside a command's
    output. It showed as a *different single test* failing on each run, which
    is what a race looks like when what it corrupts is whatever happened to be
    printing. Draining inside the lock makes the pair atomic.
  * **Locking `console_puts()` too broke the two-hart boot**, deterministically
    and in both runs. It is reached from inside `cprintf()`'s engine, which
    already holds the lock, during bring-up, on a hart that may have no task.
    It drains and does not lock.

  Verified: ten presets clean, QEMU **363/363 twice consecutively**,
  `lockselftest` 16/16 on rv32 and five consecutive clean runs on rv64-smp,
  ESP32-P4 hardware 12/12, and RP2350 hardware **25/25** -- which is better
  than the documented 22/25 baseline, since B3/B6/C2 passed too on this run.

  (One suite run hung and was not a regression: I had left manual QEMU
  instances racing it, which `plan/`'s own standing note warns about. Clean
  re-run: 363/363 in 185 s.)

* **Y5e — The documentation that was load-bearing.** `drivers/README.md`'s
  "things that will bite you" printk entry, `kernel/lock.h`'s invariant
  section, `driver_task.h`'s invariant 1, and the four driver task-body
  comments Y4 rewrote. All of them currently state a rule Y5 changes.
  *Done when:* no comment in the tree tells a reader that `printk()` from a
  driver task can deadlock, because it no longer can.

* **Y5f — Tokenised records.** §5.9, gated on the `format(printf, ...)`
  attributes. Last, and separable: everything above is finished and useful
  without it.
  *Done when:* the boot burst is ~6x smaller with `cat /proc/kmsg` output
  unchanged character for character, and a format/argument mismatch is a
  compile error rather than a corrupt record.

#### 5.8 Done, for Y5 as a whole

The kernel log is deadlock-free by construction rather than by checking:
`printk()` from any context — a driver task mid-serve, an ISR, under any
lock — appends to a ring and returns, and no path from it reaches a blocking
primitive. Output loss is possible, bounded by the ring, and reported in the
log itself. `printk_critical()` still reaches the wire before a halt, and
application output is still never dropped. Ten presets clean, QEMU
363/363, and both hardware suites at their documented baselines.

#### 5.9 Tokenised records — Y5f, and why it is last

Proposed by the user, 2026-09-11:

> *"We could make the kernel logging use symbols and parameters which get
> expanded into text within the logging code? A bit less user-friendly, but
> would dramatically reduce ring-buffer storage."*

**The measurement says yes, and by more than the question assumed.** rv32's
boot log after Y5a's trim, classified:

| | today | tokenised |
|---|---|---|
| timestamps rendered as `[    0.010] ` | 456 B | 152 B |
| literal message text | 1696 B | 76 B |
| substituted arguments (paths, names, numbers) | 217 B | 217 B |
| **boot total** | **2645 B** | **~445 B — 5.9x** |
| boots held by the 8 KB ring | 3 | ~18 |

**89% of the log is literal text**, which is exactly the part a format-string
id removes. Modelled as `2-byte id + 4-byte timestamp + inline %s args`.

**It needs no call-site churn.** A format string's `.rodata` address is
already a unique, stable, immortal id, so a macro wrapper captures `&fmt` plus
the arguments and every existing `printk("...", x)` compiles unchanged. Same
technique as Zephyr's dictionary logging.

**Expansion happens in the kernel at drain, not on a host.** The strings are
in the image regardless, so `cat /proc/kmsg` stays readable and the
out-of-band 9P route to it survives — which matters here, because reading
`/proc/kmsg` over the *other* ACM port is this project's standing answer to a
dead console. A host-side dictionary would additionally free the strings from
flash, and costs both of those; not worth it.

##### The prerequisite, which is worth doing on its own

`kernel/include/kernel/printk.h:7` declares `int printk(const char *fmt, ...)`
with **no `__attribute__((format(printf, 1, 2)))`**, so `-Wall -Wextra` checks
no call site in the tree. And `kernel/printk.c:320` is literally:

```c
if (*p == 'l') p++; // Handle %ld / %lx / %lu
```

— the length modifier is parsed and discarded, so `%ld` on an `int64_t` reads
32 bits and misaligns every argument after it. That is the standing "never
`%ld` an int64" rule, and today it costs one garbled line. Under argument
capture the same mistake corrupts the whole record's decode.

So Y5f is gated on adding the format attributes to `printk`, `cprintf`,
`printk_critical`, `printk_debug` and `ksnprintf`, and fixing what they flag.
All nine specifiers this engine supports are standard (`c s d i u x X p %`),
so the attribute fits without false positives. **This is worth its own commit
whenever, independent of Y5** — it converts a class of bug this project keeps
a note about into a compile error.

##### Why it is last

Y5b rewrites the exact emit path Y5f would rewrite again, and landing a 6x
storage change together with "logging no longer blocks" leaves two hypotheses
when something breaks. Y5f also changes what `/proc/kmsg` *is* internally,
which is easier to reason about once the consumer that reads it exists.

**Consequence, accepted at the time (user, 2026-09-11): stop hunting boot
string bytes.** Y5a's trim stands — it earns its keep on the human-readable
console, and the console is not going away. But with tokenisation on the
roadmap, literal length stops being a storage cost, so further terseness
passes would buy nothing. Y5a ends where it is.
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
* **For Y5 specifically, two assertions QEMU can make that it could not
  before**: that a `printk()` from inside a driver serve callback returns and
  its message appears, and that a burst larger than the ring produces a gap
  record naming a byte count rather than a hang. Both are deterministic once
  logging cannot block, which is the point — the hazard stops being one that
  only reproduces on hardware.

## 4. Explicitly not in this phase

* **No new locking primitives — unless one is genuinely the cleanest answer**
  (user, 2026-09-11). The original wording ruled them out flatly, and that is
  the wrong rule. The preference is real and it is strong — a solution that
  composes the existing `spinlock_t` and `ylock_t` is preferred, and simpler
  is always better — but it is a preference, not a prohibition. Where a
  scenario has no clean resolution with what exists, inventing the right
  mechanism beats contorting the code around the wrong one.

  The example that makes the distinction concrete: **a non-blocking signal.**
  Every edge this phase models is a *blocking* one, which is why cycles are
  deadlocks and why §1.1 can treat "acyclic" as the whole invariant. A
  send-and-continue notification is not an edge in that graph at all, so it
  lets two parties notify each other — circular by construction — without any
  possibility of the deadlock a synchronous `chan_call()` in both directions
  would guarantee. That is not a way of dodging the ordering question; it is a
  different relationship between the two tasks, and for some pairs it is the
  honest one.

  The test to apply, in this order: can the existing primitives express it
  cleanly? If not, can the *structure* be changed so they can? Only then, a
  new mechanism — and it arrives with its own place in §1.1's graph, or an
  argument for why it is not in the graph at all.
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
* **Y5 trades a deadlock for a silence.** The failure mode moves from "the
  board stops with no clue" to "the board runs but says nothing", and the
  second is quieter in every sense. Three things keep it findable:
  `printk_critical()` still reaches the wire, `/proc/kmsg` still holds the
  ring whatever the consumer did, and the drain task is visible in `ps` like
  any other. A board that has gone quiet is one `cat /proc/kmsg` from telling
  you why — over 9P on the other ACM port if the console itself is the thing
  that died.
* **Y5's stack buffer is a new fixed limit.** Formatting a record before
  appending it means a maximum record length, where today a `printk()` of any
  length simply streams. Pick it from the longest format string in the tree,
  measured rather than guessed, and truncate with a visible marker rather than
  silently.

## 6. Budget

Small, and front-loaded onto thinking rather than typing. Y0 is reading and a
table. Y1 is a bug fix. Y2 is the bulk — a counter, two assertions, and the
argument about where to put them. Y3 and Y4 are consolidation.

Y5 is larger than any of them and still not large, because three-quarters of
it is already in `kernel/klog.c` (§5.1). Y5a is a constant and a pruning pass;
Y5b and Y5d are each a contained change to one file plus its callers; Y5c is
the only new code, and it is one task of the shape phase 30's `driver_task.c`
now provides. Y5e is writing. The hardware suites gate it, not because the
mechanism is board-specific but because the failure it prevents only ever
showed up on real silicon.

The measure of success is that the twenty comments become redundant, and that
the next instance of §0.4's failure class is a named refusal at the moment of
the mistake rather than a crash three milestones later.
