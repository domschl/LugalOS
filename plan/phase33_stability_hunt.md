# Phase 33 — Stability hunt: fixing the known bugs before phase 28 resumes

**Status: concluded 2026-09-15.** Phase 28 (ESP32-P4 Ethernet) paused after Z3
so that the intermittents could be dealt with rather than carried into work
that needs a trustworthy suite. This is what that cost and what it bought.

Phase 28 resumes at **Z4 — frames on the wire**.

## What was fixed

Seven defects, each with the evidence that identified it.

### 1. `exec` on the ESP32-P4 — L1 is writeback, `fence.i` is not enough

The ELF loader writes a program through the data path and jumps to it, closing
the gap with `fence rw,rw; fence.i`. On the P4 that is insufficient: internal
memory is reached *through* L1 (`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`) and L1
data is **writeback**, so the loader's stores can still be dirty in the D-cache
when the fetch of those addresses misses I-cache and reads L2/RAM instead.

Identified by two consecutive `exec`s of the *same* binary from fresh boots
faulting **differently** — `cause 2, epc=entry+4, tval=0x269c790b` then
`cause 7, epc=entry+0, tval=0x1000`. A bad image or a wrong entry offset fails
identically; varying garbage at a fixed entry is stale memory.

Fixed by `esp32p4_icache_sync()` (arch/riscv/common/xip_esp32p4.c): write back
first (`Cache_WriteBack_All`), then invalidate the **instruction** cache only.
The order is not symmetric — phase 32 U2 established that invalidating L1 D
discards dirty lines rather than writing them back. ROM addresses taken from
esp-idf's `esp32p4.rom.ld`, whose `Cache_Invalidate_All` matches the constant
this file already used, which is what makes the pair verified and not inferred.

**No `exec` had ever worked on this board.** Now `UPROG_TEXT_OK` /
`UPROG_DATA_OK` print and `tests/hw/test_esp32p4.py` is 17/17.

### 2. The identity block device had no mutual exclusion

`drivers/virtio_blk_id.c` drove one shared request header, status byte and
descriptor chain, and its completion wait **yields** — so a second caller was
not merely possible, it was invited. The device is reached "from anything that
touches the record", several of which run in the same early-boot moment.

A collided read fails the record CRC; `idstore_read()` folds that into
`IDSTORE_CORRUPT`; and callers treat CORRUPT as "no valid record". So
`identity provision` stopped refusing a populated store, and a rename rewrote
the record *without* its uid, key or grants.

Found by making `identity_store_write()` say which of its three failure exits
was taken — the first soak with that diagnostic named it on run 15. Fixed with
a ylock (not a spinlock: the section contains a yield).

Measured: the `I3` flake went **5/144 → 0/100**, P(0 | unchanged) = 2.9%.

### 3. The fatal trap handler faulted recursively

`soak14/run25`: one line of the dump printed, and QEMU then logged
`reserved bits set in PTE` **2,820,123 times** — one address, one entry, a
tight loop. The first of those lines comes *after* the trap line, so the page
table damage is not the cause of the fault, it is what the handler hits while
reporting it. That run took 795 s against a 178 s norm.

`trap.c` already warned about "a second fault inside the handler reporting the
first" and nothing enforced it. Fixed with a per-hart one-shot guard: re-entry
halts without printing, because printing is what is being protected against.

### 4. The scheduler hand-off guard did not guard

`sched_check_incoming()` checks the `ra` that `ctx_switch()` is about to
restore, at both switch sites. It only tested `ra == 0` — the phase 27 E4
failure — so an `ra` pointing anywhere else walked straight through. Its own
comment said a real text-range test "would need a symbol all four linker
scripts define".

All four now define `_ktext_lo`/`_ktext_hi` from the location counter inside
`.text`, and `ASSERT` that `task_trampoline`, `sched_yield` and `task_exit`
fall inside that window — so the guard's premise is linker-checked, not
assumed. The incoming `sp` is now validated against RAM *before* being
dereferenced, because a guard that faults while checking is worse than none.

### 5. `uart_flush_critical()` could deadlock the panic path

Every `printk_critical()` drains the TX batch, and draining took
`g_tx_batch_lock` unconditionally — so a hart that died holding it silenced the
other hart's crash report. The same shape is recorded in `uart_16550.c`, where
the RX-overrun notifier was moved off `printk_critical()` because it "hung
three suite runs"; the panic path cannot be moved. Now a bounded
`spin_trylock_irqsave()` that drains its own per-hart slot regardless.

### 6. The console drain fired from inside a partially-emitted message

`console_puts()` called `klog_drain()` on every invocation, and its callers use
it *mid-message*: `line_editor.c` from thirty sites (a dozen in one prompt
redraw), and `cprintf()`'s format engine for the timestamp's `"["`, padding and
`"] "`. Worse, **`line_editor.c` never took `console_lock` at all** while
klogd's drain does — two writers, one unlocked, never serialised.

The drain moved to `console_sync()`, called at real boundaries, and both redraw
functions now hold `console_lock` across a whole redraw (never across
`console_getc()`). Y5d records that locking here once broke the two-hart boot
deterministically; the verification run was 363/363 including that target.

**This fix has no measured support** — see the "withdrawn" note in
open_issues. Its justification is its mechanism, not a statistic.

### 7. Test-harness defects

* `tests/mqttbroker.py` called `accept()` **once**, so the fixture could serve
  one connection ever, and `keep_listening` was stored and read by nothing.
  Reproduced outside QEMU (`connection 2: NO CONNACK -> TimeoutError`) and
  fixed with an accept loop. The guest's "refused the connection, or is
  unreachable" is printed for a reset *and* for a socket that opens and never
  gets a CONNACK, which is why the logs misled for so long.
* `close()` burned its full 5 s `join` on every broker, because closing a
  socket from another thread wakes neither `accept()` nor `recv()` on Linux.
  ~14 broker instances per run = the 50 s the suite had silently been paying.
  Fixed with 0.25 s socket timeouts: suite 232 s → **178 s**, below the
  182 s it started at.
* QEMU's diagnostics now go to a file (`-D`) and are summarised per session,
  deduplicated with counts, instead of being interleaved into the guest's
  console stream where every expect reads them as guest output.
* `tools/p4run.py --run` returned before its `--cmd` handling, so
  `--run --cmd "..."` reset the board and **silently discarded every command**.
* The X2 check asserted a scheduling outcome nothing guarantees; it now
  retries. Not a missing barrier — each hart increments its own slot, the
  increments are fenced by `g_sched_lock`, and the read happens seconds later.

## What is still open

**One fault, one observation.** A kernel wild jump during `exec`, captured with
instruments exactly once (`soak14/run25`). 0 in 210 runs since puts the 95%
upper bound at **1.4% per run**, but nothing changed in between that could
plausibly cure it — the recursion guard is strictly post-fault. "Fixed" and
"rare" are not distinguishable from one observation, and claiming either would
be a guess. Instruments are armed and cost nothing; routine soaking will catch
it.

`NO RESULT` runs are tracked separately and look host-side (QEMU's
`-nographic` chardev and `EAGAIN`), on the runner's own account. MQTT still
flakes at ~2.5%. Both have their own entries.

## What this cost, and the method lesson worth keeping

Four confident diagnoses in this campaign were **wrong** and had to be
withdrawn: that the wild jump landed in the embedded FAT32 image (it was
nearest-*symbol* arithmetic across a build that had moved); that the console
fix was supported by `I3`'s numbers (`I3` was the identity bug all along, and
has never once failed on the check that fix would affect); that `(a)` and `(b)`
were one event at two lengths (three different sections, only one with a
fault); and that a `mqttd exited` line correlated with failure (the runner only
prints guest output for tests that *fail*, so any guest line can only appear
in a failure).

Each wrong one came from reasoning to a conclusion the available evidence could
not distinguish from its alternatives. Each right one came from adding an
instrument that **named which branch was taken** — the identity diagnostic
found a four-soak mystery on its first run, and the P4 fault was diagnosed on
the first boot after the handler could report an instruction at all.

Prefer the instrument. It is almost always cheaper than the soak it replaces.
