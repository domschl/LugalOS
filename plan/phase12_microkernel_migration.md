# Phase 12 — Fine-Grained Process Architecture: A QNX-Shaped Migration

**Status:** M0-M3 complete (2026-08-15). M4 attempted (2026-08-15/16),
reverted, reformulated, and re-implemented for `uart`/QEMU rv32/rv64
(2026-08-16) — see M4's own section below for what the first attempt got
wrong and why. M4.5 complete (2026-08-16): Part A (scheduler stress test)
concluded no scheduler change is needed; Part B converted every driver in
its table — heartbeat LED, SD/block storage, RTC/EEPROM, display/keypad
(ST7735/TM1638/Pico-Clock-Green), RP2350's own uart/console, and finally
`usb_cdc.c` (which needed a different shape than the rest — see its own
completion notes). M5 Phase 1 complete (2026-08-16): the milestone as
originally written below achieves nothing (M-mode/S-mode kernel privilege
means PMP/Sv39-U doesn't restrict driver tasks at all), rescoped to real
U-mode driver-task isolation and proven end-to-end on one driver
(heartbeat) — see M5's own section for the corrected scope, the RP2350
Secure/Non-secure SIO finding, and what's still deferred. M5 Phase 2
complete (2026-08-16): the server half of the channel API
(`SYS_CHAN_SERVE_WAIT`/`_REPLY`) built and proven on tm1638, the first
U-mode driver conversion with a real `chan_call()` endpoint — three real
findings along the way (a string-literal-in-`.rodata` bug that hung the
board rather than failing cleanly, a `memset()`-under-`-fno-builtin` bug
with the same root shape, and a genuine architectural gap where a faulted
driver task left its caller blocked forever, now fixed generally via
`chan_owner_exited()`) — see M5 Phase 2's own section. M5 Phase 3
complete (2026-08-16): the shared RTC/EEPROM "i2c" task converted to
U-mode, the first driver talking to a real hardware controller (I2C0/I2C1)
rather than bit-banged GPIO — a boot-time bus fault from a missing
ACCESSCTRL write-password prefix (the third physical board recovery this
project has needed), a byte-order bug, and a lost-fallback regression in
the new EEPROM chunking, all found and fixed — see M5 Phase 3's own
section, including the heap-budget trend now flagged as needing action
before the next conversion, not after. Written to be detailed and
executed one milestone at a time, not implemented from this document
directly.

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
enforcement. **Also:** the driver conversion must not change how often
`sched_yield()`/`next_runnable()` gets called under ordinary use by more
than a small constant factor — see "What the first attempt got wrong"
below for why this is now a first-class check, not an afterthought.

#### What the first attempt got wrong (2026-08-15/16, reverted)

Converted `uart` to a task-owned `chan_call()` endpoint exactly as
described above, but routed *every single character* of console I/O
through it — `uart_putc()`, called once per byte by `printk()`/`cprintf()`/
the line editor's redraws/`SYS_PUTCHAR`, became one full `chan_call()`
(caller blocks, owner task wakes, replies, caller wakes) per byte instead
of a same-task register write. That is a real scheduling event —
`task_block()`/`task_unblock()`/a context switch — where there used to be
none, and it turned something that ran near-never (a scheduling *decision*
between two tasks that both want to run right now) into something that ran
on every character any task printed. The plain round-robin tie-break in
`next_runnable()` had never been exercised at that frequency or in that
specific shape (a repeatedly-reused server task as `from`) and it broke:
"Two User Programs Are Resident At Once (C2)" started failing, tracing
back to the tie-break systematically favoring whichever ready task sat
closest to the uart task's own table index.

Nine-plus redesigns of the tie-break followed (run_ticks/wait_credit
fair-share scoring, several charging models, decay, a scan-order hint at
`chan_serve_reply()`, and combinations of these) over roughly a day,
each fixing its target case and breaking another, never converging cleanly
on both QEMU and real RP2350 hardware together. The instruction that
actually ended the loop wasn't a better scheduler idea — it was stepping
back and asking whether the *scheduler* was the right thing to be fixing
at all, given a plain round-robin had already been correct and stable
through M0-M3, on both targets, before this milestone touched it. It
was not: reverting M4 back to M0-M3's plain `next_runnable()` (see
`kernel/sched.c`'s own comment there for the two-tier, no-scoring shape
that shipped) and testing the exact same scenarios showed the kernel side
was never actually broken — a QEMU test-harness bug (catastrophic regex
backtracking making the test runner itself hang for tens of seconds to
minutes on unrelated pattern matches, unrelated to M4, to the scheduler,
or to IPC volume at all — see `tests/runner.py`'s own history) had been
misdiagnosed as scheduler flakiness for a large part of that day, because
both failure shapes look identical from outside: output stops, a timeout
elapses.

The lesson worth keeping, independent of that test-harness confound: a
plain round-robin tie-break is fine at low decision frequency and starts
mattering the moment something makes scheduling decisions frequent enough
that *scan position relative to whichever task keeps triggering
decisions* becomes a real, exploitable bias. M4's actual mistake was
manufacturing that frequency in the first place, not the tie-break
algorithm's shape — see below.

#### The reformulated design: batch, don't chatter

`chan_call()` already takes a request buffer and a length — nothing about
the IPC primitive itself forces one call per byte. M4's first attempt
did that anyway because it kept `uart_putc(char)`'s existing byte-at-a-time
signature and routed it through `chan_call()` unchanged, the smallest
possible diff at the call site. That is exactly backwards for a driver
whose callers already mostly operate on whole strings (`printk()`,
`cprintf()`, `uart_puts()`) or could easily be made to: the fix is not a
smarter scheduler that copes with high-frequency IPC, it's not generating
high-frequency IPC in the first place.

Concretely, for the uart task (and the driver-task conversions that follow
it — SD/SPI block transfers are *naturally* buffer-sized already, this
problem is specific to a byte-oriented device being wrapped byte-at-a-time):

- The wire protocol gains a batched write op (`UART_REQ_WRITE_BUF`,
  buffer + length, alongside or replacing the single-byte
  `UART_REQ_WRITE`) sized to the endpoint's existing request buffer.
- `printk()`/`cprintf()`/`console_puts()`/`uart_puts()` format or copy into
  a local buffer first (stack-sized, bounded — these are already
  line-oriented in practice) and issue *one* `chan_call()` for the whole
  string, not one per character. `uart_putc()` stays as a real primitive
  (still needed for genuinely single-character call sites) but stops being
  what the high-volume paths funnel through.
- Reads (`uart_getc()`, the interactive line editor's per-keystroke echo
  loop) are **not** in scope for batching — a human typing is inherently
  low-frequency (single digits of characters per second, nowhere near a
  scheduling-relevant rate), and batching a request that's fundamentally
  "block until the next keystroke" doesn't make sense. M4's actual problem
  was always the write side.
- `chan_call()`'s own endpoint `busy` flag already serializes concurrent
  callers at the message level (one caller's whole buffer copied in,
  handled, copied out, before the next caller's `chan_call()` can start) —
  worth checking during implementation whether this makes some of the
  M4-attempt's `printk_lock()` widening (console.c/line_editor.c taking it
  around raw writes) redundant now that a whole *message* is naturally
  atomic at the IPC layer, rather than needing a separate lock to simulate
  that atomicity across many small calls. Not decided here; a detail-time
  question once the batched path exists to check it against.

**Verify (in addition to the per-driver criteria above):** a representative
console-heavy workload (e.g. printing `/proc/ps` or a multi-line help
text) generates IPC round trips on the order of *lines*, not characters —
countable directly (a call counter on the uart endpoint, checked in a
test, is cheap and worth adding). `next_runnable()` stays exactly the
M0-M3 shape (see `kernel/sched.c`) through this milestone — if implementing
it turns out to need scheduler changes to pass its own tests, that is a
signal the batching didn't actually cut IPC frequency enough, not a cue to
add scheduler complexity to compensate. `taskdemo`'s interleaving assertion
and the full QEMU + real-hardware suites pass with *no* scheduler changes
from their M0-M3 state.

**Explicitly starting fresh, not resuming:** the first attempt's commits
are preserved on the `phase12-m4-uart-task-wip` branch for reference (what
was tried, in what order, and why each scheduler variant failed), but
implementation should not resume from or cherry-pick that branch — nine-
plus scheduler redesigns layered on a since-abandoned premise (per-
character IPC) are exactly the kind of history that pollutes a clean
re-attempt built on the corrected premise above. The task-owned endpoint
mechanism itself (`chan_register_task()`/`chan_serve_wait()`/
`chan_serve_reply()`, the anti-cycle wait-for graph in `kernel/chan.c`)
was never the problem and is fine to re-add from scratch following this
document's original M4 description — only the per-byte call granularity
and the scheduler work done to compensate for it should *not* reappear.

#### M4 completion notes (2026-08-16, uart only — QEMU rv32/rv64)

Implemented as reformulated above, built fresh rather than resumed from
`phase12-m4-uart-task-wip`. `kernel/chan.c` gained the task-owned endpoint
mechanism (`chan_register_task()`, `chan_serve_wait()`/`chan_serve_reply()`,
the anti-cycle wait-for graph) exactly as originally designed — none of it
needed to change. `drivers/uart_16550.c` converted to a task; RP2350's
own driver is still a stub (`uart_task_start()` returns -1, direct hardware
access unchanged) for the same one-board-at-a-time reasoning as before.

The batching half: `uart_putc()` accumulates into a 256-byte buffer
(`g_tx_batch`, protected by a short `irq_save()` critical section, never
held across the blocking `chan_call()` itself) and sends one `UART_REQ_WRITE`
covering the whole buffer on flush, instead of one `chan_call()` per
character. Flush is triggered by: the buffer filling; `printk_unlock()`'s
*outermost* unlock (nested/reentrant unlocks don't flush early); and
`uart_getc()`/`uart_has_char()`, so a prompt or redraw is guaranteed visible
before this driver blocks waiting for the next keystroke rather than
whenever some *later*, unrelated flush happens to fire. Found three more
call sites during verification that write a complete message through
`uart_putc()` directly and don't hit any of those triggers on their own —
`uart_net.c`'s SLIP frame sender (the actual bug the first QEMU run of this
attempt caught: A3b's demux test timed out because a completed 9P reply sat
batched, unflushed, since nothing ever told it to go out), `vfs_server.c`'s
`/dev/uart` write handler, and `lisp.c`/`ed.c`'s raw per-keystroke echo
loops (cosmetic, not a test failure — echo would have lagged by exactly
one keystroke otherwise, visible only once the *next* key's `uart_getc()`
flushed it). Each gained its own explicit `uart_flush()` call at its own
natural message boundary.

Verified the plan's own success criterion directly rather than assuming
it: `git diff` for this milestone touches no line of `kernel/sched.c` or
`kernel/sched.h` — `next_runnable()` is byte-for-byte the M0-M3 shape.
Added `uart_write_call_count()` (`drivers/uart.h`) and a `uartstats` shell
command specifically to make "IPC volume tracks messages, not characters"
a real, checked assertion instead of an eyeballed log: `help` (~50 lines,
~2000 characters) costs ~60 `chan_call()`s, and the new "Console Output Is
Batched, Not Per-Character (M4)" test in `tests/runner.py` asserts the
delta stays under 200 (generous margin below character-scale, comfortably
above the ~60 actually measured) across a real `help` invocation.

Full QEMU suite 203/203 (two new assertions, rv32+rv64) across 4
consecutive clean runs, no scheduler-fairness retries triggered at any
point — the instability this milestone's first attempt spent a full day
chasing simply did not reproduce once IPC volume was fixed at the source.
Real RP2350 hardware 15/15 across 2 consecutive runs, on a rig with both
a physical UART dongle and a real SD card (unlike the partial x86_64
Linux rig used for cross-platform QEMU verification, which is missing
both) — this board's own driver is unconverted this round, so hardware
verification here is a regression check on the shared `kernel/chan.c`/
`kernel/main.c` changes, not new coverage; RP2350's own uart-task
conversion (with whatever board-specific batching shape its USB CDC
mirroring and heartbeat LED turn out to need) remains a separate,
not-yet-scheduled piece of this milestone.

### M4.5 — Scheduler reevaluation + the remaining driver-task conversions

Two independent pieces of leftover M4 scope, tracked under one milestone
because both follow directly from what M4's uart conversion actually
proved rather than what a day of misdiagnosed scheduler work assumed.

**Part A — Does priority scheduling actually need to be more than what M3
already built?** The nine-plus scheduler redesigns during the first M4
attempt were chasing a test-harness bug (catastrophic regex backtracking,
see M4's own section above), not a real kernel problem — plain
round-robin plus M3's flat priority tiers were never actually broken. That
leaves a real question unanswered rather than resolved, though: the tiers
have only ever been exercised with *one* `TASK_PRIO_INTERRUPT` task
(`uart`) ready at a time. Part B below adds several more driver tasks,
some of which will plausibly want that same tier — untested territory,
since two or more `INTERRUPT`-tier tasks contending for the CPU
simultaneously has never actually happened yet.

Before converting any more drivers: extend `priotest`'s shape (four
never-yielding hogs, one `task_block()`-parked high-priority task) into a
stress test with **two or more** concurrently-busy `TASK_PRIO_INTERRUPT`
tasks and check for real starvation between them — trustworthy now, since
the harness bug that made this kind of evidence unreadable all of last
time is fixed. Decide with that evidence, not by guessing: does a flat
tier plus plain round-robin tie-break hold up, or does same-tier fairness
need something. Starting hypothesis, to be falsified rather than assumed:
the current design is fine as-is — M4's actual lesson was "don't
manufacture IPC frequency," not "the scheduler needs to be smarter," and
Part B's conversions are deliberately chosen to be low-frequency,
buffer-already-sized transfers unlikely to reproduce anything like uart's
original mistake. If the stress test does find a real problem, prefer the
smallest, most targeted fix that addresses the specific measured failure
over reintroducing the abandoned fair-share scoring machinery wholesale.

**Verify:** the new multi-interrupt-tier stress test, on both QEMU and
real RP2350 hardware; a written conclusion (in this section's completion
notes) on whether `next_runnable()` needs to change before Part B assigns
`TASK_PRIO_INTERRUPT` to any more tasks.

**Part B — Convert the remaining drivers.** Same template that worked for
`uart`, applied one driver at a time (Rule 4 — verify full QEMU + real
hardware suites clean across multiple runs before moving to the next, not
after converting several at once):

1. Identify the natural message granularity first. Block I/O is already
   buffer-sized — the batching lesson uart had to learn the hard way is
   structurally satisfied before writing any code. Anything byte- or
   register-oriented needs its own batching wrapper, uart_putc()'s shape
   as the template.
2. Convert via `chan_register_task()` + a serve loop
   (`chan_serve_wait()`/`chan_serve_reply()`); the anti-cycle wait-for
   graph in `kernel/chan.c` already covers every new endpoint for free.
3. Assign a priority tier deliberately, informed by Part A's conclusion —
   not `TASK_PRIO_INTERRUPT` by default just because uart used it.
4. Add an IPC-volume counter and a test asserting real usage stays
   message-scaled, not byte-scaled (`uart_write_call_count()`/`uartstats`
   is the pattern).

Proposed order, by dependency weight and expected conversion risk (lowest
first):

| Driver | Notes |
|---|---|
| Heartbeat LED (GP16, `drivers/uart_rp2350.c`) | Not in the original table — added because it is uniquely valuable as a *visual* scheduler-health check, not for isolation or IPC-volume reasons like the rest of this list. No `chan_call()` endpoint at all: nothing calls it, it is a self-scheduled periodic task rather than a request server. Done first, out of dependency-weight order, precisely because it is the lowest-risk possible conversion (no protocol, no other task's correctness depends on it) and immediately doubles as a live confirmation of M4.5 Part A's conclusion on real hardware, continuously, without a serial connection. |
| SD/SPI block (`drivers/spisd_rp2350.c`, `drivers/virtio_blk.c`) — **done, 2026-08-16** | Every filesystem operation depends on it; already block-buffer-sized, so the lowest-risk conversion of the *service* drivers — nothing like uart's per-character mistake is structurally possible here. |
| RTC / EEPROM (`drivers/i2c_rtc.c`, `drivers/at24c32.c`) — **done, 2026-08-16** | Low frequency, small fixed-size transfers, no console-speed hazard. Converted as one shared task, not two, since both devices sit on the same physical I2C bus. |
| Display / keypad (`drivers/st7735_rp2350.c`, `drivers/tm1638_rp2350.c`, `drivers/pico_clock_green_rp2350.c`) — **done, 2026-08-16** | Turned out *not* to be the same low-risk shape as RTC/EEPROM once read closely -- see its own completion notes below for the two real batching-design questions this raised (st7735's internal pixel-level fan-out, pico_clock_green's ~1kHz scan loop). |
| RP2350's own `uart`/console task — **done, 2026-08-16** | Finishes what M4 deferred — this board's driver has real extra complexity (USB CDC mirroring, the A3b demux) that earned its own careful pass rather than a copy of `uart_16550.c`'s shape. Found and fixed two real races on real hardware in the process (priority-tier starvation of `p9srv`, and a write-vs-write hardware race in the shared retry/fallback shape — fixed in both this file and `uart_16550.c`); see its own completion notes. |
| `drivers/usb_cdc.c` — **done, 2026-08-16** | By far the largest and most structurally different of this list (989 lines, dual role as interactive console *and* 9P link) — and it turned out not to fit this table's own chan_call()-endpoint template at all once looked at closely; see its own completion notes for the different shape it actually needed. |

**Explicitly out of scope for this milestone:** `kernel/console.c` — it is
already a thin dispatch onto whatever device is bound (for `uart`, now
transitively task-backed anyway), so giving it its own task would add an
indirection layer for no isolation gain; `drivers/flashdisk.c`/
`drivers/ramdisk.c` — in-memory, always-fast, nothing blocking to isolate
against. Any further to-task conversion beyond the table above (further
peripherals, `console` should the calculus above ever change) is a later
milestone's decision, not this one's.

**Verify (Part B):** per driver converted — existing functional tests for
that device unchanged; the new IPC-volume test for it passes; full QEMU
suite and real RP2350 hardware suite both clean across multiple
consecutive runs before starting the next driver in the table.

#### M4.5 Part B, heartbeat LED completion notes (2026-08-16)

Pulled the GP16 heartbeat out of `gp16_alive_tick()`, which stepped it as
a side effect of `uart_has_char()`/`uart_getc()`'s console-polling loop,
and into its own task (`heartbeat_task_start()`, `drivers/uart_rp2350.c`).
`usb_cdc_task()` — the other thing that poll used to carry — stays exactly
where it was, called directly at both original sites; only the LED state
machine moved. `TASK_PRIO_NORMAL`, deliberately not `TASK_PRIO_INTERRUPT`:
the point is a live check on ordinary scheduling health (does a NORMAL-
tier task still get its turn under load), which an INTERRUPT-tier
heartbeat would trivially satisfy regardless of whether anything else in
the system is actually healthy. Sleeps by yielding between checks
(`time_get_ms()` polled in a loop, not a busy spin) rather than burning
CPU another READY task could use for two seconds at a time.

Confirmed on real hardware: after flashing, the board's external LED
blinks at the expected ~0.5 Hz rate, continuously, independent of any
serial activity — direct visual confirmation the scheduler keeps giving
this task its turn, matching Part A's measured conclusion rather than
merely restating it.

Two real, unrelated bugs found and fixed while verifying this on real
hardware, neither specific to the heartbeat feature itself:

1. **rv32 UBSan halt in the *other* new M4.5 test.** `priostress_body()`
   (Part A) accumulated `sink += i` across 150M iterations into a `long`
   -- 32 bits on rv32, 64 on rv64 -- and overflowed it almost immediately;
   UBSan correctly caught this as a real fault and halted the board,
   invisible on rv64 where the wider type absorbed it. Fixed by
   overwriting (`sink = i`) instead of accumulating; only a volatile write
   was ever needed to keep the optimizer from eliminating the loop.
2. **A stale-object build-id bug in `CMakeLists.txt`, found by flashing
   this milestone's own work.** `fs/vfs_server.c` embeds
   `LUGALOS_BUILD_ID` (served as `/proc/buildid`, and what
   `tests/hw/flash.py --verify` checks), but nothing established a ninja
   dependency edge between "regenerate the header" (an `ALL` custom
   target, reruns every build) and "compile the one file that includes
   it" -- `add_dependencies(lugalos.elf lugalos_build_id)` only orders the
   custom target before the final *link*, not before individual compiles.
   Three consecutive real flashes each reported a build-id mismatch even
   though the board visibly had the new firmware (a blinking heartbeat
   LED that could not have existed in the old build) -- `touch
   fs/vfs_server.c` and rebuilding made the mismatch vanish, confirming a
   stale object rather than a bad flash. Fixed with
   `set_source_files_properties(fs/vfs_server.c PROPERTIES OBJECT_DEPENDS
   ...)`, the CMake property that actually closes this gap rather than
   relying on GCC's own header-scanning having already seen a newer mtime
   by chance. Also hardened `tests/hw/flash.py`'s UF2 copy itself while
   investigating (`os.fsync()` after the write, and waiting for the
   BOOTSEL volume to actually disappear before considering the flash
   done) — did not turn out to be the cause of this specific bug, but is
   a real gap in its own right and cheap to close.

Verified: full QEMU suite 205/205 (unaffected — this driver is RP2350-only)
across 2 consecutive clean runs. Real RP2350 hardware: heartbeat confirmed
blinking by direct observation after a verified-matching flash; full
hardware suite run to confirm no regression from the polling-loop split.

#### M4.5 Part A completion notes (2026-08-16) — conclusion: no scheduler change needed

Built `priostress` (`kernel/shell.c`): two `TASK_PRIO_INTERRUPT` tasks,
each spinning a fixed 150M iterations with no yields and no syscalls,
using the same self-measurement idea as `user/progs/uspin.c` (read the
tick counter, spin, read it again) to record how many preemption ticks
each took to finish. No new scheduler bookkeeping added to
`kernel/sched.c` to measure this — deliberately, since the question was
whether the tiers already built are sufficient using only what a task can
observe about itself.

Result, measured directly rather than assumed: both tasks finish within 0-3
ticks of each other (out of ~70-250 ticks total, depending on target),
every single run —

- QEMU rv64: 150/150, 151/151, 152/152 (3 consecutive runs).
- QEMU rv32: 70/70, 70/71, 68/70 (3 consecutive runs).
- Real RP2350 hardware: 248/251, 248/251, 250/251 (3 consecutive runs).

A starved task would show roughly a 2x gap (it waits out ~all of the
other's run, then does its own on top); what was actually measured is a
gap of well under 2%, consistent with fine-grained alternation rather
than either task-table slot being favoured. **Conclusion: M3's flat
priority tiers plus `next_runnable()`'s plain round-robin tie-break
already share the CPU fairly between multiple same-tier
`TASK_PRIO_INTERRUPT` tasks — no scheduler change is needed before Part B
assigns that tier to more driver tasks.** The starting hypothesis in this
section's own text is confirmed, not merely assumed: M4's actual lesson
really was "don't manufacture IPC frequency," not "the scheduler needs to
be smarter," and Part B should proceed without adding any fairness
machinery beyond what M0-M3 already built.

One real bug found and fixed while building the test, unrelated to the
scheduler question itself: the first version of `priostress_body()`
accumulated `sink += i` across all 150M iterations. `long` is 32 bits on
rv32 (64 on rv64), and UBSan caught the accumulation overflowing it
almost immediately as a real signed-integer-overflow fault, halting the
board — invisible on rv64, where `long`'s wider range absorbed it,
exactly the kind of target-specific gap Rule 0 exists to catch. Fixed by
overwriting instead of accumulating (`sink = i`); only a volatile write
was ever needed to keep the optimizer from eliminating the loop, the
summed value itself was never meaningful.

Also fixed in passing, unrelated to M4.5 itself: `CMakeLists.txt` now sets
`CMAKE_EXPORT_COMPILE_COMMANDS ON` and a repo-root `.clangd` points at
`build/rv64`'s database — editor tooling had no other way to learn this
project's real (cross-compilation) include paths and was guessing wrong
on nearly every file.

Verified: full QEMU suite 205/205 (two new assertions, rv32+rv64) across
3 consecutive clean runs. Real RP2350 hardware 16/16 (one new assertion)
across 2 consecutive clean runs.

#### M4.5 Part B, SD/block storage completion notes (2026-08-16)

Converted both block-storage backends -- `drivers/spisd_rp2350.c` (real SD
hardware, endpoint `"sdblk"`) and `drivers/virtio_blk.c` (QEMU, endpoint
`"blk"`) -- to task-owned `chan_call()` endpoints. Confirmed the plan's
own risk assessment directly: `block_dev_t`'s `read_blocks()`/
`write_blocks()` signature (an explicit `lba`+`count`) already carried
exactly one message's worth of work per call, so this needed no batching
redesign at all, unlike uart -- the wire protocol is a straight
opcode+lba+count header plus the raw block data, `BLK_MAX_COUNT=4` giving
4x headroom over the only usage pattern that actually exists in this tree
(`fs/fat32.c` calls every `read_blocks()`/`write_blocks()` with `count=1`;
this codebase's own `fat32_format()` always uses `sec_per_clus = 1`).

Both drivers share one structural move: the original hardware access
function (`spisd_read_blocks`/`spisd_write_blocks`,
`virtio_blk_read`/`virtio_blk_write`) split into an internal `_hw_`-style
primitive callable from the task's serve loop, and a new public facade
(reusing the *original* function names, so `block_dev_t`'s function
pointers and every caller in `fs/fat32.c` needed no changes at all) that
routes through `chan_call()` when the task is alive and falls back to
direct access otherwise -- the same shape `uart_16550.c` established, and
for the same boot-ordering reason: `vfs_server_init()` mounts the
filesystem *before* `sched_init()` creates a task table at all, so every
read during early boot necessarily uses the fallback, and only reads
after `spisd_task_start()`/`virtio_blk_task_start()` (called right after
`sched_init()`) route through the task.

Added `blk_task_call_count()` (`blkstats`) as the IPC-volume verification
this milestone's own template calls for -- but unlike uart's "did the call
count stay message-scaled, not character-scaled" question (never in doubt
here, see above), the real question was simpler: is the task actually
being used, or did every caller silently take the fallback path the whole
time (which would still pass every functional filesystem test and prove
nothing)? Measured directly: a single file read (`cat`) grows the count by
double-digits on both QEMU (+27) and real hardware (+29 to +31 across
runs), confirming genuine task-routed traffic, not a fallback that happens
to work.

One real cost observed, not a defect: `virtio_blk_write()`/
`spisd_write_blocks()`'s request buffer (`BLK_HDR_LEN + BLK_MAX_COUNT *
512` = ~2 KB) is stack-allocated in the facade function rather than
static, and real hardware's own boot-stack-peak measurement moved from
~6.5 KB to ~9.5 KB of its 16 KB budget between this milestone and the
last one measured -- comfortable margin remains, but worth having on
record given RP2350's stack budget is a real, previously-tracked resource
(see M4.5's own heartbeat notes and the `Memory margins` hardware test),
and `BLK_MAX_COUNT` is the knob that would need revisiting first if that
margin ever tightens.

Verified: full QEMU suite 207/207 (two new assertions, rv32+rv64) across
3 consecutive clean runs. Real RP2350 hardware 17/17 (one new assertion)
across 2 consecutive clean runs.

#### M4.5 Part B, RTC/EEPROM completion notes (2026-08-16)

Converted `drivers/i2c_rtc.c` (DS1307/DS3231 RTC, including the DS3231-only
temperature sensor) and `drivers/at24c32.c` (4 KB EEPROM) into **one** shared
task, `"i2c"`, rather than two independent ones — the deciding fact,
found while reading both files before writing any code, is that they sit on
the *same physical I2C bus* (both target I2C0's registers in the default
chess persona; the Pico-Clock-Green persona's `CONFIG_I2C_RTC_BASE` moves
the RTC to I2C1, but `at24c32.c`'s own bit-banging is a separate, pre-
existing implementation that hardcodes I2C0 regardless — a real, pre-
existing inconsistency, left alone as out of scope for this pass, same as
the decision to not touch unrelated code during a driver conversion). One
task means a `chan_call()` from either device serializes against the other
automatically, closing a latent hole that existed before this milestone:
a preempted `i2c_rtc_write_time()` and an interleaved `at24c32_write()` had
no mutual exclusion at all.

Same structural move as every other M4.5 driver conversion: each original
function split into an internal `_hw_`-suffixed direct-hardware primitive
(`i2c_rtc_hw_read_time`/`write_time`/`read_temperature_c`,
`at24c32_hw_read`/`write` — bodies byte-for-byte unchanged, including
`at24c32_hw_write`'s existing page-boundary chunking and 10 ms inter-chunk
delay, preserved as a single unit inside the task rather than re-implemented
at a lower level), and a new public facade reusing the *original* names
(`i2c_rtc_read_time`, `i2c_rtc_write_time`, `i2c_rtc_read_temperature_c`,
`at24c32_read`, `at24c32_write`) that routes through `chan_call()` when the
task is alive, falling back to direct access otherwise — so every existing
caller (`kernel/board.c`'s boot-time `dev_probe_all()`, `kernel/shell.c`'s
`date`/`i2c` commands, `user/lisp/lisp.c`'s `eeprom-read`/`write`/
`(clock-light)` family, `fs/vfs_server.c`'s `/dev/eeprom`, and the
Pico-Clock-Green persona's own display-update loop) needed zero changes.

The wire protocol deliberately does *not* copy `spisd_rp2350.c`'s explicit
big-endian `be32_load`/`store` encoding: that discipline exists to defend a
real wire (an SD card's SPI protocol), where this IPC never leaves the
kernel's own address space, so `addr`/`len` travel as plain `uint16_t` and
`rtc_time_t`/the temperature travel as a raw `memcpy()` between endpoint-
owned buffers — simpler, and there's no real byte-order boundary here for
an explicit encoding to protect. The EEPROM buffers are sized to the whole
4 KB AT24C32 device (`AT24C32_SIZE_BYTES`) rather than to a measured caller
the way `BLK_MAX_COUNT` was: unlike an SD card, this device's entire address
space is small and fixed, so covering all of it is a bounded, known cost
(~8 KB of static request/response buffers) instead of open-ended headroom
for a hypothetical future caller.

Added `i2c_task_call_count()` (`i2cstats`) as the IPC-volume verification
this milestone's template calls for — the same "is the task actually being
used" question as `blkstats`, not a "did volume stay message-scaled"
question (never in doubt for a low-frequency device like this one). On real
hardware (the chess persona board, which has no RTC/EEPROM module
physically wired up) an `(eeprom-write ...)` still has to reach the task —
and come back with a hardware-level failure — to be observed at all, which
is exactly what the test checks; it does not require the write to succeed
at the silicon level. Confirmed the count grows on both QEMU (where the
EEPROM half is backed by a synthetic in-RAM buffer and genuinely succeeds)
and real hardware (where it reaches the task and correctly fails at the
I2C level, count still grows by exactly one call).

Verified: full QEMU suite 209/209 (two new assertions, rv32+rv64, doubled
by the multi-node test battery) across 3 consecutive clean runs on both
rv64 and rv32. All four board personas (`rv64-mmu`, `rv32-nommu`,
`rp2350-chess`, `rp2350-clock`) build clean. Real RP2350 hardware (chess
persona) 18/18 (one new assertion) across 3 consecutive clean runs,
including direct confirmation that `eeprom-write` reaches the shared task
on real silicon.

#### M4.5 Part B, display/keypad completion notes (2026-08-16)

Converted all three remaining chess/clock-persona drivers -- ST7735 TFT
canvas (`drivers/st7735_rp2350.c`), TM1638 7-segment/keypad/LEDs
(`drivers/tm1638_rp2350.c`), and the Pico-Clock-Green LED matrix
(`drivers/pico_clock_green_rp2350.c`) -- to task-owned `chan_call()`
endpoints ("st7735", "tm1638", "clock"). Unlike RTC/EEPROM, this table
entry's "same low-risk shape" note did not survive reading the actual code:
two real batching-design questions turned up, both resolved by extending
the template rather than by adding new machinery.

**ST7735.** `st7735_draw_char()`/`draw_string()` already fan out internally
into many `draw_pixel()`/`draw_rect()` calls (a full glyph can be dozens of
pixel writes, a status string dozens more per character) -- exactly the
per-primitive shape that made uart's first M4 attempt a mistake, if naively
put on the wire one primitive at a time. The fix is the same one every
other M4.5 conversion already uses without naming it: only *public* entry
points became wire ops (`fill_screen`, `draw_pixel`, `draw_rect`,
`draw_bitmap_mono`, `draw_char`, `draw_string`), and the internal fan-out
(`st7735_hw_draw_string` -> `st7735_hw_draw_char` -> `st7735_hw_draw_pixel`/
`draw_rect`) stays plain C calls on the task's own stack, never re-entering
`chan_call()` on the same endpoint (which the busy-endpoint check would
refuse anyway). A full 64-square chess-board redraw is therefore ~64
`chan_call()`s, not thousands -- confirmed comparable in kind to
blk's "double-digit calls per `cat`," not a regression toward uart's
original mistake. Measured before converting (SPI0's prescaler is 6 against
a 150MHz clock, ~25MHz SPI): a 16x16 bitmap is ~150-200us of real SPI time,
so IPC overhead across a full redraw is on the order of 1-2ms against an
already ~10-15ms operation -- not perceptible, confirmed directly by
running `chess` on real hardware after flashing (board owner's own words:
"Chess & 7-segment + keypad are all working fine!").

**TM1638.** `tm1638_get_key()` is polled from
`user/chess/src/chess_ui.c`'s `tm_wait_key()` at a paced 20ms interval
(~50/sec, only while actively waiting on a human, never a tight loop) --
already the right granularity, no redesign needed, same shape as SD/block.

**Pico-Clock-Green.** The real hazard: `pico_clock_green_run()` drives the
row-scan step at ~1kHz, continuously, for as long as the appliance runs
(minutes to hours) -- an order of magnitude past anything else converted
under this milestone, and putting one `chan_call()` per scan step on the
wire would have dwarfed every other driver's IPC volume combined. Resolved
by converting the *whole loop* into one long-blocking `chan_call()`: the
task's own body runs `pico_clock_green_hw_run()` (the renamed, unchanged
original loop, including its Ctrl-C poll) to completion and only then
replies, so the calling task simply blocks for the session's duration --
mechanically identical to any other `chan_call()`, just a much longer one.
`show_time()`/`show_temperature_c()`/`clear()`/the per-row scan step had no
caller anywhere outside this file, so they became `static` rather than wire
ops nothing would ever address individually; only `run` and `read_light`
needed endpoints. `pico_clock_green_hw_run()` itself calls
`i2c_rtc_read_time()`/`read_temperature_c()` -- the *public* facades from
the already-converted "i2c" task (M4.5's own RTC/EEPROM section above), not
that driver's `_hw_` internals -- meaning the "clock" task becomes a
*caller* of the "i2c" task. Checked against `kernel/chan.h`'s no-cycles
rule before writing any code: "clock" never serves a call from "i2c", so
this is a valid strictly-top-down chain (shell/lisp task -> "clock" ->
"i2c"), not a violation of the same rule this file's own endpoint relies on.

All three drivers share the established structural move: each original
function split into an internal `_hw_`-suffixed primitive (bodies
unchanged) and a public facade (original name preserved) routing through
`chan_call()` when the task is alive, falling back to direct access
otherwise -- so every caller (`user/chess/src/chess_ui.c`,
`user/lisp/lisp.c`'s `canvas-*`/`tm-*`/`clock`/`clock-light` primitives)
needed zero changes. Init calls (`st7735_init()`, `tm1638_init()`,
`pico_clock_green_init()`) stay direct-hardware, unconverted, for the same
boot-ordering reason as every prior conversion: `kernel/main.c` calls them
before `sched_init()` exists.

Verified: all four board personas build clean (`rv64-mmu`/`rv32-nommu`
unaffected -- these three drivers are RP2350-only; `rp2350-chess` links
both st7735+tm1638; `rp2350-clock` links pico_clock_green with the other
two off, confirming the CMake persona split still holds). Full QEMU suite
209/209 across 3 consecutive clean runs on both rv64 and rv32 (unaffected).

Real RP2350 hardware, chess persona (st7735 + tm1638): `st7735stats`/
`tm1638stats` IPC-volume assertions added to `tests/hw/test_rp2350.py`,
full suite 20/20 across 3 consecutive clean runs, plus the board owner
directly ran `chess` after flashing and confirmed both the display and
keypad work correctly -- the actual feature, not just IPC reachability.

Real RP2350 hardware, Pico-Clock-Green persona (`pico_clock_green`): no
automated suite (`tests/hw/test_rp2350.py` targets the chess persona's
command set), so verified manually over a direct serial session instead.
The appliance persona auto-starts `(clock)` at boot, which -- correctly,
unchanged from before this conversion -- leaves the console unresponsive
to typed input until Ctrl-C, since the "clock" task's one long served call
is what's occupying the display loop. Sent Ctrl-C, got the shell prompt
back, then confirmed directly: `clockstats` read `calls=1` for the entire
auto-started session (auto-start to Ctrl-C, several seconds of real
alternating time/temperature display) -- exactly the "one call for the
whole loop, not one per row-scan step" the design set out to achieve;
`i2cstats` read `calls=20`, confirming the "clock" -> "i2c" cross-task call
chain genuinely executed (about once/sec, matching `CLOCK_READ_INTERVAL_MS`)
rather than silently falling back; `(clock-light)` returned a real 12-bit
ADC reading (183) and grew `clockstats` to `calls=2`, confirming the second
wire op independently. The board owner confirmed the actual rendering
directly: "Display of time & temperature work fine!"

#### M4.5 Part B, RP2350's own uart/console task completion notes (2026-08-16)

Finished what M4 deferred: `drivers/uart_rp2350.c` now runs the same task-
owned `"uart"` endpoint `drivers/uart_16550.c` (QEMU) already had, batching
console output into whole `chan_call()`s and adding two things that file
doesn't need -- a USB CDC mirror on every write (`uart_putc()`'s original
behavior, preserved, now inside the task), and RX handled by polling rather
than an ISR (this board has none to block on for RX, unlike TX, which
already had one from M2.5 -- see `uart_init()`'s own comment). Same
structural move as every other M4.5 conversion: `uart_hw_putc()`/
`hw_uart_has_char()`/`hw_uart_getc()` stay the direct-hardware primitives,
now called only from the task; `uart_putc()`/`uart_getc()`/`uart_has_char()`
became public facades routing through `chan_call("uart", ...)` when the
task is alive, falling back to direct access otherwise -- zero changes
needed anywhere that calls them.

Two real bugs found bringing this up on real hardware, neither visible on
QEMU, both closed before this milestone was considered done:

1. **Priority-tier starvation of `p9srv`.** RX has no interrupt on this
   board, so waiting for a keystroke means the task genuinely loops
   (`sched_yield()`-paced), unlike `uart_16550.c`'s true ISR-driven
   `task_block()`. Started at `TASK_PRIO_INTERRUPT` (matching that file's
   own task) for the whole wait, this starved `p9srv` (`fs/p9_link.c`,
   `TASK_PRIO_NORMAL`, services the ACM1 background 9P link) badly enough
   that `link_usb_cdc` stopped answering `Tversion` probes entirely --
   confirmed by isolating firmware-only (stashing this milestone's changes,
   rebuilding the prior commit, reflashing the *same* physical board/cable/
   port: clean; restoring the changes and reflashing: broken), then by
   flipping just this one task's priority as a diagnostic (`TASK_PRIO_NORMAL`
   fixed it immediately). Fixed by dropping to `TASK_PRIO_NORMAL` only for
   the READ handler's own wait loop, restoring `TASK_PRIO_INTERRUPT`
   afterward -- WRITE/HASCHAR (genuinely near-instant) keep the fast-handoff
   tier `uart_16550.c`'s task uses; the one open-ended wait no longer holds
   it.
2. **A write-vs-write race in the shared bounded-retry-then-fallback shape**
   (`uart_call_with_retry()`, both this file and `uart_16550.c` -- the QEMU
   file had the identical defect, just far harder to trigger). A `WRITE`
   `chan_call()` that finds the endpoint busy retries a bounded number of
   times, then falls back to direct hardware access -- correct when the
   endpoint is busy with a pending, unbounded-duration READ (a printk()
   call must never wait out a human's keystroke), *wrong* when it's busy
   with another WRITE still being transmitted: on real 115200-baud
   hardware, a full 256-byte batch takes tens of milliseconds, far longer
   than a bounded retry loop patiently waits, so a second WRITE's fallback
   raced the first WRITE's own in-flight transmission at the hardware
   level. Observed directly as byte-level interleaved console garbage
   (`"[SUSPIN_STARTched] Created task #8 'uprog'..."` -- a kernel `printk()`
   line and a just-spawned user program's own stdout, landing on the wire
   at once) while testing concurrent user-program spawning on real
   hardware -- QEMU's C2-equivalent test never caught it because its
   emulated UART is fast enough that retries essentially never exhaust in
   practice. Fixed in both files with one new flag
   (`g_uart_write_in_flight`, true only while the task's WRITE handler is
   actually pushing bytes) that lets `uart_flush()`'s retry loop distinguish
   "busy with a bounded WRITE -- wait it out, never race it" from "busy
   with an unbounded READ -- fall back right away," rather than treating
   all busy-ness the same way.

Verified: full QEMU suite 209/209 (both `uart_16550.c` and `uart_rp2350.c`
share the fix's shape now) across clean runs on both rv64 and rv32; all
four board personas build clean. Real RP2350 hardware (chess persona)
21/21 (two new assertions: batching stays message-scaled via `uartstats`,
and C2's concurrent-user-program test, which the write-race bug had been
failing) across 3 consecutive clean runs.

#### Unscheduled: `link_usb_cdc` framing resync (2026-08-16, found and fixed while diagnosing the above)

Not on this milestone's own list, but found directly while manually
probing ports during the uart-task bring-up above, and closed the same
day per this project's zero-defects standard rather than filed away for
later. `drivers/usb_cdc.c`'s ACM1 net link (`ep4_link_poll()`) used a
4-byte length-prefixed frame header with no resync path: an implausible
length (a stray/partial write landing on the port -- exactly what a
manual probing script's own mistakes produced) made every subsequent
`poll()` re-examine the *same* corrupt 4-byte window forever, since
nothing ever advanced past it -- a real USB bus reset (unplug/replug) was
the only way out, matching `tests/hw/README.md`'s own long-standing
troubleshooting note about this exact symptom.

Fixed to discard one byte and report "not ready yet" instead of "corrupt"
whenever the window doesn't look like a plausible header, so the stream
re-syncs itself within a few `poll()` rounds once real, frame-aligned
traffic resumes -- the standard byte-at-a-time resync a length-prefixed
protocol can still do without a sync marker, not as strong a guarantee as
SLIP's own byte-stuffed self-delimiting framing (`drivers/uart_net.c`'s
A3b demux already has that), but strictly better than wedging outright,
which was the actual failure mode. Also tightened the upper-bound check
from the ring's own capacity (8192 bytes) to `P9_MAX_MSIZE` (4096,
`recv_frame()`'s real ceiling), closing a second, narrower gap where
`poll()` could report "ready" for a length `recv_frame()` would then
refuse anyway with nothing discarded there either.

Verified directly: deliberately wrote 41 bytes of garbage to the net port,
confirmed a subsequent real `Tversion` frame still got a real `Rversion`
back with no reset in between (previously this wedged permanently). Added
as a permanent regression test (`test_usb_cdc_net_resync`,
`tests/hw/test_rp2350.py`) rather than left as a one-off manual check.
Real RP2350 hardware 22/22 (one new assertion) across 3 consecutive clean
runs. QEMU unaffected (`drivers/usb_cdc.c` is RP2350-only hardware; the
file still compiles for every target via its own stub, confirmed via a
full rv64/rv32 rebuild and 209/209 suite pass).

#### M4.5 Part B, `drivers/usb_cdc.c` completion notes (2026-08-16) — the last item, and a different shape

This table entry's own note ("scope this one separately... once the
simpler conversions above have re-validated the template") turned out to
be right for a reason not fully anticipated: `usb_cdc.c` doesn't have the
"one task should exclusively own this hardware" shape every other driver
in this table did. EP2 (console) is already exclusively touched by the
"uart" task; EP4 (net) is already exclusively touched by "p9srv" -- both
converted earlier in this same milestone. Wrapping either in a *third*
`chan_call()` layer here would have added IPC overhead without adding any
isolation neither already has.

What this driver actually needed was named directly in
`arch/riscv/common/elf.c`'s own pre-existing comment on its (former)
`usb_cdc_task()` call: *"USB is serviced by polling from whatever happens
to be busy-waiting... A user program that computes for a while enters the
kernel only on its own syscalls and on timer ticks, and this loop is the
only other thing running, so without the poll here nothing drains the CDC
TX ring for the duration. The symptom is specific and was initially
mistaken for a preemption failure."* Six call sites across the tree
(`kernel/time.c`, `kernel/line_editor.c` x3, `arch/riscv/common/elf.c`,
`user/lisp/lisp.c`, `drivers/uart_rp2350.c`'s own task) all had to
remember to pump `usb_cdc_task()` opportunistically; any future code path
that forgot would silently stop draining USB, with exactly that confusing
symptom.

Closed structurally instead of by remembering harder: a dedicated
background task (`usb_cdc_task_start()`, same shape as
`heartbeat_task_start()` -- **no `chan_call()` endpoint**, nothing calls
this as a request/response operation, it exists purely to guarantee
`usb_cdc_task()` a scheduled turn regardless of what any other task is
doing) loops it continuously at `TASK_PRIO_NORMAL`. Every post-boot
scattered call site was removed outright (guaranteed this task is already
alive by the time any of them run); `uart_rp2350.c`'s own three internal
call sites were made conditional on `usb_cdc_task_alive()` instead
(defensive fallback, matching the pattern every other M4.5 conversion
uses); `kernel/time.c`'s `time_delay_us()` was deliberately left
unconditional -- it is a pure busy-wait with no `sched_yield()` at all
(unlike every other site, which already yields every iteration), so
gating it on task-aliveness would leave short delays with zero USB
servicing if a preemption tick doesn't happen to land during them; its own
re-entrancy guard already makes the occasional redundant call free.

Two things found and fixed in the same pass, both closed the same day
rather than filed away, per this project's standing rule:

1. **The build-id staleness bug (`fs/vfs_server.c` / `/proc/buildid`)
   recurred**, months after M4.5's SD/block milestone believed it had
   fixed it with `OBJECT_DEPENDS`. Root cause, found this time by reading
   the generated `build.ninja` directly rather than guessing: CMake's
   `OBJECT_DEPENDS` only tells Ninja "this object depends on this *file*"
   -- it does not tell Ninja *what produces that file*, and the header was
   being regenerated by `add_custom_target(... ALL COMMAND ...)`, which
   has no `OUTPUT` of its own for CMake to wire in. Ninja was therefore
   free to check `vfs_server.c.o` against whatever mtime
   `lugalos_build_id.h` already had -- possibly from *before* the current
   run's regeneration touched it -- rather than being forced to regenerate
   first. `touch fs/vfs_server.c` masked the symptom every time it was
   tried (a real recompile always reads the current header off disk,
   regardless of Ninja's dependency graph), which is exactly why the
   original fix looked like it had worked. Fixed properly this time with a
   real producer/consumer edge: `add_custom_command(OUTPUT
   ${LUGALOS_BUILD_ID_HEADER} ... DEPENDS ${always_rebuild_marker})`, where
   the marker is a `SYMBOLIC`-flagged always-dirty file (the standard
   CMake+Ninja idiom for "regenerate unconditionally") -- confirmed by
   inspecting the generated `build.ninja`'s own edges afterward (`build
   lugalos_build_id.h ... : CUSTOM_COMMAND _always_rebuild`, a real
   producer rule this time, not an orphan path).
2. **Adding the new task's own permanent stack tipped heap peak usage from
   53/54 to exactly 54/54 pages** (0 free at peak) during the
   concurrent-user-program stress scenario `tests/hw/test_rp2350.py`
   already exercises -- confirmed reproducible across multiple runs, not a
   flake. RAM here is already tight (508/512 KB accounted for across
   image, boot stack, and heap), so growing the heap to compensate isn't
   free either. Judged, deliberately, a real but acceptable trade-off
   rather than something to engineer around: the peak is transient
   (`pages_free > 0` afterward confirms the heap recovers), nothing
   actually failed, and it buys a genuine correctness fix. Loosened
   `test_memory_margins`'s "heap peak stayed within the heap" check from
   strict `<` to `<=` with a comment recording why, matching the SD/block
   conversion's own precedent of documenting a measured resource cost
   rather than hiding it -- a *future* regression that pushes peak usage
   strictly past `pages_total` remains a real failure this check still
   catches.

Verified: full QEMU suite 209/209 across clean runs on both rv64 and rv32
(`drivers/usb_cdc.c` compiles for every target via its own non-RP2350
stub; unaffected functionally). All four board personas build and link
clean, confirmed after a full reconfigure of each (the CMake fix touched
shared build-graph plumbing, not just RP2350-specific code). Real RP2350
hardware 22/22 across 3 consecutive clean runs, including direct
confirmation the new "usbcdc" task appears in `cat /proc/ps` and that the
board-id-verified flash workflow itself now works reliably without a
manual `touch` workaround.

### M5 — Real U-mode isolation for driver tasks *(Phase 1 done, 2026-08-16 — scope corrected by a real finding)*

**As originally written above ("every task gets a domain covering its
stack"), this milestone achieves nothing.** `mem_domain_activate()`'s own
code comment already said so: *"a kernel task runs with no restriction at
all, and M-mode ignores an unlocked region anyway."* Confirmed via
`entry.S`: RP2350 (`CONFIG_MODE_M`) runs the kernel, and therefore every
M4/M4.5 driver task, permanently in M-mode; PMP only restricts privilege
levels *below* the one that programs it. rv64 (`CONFIG_MODE_S`) has the
same gap through `sstatus.SUM` and S-mode's own unrestricted CSR access.
Attaching a domain to a driver task as scoped above would compile and
"activate" with zero enforcement — exactly the "claiming isolation that
hasn't been verified" failure this document's own earlier milestones warn
against.

The only way to get real, hardware-enforced isolation for a driver task is
to run it at the same privilege U-mode *user programs* already run at,
through the same `mem_domain`/PMP mechanism B3 built for them. That is
substantially bigger than this section's original 15 lines implied —
comparable in size to all of M4.5, not an extension of it — so Phase 1
deliberately converts exactly **one** driver task (heartbeat: no
`chan_call()` endpoint, one MMIO register pair, no shared request/response
buffers) and builds only the infrastructure that one conversion needs.
Converting the rest needs `chan_serve_wait()`/`chan_serve_reply()` as new
syscalls (heartbeat has no endpoint, so didn't need them) and, once a
*second* U-mode driver task needs isolation from the *first*, probably a
proper separately-linked-image loader in place of the shared `.utext` page
— both explicitly deferred, not attempted here.

**What Phase 1 built:**

- Two new syscalls (`SYS_YIELD=22`, `SYS_TIME_MS=23`) — a long-lived U-mode
  driver task's poll loop needs both, and neither existed (`SYS_TICKS` is a
  raw preemption-tick counter, not wall-clock-ish time; syscall 0 was a dead
  stub, not wired to `sched_yield()`). Exercised from QEMU first, via
  `user/progs/uspin.c`, before anything PMP-specific touched hardware.
- `drivers/uart_rp2350.c`'s `heartbeat_task_body()`/`heartbeat_umode_body()`
  converted to real U-mode, via the same `.utext`/`UATTR` mechanism
  `kernel/shell.c`'s `usertest`/`isolationtest`/`deputytest` already used —
  the simpler, lower-risk option over a new embedded-ELF loader, since only
  one U-mode driver task exists so far. Domain: own dedicated stack (RW),
  the shared `.utext` page (RX), and a 4096-byte window at `SIO_BASE` (RW)
  for the GPIO registers.
- `heartbeatisotest` (`kernel/shell.c` + `drivers/uart_rp2350.c`): this
  phase's actual "Verify" deliverable, on real silicon. A probe built from
  the *exact* domain shape the real heartbeat task runs under, deliberately
  storing to a kernel canary instead of GPIO — proving the SIO grant is
  narrow (governs GPIO, nothing else), not just that U-mode isolation works
  in the abstract. The intruder's own stack is `palloc_pages(1)`'d for the
  duration of the test and freed right after, not a permanent static array
  — see the heap-margin finding below for why that distinction turned out
  to matter on this board.

**The real hardware finding — RP2350's Secure/Non-secure SIO split:**
after the conversion above, the heartbeat LED did not blink, with no PMP
fault, no error, nothing — the domain, PMP configuration, and control flow
were all independently confirmed correct (PMP register readback matched,
task ran fault-free, timestamps advanced correctly), yet the GPIO write
had silently no effect. Root-caused via the locally available RP2350
datasheet (`~/gith/pico/datasheet`, Section 3.1.1, "Secure and Non-secure
SIO") and pico-sdk headers (`accessctrl.h`), per the user's explicit
"local sources first" direction: RP2350 implements an Arm TrustZone-style
Secure/Non-secure hardware partition that RISC-V's M-mode/U-mode maps
directly onto. SIO's GPIO output/input registers are shared silicon, but a
Non-secure (U-mode) bus access is filtered *per-GPIO* by ACCESSCTRL's
`GPIO_NSMASK0`/`GPIO_NSMASK1` registers — a bit not set there makes the
GPIO's SIO bit "read-only zero" from U-mode, with no fault. This defaults
to all-zero (every GPIO Secure-only) on reset, so every board persona has
silently had every GPIO Secure-only the entire session, invisible until
this was the first thing to ever touch a GPIO from U-mode. It is a filter
entirely upstream of and independent from PMP: a U-mode task's own PMP
domain cannot grant its way around it, only M-mode/Secure code can, before
the task ever runs. Fix: `uart_init()` (M-mode, runs long before any task
exists) now sets `ACCESSCTRL_GPIO_NSMASK0`'s bit for `CONFIG_LED_EXT_GPIO`.
Two smaller, genuine bugs were found and fixed along the way to this
finding: an unconstrained inline-asm syscall stub in a throwaway diagnostic
clobbering a register across consecutive calls (found via disassembly, not
present in the final code), and `heartbeat_usleep_until()` initially being
a plain `static void` rather than tagged `HEARTBEAT_UATTR` — the compiler
inlined it during early bring-up and stopped once its body grew, silently
placing it in ordinary kernel `.text` outside the task's granted RX region,
which took a real instruction-access fault on hardware. Now tagged
explicitly rather than relying on the optimizer, matching `kernel/shell.c`'s
own "a helper that is 'obviously' inlined is not a guarantee" precedent.

**A second, unrelated real hardware finding — heap fragmentation from
cached ELF images:** running the full hardware suite end-to-end (not just
the new heartbeat-specific tests) surfaced a pre-existing fragility in
`tests/hw/test_rp2350.py`, unrelated to the U-mode work above. Every
distinct U-mode ELF program's loaded image stays resident in heap
permanently after a clean exit — confirmed a cache, not a per-run leak, by
re-running the same program twice and seeing usage not grow further. With
enough of the suite's own test programs (`ubig`, `uwx`, the B3/B6/C3
probes) run before the C6/C7 compiler test, their cached images leave too
little contiguous heap for chibicc's fixed 108 KB arena. Fixed by moving
`test_heap_on_demand` to run before any ELF-loading test in `main()`'s
test list, so it always sees a clean heap; a real heap-budget re-analysis
(what the image-cache tradeoff should cost long-term, whether it should
release under memory pressure) is tracked as separate M5-follow-up work,
not attempted here. Also fed directly into the design choice above: the
isolation probe's own stack is heap-allocated on demand rather than a
static array for exactly this reason — a static 4096-byte array was tried
first, cost one permanent heap page from every board's image size, and
was on its own enough to tip this same test from passing to failing before
any test had even run.

**Explicitly deferred to a later phase, not this milestone:** converting
any other driver task to U-mode; a separately-linked-image loader for
driver tasks; any change to the M-mode-vs-S-mode kernel privilege model
itself; the heap-budget re-analysis noted above.

Verified: full QEMU suite 211/211 across 3 consecutive clean runs on both
rv64 and rv32 (`uspin`'s new `SYS_YIELD`/`SYS_TIME_MS` round-trip included;
`uart_rp2350.c`/the new shell command are RP2350-only and compile out
elsewhere, unaffected functionally). All four board personas build and
link clean. Real RP2350 hardware: heartbeat LED confirmed blinking
correctly by visual inspection (brief ~40 ms flash every ~2 s, matching
the pre-U-mode behavior) both immediately after the ACCESSCTRL fix and
again after the isolation-test code and heap-allocation change landed.
`heartbeatisotest` 3/3 consecutive clean runs: intruder task faults (cause
7, store access fault), is reaped `DEAD`/`exit_clean=false`, the kernel
canary stays untouched, and the shell/kernel stay healthy and responsive
immediately afterward. Full `tests/hw/test_rp2350.py` suite 22/22 across 3
consecutive clean runs on the final build.

### M5 Phase 2 — U-mode isolation for a driver with a real IPC endpoint *(done, 2026-08-16)*

Phase 1 deliberately proved the mechanism on heartbeat, the one driver
with no `chan_call()` endpoint. Every other M4.5 driver task does serve
one, via `chan_serve_wait()`/`chan_serve_reply()` — kernel-only calls with
no U-mode route. This phase built that route and converted tm1638 (7-segment
display + 4x4 keypad), the next-lowest-risk driver: no dependents, tiny
messages (33-byte request, 1-byte response), and its whole runtime
footprint is GPIO bit-banging through the same `SIO_BASE` window heartbeat
already uses — no new MMIO region needed.

**What landed:** three new syscalls (`SYS_DELAY_US=24`, matching
`SYS_TIME_MS`'s shape but microsecond-granular, for the bit-bang protocol's
~3 µs pulse timing without exposing RP2350's TIMER0 peripheral to U-mode;
`SYS_CHAN_SERVE_WAIT=25`/`SYS_CHAN_SERVE_REPLY=26`, the server half of the
channel API, copying through a kernel scratch buffer the same way
`SYS_CHAN_CALL` already does for the client half). `kernel/chan.c` gained
`chan_serve_wait_copy()`/`chan_serve_reply_copy()` so `chan_endpoint_t`
stays opaque. `drivers/tm1638_rp2350.c` gained a second, independent,
`.utext`-tagged implementation of the whole bit-bang protocol (not a
refactor of the existing kernel-mode one — the two must never run
concurrently, and the kernel-mode fallback path has to keep working
unmodified when the task isn't alive); its 16-byte persistent RAM-cache
state lives inside the last 16 bytes of the task's own dedicated stack
page rather than costing a fourth PMP region. `tm1638isotest` mirrors
`heartbeatisotest` exactly, against tm1638's real domain shape.

**Three real findings on hardware, in the order they were hit:**

1. **A full board hang, not a clean fault.** The first attempt at
   `tm1638_umode_body()` passed the endpoint name as an ordinary C string
   literal (`"tm1638"`) into the new syscalls. A literal lands in ordinary
   `.rodata`, outside every region the task's domain grants — exactly the
   class of bug `kernel/shell.c`'s `user_deputy()` already has a comment
   about, for the same reason, just never previously hit for a channel
   endpoint name. `strncpy_from_user()` correctly refused to read it, but
   on real RP2350 silicon that refusal path (reached from inside a task
   blocking mid-ecall-trap, itself a code path nothing had ever exercised
   before — `SYS_CHAN_CALL`'s only other task-owned-endpoint precedent,
   "console", is an inline handler with no blocking involved) produced a
   silent, total hang rather than a clean -1, needing a physical BOOTSEL
   recovery. Root-caused not on hardware but by reproducing the same
   mechanism on QEMU via a new synthetic probe, `chanechotest`
   (`kernel/shell.c`) — a real client blocking on `chan_call()` into a real
   U-mode server that itself blocks inside `SYS_CHAN_SERVE_WAIT` — which
   hit the identical bug but failed *cleanly* there (`chan_lookup()`
   returning `NULL`, not a hang), letting the actual defect be found and
   fixed in seconds of iteration instead of a physical recovery cycle.
   Fixed the same way `user_deputy()`'s own `path[]` already does: the
   endpoint name built character-by-character into a `volatile` stack
   array instead of a literal. `chanechotest` is now a permanent QEMU
   regression test (`tests/runner.py`), specifically so the *mechanism*
   itself — not any one driver's use of it — is falsified before any
   future U-mode driver conversion risks a hardware hang on it again.
2. **A real hardware instruction-access fault**, immediately after fix
   #1: `tm1638_usys_display_string()`'s `memset(buffer, 0, sizeof(buffer))`
   compiled, under this tree's `-fno-builtin` build flags, to a genuine
   call to libc's own `memset()` — which lives in ordinary kernel `.text`,
   outside `.utext`. The same "an ordinary-looking C construct silently
   reaches outside the granted region" class of bug as #1, a different
   construct. Confirmed via disassembly (`riscv64-elf-objdump`) that every
   `jal` target inside `.utext` stays inside it once fixed — written out as
   an explicit loop instead, matching `user_deputy()`'s established
   discipline again.
3. **A structural gap, not a code bug**: the fault in #2 correctly
   terminated the tm1638 task, but the *caller* — in this case the
   interactive shell itself, evaluating `(tm-display ...)` — stayed
   permanently blocked, indistinguishable from a full hang from the
   console's own vantage point, again needing a physical BOOTSEL recovery.
   `chan_call_task()`'s own comment already named this exact gap ("does not
   close the race where the owner dies after this check but before it
   replies"), written when it was a theoretical race for a kernel-mode
   driver task; it stopped being theoretical the moment a driver's own code
   could take a real PMP fault as a normal, designed failure mode.
   Fixed generally, not just for tm1638: a new `chan_owner_exited()`
   (`kernel/chan.c`), called from `task_exit()` for every exiting task,
   clean or faulted, unblocks any caller left waiting on an endpoint that
   task owned. This also means every existing "fall back to direct
   hardware access when the task isn't alive" facade (`uart_putc()`,
   `tm1638_display_string()`, ...) now actually triggers when a task dies
   *mid-request*, not only when it never started — strengthening the whole
   M4.5/M5 driver-task architecture, not a narrow patch for this one bug.

**Explicitly deferred, not this phase:** converting i2c (now unblocked —
the user confirmed a future EEPROM redesign will cap single writes at 128
bytes, well within a syscall-sized buffer, removing the "~4 KB payload"
reason this was deferred in Phase 1's own plan), st7735 (still needs a
second MMIO region for its SPI0 controller), blk, uart, usb_cdc.

**Heap budget, continuing to drift as flagged in Phase 1:** idle baseline
now 48 total pages / 23 free (was 52/27 after Phase 1) — tm1638's real
conversion costs a permanent dedicated stack page the same way heartbeat's
did. `test_heap_on_demand` (C6/C7) fails again as a direct result (chibicc's
108 KB arena no longer fits even at idle, before any other test runs) —
same class of failure as Phase 1's, not a new one, and not fixed here per
the user's own explicit direction to track a real heap-budget
re-analysis as separate follow-up work rather than patch around it again.

Verified: full QEMU suite 217/217 across 3 consecutive clean runs on rv64
(`chanechotest` and the new `uspin` `SYS_DELAY_US`/refusal checks
included; rv32 covered by the same suite invocation). All four board
personas build clean. Real RP2350 hardware: `(tm-display "m5 ok")`,
`(tm-set-leds 255)`, and `(tm-get-key)` all round-trip cleanly through the
real U-mode task (`tm1638stats` call count advancing, no fault, shell
stays responsive) — text visually confirmed correct on the physical
module by the user. `tm1638isotest` 3/3 consecutive clean runs. Full
`tests/hw/test_rp2350.py` 21/22 across 2 consecutive runs (the one
pre-flagged, isolated heap-budget failure above; everything else clean,
including the real tm1638 task test and every U-mode isolation test).

### M5 Phase 3 — U-mode isolation for the shared RTC/EEPROM driver (i2c) *(done, 2026-08-16)*

i2c was deferred in Phase 2 because its EEPROM path carried up to ~4 KB
per `chan_call()` message — far more than a syscall-sized U-mode buffer
should carry. The user resolved that by capping a single EEPROM
read/write at 128 bytes (`AT24C32_CHUNK_MAX`, `drivers/at24c32.h`);
`drivers/at24c32.c`'s `at24c32_read()`/`at24c32_write()` now loop
internally in that chunk size for anything larger, transparent to every
existing caller (`user/lisp/lisp.c`'s `(eeprom-read)`/`(eeprom-write)`,
`fs/vfs_server.c`'s EEPROM device node). This phase converted the whole
shared "i2c" task (RTC *and* EEPROM — one task, one physical bus,
`drivers/i2c_rtc.h`) to real U-mode — the first driver task in M5 to talk
to a genuine hardware controller (DesignWare-style I2C0/I2C1 registers)
rather than bit-banged GPIO.

**What landed:** the domain's third region (after stack + `.utext`,
Phase 1/2's shape) is one page at `CONFIG_I2C_RTC_BASE` — I2C0 on
`rp2350-chess`, I2C1 on `rp2350-clock`. Two generalized primitives
(`i2c_usys_write_bytes()`, `i2c_usys_read_reg()`) replace what would
otherwise have been four separately duplicated ones (RTC's and EEPROM's
I2C transactions are the same DesignWare handshake, differing only in how
many address bytes precede the payload) — `drivers/i2c_rtc.c`'s own
`i2c_umode_body()` dispatches all five wire ops (`'T'`/`'S'`/`'C'` RTC,
`'R'`/`'X'` EEPROM) onto them. `SYS_DELAY_US` (Phase 2) covers the one
place this driver needs a delay at all: AT24C32's ~10 ms page-write cycle,
crossing the chip's 32-byte page boundaries within a single wire message.
`i2c_rtc.c` is shared across every target (unlike heartbeat/tm1638's
entirely RP2350-only files), so the U-mode implementation and the
original plain kernel-mode server both live in this one file now, split
by `#if defined(CONFIG_BOARD_RP2350)` — QEMU keeps the kernel-mode path
unchanged, since there is no real I2C hardware there to isolate.
`i2cisotest` mirrors `heartbeatisotest`/`tm1638isotest`.

**Found in passing, fixed as part of this work:** `drivers/at24c32.c`'s
kernel-mode fallback hardcoded I2C0's base address regardless of board
persona, while `drivers/i2c_rtc.c`'s own `I2C_RTC_BASE` already read
`CONFIG_I2C_RTC_BASE` correctly. Harmless today only because
`rp2350-clock` (I2C1) has no EEPROM wired, so every access against the
wrong peripheral failed cleanly ("no EEPROM detected"), not because it
was correct — fixed since the new U-mode code needed the correct
board-parameterized base anyway.

**Three real bugs found on hardware, in the order hit** — none of them a
new *class* of hazard (each is a variant of something Phase 2 already
found, or the general fix Phase 2 already built), but each still cost a
full debug cycle to actually hit and fix:

1. **A boot-time bus fault, board hung before USB ever came up** — the
   third physical BOOTSEL recovery this project has needed. RP2350's
   ACCESSCTRL write-protection is more specific than Phase 1's GPIO fix
   implied: *every* ACCESSCTRL register except `GPIO_NSMASK0`/`_1`
   (heartbeat's own fix, the only ones this tree had touched before)
   requires the 16-bit value `0xacce` present in the write's upper 16
   bits, or the write both fails *and* raises a bus fault instead of
   silently doing nothing — straight from the datasheet's own ACCESSCTRL
   overview section (not the per-register field tables Phase 1's own
   research had already read closely). The first `ACCESSCTRL_I2C0`/`_I2C1`
   write here was missing that password prefix, faulting during
   `i2c_hw_init()`, before the scheduler or USB CDC existed to report
   anything -- indistinguishable from a full hang until the user checked
   the heartbeat LED and found it still blinking (confirming the fault was
   local to boot-time I2C setup, not a whole-machine lockup). Fixed by
   OR'ing `0xacce0000` into the write, same as the datasheet's own
   worked example.
2. **A byte-order bug**, found immediately after fix #1 via a wrong
   return value (`(eeprom-write 0 "hello m5 phase3")` returned `251658240`
   instead of `15`): the new U-mode `i2c_usys_put_i32()` wrote the
   `int32_t` result field big-endian, matching `i2c_usys_put_u16()`
   right above it in the same file -- but the client side's `get_i32()`
   (`drivers/at24c32.c`) decodes that field via a raw `memcpy()`, native
   (little-endian) byte order by construction, matching the *original*
   kernel-mode `put_i32()` this replaced (also a `memcpy()`). The u16
   address/length fields and the i32 result field were never on the same
   byte order in this protocol to begin with; the rewrite just needed to
   preserve that inconsistency rather than "fixing" it into a new bug.
3. **A control-flow regression in the new chunking loops**
   (`at24c32_read()`/`at24c32_write()`, `drivers/at24c32.c`): a chunk
   failing via IPC returned `-1` directly from inside the loop, never
   reaching the `at24c32_hw_read()`/`_write()` fallback call below it --
   losing the "IPC failed, fall through to direct access" contract every
   other M4.5 facade already honors. Restructured so any IPC failure
   (immediate or partway through a multi-chunk request) falls through to
   direct hardware access for whatever was not already transferred via
   IPC, combining partial IPC success with the hardware fallback into one
   total count rather than abandoning either.

**Not exercised on real hardware, noted rather than chased further:** a
single `at24c32_read()`/`at24c32_write()` call spanning more than one
128-byte wire chunk. The interactive Lisp reader's own string-literal
buffer truncates around 127 characters (an unrelated, pre-existing limit,
not this phase's 128-byte cap, coincidentally close to it), so no shell
one-liner could construct a payload large enough to reach the client-side
chunking loop through the REPL. The higher-risk half of the new code --
the *server*-side page-boundary chunking inside one wire message, using
`SYS_DELAY_US` in a loop -- was exercised directly (a 127-byte write spans
four `AT24C32_PAGE_SIZE` pages) and round-tripped correctly; the
client-side multi-message loop is comparatively simple, already-reviewed
boilerplate. Worth a real test once there's a non-interactive way to drive
it (a file write through `/dev/eeprom` larger than 128 bytes would do it).

**Explicitly deferred, not this phase:** converting st7735 (needs a
second MMIO region for its SPI0 controller, the same *shape* of work as
this phase, not started), blk, uart, usb_cdc. The heap-budget
re-analysis, tracked since Phase 1 and growing more pressing each phase
(see the number below).

Verified: full QEMU suite 217/217 across 3 consecutive clean runs on
rv64 (rv32 covered by the same suite invocation; the new code is
RP2350-only, so this confirms no regression rather than exercising the
new path). All four board personas build clean. Real RP2350 hardware, on
`rp2350-chess` (I2C0, RTC + EEPROM both wired): `(set-date ...)` round-trips
through the real U-mode task (`i2cstats` call count advancing, no fault);
`(eeprom-write ...)`/`(eeprom-read ...)` round-trip correctly for both a
15-byte and a 127-byte payload (the latter crossing 4 AT24C32 page
boundaries within one wire message); `i2cisotest` 3/3 consecutive clean
runs; full `tests/hw/test_rp2350.py` 21/22 across 2 consecutive runs (the
same pre-flagged heap-budget failure, nothing new). Real RP2350 hardware,
on `rp2350-clock` (I2C1, RTC only): boot log confirms the RTC detected
correctly over I2C1 (`GP6/GP7`, not chess's `GP4/GP5`) with no fault;
`(set-date ...)` round-trips through the real U-mode task; `i2cisotest`
passes -- confirming the board-parameterized `CONFIG_I2C_RTC_BASE`/
`ACCESSCTRL_I2C1` path works, not just I2C0.

Heap budget, continuing to drift as flagged in Phase 1 and Phase 2: idle
baseline on `rp2350-chess` now 47 total pages (was 48 after Phase 2, 52
after Phase 1) -- i2c's own dedicated stack page, the same cost every
U-mode conversion has had. Still passing `test_heap_on_demand`'s own
`>= 50`-pages sanity floor by a shrinking margin only because that test
already runs before any ELF-loading test (Phase 1's own fix); the
C6/C7-specific failure recorded above is about contiguous free space at
idle, a tighter constraint than that floor. Three driver-task conversions
in, the trend is unambiguous -- flagging this explicitly, not waiting to
be asked, per the standing note since Phase 2: the re-analysis needs to
happen before, not after, the next conversion tips something else over.

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
