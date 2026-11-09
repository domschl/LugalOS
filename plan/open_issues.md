# Open issues

Things that are known and deliberately not fixed yet, kept visible so that
whoever meets one next recognises it as known rather than spending an
afternoon rediscovering it. Planned work lives in the phase documents; an
entry here either names the phase that will take it, or says what evidence
is still missing.

**Sorted 2026-10-02 (phase 38, 38.0b)** into four parts, because the file had
grown two different kinds of entry under one heading:

* **[A. Actionable](#a-actionable)** -- the cause is known or the trigger is
  deterministic, and the fix has a shape. Each entry names its
  **destination**: a milestone of phase 38, an item of
  [`plan/phase40_backlog.md`](phase40_backlog.md), or the owner's bench.
* **[B. Unexplained intermittents](#b-unexplained-intermittents)** -- seen,
  not reproduced on demand, no proven cause. Each entry says **what evidence
  would settle it**. These are not scheduled: they are worked on when they
  come back, and the job until then is to make sure that next time they leave
  evidence. Since 40.1 the runner does that for a stuck QEMU guest: a
  `[Stuck Guest]` report with every hart's pc/ra/sp (from the QEMU monitor,
  named against the build) and, if the guest still reads its console,
  `/proc/ps` and the tail of `/proc/kmsg`.
* **[C. Deliberate limits that look like faults](#c-deliberate-limits-that-look-like-faults)**
  -- by design, listed only because each has cost an investigation once.
* **[D. Lessons kept from closed entries](#d-lessons-kept-from-closed-entries)**.

**The rules.** A fixed entry is deleted; the phase doc and the commit carry
the history. A B entry that recurs and yields a cause moves to A. A B entry
that stays silent for a long time is not thereby fixed -- say so only with a
number (the `exec` entry below shows how). Entries deleted in the 2026-10-02
sort -- the FIXED/CLOSED ones, and the WLAN credential flake retired by
36.3a's completion sentinel -- are in git history, and the 2026-09-14/15
stability campaign's are written up in
[`plan/phase33_stability_hunt.md`](phase33_stability_hunt.md). Comments in
the code that cite "plan/open_issues.md" for a fixed bug refer to that
history.

---

# A. Actionable

## The RP2350's SPI SD driver takes every card for 1 GiB (found 2026-11-09)

**Destination: phase 40 backlog, or the next RP2350 storage work.** `drivers/spisd_rp2350.c:951`
sets `num_blocks = 2097152` and never reads the card's CSD. `format /sd0` on the chess board's
8 GB card therefore made a 1 GiB volume, and on a larger card formatted on a PC every cluster
past the first GiB is refused (`lba + count > dev->num_blocks`): files there read as errors.
**Fix shape:** CMD9 at init, C_SIZE from CSD v2 (SDHC/SDXC: `(C_SIZE + 1) * 1024` blocks), v1 for
SDSC; `format` then sizes the card right.

## A kernel log line during a shared-wire 9P session breaks its framing (found 2026-11-09)

**Destination: the next 9P/console work.** With `p9share`, 9P and the console share one UART.
A `printk` from a 9P request's own work (76.1 found it with `mkdir`'s "Subdirectory created",
since removed) is written by klogd between a reply's bytes: the host sees a SLIP frame with log
text inside it, and p9lib reports "connection closed while reading a SLIP frame". Seen on the
VisionFive 2 (its log goes to UART0); the RP2350's 9P has its own CDC port and is immune. **This
is likely the cause of the LCD-7B entry below** ("9P over its console stops answering").
**Fix shape:** while a shared-wire peer is attached, hold kernel log output back (klogd keeps it)
or send it as SLIP-framed console text between frames, never inside one.

## The VisionFive 2 sees an older /sd0 right after U-Boot `fatwrite` (found 2026-11-09)

**Destination: the VisionFive 2 SDMMC driver.** Booted by `test_visionfive2.py` (YMODEM, then
U-Boot `fatwrite` of `/sd0/lugalos.bin`), LugalOS's `ls /sd0` does not list `lugalos.bin` and
`fsck /sd0` reports ~190 lost clusters (the file's 6 MB); the count varied run to run (105, 188,
190, 192, 0). Booted with `vf2run.py --method sd` from the same card, the file is found and
`fsck` is clean, so the card is consistent. **Suspect:** a read of a sector U-Boot just wrote
served from a stale cache line or buffer (DMA without invalidation), or U-Boot's own write-back
order. **To settle:** after a `fatwrite` boot, read the root directory's sectors twice with an
explicit invalidate between, and compare.

## Two bus locks became ylocks -- the gateway half not yet run

**Destination: the owner's bench, the ENC28J60 gateway** (phase 40's item
2). The Pico 2 W half passed on 2026-10-04: `wifi probe` under live
traffic, rejoins, 120 pings, no `[Lock BUG]`. Code done 2026-10-03.

`drivers/cyw43_rp2350.c`'s `g_bus_busy` and `drivers/enc28j60_rp2350.c`'s
`g_busy` were a flag and a yield loop: no owner, so no edge in phase 31's
wait-for graph, and not re-entrant. It cost a BOOTSEL once: a gratuitous ARP
under `net_set_address()` re-took the bus lock a frame below its holder and
deadlocked `wifi probe`. Both are `ylock_t` now, and `rp2350-wifi`,
`rp2350-clock` and `rp2350-gateway` build clean with them.

**To close:** on a Pico 2 W, `wifi probe`, `wifi join` and a few minutes of
traffic; on the gateway with the ENC28J60, `net txtest`/`net rxtest` and the
gateway suite (`tests/hw/test_gateway.py`). Any `[Lock BUG]` on the console is
a caller taking the bus under a spinlock, which the flag never reported.

## The LCD-7's PSRAM reference rate is stale

**Destination: the owner's bench.** tests/hw's 38.2 "read cached cold"
measures 21.1-23.8 MB/s on the RP2350-LCD-7 against a 26.1 reference, and
fails its 15 % band about one run in three -- on phase 40's first firmware
(`b0f6c01`) as on its last. Likely code layout (the effect part C records for
the chess search), not a regression. **To close:** a handful of runs on one
firmware, then re-baseline the reference, or find the layout cause.

## The clock board's DS3231 does not survive a power cut

**Destination: the owner's bench** -- fit a working CR2032.

Established by experiment on 2026-09-02 (OSF cleared, confirmed clear, power
removed, OSF set again), not by reading the sticky flag. The software already
handles it: the kernel clock is not seeded from a chip whose OSF is set, the
face falls back to NTP/DCF time and lights the PM lamp until the first write
clears OSF -- verified on hardware the same day. Whether the cell is flat,
missing or not contacting, only a meter says.

## Something writes the 1-bpp screen's top rows while a reader has focus

**Destination: phase 71, milestone 71.1** (found 2026-11-05 by `uxbench`'s
new screen check). With a reader window focused, on the LCD-7B and the Tab5
alike, the screen check (canvas request `'E'`: band hashes of the 1-bpp
screen, then `draw_all()`, then the hashes again) reports rows 0..15
differing from a full repaint, and rows 16..31 (the menu bar's rule and the
title bars' top) **changing between two reads inside the check itself**,
which holds console_lock. So some context writes the screen's 1-bpp frame
without console_lock while a reader is focused. Not with the launcher or a
terminal focused. Until 71.1 the reader's full repaint after every update
painted over it. The glass check (frame against a full conversion of the
same 1-bpp state) passes, and the owner sees no fault on the 7B when
scrolling. **Next step:** find the writer (a write watch on the 1-bpp frame's
rows 16..31, or `racescan --why` for the reader's draw paths).

## Touch thresholds are not in millimetres; GT911 taps land 250 ms late

**Destination: phase 71, milestone 71.2.** drivers/touch_gt911.c: `TAP_PX` is
48 or 24 px by font size, and a swipe must cross 1/8 of the screen (160 px on
the Tab5 in landscape, 90 px in portrait), so the 7B (170 DPI, scale 1) gets
the RP2350's targets. A GT911 release is detected 250 ms after its last
report, so every tap on the 7B is at least that late. Gestures are classified
only at release, so nothing can show a pressed state.

## The Tab5 ribbon converts a forced full frame per animation step

**Destination: phase 71, milestone 71.1.** Measured by `uxbench` on
2026-11-05 (plan/phase71's 71.0 results): a focus move is 7 frames, 6 of them
forced full frames of ~126 ms (`term_sync_post()` sets `g_t_force`). That is
~1 s per move, 0.86 s per reader line scroll and 1.4 s per page. The 1-bpp
conversion (~60 ms) and the reader/launcher composite (~57-70 ms) write the
same pixels one after the other. Portrait is only 7-15 % faster, so the RGB565
frame's PSRAM traffic dominates, not the rotation.

---

# B. Unexplained intermittents

Each: what was seen, how often, what is already armed to catch it, and **what
evidence would settle it**. "Phase 40, item 1" (40.1) is the runner change
that makes the next occurrence leave that evidence: a `[Stuck Guest]` block in
the run log, and an `[Attempt]` line per architecture attempt with its
duration and first failure.

## rv32 crashed in its own trap vector during the B6 confinement test (once)

**Seen:** 2026-11-01, `tests/runner.py` (534/534 after a retry): the first rv32 attempt stopped at
`Loaded User Program Is Confined By Its Domain (B6)` (`exec uisolate.elf`, a U-mode store to
0x40000000 that must fault and kill the task). The `[Stuck Guest]` report: hart 0 in M-mode at
`pc=0x800000b0 <trap_vector_entry+0x0>`, `mcause=2` (illegal instruction), `mepc=0x800000b2`,
`sp=0x80000080`. That is a trap frame (144 bytes) sitting over the trap vector itself
(0x80000080-0x8000010f covers 0x800000b0): the frame's saves had overwritten the vector's first
instructions, so every trap re-trapped on the garbage. The guest printed no fault marker, so the runner
took it for a host stall. The second attempt passed; `uisolate.elf` alone passed 300 times in a row.
**Reading:** a trap path that kept trapping while the stack descended (each kernel-mode re-entry
subtracts 144 from sp) until it reached the start of the image. The candidate checked and ruled out: a
U-mode trap with the hart record's `kernel_sp` at zero (only `SETUP_HART_POINTER` at boot writes zero).
**Armed since:** the runner names a hart parked on `trap_vector_entry+0x0` as a guest crash, and ends a
session after three silent commands instead of letting every remaining test time out (the first attempt
here took 66 minutes).
**What would settle it:** the stack pointer at the first nested trap, not the last. A QEMU `-d int`
trace of the rv32 target (or a `mscratch`-independent guard in the vector that halts on an sp below the
kernel stacks) during a full rv32 attempt that reproduces it.

## rv64-smp switched into a task whose parked sp was 0, during `exec uisolate.elf` (once)

**Seen:** 2026-11-07, `tests/runner.py` (747/763, during 71.4's focus/suspend events): the SMP target's
first isolation check (`exec /flash0/system/bin/uisolate.elf`, X2) printed
`[Sched BUG] switching 1 'idle' -> 5 'p9srv': parked sp=0` and halted; every later SMP check failed with
`[Stuck Guest]`. The report: both harts in S-mode on `trap_vector_entry`, `scause=2`, hart 1 with
`sp=0x0`. The same sequence (boot, `smptest`, `lockselftest`, `uisolate.elf`, `smpload start`,
`uisolate.elf` again) then passed 21 times in a row on the same build.
**Reading:** the same test as the rv32 trap-vector crash above, and again a stack pointer that should
not be there; both may be one fault in the U-mode kill path. `sched_check_incoming()` caught this one
before the switch, so the table was intact when it halted.
**What would settle it:** the `[Sched Table]` dump of the next occurrence (the runner's log keeps it),
and whether p9srv's slot had just been reused by a task that ended -- `uprog` is killed in this test.

## The runner sat 22 minutes on its first rv64 guest (once)

**Seen:** 2026-10-29, `tests/runner.py` after 52.11b: the host checks passed, `[Target: RV64 Sv39 MMU Virtual
Memory]` printed, and nothing followed for 22 minutes with one `qemu-system-riscv64` running. No
`[Stuck Guest]` report came, so the runner was waiting on something other than a silent console. Stopped by
hand. The same test alone passed, and two full runs (one before, one after) passed 527/527 and 529/529.
**Ruled out:** the guest (the target passes alone, 8 of 8 in a loop with a 10-minute traceback timer, and
in 5 full runs since); the host-side patterns (the multi-`.*` ones are literal-anchored and cheap, and
every wait in `QemuSession` has a deadline). Not captured, because it was stopped by hand first: whether the
runner was spinning or sleeping.
**What would settle it:** `tests/runner.py` now prints the stack of every thread to stderr if it makes no
interaction with a guest for 7 minutes, and again every 7 minutes (`WATCHDOG_S`). On the next occurrence,
look there before stopping it, and note the runner's CPU use (`ps -o pcpu`): a spin points at a regex, a
sleep at a wait without a deadline.

## A kernel wild jump into `.rodata` during `exec` (QEMU, once)

**Seen:** once with a cause code, `soak14/run25` (2026-09-14), RV64 SMP:
illegal instruction at an `epc` inside `.rodata` (the word was ASCII
`_app`, plausibly the string literal `"ring_append"`), in kernel mode, while
`exec`-ing `uisolate.elf` -- the program that takes a domain fault on
purpose. Three earlier soak hangs during `exec` fit the same shape without a
dump.

**Bound:** 0 in 210 instrumented runs since (`soak15`, `soak16`), so the 95 %
upper bound is 1.4 % per run. Nothing that could cure it changed in between;
"fixed" and "rare" cannot be told apart.

**Armed:** `[Trap Where]` classifies `epc`/`ra` against the running build's
text; a per-hart guard stops the fatal handler faulting recursively;
`sched_check_incoming()` checks the parked `ra` against `_ktext_lo/_hi` and
`sp` against RAM; QEMU's own diagnostics go to a file (`-D`).

**What would settle it:** one more capture. First suspect is the kernel's
handling of a U-mode fault (a corrupted return address, or a function pointer
built from data). A capture with `ra` outside text names the corrupt
hand-off; one with `ra` inside text names the caller.

## The RV32 guest hung after `exec` of a freshly compiled binary (once)

**Seen:** 2026-09-13 (phase 32 U5): `cc /sd0/hello.c /sd0/hello.elf`
succeeded, `exec /sd0/hello.elf` never returned, QEMU at 99.7 % CPU for ten
minutes -- a loop, not a trap. Not reproduced; next run 363/363. Checked not
phase 32 (byte-identical `.text.entry`).

**What would settle it:** a capture of where the guest spins (phase 40,
item 1's scheduler dump, or `info registers` from the QEMU monitor). Candidates:
the loader reading a file whose last block had not reached the device, or the
U-mode entry spinning on something that never arrives. Possibly the same
defect as the next entry; kept apart until a capture says so.

## The RV32 guest stopped in the Lisp `spawn` test (once)

**Seen:** 2026-09-30, one run in four that day: the guest stopped right after
`(spawn "/flash0/system/bin/uhello.elf")` created its task; 376/376 on every
other run. The harness half (the runner then blocked for 20 minutes) is
fixed by 40.1: an undrained console write gives up after 15 s with a report.

**What would settle it:** the same capture as above. Single-hart RV32 and a
just-created U-mode task, like the entry before it.

## rv64-smp went silent after `usertest 1`, under heavy host load (once)

**Seen:** 2026-10-01 (37.3b), in a full suite overlapping other QEMU sessions
and a board run: nothing after `usertest 1` (a U-mode task syscalling from
hart 1), every later test on that target timed out. Fifteen SMP-only runs and
the next idle-host suite passed.

**What would settle it:** the stuck guest's `[Sched Table]` and per-hart
`pc` (phase 40, item 1). A hang that needs load to show is the kind that comes
back.

## `NO RESULT` runs: a host-side stall, or a silent guest

**Seen:** four occurrences in ~250 soak runs (2026-09), in three different
sections, none with any guest fault marker. `soak15/run34` named itself a
"possible QEMU host-stdio stall" (QEMU's `-nographic` chardev sets
`O_NONBLOCK` and may drop a write on `EAGAIN`) and then spent the whole soak
budget in its retries.

**What would settle it:** per-attempt durations and the test each attempt
stalled in (phase 40, item 1). A stall pinned to one test is a guest bug; a
stall that moves with host load is the chardev.

## The console splice, and an input that was never echoed

**Seen:** before 2026-09-14, roughly one suite run in five failed a different
single test each time with truncated output; one capture showed a klog record
spliced into the middle of `LOCK_SELFTEST_OK`, another an `identity
provision` that was never echoed. **Fixed by mechanism** (2026-09-14): the
drain left `console_puts()` for a named `console_sync()` at real message
boundaries, and `kernel/line_editor.c` now takes `console_lock` across a
redraw. **No measured support** for the fix -- the `I3` numbers once credited
to it belonged to the identity-store bug.

**What would settle it:** a failing run's raw console bytes against
`/proc/kmsg`, which is authoritative: right ring order with wrong console
order is a sync-point gap; wrong ring order is something else. The never-echoed
input points at the input path or a shell that was not running, which nothing
has looked at yet.

## MQTT tests flake: the residual after the broker fix

**Seen:** 9 failures in 104 soak runs before 2026-09-14's fixture fix
(`tests/mqttbroker.py` accepted exactly one connection, ever), 1 in 40 after
-- P = 12.6 % that the rate did not change, so the soak alone proves nothing;
the mechanism was proven outside QEMU.

**Armed:** a failure now reports `accepting.connections` /
`refusing.connections`. **What would settle it:** the next failure's counts.
Non-zero means the broker accepted and the fault is above the socket; zero
means nothing arrived.

**2026-10-12: one cause found and fixed** (net/tcp.c, `tcp_emit()`). A
subscribe-test failure whose client counted the malformed packet yet still read
`CONNECTED`: the status print read the state first and the counters after, and
the error landed in between, ~20 s late. The bytes were late because a window
update was lost. `tcp_emit()` read the advertised window and *then* cleared
`need_ack`, so a stream read on another task in between had its update
request erased while the segment carried the stale, closed window, and the peer
waited on its persist timer. Now cleared before the window is read. Not
reproduced on demand (40 runs, 4 in parallel), so the fix rests on the code and
the 20 s signature. Whether this entry's older flakes were the same race is
open: if they stop, delete the entry.

## `lockselftest`'s log-burst case flakes on rv64-smp (~1 in 8, lately more)

**Seen:** 2026-10-01 (37.1), once in a full suite and once in eight
standalone `lockselftest` runs on `-smp 2`; never on one hart. Waiting 2 s
instead of 200 ms for klogd (2026-09-30) did not remove it. During phase 38
(2026-10-02) it failed 3 of about 12 full suites, 2 of the last 4, each
time passing on the rerun; no phase-38 change touches `kernel/lock.c` or
klog.

**What would settle it:** print `klog_gaps() - gaps_before` and
`klog_gap_bytes()` on failure. The check needs exactly one new gap; a burst
counted as two, or a gap counted before `gaps_before` was read, both fail it
and need different fixes. At ~1 in 8 this is the cheapest entry in part B to
settle -- a small step for whichever phase next touches `kernel/lock.c`.

## The console ran away when written to while the clock app handed it back (once)

**Seen:** 2026-09-01 on `rp2350-clock`: Ctrl-C, then a command typed
immediately; the shell banner, then the session evaluating fragments of its
own output (`Unbound symbol: lsh:`), with "command line too long, ignored"
first and "Node pool exhausted" alongside. Network, radio and 9P kept
running.

**What would settle it:** the gesture repeated on purpose with the console's
echo path instrumented, telling a write racing the hand-back apart from
buffered clock-app output re-read as input. Likely in
`kernel/line_editor.c`'s handling of input while a foreground application
releases the console.

## The clock board stopped answering after ~2 h of logging (once)

**Seen:** 2026-09-02, `rp2350-clock`: broadcasts and ICMP stopped together,
permanently, after 2.5 h up. `/proc/kmsg` was lost to the power cycle.
**Mitigated:** the WLAN supervisor now forces a rejoin when the receive
counter has not moved for five minutes, instead of trusting carrier -- which
an AP that silently vanishes never drops. A recurrence after that is evidence
of something else.

**What would settle it, in this order, before rebooting:** `/proc/kmsg` and
`/proc/ps` over the out-of-band 9P channel on the second CDC port, then `net`
for the interface counters. Other candidates: a hung task (no watchdog), or
power.

## The Tab5's panel stays black on some boots (~1 in 10, up to 11 in 30 after back-to-back resets)

**Seen:** 2026-10, M5Stack Tab5. The kernel runs and the console answers; the
panel shows nothing. The rate drifts over the day with the same code (11, 10
and 3 black of 30), so a 30-60 boot run cannot show a small effect.

**Known:** a TSL2591 against the glass told lit (~180 lux) from black (~2.6
lux with the backlight on); `tools/panelboot.py --board tab5 --boots N`
counted them. The sensor has since been removed from the board, so counts
need it back or a count by eye. The panel itself is fine: `lcd reinit phy` (DSI PHY and timing)
brings the picture back every time (5 of 5), `video` and `panel` do not. No
boot-log line or event time differs between lit and black boots (153 lit, 27
black compared). Five bring-up changes each looked like the fix in one run and
did not hold; M5Stack's own sequence (54/60) is kept. Details: plan/phase63,
§63.7 item 1.

**What would settle it:** the factory firmware put through the same reset
series, counted by eye. If it goes black too, the cause is not our bring-up.

## The RP2350-LCD-7 hung during boot after a flash (once)

**Seen:** 2026-10-01 (37.5a), second of two flashes of one image: USB
enumerated, the console never answered, the panel cycled the ST7262's own
test pattern (no scan-out started). A power cycle booted normally; the suite
passed 25/25.

**What would settle it:** a UART adapter on header H7 for the early boot log,
and the next boot's `[CLK] last reset:` line. USB up and panel down is
before the console exists.

## A P4 hears nothing from its C6 for 3 s, now and then: the link drops (found 2026-11-01)

**Seen:** pushing an update to a P4 board's C6 (`lota.py push ... --prefix /c6`), about 1 push in 10
on the Tab5 (4 of ~45) and once on the NANO: `commit` answers "I/O error", although the C6 committed
(its log: "verified; the next boot tries it once"). At that moment the P4 logs `[LLP] link down` and
`link up` 20-500 ms later with the same peer session, and its `downs` counter rises; the C6 never sees
the link drop. `commit` takes 1.2 s on the C6 (1.16 s of it the slot's SHA-256, yielding every 4 KB).
**Ruled out:** a stalled pump on either side (`pump_gap_max_ms` in `/proc/llp`: 30 ms on the P4, 69 ms
on the C6 across a failure), the C6 refusing to send (`refused=0`, `held_for_credit=0`), and an SDIO
error (none logged then, `channel_restarts` unchanged). So the C6 believes it sent its keepalives, and
the P4 polled every 20 ms and read nothing for `LLP_DEAD_MS` (3 s): the frames are lost or late
between the C6's SDIO slave and the P4's reads. **Worked around:** `lota.py` reads the node's state
after an error at `commit` and goes on when it says committed. **What would settle it:** on both
sides, the time of the last frame sent and received, and the SDIO slave's token and packet length as
the P4 last read them, frozen at the moment the P4 declares the link down.

## The LCD-7B's 9P over its console stops answering partway through a session (found 2026-11-04)

**Seen:** `lota.py push`/`push-board --p4 lcd7b`, 3 of 6 sessions while updating to 0.19.0, with the P4 on
build `1315.4128a2fa+`: a walk or a read of `/dev/ota` gets no reply, and p9lib reports "connection
closed while reading a SLIP frame" (its read timed out: pyserial returns nothing on a timeout), or "no
Rversion ... after warm-up retries" when the next session starts. Afterwards the console does not
answer typed commands either (one stray byte), while the board answers pings and serves 9P over
Wi-Fi: not a hung board, a stuck console. A reset (`p4run.py --run`) or the reboot into a new image
restores it. The other sessions ran to the end, one of them a whole C6 update through `/c6`. Not
seen on the Tab5 in this round (its console was used once, for a C6 push).
**Worked around:** `push-board --tcp HOST --keyfile KEY`; both P4s were updated that way.
**What would settle it:** whether 0.19.0 does it too; and, at the moment it happens, read over 9P on
TCP: the console demultiplexer's state (`/proc/uartdemux`, or what it has instead), the 9P link's
counters and the tail of `/proc/kmsg`.

## A C6 up an hour or more could not rejoin a network it lost (found 2026-10-31)

Seen on every C6 that afternoon and evening (the C6-Zero, the NANO's, the Tab5's): after an hour or
more of uptime, once the link was lost (or a `rejoin` asked for), every attempt failed until the C6
was restarted; freshly started, each rejoined in 2-3 s. A capture after 65 minutes showed the
association succeed (`auth -> assoc -> run`) and fall back to `init` 2.5 s later, again and again,
with the blob's log lines **350 ms apart** (1 ms at boot). **Cause:** `radio_osi_log_timestamp()`
divided microseconds by 1000 with a subtraction loop, one step per millisecond of uptime: 4.25 million
steps, about 340 ms, per log line an hour in, and a reconnection logs dozens of lines, so the radio
thread stalled past the WPA2 handshake's timeout. The spinning radio thread also starved the kernel's
Wi-Fi supervisor, so its restart rule could not fire. **Fixed** (`5929206`): constant-time division
(and the same in `radio_osi_get_time()`). On the way: the receive buffers that could take the whole
radio heap were capped (`4d30bf6`, no refusals since), and a C6 not joined for 15 minutes restarts
(`775efc9`, `4d30bf6`). **Verified:** after 70 minutes of uptime, a rejoin requested over 9P with no
console attached came back in 4 s on the C6-Zero and 3 s on the NANO's C6. Kept here until it has
held for a few days of unattended running; then it moves to section D.

## The ENC28J60 clears MACON1.MARXEN / ECON1.RXEN when idle

**Seen:** within seconds on an idle gateway with the module attached.
**Worked around** (2026-08-31): `enc_poll_locked()` notices and redoes the
MAC/PHY init, escalating to a hardware reset; 0 % loss under sustained
traffic. `ntruchsess/arduino_uip#167` landed on the same workaround on
genuine Microchip silicon. Ruled out: a dedicated regulator, three capacitor
values, SPI clock both ways.

**What would settle it:** a soldered build instead of jumper wires. Only
worth doing if it ever causes an operational problem; full account in
`plan/phase19_ip_stack_and_ethernet.md` under R4.

---

# C. Deliberate limits that look like faults

## The P4 panels lose every second frame while flash is written (found 2026-11-04)

**Seen:** the LCD-7B's screen flickers rapidly during an update. Measured with `lcd fps`: 60.0 Hz
idle, 28 Hz while a file is written to `/flash0` over 9P, with no DMA error and no bridge underrun.
**Cause:** the scan-out DMA sends one frame per descriptor, and `gdma_isr()` re-arms it at each
frame end (`drivers/dsi_esp32p4.c`, as IDF's `mipi_dsi_dma_trans_done_cb()`). A sector erase masks
interrupts on the writing core for tens of milliseconds and parks the other
(`drivers/flash_esp32p4.c`: the trap handler is in flash, so no interrupt may be taken while the
chip is busy). The frame end waits, and the panel gets no picture for that frame. IDF does not
show it because it keeps IRAM-resident interrupt handlers running during flash operations. The
Tab5 uses the same driver and very likely shows it too (not measured).
**Shape of a fix**, in order of preference: a scan-out that runs without the CPU (the DW-AXI-DMAC's
reload multi-block mode, the interrupt only for double-buffer flips), to be measured with `lcd fps`
during writes; or the MSPI's erase auto-suspend, so a sector erase no longer needs interrupts
masked at all (chip- and flash-dependent, a larger change).
**Accepted (owner, 2026-11-04):** cosmetic, and only while flash is written; the update itself is
not affected (both P4s updated with it). Not to be fixed unless it starts to disturb an update.

## Rapid 9P reconnects are refused (4 slots, 2 s TIME_WAIT)

A connect in a tight open/close loop is refused once every slot is open or in
TIME_WAIT. `net/tcp.c` keeps `TCP_MAX_CONNS = 4` and `TCP_TIME_WAIT_MS = 2000`,
both deliberate and commented. Listed because a `ConnectionRefusedError` looks like a transport
failure and cost an investigation in R5; `tests/hw/test_wifi.py` paces past
TIME_WAIT. Raising the limit is trivial when a workload needs it.

## host/fuse-p9 is unverified on macOS

On Apple Silicon under "Full Security", macFUSE's system extension needs
Reduced Security set from Recovery Mode -- a security trade on a developer's
machine that was documented rather than pushed. The code reaches the real
macFUSE mount call; see `host/fuse-p9/README.md`.

## The P4's one objcopy warning

Every `esp32p4` link warns `empty loadable segment detected at
vaddr=0x40000000`. Intended and explained at the `lugalos-ram.elf` step in
`CMakeLists.txt`: removing `.text`/`.rodata` empties the flash-window
PT_LOAD, which is precisely the point (the ROM must not be handed a segment
there). `readelf -lW lugalos-ram.elf` shows four PT_LOADs, all at `0x4ff…`.
The one place `memory/zero_warning_policy.md` is knowingly bent.

## The chess persona's search speed depends on the link (18 % spread)

**Decided (owner, 2026-10-02): stays.** On a board without PSRAM the
engine's hot path executes in place from flash through the 16 KB XIP cache,
and a depth-6 search varies by 18 % with how unrelated code shifts the link
(3 264-3 849 ms, measured on four LCD-7 builds in 38.9). `CONFIG_CHESS_HOT_RAM`
removes it -- the LCD-7 sets it -- for 31 KB of SRAM, which on the Pico 2 is
heap worth more than the spread. A cross-build search benchmark on that
persona carries this noise; perft no longer does (1 %).

---

# D. Lessons kept from closed entries

**A comment that reads like a justification is not one.** The P4's EMAC lost
~1 % of frames (closed 2026-09-16, phase 28 Z5) because `pad_iomux()` forced
maximum drive on every RMII pad "because RMII runs at 50 MHz" -- an
assumption nobody checked; ESP-IDF leaves the default. It survived a long
hunt through the descriptor path because the comment sounded like a reason.
What localised it: two loopback modes bisecting the physical path
(MAC-internal perfect, PHY loopback corrupting frames with every error
counter clean -- a signal-integrity signature, not a logic one).
`emac loopback [phy]` stay in the driver for that.

**Park an issue on a named future event, not on a design.** The P4's console
was its only 9P wire (closed 2026-09-16): the two fixes were a USB device
stack or an untestable UART link. It was parked with "phase 28 brings up
Ethernet, at which point the question changes shape" -- and it did; neither
design was needed.

**Make the failure say why before guessing at the cause.** "No directory
could ever grow" (fixed 2026-09-17) was reported as a P4 problem, possibly
phase 34's; it was three steps away, in `fs/fat32.c`, reproduced in one QEMU
session once looked for there. The report's own last line was the lesson:
"everything above was inferred from a boolean". `fat32_write_file()` now
names what it could not do. The identity-store flake (phase 33 §2) taught the
same thing from the other side: three soaks studied the write path, and the
first soak with a message on every failure branch showed it was the read.

**A test that stops at the first answer has to know what the answer is.** The
TCP recovery test failed now and then with "a duplicated request produced 0
replies" (2026-11-05 to 2026-11-09, at last in two full runs in a row; closed
2026-11-09). The stack was right: it acknowledges a request at once and sends
the 9P reply when the server task has run, two segments, and the test's
`segments()` returned at the first one, a bare ACK, unless the reply came in
the same 20 ms poll. A recording of every segment the guest sent settled it in
one run where reruns and loaded hosts had not. `TCPDriver.segments_with_data()` /
`segments_until()` (tests/netpeer.py) wait for what is wanted; the TCP state
machine test had the same race at its two replies and its FIN.
