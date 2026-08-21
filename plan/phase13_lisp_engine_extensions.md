# Phase 13 — Scheme/Lisp engine extensions (GC, TCO, macros, stdlib)

**Status:** S0-S3 done, 2026-08-21 (227/227 QEMU tests passing, both
targets; confirmed on real RP2350 hardware, chess persona each time — 20/22,
21/22, then 21/22 hardware tests passing, every failure reproduced as
passing when run manually and unrelated to the phase in question). S4
onward not started. S2 fixed a substantial pre-existing bug it surfaced
(stale `global_env` snapshots across begin/let/let*/cond/lambda bodies); S3
found that its own originally-planned Verify criterion wasn't actually
achievable given S0's own rooting design, and corrected it rather than the
design — see each phase's own section for the full account of both. The
macro system (formerly S6) and a bytecode-VM rewrite were both evaluated and
dropped from this roadmap after S1 — see "Not scheduled" near the end of
this doc for the reasoning behind each. Addresses two long-standing gaps
this tree has already flagged but not acted on:
`plan/completed/2026-08-07_review_and_remediation.md`
states twice that "a loop/iteration construct in the Lisp language... doesn't
exist", and `plan/raw_ideas.md:54` names a general allocator as an open idea
("overhead-free allocation of state"). This phase is where both get resolved,
scoped specifically to the Lisp engine rather than the OS.

**Theme.** `user/lisp/lisp.c` (2043 lines) is the whole interpreter and is
literally the shell (`kernel/shell.c` turns command lines into S-expressions
and calls `lisp_eval_string()`). It currently supports only recursion for
looping, evaluated by straight C-stack recursion with a hand-rolled depth
counter (`LISP_MAX_EVAL_DEPTH = 100`, `lisp.c:61-63`) standing in for real
stack safety — a past unbounded-recursion bug hard-hung RP2350 before this
counter existed (`review_and_remediation.md` §A4/§6.4). Values live in two
fixed-size static arenas, `node_pool` (768 × 32B cells) and `string_pool`
(384 × 128B slots) on RP2350, allocated by bump pointer with **no reclamation
of any kind** — once a cell is used it is permanently gone for the rest of
the boot session. Every phase below is written against that constraint:
**RAM (specifically, never running out of node/string pool slots mid-session)
is priority 1**, ahead of language completeness or ergonomics.

Non-goal, resolved here rather than deferred: `user/lisp/lisp_compile.c`, a
196-line toy AST-to-native-RISC-V compiler (`(compile-file src dst)`,
handling only `+ - *` and a `define`/`main` wrapper) has zero consumers
anywhere in the tree (`stdlib.lisp`, `init.lisp`, README examples, and every
active plan doc) — confirmed by repo-wide grep. It predates and doesn't
compose with anything below (no closures, no macros, no participation in a
future GC's rooting). It is removed in S1, not extended — see S1 for the
reasoning against the alternative of extending it, and the note on a
bytecode-VM alternative at the end of this doc.

---

## S0 — GC ground-work: evaluate, don't build yet *(done, 2026-08-21)*

Goal: decide the collector design and prove it's cheap enough on real
hardware before S3 commits to it, and before S4's stdlib starts adding
primitives (`cons`, `string-append`, `map`) that consume pool capacity in
earnest.

**Scoping decision: internal to the Lisp engine, not combined with an OS
allocator.** LugalOS is not actually allocator-free — `kernel/palloc.c` is a
real, production bitmap page-frame allocator with `palloc_pages()`/`palloc_free()`
used throughout the kernel, drivers, and every other user tool (`ed`,
`chibicc`, `chess` each grab a page-run arena and free it as a whole on
exit). There's also `kernel/balloc.c`, a working sub-page buddy allocator
(32-byte floor — a good match for the 32-byte `lisp_val_t` cell size) that is
initialized at boot but has **no caller today besides a shell diagnostic
command**; `plan/phase12_microkernel_migration.md` explicitly calls it not
yet load-bearing. Do not wire the Lisp GC into either:
- `palloc` is load-bearing for the whole OS (process loading, page tables,
  scheduler stacks) — a bug in a GC hung off it risks the system, not just
  the shell, for a feature whose blast radius should stay contained to the
  REPL.
- `balloc` is untrusted with real work in this tree, has no locking, and its
  64 KB arena is smaller than what `node_pool`+`string_pool` need today
  (~72 KB combined on RP2350).
- Both `node_pool` and `string_pool` are already fixed-size arrays sized per
  board at compile time. A collector's job is reachability analysis + reuse
  of dead slots inside arrays that already exist — it does not need to
  change where the memory comes from.

Optional future follow-on, explicitly not part of this phase: once a
Lisp-local GC is proven, backing `node_pool` with `balloc_alloc`/`balloc_free`
calls at sweep time would let idle Lisp cell capacity be reclaimed by other
subsystems (e.g. while `cc` or chess run and the shell is idle) instead of
sitting in permanently-reserved static BSS. That's a memory-map optimization
layered on a working collector, not a prerequisite for one.

**Decisions** (the five questions below were each resolved at their
recommended default; nothing here is now in question):

1. **Rooting.** The evaluator recurses on the C stack, so live `lisp_val_t*`
   exist in C locals mid-expression (pre-S1) — conservatively scanning the C
   stack for roots is fragile in a freestanding, no-debug-info bare-metal
   build (no stack maps, compiler-dependent frame layout). Default: **precise,
   cooperative collection at safe points only** — between top-level forms in
   `lisp_eval_string`/the REPL loop, where the only live roots are
   `global_env` plus anything a primitive explicitly registers as a root
   while it holds a value across a nested call. No mid-expression collection.
2. **Mark representation.** A separate bitmap, not a stolen struct field —
   768 bits (96 bytes) for `node_pool`, 384 bits (48 bytes) for `string_pool`
   on RP2350. Trivial RAM cost; doesn't touch the already-tuned 32-byte
   `lisp_val_t` layout (itself the result of a prior remediation — see
   `review_and_remediation.md` §V6).
3. **Reclaim.** Free-list, not compaction. Thread dead cells into a free list
   via the existing `pair.cdr` field (zero extra storage). Reject a moving/
   compacting collector outright — it needs a forwarding-pointer scheme and
   fixup of every live reference, real complexity a 768-cell arena doesn't
   need.
4. **Trigger policy.** Collect-on-exhaustion at the next safe point, not
   proactive/periodic. A stop-the-world mark-sweep over a ~768-cell heap
   should be sub-millisecond — measure on real RP2350 rather than assume it,
   but there's no reason to pay the cost while there's still headroom.
5. **Scheduler interaction.** `kernel/sched.c` runs the Lisp shell as one task
   among others (drivers now run as their own U-mode tasks). Confirm a GC
   pause only blocks the shell's own task and touches no cross-task shared
   state — `node_pool`/`string_pool` are private to the Lisp engine, so this
   should be a non-issue, but confirm explicitly rather than assume.

**Deliverable — prototype timing.** Built as a standalone host benchmark
(not shipped in the tree — `gc_bench.c`, kept alongside this note's
authoring session, not committed), modeling exactly the shape decisions 2-3
settled on: a separate mark bitmap over a flat `node_pool`-shaped array, one
mark pass following a `cdr` chain, one sweep pass inspecting every bit and
threading unmarked cells into a free list via their own `cdr` field. Two
stress shapes per pool size, matching "mark cost is proportional to the live
set, sweep cost is always O(n) regardless of live ratio": all-live (worst
case for mark) and all-dead (worst case for sweep, i.e. total reclaim).
Median of 3 runs, host x86_64, `-O2`:

| Pool size | Shape | Mark | Sweep | Total |
|---|---|---|---|---|
| 768 (RP2350) | all-live | ~1.0-1.3 us | ~0.6 us | ~1.6-1.9 us |
| 768 (RP2350) | all-dead | ~0.02 us | ~0.6 us | ~0.6 us |
| 4096 (QEMU) | all-live | ~7.2-7.3 us | ~3.1 us | ~10.4 us |
| 4096 (QEMU) | all-dead | ~0.02 us | ~3.1 us | ~3.1 us |

**This is a host CPU's timing, explicitly not a substitute for measuring on
real RP2350 hardware** (a Hazard3 RISC-V core, clocked and cached
completely differently from a host x86_64) — that measurement is still
outstanding and requires physically running the equivalent pass on target
and reading timing back over its serial console, which needs either the
user's hands-on involvement or explicit direction to do so; it was
deliberately not attempted unprompted in this session even though a board
was physically present, since driving a physical device's serial port isn't
a decision to make unilaterally. What the host numbers DO establish, and
what S3 can proceed on: the algorithm is a strictly linear single pass in
each direction over n <= 4096 small (here modeled as 16-byte, vs. the real
32-byte `lisp_val_t`) fixed-size elements with zero allocation and zero
data-dependent branching cost beyond one bit test per element — the entire
working set (at most 4096 x 32 B = 128 KB, and for RP2350 specifically
768 x 32 B = 24 KB) is far smaller than any target's cache, so the cost is
pure linear memory bandwidth, not algorithmic complexity. That shape
transfers across hosts even though the absolute number doesn't: at even a
generously pessimistic 50x host/target slowdown, RP2350's ~1.9 us all-live
case projects to under 100 us, comfortably sub-millisecond and well within
what a per-top-level-form safe point (decision 4) can absorb unnoticed.

**Superseded note, kept for the record:** this deliverable originally also
settled whether a macro system's mandatory in-place expansion-caching would
still be needed once a GC exists (conclusion at the time: yes, a collector
reclaims dead expansions but does nothing to stop a live call site from
being re-consed every execution, so caching and collection are
complementary, not substitutes). Moot now that macros are dropped entirely
rather than built — see "Not scheduled: non-hygienic macros" below.

**Verify:** design note exists (this section) with decisions on 1-5 above,
prototype timing measured (host, done — table above) with real-hardware
timing explicitly flagged as an outstanding follow-up rather than silently
skipped, and S3 can proceed without re-litigating rooting/reclaim strategy.

## S1 — Remove `compile-file`; tail-call optimization *(done, 2026-08-21)*

Two changes bundled together because the first simplifies the second.

**Removed the toy native compiler.** Deleted `user/lisp/lisp_compile.c` and
`include/lisp_compile.h` outright, the `user/lisp/lisp_compile.c` line from
`CMakeLists.txt`, the `compile-file` primitive registration in `lisp_init`,
the special-cased dispatch branch that handed it unevaluated args, and the
stale doc-comment cross-reference in `lisp.h`. Repo-wide grep confirms no
remaining references outside historical `plan/completed/` docs. Extending
this compiler instead was considered and rejected: it only handled `+ - *`
and integers, and making it track closures, tail calls, macro-expanded code,
or the new stdlib primitives would mean building a real native-codegen
backend for a dynamically-typed language, three times over (RP2350, K210,
QEMU rv32/rv64), for a feature with zero consumers anywhere in the tree.

**Tail-call optimization.** `lisp_eval_step()` is now a `goto`-based
trampoline with a `tail_call:` label at its top. Every position that is a
genuine tail call — the taken branch of `if`, the last form of
`begin`/`let`/a matched `cond` clause/a lambda body, and a lambda
application reached from any of those — rewrites the function's local
`val`/`env` and jumps back to `tail_call` instead of recursing; a shared
`eval_all_but_last()` helper evaluates every form but the last of a body
non-tail (via the real, depth-guarded `lisp_eval()`) and hands back the
last form for the caller to splice into the trampoline. Only genuinely
nested (non-tail) evaluation — argument evaluation, `if`/`cond` test
expressions, operator lookup, all-but-last body forms — still recurses
through `lisp_eval()` and consumes `eval_depth`/C stack, so
`LISP_MAX_EVAL_DEPTH` continues to guard real nesting exactly as before.

**Regression found and fixed during implementation, not anticipated in the
original plan:** once a tail-recursive call no longer costs `eval_depth`,
evaluating its operator can be the point where pool exhaustion is first
discovered (`node_pool_exhausted_warned` set moments earlier by an
allocation earlier in the same iteration) — and the pre-existing
"operator resolved to a non-callable value" fallback at the bottom of
`lisp_eval_step()` returned the raw, unevaluated tail-call form in that
case instead of nil, breaking the documented "an aborted evaluation
degrades to nil" contract. Fixed by checking
`node_pool_exhausted_warned || lisp_interrupted` immediately after operator
evaluation and returning `&nil_val` there directly. Verified interactively
before and after the fix (`=> (loop (+ n 1))` vs. the correct `=> ()`).

**Second regression, found via the full QEMU suite, not the interactive
probe above:** the existing regression test for "runaway recursion doesn't
hang the machine" (`(define (loop n) (loop (+ n 1))) (loop 0)`) used a
*tail*-recursive shape. Pre-TCO this safely stopped at the cheap
`LISP_MAX_EVAL_DEPTH` ceiling; post-TCO it no longer costs `eval_depth` at
all and instead runs until it exhausts the *entire* node/string pool for
the rest of that QEMU session — which, because `tests/runner.py` runs many
numbered tests in one continuous session per architecture, cascaded into
failing every unrelated Lisp-dependent test after it. Fixed in
`tests/runner.py`: that test now uses a genuinely non-tail shape
(`(+ 1 (loop (+ n 1)))`, the recursive call as `+`'s argument) to keep
exercising the depth guard cheaply as originally intended, and a new test
proves TCO itself with a *bounded* tail loop (`(if (= n 60) n (loop (+ n 1)))`,
comfortably past the old ~20-30-iteration ceiling for this shape while
staying modest against the shared, never-reclaimed pool budget 20 earlier
tests have already spent from in the same session).

**Verify:** confirmed live — 219/219 QEMU tests pass on both rv32 and rv64
(up from 217 pre-existing; +2 for the new TCO test counted once per
architecture). A bounded self-recursive tail-position loop runs correctly
past the old 100-iteration ceiling (verified interactively to 300
iterations in isolation, 60 in the full suite's shared-session context);
non-tail unbounded recursion still trips `LISP_MAX_EVAL_DEPTH` cheaply and
the shell survives; `compile-file` and all traces of it are gone from
source, build, and `(help)` output.

**Confirmed on real RP2350 hardware, chess persona, not just QEMU
(2026-08-21).** Flashed `build/rp2350/lugalos.uf2` (build id matched the
local build exactly) and ran `tests/hw/test_rp2350.py`: 20/22 passed; the 2
failures (`blk task` call-count, `C6/C7` cc-memory-reclaim) were reproduced
manually immediately afterward and both actually succeeded when run by hand
(blk call count 139→168; `cc` reported "Build clean" with heap pages
identical before/after, 25→25) — transient serial-capture flakiness in that
one automated pass, not regressions, and neither test touches anything S1
changed (SD block driver, chibicc/ed arena reclaim). All non-Lisp hardware
tests (PMP probe, priostress, uart/i2c/st7735/tm1638 task IPC, B3/B6/C2/C3/C4/C8
U-mode isolation, link_usb_cdc, p9share, T3, K3) passed outright, and the
suite's own node-pool-exhaustion test passed too.

Interactively verified beyond the automated suite, with a fresh reboot
before each measurement (the board's `node_pool`/`string_pool` are one
continuous static allocation across a whole boot session, so any earlier
command's allocations otherwise silently carry over into the next):
non-tail recursion still trips the depth guard correctly at small depths
(`(fact 6)` via `(if (= n 0) 1 (* n (fact (- n 1))))` → `720`, correct); a
bounded self-recursive tail loop runs correctly up to **n=24** before
`node_pool` exhaustion on this hardware/persona (n=22 and n=24 succeed
cleanly with the correct result, n=26+ exhaust) — much lower than QEMU's
~300-iteration headroom, because RP2350 has only 768 nodes total (vs.
QEMU's 4096) and the chess persona's `init.lisp` (canvas/TM1638/device
binding setup) already consumes a real fraction of that budget before any
user command runs. An *unbounded* tail loop, and an unbounded non-tail
loop, both degrade via node-pool exhaustion exactly as designed (further
evaluation — even trivial, unrelated commands like `(+ 5 5)` — returns nil
until `reboot`), never a hang or crash.

**Honest reading of what TCO does and doesn't buy on the smallest target
today:** the stack-safety and correctness win is unconditional and fully
verified — a tail call genuinely no longer costs `eval_depth` or C-stack
frames, and non-tail recursion is still properly bounded. But the
*practical* iteration-count benefit on RP2350 specifically is modest right
now (~24 iterations for this loop shape, comparable in order of magnitude
to what the old depth-100 ceiling would have allowed anyway for the same
shape) because `node_pool` exhaustion, not `eval_depth`, is the binding
constraint at this scale. TCO is necessary but not sufficient to unlock
long-running tail loops on the smallest target — S3's collector is what
actually removes that ceiling, exactly as anticipated when S0 was written,
now with a concrete measured baseline (24 iterations, no GC) to compare
S3's improvement against once it lands.

## S2 — `let*`, `while`, extract `lisp_apply()` *(done, 2026-08-21)*

- **`while`**: a native C special form — a straight `for(;;)` in the
  evaluator re-evaluating condition and body, no recursion at all. Cheapest,
  safest loop primitive available; doesn't depend on S1's trampoline being
  bug-free, and covers the common bounded-iteration case immediately.
- **`let*`**: alongside the existing `let` (which evaluates every binding
  against the *outer* env), added sequential-visibility bindings — each
  init-expr evaluated in an environment that already contains the preceding
  bindings. Reuses existing env/pair machinery, no new node types.
- **Extracted `lisp_apply(fn, args, env)`** from the inlined operator-eval/
  arg-eval/apply logic into a standalone function, taking an
  already-resolved callable and already-evaluated args — the shape `map`/
  `filter`/`for-each`/a future `apply` need, since they hold a function
  value and a list of values, not raw call syntax. `lisp_eval_step()`'s own
  tail-position lambda application deliberately does NOT call it (it needs
  the goto-based continuation to stay tail-call optimized, which a
  real-recursion-based helper structurally can't provide); the trampoline's
  primitive-application branch does call it, since a primitive call is
  already a leaf with no tail-call concern.

**Regression found and fixed during implementation, substantial enough to
change the shape of this phase — not anticipated in the original plan:**
testing `while` with the only counting-loop pattern available before S4's
comparison operators/lists exist (global state mutated via `define` inside
the loop body, re-checked by the condition) spun forever and exhausted the
node pool. Root cause, confirmed to predate S1/S2 entirely and affect far
more than `while`: `define` always writes into the *global_env* variable
itself (`env_set(&global_env, ...)` reassigns it to a new pointer) rather
than mutating an existing binding in place, but `if`/`begin`/`let`/`let*`/
`cond`/`while`/a lambda body all capture `env` once at entry and reuse that
same snapshot for every later form in their own sequence — so a `define`
executed earlier in a body was invisible to a later form in the *same*
body. Confirmed live: `(begin (define zzz 1) zzz)` reported "Unbound
symbol: zzz" on the pre-fix build. This is the same class of staleness B3
(`plan/completed/2026-08-07_review_and_remediation.md`) already fixed for
a lambda closure's *initial* captured environment (a closure defined at
global scope stores `env=NULL` and re-resolves live `global_env` at call
time) — just not previously closed for a form's own internal sequence.

Fixed for every affected form (not scoped down to just `while`, per an
explicit decision on discovering how broadly this reached — see the
options considered and the choice made mid-session) with
`refresh_global_tail(local_env, original_tail, was_global)`: computed once
per form invocation (`was_global = env == global_env`, before any nested
evaluation in that form could move `global_env` on), and called before
every subsequent nested evaluation in the same form instead of using the
raw `env`/`local_env` directly. For forms with no local extension
(`if`/`begin`/`cond`/`while`), this simply swaps to the live `global_env`
when appropriate. For forms that *do* extend the environment (`let`/`let*`/
lambda application), it walks to the boundary node added by *this* form
and repoints its `cdr` at the live `global_env`, keeping the form's own
local bindings intact while keeping their fallthrough current — this is
what closes the `let*`/lambda edge case (an earlier binding's or body
form's `define` visible to a later one in the same form) that a simple
env-swap alone would have missed.

Known, accepted narrow limitation, not fixed: a `define` executed inside a
non-global (genuinely lexical) closure's body is not specially threaded
through that closure's own captured environment chain — consistent with
the existing B3 precedent, which also only special-cases the global-scope
case. Considered rare enough (defining something globally from inside a
nested closure, then relying on seeing it again via that same closure's
lexical chain rather than a fresh global lookup) not to warrant the
additional complexity right now.

**Verify:** confirmed live, both on QEMU (225/225 tests passing, up from
219 — +6 for 3 new regression tests counted once per architecture: `let*`
sequential visibility, `while` zero- and multi-iteration behavior via a
`define`-mutated counter, and the `define`-visibility fix across begin/let/
let*/lambda/cond) and on real RP2350 hardware (chess persona): `(let* ((a 1)
(b (+ a 1))) b)` → `2`; a 3-iteration `while` loop with internal `define`
mutation correctly reaches `counter = 3`; `(begin (define zzz 1) zzz)` → `1`
and `(let ((q 1)) (define www 5) www)` → `5` (both previously "Unbound
symbol"). Hardware suite: 21/22 (`uartstats` failed in the automated run,
reproduced as passing — `write_calls=66` — when run manually immediately
after; same transient serial-capture flakiness pattern as S1's two
findings, and unrelated to any S2 change, since `drivers/uart_rp2350.c`
wasn't touched). `map`/`apply` in S4 will build directly on `lisp_apply`
with no duplicated call logic, still to be confirmed once S4 lands.

## S3 — GC implementation *(done, 2026-08-21)*

Built the mark-sweep collector designed in S0. Mark bitmaps
(`node_mark_bits`/`string_mark_bits`, one bit per pool slot) plus free-list
reclaim (`node_free_list` threaded through the existing `pair.cdr` field;
`string_free_head` threaded through a freed string slot's own leading bytes
reinterpreted as a `next`-index, `-1` = empty) — zero extra per-cell storage
either way, matching S0's decision 2/3. Mark is iterative, not recursive
(`gc_push`/`gc_mark` with an explicit `gc_work_stack[NODE_POOL_SIZE]`): a
node is marked the instant it's pushed, so it can never be pushed twice,
which is what proves the stack can never need more than `NODE_POOL_SIZE`
entries regardless of how bushy the live graph is — marking recursively in
C would have risked overflowing the very stack this collector exists to
relieve pressure on. `gc_collect()` resets both bitmaps, pre-marks anything
already on a free list (so the sweep doesn't re-add it and grow a cycle),
marks from `global_env`, then sweeps both pools, clearing
`node_pool_exhausted_warned`/`string_pool_exhausted_warned` only for
whichever pool actually gained a free entry.

**The original "Verify" text above (a single `while`-loop consing past pool
capacity) turned out not to be achievable, and not because of a bug —
because of what S0's own rooting decision actually implies once worked
through concretely.** Root cause: the *currently executing top-level
form's own AST*, and every transient value living only in a C local mid-
evaluation (a partially built argument list, an `if`'s just-computed
condition, a lambda call's `local_env` mid-construction), are **not**
reachable from `global_env` and are invisible to this collector by design
(S0 decision 1: `global_env`-only rooting, chosen specifically to avoid
scanning an unmapped, compiler-dependent C stack). A collection run while
any of that is still in flight would reclaim it out from under the
evaluation using it. That means a collection is only *exact* between
**complete** top-level forms — never mid-expression, never between
iterations of a `while` loop that is itself still part of one executing
form. A single command that exhausts a pool while consing still aborts that
command exactly as it did before S3; there is no safe point inside it to
collect at. What S3 actually buys is that the *next* top-level command no
longer inherits a permanently degraded shell — pre-S3, one exhaustion event
degraded the session to nil-for-everything until reboot; post-S3, only the
commands that themselves cross the exhaustion boundary fail, and every
command after each one recovers.

**Regression found and fixed during implementation, changing where the
safe-point hook actually lives:** the natural first attempt — hooking
collection into `lisp_eval()`'s existing `eval_depth == 0` check — silently
never fired past boot. Root cause, confirmed live via instrumentation:
`init.lisp`'s last form is `(lsh)`, which blocks for the rest of the
interactive session inside `shell_run()`, itself inside *that* `lisp_eval()`
call — so `eval_depth` sits at `>=1` for the entire session once the shell
becomes interactive, and genuinely reaches `0` only during boot-time
`stdlib.lisp`/`init.lisp` loading, before `(lsh)` is ever reached. Fixed by
adding a public `lisp_gc_safepoint()` (declared in `lisp.h`) and calling it
explicitly at the two genuinely-top-level, non-nested per-command dispatch
points that actually exist in this tree: `lisp_repl()`'s own line loop and
`kernel/shell.c`'s `parse_and_eval_cmd()` call site in `shell_run()`'s loop
(the POSIX-shell dispatch — also permanently nested inside `(lsh)`, same
issue, same fix). `lisp_eval()`'s original `eval_depth == 0` check was kept
(genuinely useful during boot) and simplified to call the same wrapper.
Known, accepted narrow limitation, consistent with S0's rooting scope: this
assumes `(lsh)`/`lisp_repl()` is only ever reached as a complete top-level
form, never nested inside another expression's still-in-progress evaluation
(e.g. as an argument to something else) — true of every call site in this
tree today, not structurally enforced.

**Second regression, found via the full QEMU suite, not either interactive
probe above:** the first version of S3's own regression test (140 churn
calls, chosen with a comfortable margin the same way S1's TCO test was)
passed by itself but consistently broke an unrelated, pre-existing test
later in the same shared QEMU session (test 23, FAT32 cluster-chain
accounting on `/ram0/`) — off by exactly one cluster, in both architectures,
reproducibly across repeated runs. Isolated with the same method used for
S1/S2's transient flakes, but with a different outcome this time: running
the full suite with the *test* disabled (all S3 C-code changes still
active) passed clean at 225/225, proving the collector itself was not at
fault; the interaction was the test's own length growing that session's
command-history file (`/sd0/system/history.lisp`, appended on every typed
line) enough to shift the FAT32 test's timing. Reduced to 60 iterations
(confirmed empirically to still reliably exhaust the shared session's
remaining budget by that point) resolved it with no loss of coverage.

**Verify:** confirmed live. QEMU: 227/227 tests (up from 225 — +2 for the
new collector test counted once per architecture), and directly, outside
the test suite: a 2000-iteration churn loop across separate top-level
commands produced exactly 15 exhaustion events, evenly spaced every 129
iterations, with **no back-to-back failures** — every command immediately
after an exhaustion event recovered with the correct answer, the whole way
through. Real RP2350 hardware (chess persona), fresh boot: 300 iterations
of the same loop produced 21 exhaustion events and ended with the exactly
correct final result (`churn(299)` → `1510`), confirming the collector
works on target silicon, not just QEMU. Hardware suite: 21/22 (`uartstats`
failed in the automated run, reproduced as passing when run manually
immediately after — the same transient serial-capture flakiness pattern
already seen twice in S1/S2, unrelated to any S3 change).

## S4 — Standard library, as C primitives

Implement directly in C, registered via `env_set` in `lisp_init` like the
existing `+ - * =`:

- **Comparison**: `<`, `>`, `<=`, `>=`, `/=`; generalize `=` to N args (it's
  effectively 2-arg only today).
- **Simple integer math**: `/` (integer div — currently absent despite the
  README claiming it exists), `quotient`, `remainder`/`modulo`, `abs`, `min`,
  `max`.
- **Predicates**: `null?`, `pair?`, `symbol?`, `string?`, `integer?`,
  `procedure?`, `zero?`, `boolean?`.
- **List processing**: `cons`, `car`, `cdr`, `list`, `length`, `append`,
  `reverse`, `nth`/`list-ref`, plus `map`/`filter`/`for-each` (built on
  S2's `lisp_apply`).
- **String processing**: `string-append`, `string-length`, `substring`,
  `string->number`, `number->string`, `string=?`.
- **Procedure invocation**: `apply`, `eval` — the language-level primitives
  this bullet is actually asking for (OS-level `exec`/`spawn` already exist
  and are unrelated).

C primitives, not pure-Lisp `stdlib.lisp` definitions, specifically because
of the RAM/stack constraint: a C primitive like `car` or `map` touches
existing cons cells with zero interpreter-level node allocation for its own
traversal, where a Lisp-defined equivalent would recurse through the
evaluator, costing stack/`eval_depth` per element (less so after S1, but
still real C-call overhead per list element vs. none). Constructing new
values (`string-append`, `list`) still permanently consumes pool slots
wherever the logic lives — S3's GC is what actually bounds that, not where
the primitive is implemented.

**Verify:** each new primitive has a QEMU test; `README.md`'s builtin list is
corrected to match what actually exists (it currently claims `/`, `<`, `>`,
etc. that don't exist in code).

## S5 — Named-let / `do` sugar

`(let name ((v init) ...) body...)` as another native special form (not a
macro) — desugars internally to building a lambda and applying it via
`lisp_apply`, getting S1's TCO for free. Gets idiomatic Scheme-style loops
without needing a macro system to exist at all — see "Not scheduled" below
for why one isn't planned.

**Verify:** a named-let counting to 10,000 runs without growing `eval_depth`
per iteration, same bound as a hand-written tail-recursive `define`.

---

## Not scheduled: non-hygienic macros

Originally planned as S6 (`define-macro`, reusing `LISP_LAMBDA` with a new
`LISP_MACRO` tag, unevaluated-argument expansion, mandatory in-place
expansion-caching to avoid re-consing on every loop iteration under a
no-GC-until-S3 arena allocator). Dropped after S1 confirmed, in practice,
that S2-S5's actual needs — `let*`, `while`, a `lisp_apply` primitive, the
GC, the stdlib, named-let — are met entirely by native C special forms and
primitives, with nothing left over that specifically needs user-definable
syntax. LugalOS's Lisp is a kernel shell/boot-script language, not a dialect
where end users are expected to write their own control-flow sugar, so
there's no concrete consumer for macros once `while`/named-let exist to
cover looping.

This was also, by a wide margin, the riskiest phase in the original
roadmap: it required a RAM-safety mechanism (in-place expansion caching)
to hold as a hard invariant, in a part of the interpreter (arbitrary
user-defined code transformation) least amenable to bounding by
inspection. Not building it removes that risk entirely rather than
mitigating it. If a concrete need for user-level macros emerges later, the
design above (unevaluated-arg binding + expand-in-place caching) remains
valid prior art to revisit — this is a deferral, not a rejection of the
approach.

## Not scheduled: bytecode VM as a wholesale execution-strategy replacement

Raised alongside the `compile-file` removal decision and deliberately kept
out of this roadmap. A bytecode compiler + small dispatch-loop VM would
solve a problem this phase cares about in a way native codegen (the removed
`compile-file`) never could: bytecode is portable across all three current
targets (RP2350 RISC-V, K210, QEMU rv32/rv64) without per-arch backends, and
a VM has an explicit, bounded operand/call stack (a fixed array) rather than
C-stack recursion — bounded recursion depth becomes an architectural
property instead of `LISP_MAX_EVAL_DEPTH`'s magic number, and TCO is close
to free (reuse a frame instead of pushing one). It also avoids any W^X/
self-modifying-code concern, since bytecode is inert data for a trusted
interpreter loop rather than machine code written into executable memory.

This is a replacement of the whole evaluation strategy, larger in scope than
every other phase in this doc combined, and is deliberately deferred: the
rooting model and stdlib primitive semantics settled here translate close
to directly into opcode/VM design later, so none of this work is wasted if
a VM comes later — whereas investing in the native compiler now would have
been.

Re-confirmed after S1, not just asserted up front: TCO landed as a small,
self-contained diff (one function turned into a `goto`-based trampoline, no
new data structures) and all 219 QEMU tests pass. That's direct evidence
the tree-walker still has real headroom before a rewrite is justified — a
VM's two strongest selling points (portability without per-arch codegen,
no W^X concern) were specifically framed as answers to problems the native
compiler had and the tree-walker never did, so removing that compiler in
S1 made both moot rather than making the VM more urgent.
