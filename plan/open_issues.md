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

## Two hand-rolled yielding locks are outside the wait-for graph

**Destination: phase 40, item 2.**

`drivers/cyw43_rp2350.c`'s `g_bus_busy` and `drivers/enc28j60_rp2350.c`'s
`g_busy` are a flag and a yield loop: no owner, so no edge in phase 31's
wait-for graph, and not re-entrant. It has cost a BOOTSEL once: a gratuitous
ARP under `net_set_address()` re-took the bus lock a frame below its holder
and deadlocked `wifi probe`.

**Fix:** make both `ylock_t` -- re-entrant for the owner (the failure above)
and in the graph for free. Needs a board with each chip on the bench.

## Pulling the SD card out of a running board hangs it

**Destination: phase 40, item 3.** The trigger is deterministic, so this is
work, not waiting.

Seen 2026-08-31 on `rp2350-wifi`: card pulled while mounted, console frozen,
1200-baud touch ineffective, power cycle needed. Booting *without* a card is
fine.

The entry used to blame unbounded waits in `drivers/spisd_rp2350.c`; the
driver's loops are bounded today (token wait 10 000, busy wait 100 000,
ACMD41 by deadline), so the hang is somewhere else -- the VFS or FAT32 retrying
a failing read forever, or a timeout that returns garbage the caller then
follows. **Fix:** pull the card on purpose on the LCD-7 with the console
attached, find the loop, make it fail the operation; the FAT32 read path now
returns an end-of-chain on a read error (38.0), which is the shape the rest
should have.

## C6/C7's exact heap comparison is disturbed by background allocation

**Destination: phase 40, item 7.**

`tests/hw/test_rp2350.py` requires `Pages Used` equal before and after a
compile. On `rp2350-clock` in the first minute after a flash, while the WLAN
supervisor was still joining, it differed once in four runs (2026-09-03) --
consistent with the join allocating in between, inferred from timing.

**Fix:** not a tolerance (that blunts the leak detector): take both readings
on a demonstrably quiet system, or compare only what the compile owns, which
`/proc/meminfo`'s peak already separates.

## The clock display flickers while the radio comes up

**Destination: phase 40, item 8.**

On `rp2350-clock` with stored credentials, the software-multiplexed display
flickers once a second for ~15 s while the CYW43439's 231 KB firmware is
bit-banged over gSPI. Parked in phase 19 until there was a second core;
phases 22/23 provided it. **Fix:** run the firmware upload (or the display
refresh) pinned to the other core.

## No clean way to leave a BSS before re-joining

**Destination: phase 40, item 9.**

`wifi join` over a live association: `mfp` is now stepped over (a capability
hint), so the re-join works, but `CYW43_IOCTL_SET_DISASSOC` issued exactly as
the reference does (`cyw43_wifi_leave()`: length 0, NULL, `CYW43_ITF_STA`)
answers BADARG and the firmware stays associated. Boot joins and the
supervisor's re-join after link loss are unaffected.

**Fix:** find the leave the firmware accepts (compare the reference's
ioctl framing byte for byte, or a `bsscfg:` iovar), then leave before every
manual join. Any other setup iovar refused while associated needs `mfp`'s
judgement meanwhile.

## Pressure is published as station pressure, not reduced to sea level

**Destination: phase 40, item 11** -- needs the owner's decision on point 3.

`drivers/bme280.c` publishes the pressure where the sensor sits: `963.69` at
520 m, where every weather service says about `1025`. Both are correct; they
are different quantities, and the difference (~60 hPa) looks like a permanent
storm.

**Fix:** (1) an installation altitude in metres in the identity record,
settable like the broker; (2) the barometric reduction
`p0 = p * (1 - 0.0065 h / (T + 0.0065 h + 273.15)) ** -5.257` with the
measured temperature -- no FPU and no libm in the kernel, so an interpolation
table over 0-3000 m; (3) publish both `pressure` and `pressure_msl` (one more
source slot) rather than replace the honest measurement.

## An identity write reboots the board

**Destination: phase 40, item 12.**

`drivers/idstore_rp2350.c` reboots after every write -- `identity name`,
`provision`, `key` -- for two reasons it records: the USB console did not
survive ~60 ms of erase/program with interrupts off (measured once,
2026-09-01, on a USB driver that has changed since), and XIP was left slow.
38.1 removed the second. **Fix:** measure the first again on today's driver
-- one write on a board whose record is already stored, console attached --
and drop the reboot if the console survives, or keep it with one reason
instead of two. A rename that does not reboot also stops dropping 9P and
MQTT sessions.

## The clock board's DS3231 does not survive a power cut

**Destination: the owner's bench** -- fit a working CR2032.

Established by experiment on 2026-09-02 (OSF cleared, confirmed clear, power
removed, OSF set again), not by reading the sticky flag. The software already
handles it: the kernel clock is not seeded from a chip whose OSF is set, the
face falls back to NTP/DCF time and lights the PM lamp until the first write
clears OSF -- verified on hardware the same day. Whether the cell is flat,
missing or not contacting, only a meter says.

---

# B. Unexplained intermittents

Each: what was seen, how often, what is already armed to catch it, and **what
evidence would settle it**. "Phase 40, item 1" (40.1) is the runner change
that makes the next occurrence leave that evidence: a `[Stuck Guest]` block in
the run log, and an `[Attempt]` line per architecture attempt with its
duration and first failure.

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

## The RP2350-LCD-7 hung during boot after a flash (once)

**Seen:** 2026-10-01 (37.5a), second of two flashes of one image: USB
enumerated, the console never answered, the panel cycled the ST7262's own
test pattern (no scan-out started). A power cycle booted normally; the suite
passed 25/25.

**What would settle it:** a UART adapter on header H7 for the early boot log,
and the next boot's `[CLK] last reset:` line. USB up and panel down is
before the console exists.

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

## Rapid 9P reconnects are refused (2 slots, 2 s TIME_WAIT)

The third connect in a tight open/close loop is refused. `net/tcp.c` keeps
`TCP_MAX_CONNS = 2` and `TCP_TIME_WAIT_MS = 2000`, both deliberate and
commented. Listed because a `ConnectionRefusedError` looks like a transport
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
