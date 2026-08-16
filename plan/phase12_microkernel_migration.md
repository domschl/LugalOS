# Phase 12 — Fine-Grained Process Architecture: A QNX-Shaped Migration

**Status:** M0-M3 complete (2026-08-15). Written to be detailed and executed
one milestone at a time, not implemented from this document directly.

**Origin.** `plan/redesign_eval.md` asked whether LugalOS's core architecture
needed a rewrite or restart: monolithic structure, inconsistent polling,
cooperative multitasking, no granular memory primitive. Evaluated against the
current tree rather than against the framing of that question: the diagnosis
was partly stale (Track B, `plan/phase5_distributed_design.md` §5, already
built preemption on all three targets and a copy-always IPC/9P namespace that
*is* the microkernel's service layer) and partly exactly right (every
blocking wait in the tree is a hand-rolled `while (!ready) sched_yield();`
spin; `task_block()`/`task_unblock()` have zero call sites outside `sched.c`
itself; drivers are libraries called synchronously by whichever task needs
them, not tasks of their own; every internal server runs `domain=NULL`,
unrestricted, in one shared fault domain). This phase is the resolution of
that discussion: not a rewrite, a migration, using primitives that mostly
already exist.

**Reference architecture.** QNX Neutrino, specifically for how closely its
mechanisms already map onto this tree:

| QNX | LugalOS |
|---|---|
| `MsgSend`/`MsgReceive`/`MsgReply` | `chan_call()` — copy-always, Rule 1 |
| Pathname resource managers | VFS mount table + 9P-over-`chan_t`, Rule 2 |
| `InterruptAttach()` + pulse | **Gap** — `task_block`/`task_unblock` exist, unused |
| Priority-preemptive scheduler | Cooperative round-robin + an idle preemption timer |
| Driver crash ≠ kernel crash | Every driver runs at kernel privilege today |

**The north star.** `ps` on the RP2350 build should show a process list
structurally equivalent to the RV64/MMU build's — same granularity of
decomposition, same reporting shape — even though RP2350 cannot back every
entry with the same enforcement guarantee. Getting there is priced, not just
proposed: task creation's dominant cost is its kernel stack (today a flat
`TASK_STACK_PAGES=2` = 8 KB regardless of what the task does), `MAX_TASKS=8`
and `CHAN_MAX_ENDPOINTS=4` are `#define`s sized for the tree as it stood
before this phase, not hardware ceilings, and a realistic full decomposition
of even the busiest board persona (chess: console, `usb_cdc`, SD/SPI block,
ST7735, TM1638, 9P, VFS, shell) already reaches 8 tasks on its own. The
pricing work done in conversation put a flat-8 KB ceiling at ~128 KB for a
16-task image against a 312 KB-class heap — affordable but real; right-sizing
stacks per task class brings that under 60 KB.

**What stays untouched throughout.** 9P's wire format and dispatch, the
mount table, Lisp, `cc`/`ed`, the ELF loader and its `mem_domain` use for
*user* programs. This migration is entirely about the driver/service layer
underneath those, which is exactly the layer Track B's Rule 2 already
isolated as "the microkernel part."

---

## Design rules for this phase

Extends `plan/phase5_distributed_design.md` §5.1's Rules 0–3, same spirit:
constraints imposed from the first commit because they're cheap up front and
expensive to retrofit.

**Rule 4 — Decompose for structure even where enforcement can't follow.**
On a target where PMP can't afford to isolate everything, granularity of
*process structure* still scales the way it would on an MMU target — a
trivial peripheral driver still gets its own task, its own name in `ps`, its
own crash boundary in the scheduler's bookkeeping — even when there's no PMP
budget left to actually enforce that boundary against every other task on
the system. The alternative (falling back to fewer, coarser tasks because
full isolation isn't affordable) throws away the diagnosability and the
migration-to-bigger-hardware story for no enforcement gained on the paths
that were never going to be isolated anyway.

**Rule 5 — Isolation is minimal by default, not absent.** Rule 4 must not
mean `domain=NULL`-by-default for cost reasons. Every task gets at least a
domain covering its own stack, spending one of PMP's five dynamic regions
(a per-context-switch register cost, not a RAM cost — see M1/M5 below), so a
wild write degrades to an immediate fault and a `DEAD`/`exit_clean=false`
task the fault handler can name, instead of silent corruption discovered
three subsystems later. Full multi-region isolation (a device's MMIO window,
a private buffer) is spent where it earns its keep: drivers parsing
external/untrusted input (9P framing, USB, FAT32/SD), not GPIO toggles.

**Rule 6 — Power-of-two or nothing, for anything that might become a PMP
region.** A generic first-fit allocator hands back arbitrary offsets and
sizes that can't be expressed as a NAPOT region at all, which would silently
defeat Rule 5. Anything that might end up as a task's stack goes through an
allocator that only ever returns power-of-two, self-aligned blocks — see M1.

---

## Milestone ladder

Ordered by dependency, not by priority — each milestone below is usable on
its own and should get its own detailed plan (and its own completion notes,
per the project's standing habit) when picked up.

### M0 — Groundwork constants and per-task stack sizing *(done, 2026-08-15)*

Raise `MAX_TASKS` (8 → ~24) and `CHAN_MAX_ENDPOINTS` (4 → ~16); both are
static arrays sized by the constant, so the cost is under 2 KB combined, not
a structural change. Give `task_create()` a page-count parameter instead of
hardcoding `TASK_STACK_PAGES` for every caller — `task_t.stack_pages`
already exists per-task, only the call site is uniform today. Existing
callers keep the current default; nothing changes behaviorally. This alone
makes the M4 pricing story real before any new subsystem exists.

**Verify:** existing suite unchanged; a task explicitly created with 1 page
runs correctly and is reaped for the right page count.

#### M0 completion notes (2026-08-15)

Implemented as planned, plus one API decision made at build time: rather than
changing `task_create()`'s signature (which would have touched all seven
existing call sites for no behavioral gain), added `task_create_sized(name,
entry, arg, stack_pages)` as the real primitive and made `task_create()` a
thin wrapper calling it with `TASK_STACK_PAGES` — the same "as above, plus
X" pattern this file already uses for `sched_task_info`/`sched_task_info_ex`.
Every pre-M0 caller (the shell's demos, `p9srv`, the ELF loader's
`user_program_body`, Lisp's `pump` task) is untouched.

`MAX_TASKS` 8 → 24, `CHAN_MAX_ENDPOINTS` 4 → 16 — both `#define`s, no other
code depended on the specific value (checked: `/proc/ps`'s
`sched_task_info_ex()` loop already terminates on the first genuinely-unused
slot, not on `MAX_TASKS`, so it needed no change to report more tasks).

Verified: all three targets (rv32/rv64/rp2350) build clean. Added
`sizedtaskdemo` (`kernel/shell.c`, alongside `taskdemo`) exercising
`task_create_sized()` with a 1-page stack, wired into `tests/runner.py` as
two assertions — the requested size actually took effect (`stack ..., 4 KB`,
not the default 8), and `palloc_free()` released exactly that many pages,
not `TASK_STACK_PAGES` (the more dangerous failure mode: a size mismatch
here would leak a page per run or free one page too many into a neighboring
allocation). Confirmed live on QEMU RV32 before wiring the permanent test:
`[Sched] Created task #2 'sized1' (stack 802c0000, 4 KB)` ...
`free before=4078 after=4078`. Full suite 191 → 195/195 (rv32 + rv64, the
two new assertions on each). Not flashed to RP2350 hardware: this milestone
touches no PMP/hardware-register code path (Rule 0 — `sched.c`, `chan.h`,
`shell.c` are architecture-independent), unlike M2/M5 below, where hardware
verification is not optional.

### M1 — Buddy allocator *(done, 2026-08-15)*

Closes the "no granular memory allocation primitive" gap from
`plan/raw_ideas.md`'s "True allocator" line, and is the prerequisite for
sub-page stacks and Rule 6. Deliberately **not** a general first-fit/best-fit
heap: buddy allocators only ever hand out power-of-two, naturally-aligned
blocks, which is what keeps every block PMP-representable and gives every
block a known, same-size, isolatable neighbor for a Rule-5 guard region.
Floor is the hardware's NAPOT granule (32 B on RP2350, 8 B on QEMU — already
respected as a floor by `mem_domain_add()`'s size check). Sits under or
replaces `palloc`'s page arena; `palloc_pages()`/`palloc_pages_aligned()`
callers are preserved. Open design question to resolve at detail time: how
existing non-power-of-two multi-page requests (if any survive M0) map onto
buddy size classes.

**Verify:** stress alloc/free churn (create/destroy many small tasks in a
loop) shows bounded fragmentation via coalescing, unlike the current
bitmap's `largest_free_run` degradation; existing palloc-consumer tests
(stacks, page tables, ELF loads) unchanged.

#### M1 completion notes (2026-08-15)

Landed as an **additive** module (`kernel/balloc.c`/`.h`), not a
`palloc`-replacement — resolves the "sits under or replaces" question from
the milestone text in favor of the lower-risk option. `palloc_pages()`/
`palloc_pages_aligned()` and every one of their callers are byte-for-byte
untouched; `balloc_init()` (called from `kernel/main.c`, right after
`palloc_init()`) simply reserves a self-aligned 64 KB slice of the page heap
via the existing `palloc_pages_aligned(BALLOC_ARENA_PAGES, BALLOC_ARENA_PAGES)`
— self-aligned because that is what makes every block the buddy allocator
later hands out of *that* arena land on an address aligned to its own size in
*absolute* terms, not just relative to an arbitrary base (the property Rule 6
actually needs). Deliberately modest for M1: 64 KB, since nothing in the tree
calls `balloc_alloc()` yet — M4/M5 are what will make the arena size a real
sizing question.

Mechanism is the classic "buddy2" binary-tree-of-largest-free-block scheme
(one `uint16_t` array, no intrusive freelist pointers written into free
memory) rather than a freelist-based buddy — chosen so the allocator's
entire state lives in one side array, nothing about it depends on what a
caller does with memory before first use, and `balloc_free()` needs no size
argument (the tree walk that locates a block's owning node already recovers
it). Floor is 32 bytes, matching `mem_domain_add()`'s own floor rather than
QEMU's looser 8-byte granule — same "stricter of both targets" call
`mem_domain.c` already makes, kept consistent rather than re-litigated.

Verified: all three targets build clean. Added `ballocdemo`
(`kernel/shell.c`), wired into `tests/runner.py` as two assertions distinct
from each other on purpose — rounding *and* self-alignment together (a
33/65/1025-byte request comes back as a self-aligned 64/128/2048-byte
block), and full coalescing after a 50-round mixed-size churn across 8
concurrently-live slots (`largest_free_bytes` back to exactly `arena_bytes`,
not merely nonzero). Confirmed live on QEMU RV32 before wiring the
permanent test: `Rounding/alignment: OK`, `largest=65536 arena=65536` after
the churn. Full suite 195 → 199/199 (rv32 + rv64). Not flashed to RP2350
hardware, same reasoning as M0: no PMP/register code touched yet — this
milestone builds the allocator, it does not yet hand any of its blocks to
`mem_domain_add()` or a PMP region. That wiring is M5's job, and is exactly
where hardware verification stops being optional again.

### M2 — ISR-driven wake *(done, 2026-08-15 — scope reduced by a real hardware finding)*

The concrete gap identified in conversation: `task_block()`/`task_unblock()`
exist since B2 and are called nowhere outside `sched.c`. Give each polling
driver's interrupt handler a stored waiter pid and a `task_unblock()` call;
replace its `while (!ready) sched_yield();` with `task_block()`. Start with
`uart_putc()`'s known, currently-unfixed unbounded spin
(`plan/phase5_distributed_design.md`, "Known issue," 2026-08-09) as the
forcing example — it's the one driver wait with a documented real symptom
already attached to it.

**Verify:** the `uart_putc()` hang symptom (deterministic before this
milestone) is gone; console output under load no longer risks the wedge;
QEMU suite unchanged.

#### M2 completion notes (2026-08-15)

**Landed, verified on both QEMU targets and real RP2350 hardware:**

- `kernel/devirq.c`/`.h` — the generic IRQ-number-to-handler dispatch table
  M2's own text called for, arch-independent by construction.
- `arch/riscv/common/trap.c` — real controller code for both external-
  interrupt mechanisms this tree now has: QEMU's standard PLIC (claim/
  complete, verified against `qemu/include/hw/riscv/virt.h`) and RP2350's
  Hazard3 Xh3irq array (`meiea`/`meinext`, verified against the Pico SDK's
  own `hardware/regs/rvcsr.h` and `hardware_irq/irq.c` — the local
  `~/gith/pico` checkout, not guessed or scraped from a web search, once the
  user pointed at it). `arch/trap.h` gained `arch_irq_enable(irq_num)` as
  the one arch-level primitive every future driver attaches through.
- **A real, target-specific bug found and fixed before it shipped**: the
  first cut gated the Xh3irq-vs-PLIC choice on `CONFIG_MODE_M`, not
  `CONFIG_BOARD_RP2350`. QEMU RV32 is *also* `CONFIG_MODE_M` (a plain
  M-mode target, no S-mode transition) but has none of Hazard3's custom
  CSRs — reading `meinext` there is an illegal instruction. Caught
  immediately: RV32 hung mid-suite while RV64 (`CONFIG_MODE_S`, a different
  branch) sailed through, and the asymmetry was the tell. Fixed by
  discriminating on the board, not the privilege mode — see trap.c's own
  comment on why that distinction is load-bearing.
- `uart_getc()` (`drivers/uart_16550.c`, QEMU only) converted to real
  ISR-driven block/wake. Safe because it replaces a wait that *already*
  yielded on every poll (B2's original `sched_yield()` spin) — no new
  reentrancy window opened. Verified by the full suite (every keystroke in
  every test goes through this path) and directly: `[Sched] Created task
  ... stack ..., 4 KB` style boot traces stayed clean under load.

**Reverted, after a second real hardware finding — the actual point of this
note:** TX-side blocking (`uart_putc()` on both `uart_16550.c` and
`uart_rp2350.c`) was built, and QEMU's full suite passed 199/199 with it in
place. Real RP2350 hardware testing (`tests/hw/test_rp2350.py`) then showed
C2 and C4 failing in a way that read like a PMP/isolation regression —
until the boot log itself gave it away: `[Sche5d]` where `[Sched]` should
have been, a stray character mid-word. That is the signature of two tasks'
console output interleaving character-by-character, not an isolation
failure. Confirmed by bisection: `git stash`, rebuild `HEAD` clean, flash,
re-run the identical hardware suite — 14/15 (only a pre-existing, unrelated
`K3` pin-config assertion fails) on unmodified `HEAD`, 10/15 with TX
blocking added, same board, same test, nothing else different.

Root cause: `printk()` is not atomic — it calls `uart_putc()` once per
character with no lock held across the line — and B0's own design notes
already said why the old unbounded, *non-yielding* spin was load-bearing
rather than an oversight: *"yielding mid-printk() would let two tasks
interleave output character by character... that trade only becomes
acceptable once B4 gives the console a single owning server."* B4's console
channel serves U-mode's `SYS_CHAN_CALL` path, not printk's direct one, so
that precondition was never actually met. `task_block()` is exactly the
yield this specific wait never had. **QEMU's clean pass was not
reassurance — it was the same blind spot Track B has hit before, just in a
new shape**: not "QEMU hides a Hazard3 divergence" this time, but "QEMU's
virtual UART never backpressures, so the risky branch was never actually
exercised there." Only a real board's real baud rate found it.

Both `uart_putc()`s are back to their original plain, non-yielding spins,
documented with a forward pointer to what fixing them for real needs: a
console output lock that survives a block, not merely an interrupt to wait
on — a real primitive, out of scope for this milestone, not a corner cut.
RP2350's Xh3irq controller code (`trap.c`, `arch_irq_enable()`) stays in the
tree as verified-on-hardware infrastructure with no current driver
attached to it; `uart_rp2350.c`'s RX side was never converted in the first
place, for an unrelated, independently-sufficient reason recorded in the
milestone's own commit: it polls three genuinely different sources
(physical UART, USB CDC, the heartbeat LED) and this tree has no
block-with-timeout or wait-on-any-of-N-sources primitive yet.

**M2's original motivating bug (`uart_putc()`'s unbounded spin) is
therefore still open.** Its actual fix needs a console lock, not an
interrupt — recorded as a real, named prerequisite for whichever future
milestone gives the console a genuine single owning server, rather than
left implied.

### M2.5 — `printk_lock()`, and M2's full original scope *(done, 2026-08-15)*

The named prerequisite above, closed the same day rather than left as debt:
a real, block-capable lock around `printk()`/`printk_debug()`/`cprintf()`,
so a whole message is atomic across a `task_block()`, not just across a
spin — which is what finally makes TX-side ISR-driven blocking safe to turn
back on.

`kernel/printk.c` gained `printk_lock()`/`printk_unlock()`: single owner,
single waiter slot (the same shape as `kernel/chan.c`'s endpoints and M2's
own UART waiters — this tree's concurrency level doesn't justify a real
wait queue), with one deliberate refinement B6's original mask-based
version didn't need: **reentrancy**. `kernel/devirq.c`'s "unhandled
interrupt" fallback calls `printk()` from inside `trap_handler()`, which
runs on the interrupted task's own stack, as that task — so an unhandled
IRQ firing while that same task already holds the lock (mid an outer
`printk()` call of its own) must not try to block on itself. Tracked via an
owner pid and a depth counter; the same task re-entering proceeds for free
instead of deadlocking. Blocking *on a different task's* held lock from
inside an ISR is unremarkable by contrast — mechanically identical to
ordinary timer preemption, which has done exactly this (suspend the
interrupted flow, resume it later at the same point) since B6.

This replaces B6's `irq_save()`/`irq_restore()` pair around each entry
point, not just adds to it — and revisiting *why* that mask was there in
the first place is what actually explains M2's bug in full, more precisely
than "task_block() yields when the old code didn't": `irq_save()` masks a
single, global, per-hart CPU bit, not a per-task saved context. Holding it
*across a context switch* — which is exactly what M2's first `uart_putc()`
did — leaves interrupts off for whatever *other* task now runs, not just
the one that asked, with no record of who still owes the matching
`irq_restore()`. B6's own comment already named the constraint this broke:
*"a lock that could block would be a deadlock rather than a wait."* M2
first tried "block without a lock" (unsafe, per the finding); M2.5 is the
other side of the same sentence: a lock that *can* be safely held across a
block.

With that in place, both `uart_putc()`s (`drivers/uart_16550.c`,
`drivers/uart_rp2350.c`) got their TX-side `task_block()`/`task_unblock()`
conversion back, unchanged in shape from M2's original attempt — only the
precondition changed. RX-side reasoning is untouched: QEMU's was already
safe pre-M2.5 (RX always yielded), and RP2350's stays polling-only for the
independent, still-unresolved multi-source-wait reason.

**Verify:** QEMU suite 199/199 (unchanged count — this milestone fixes a
hardware-only symptom, not a QEMU-visible one). Real RP2350 hardware,
`tests/hw/test_rp2350.py`: back to the clean-`HEAD` baseline, 14/15 (only
the pre-existing, unrelated `K3` pin-config assertion fails) — C2 and C4,
the two tests M2's bug broke, both pass, with TX blocking active this time.
No `[Sche5d]`-style corruption anywhere in the captured boot log. **M2's
original scope is therefore fully delivered**: `uart_putc()`'s unbounded
spin (the milestone's own motivating bug) is fixed for real, not deferred.

### M3 — Priority tiers *(done, 2026-08-15)*

Flat round-robin defeats M2: a driver task an ISR just woke should run
before a long Lisp eval or chess search, not wait its turn in the ring.
A handful of tiers (interrupt-woken > foreground shell > background server >
idle) covers everything in the tree today — this is a change to
`next_runnable()`'s selection policy, not to `ctx_switch()` or the task
table shape.

**Verify:** a synthetic test with a busy low-priority task and an
interrupt-driven high-priority one shows bounded wake-to-run latency
regardless of what the low-priority task is doing.

#### M3 completion notes (2026-08-15)

`kernel/include/kernel/sched.h` gained four static tiers
(`TASK_PRIO_IDLE` < `TASK_PRIO_BACKGROUND` < `TASK_PRIO_NORMAL` <
`TASK_PRIO_INTERRUPT`), a `priority` field on `task_t` defaulting to
`TASK_PRIO_NORMAL`, and `task_set_priority(pid, priority)` — attached after
creation, mirroring `task_set_domain()`'s own "usually only knows what a
task *is* once it exists" shape, deliberately static rather than boosted
transiently on wake (a driver task is always latency-sensitive; that is a
property of its role, not its history). `next_runnable()` became one linear
scan that only replaces its running-best candidate on a *strictly* higher
priority — which is what makes it collapse to exactly the pre-M3
round-robin scan when every task shares one tier, true of everything in the
tree before M4 gives a driver task `TASK_PRIO_INTERRUPT`. Confirmed by the
full suite: `taskdemo`'s ordering assertion and `preempttest` both passed
unchanged.

**A real, non-obvious bug found and fixed while building the verify test,
unrelated to the scheduler logic itself.** The first version of `priotest`
(`kernel/shell.c`) used a `volatile bool` flag, set by an unblocked
high-priority task and polled by a non-yielding busy-wait in the caller —
structurally the same pattern `preempttest` already used successfully. It
failed every time: the flag-setting task visibly ran and exited (per the
boot log), yet the polling loop spun to its full bound as though the flag
had never changed. Isolated by elimination — hogs, priority, `task_block()`/
`task_unblock()`, and the debug `printk()`s were each removed in turn
without changing the outcome, until only the flag's *type* was left to
change. Switching `g_urgent_ran` from `volatile bool` to `volatile
uint32_t`, with nothing else touched, fixed it outright and immediately.
`preempttest`'s own `g_preempt_flag` was already `uint32_t`, which is
presumably why B6 never tripped over this. Not chased to a root cause in
the toolchain — recorded as a real, load-bearing constraint instead of
papered over: **a `volatile` flag polled across a preemption boundary in
this tree should be word-sized, not `bool`.**

Verified: all three targets build clean; QEMU suite 201/201 (two new
targets × the new `priotest` assertion). `priotest` itself creates four
never-yielding `TASK_PRIO_NORMAL` hogs sized to still be genuinely READY
(not already exited) at the moment a `task_block()`-parked
`TASK_PRIO_INTERRUPT` task is unblocked — otherwise the test could pass
without ever exercising the tie-break it claims to, which an earlier,
faster-finishing hog size actually did. Confirmed on real RP2350 hardware
too, both via the existing hardware suite (15/15, unaffected) and a direct
`priotest` run: `ran=1 ticks_to_run=1 -- BOUNDED (priority won)`, urgent
winning the very next reschedule while the hogs were still demonstrably
alive.

### M4 — Drivers become tasks

The structural core of the migration, and where Rule 4 is actually spent.
Convert drivers one at a time from "library called synchronously by whoever
needs them" to "long-lived task reachable only via `chan_call()`" — `p9srv`
already proves the pattern. Needs M0 (task/endpoint headroom), benefits from
M1–M3 (affordable stacks, responsive wake). Suggested order, by how much
each currently costs the system in polling or fragility: `uart`/console
(already half a server), SD/SPI block, then display/keypad/RTC/EEPROM.
Register a named chan endpoint per driver; delete the direct call sites as
each one moves.

**Verify:** per driver converted — existing functional tests for that
device unchanged; `ps`-equivalent output shows the new task with a real
name; killing/restarting the task (manually, at first) doesn't take the
kernel down with it structurally, even before M5 makes that also true under
enforcement.

### M5 — Minimal domain by default (Rules 5 & 6 land)

Every task from `task_create()` onward gets at least a domain covering its
own now-buddy-allocated (hence NAPOT-legal) stack. Drivers identified as
handling external/untrusted input get additional regions for their device
window and private buffers, spent from PMP's 5-dynamic-region budget per
active domain — a reprogrammed-on-switch register cost, not a per-task RAM
cost, so this scales with how many *isolated* regions one task needs, not
with how many tasks exist system-wide.

**Verify:** the hardware isolation-probe tests from B3
(`plan/phase5_distributed_design.md`) extended to cover at least one
converted driver task; a deliberately corrupted build of that driver faults
and is reaped as `DEAD`/`exit_clean=false` rather than corrupting kernel
state — reproduced and confirmed the way B3/B6 findings always were, on
real RP2350 silicon, not only QEMU.

### M6 — Process visibility parity

The `ps` north star made real. Extend `sched_task_info_ex()`'s existing
reporting (already carries pid/state/name/exit info) into a real listing
command, same output shape on RP2350 and RV64/MMU. Deliberately last: it's
mostly a reporting layer over state M4/M5 already populate, and it's the
milestone where the vision statement at the top of this document becomes a
thing you can point at rather than describe.

**Verify:** side-by-side `ps` output from an RP2350 build and a QEMU RV64
build of a comparable persona shows the same task set, same shape, differing
only in which have hardware-backed isolation behind them.

---

## Effort and risk, at macro grain

| Milestone | Effort | Risk | Why |
|---|---|---|---|
| M0 | Low | Low | Constant bumps + one new parameter, default-preserving |
| M1 | Medium | Medium | New allocator core; must not regress existing palloc consumers |
| M2 | Medium | Medium — falsify on hardware | Interrupt wiring is exactly where B6 found real RP2350-only bugs before |
| M3 | Low–Medium | Low | Confined to `next_runnable()`; switch/frame layout untouched |
| M4 | High | Medium, incremental | Large surface, but done driver-by-driver, each independently shippable |
| M5 | Medium | High — falsify on hardware | PMP/erratum surprises have hit every prior milestone that touched it (E6, NAPOT-only, hardwired grants) |
| M6 | Low | Low | Reporting only, once M4/M5 exist |

**Ordering is load-bearing**, not incidental: M4 without M1/M0 reproduces the
"8 tasks and we're already at the ceiling" problem found while pricing this
in conversation; M5 without M1 has no legal way to express a stack as a PMP
region; M3 without M2 has nothing to preempt for.

## Not in scope for this phase

Adaptive-partitioning-style scheduling, priority inheritance, a general
`kmalloc`-over-buddy layer for non-PMP-relevant allocations, and multi-core —
all real, all deferred, none blocking the milestones above.
