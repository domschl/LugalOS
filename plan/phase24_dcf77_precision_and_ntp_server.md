# Phase 24 — A clock that serves time, and knows how wrong it is

**Status: planned 2026-09-01.** Succeeds `plan/phase19_ip_stack_and_ethernet.md`,
which is concluded: R6 built the NTP *client*, and this phase builds the
inverse -- a Pico-Clock-Green persona disciplined by DCF-77 that answers NTP
queries for the segment, as a local time source that survives an internet
outage.

**Scope.** Make the DCF-77 path as accurate as the signal allows, measure how
accurate that actually is -- first against a GPS-disciplined stratum-1 on the
LAN (P0, done), then against a GPS module's own PPS wired to the board (§3.4),
which is three orders of magnitude better and removes the network from the
measurement entirely -- keep the clock disciplined between syncs rather than
set once a night, and then serve it. The protocol is the small half; §2 is
about the other one.

**The GPS is a transfer standard, not a source.** It is attached for the
calibration and removed afterwards; nothing in the shipped appliance depends
on it. That distinction is what keeps this phase about an independent
longwave clock rather than about a satellite one, and §7 draws the line
explicitly because the drift from one to the other would be gradual and
nobody's decision.

**Out of scope, explicitly:** the phase-modulation (PZF) time code, PIO-based
capture, leap-second smearing, NTS or any authenticated NTP, IPv6, serving to
anything beyond the local segment. Each is argued in §7 rather than merely
listed -- three of them are ideas that will otherwise come back every six
months.

---

## 0. Why this is its own phase, and not R6 with more milestones

R6 was half a day and it is finished. This is not that.

The protocol work here -- answering a 48-byte packet -- is genuinely small,
perhaps 250 lines, and it is the least interesting thing in the phase. What
makes a time *server* different from a time *client* is that a client is
allowed to be wrong between syncs and a server is not: a client that drifts
two seconds by evening simply corrects itself at the next query, while a
server that drifts two seconds by evening has spent the day telling everything
downstream a lie with a confident stratum number attached.

So the phase is really about three questions, in this order:

1. **How wrong is the DCF-77 path, in milliseconds, measured rather than
   estimated?** Nothing here can be designed until that number exists.
2. **How fast does this board's clock drift when nothing is correcting it?**
   Also measured. It is the number that decides how often discipline has to
   happen and what dispersion the server must honestly advertise.
3. Only then: what does the server say about itself?

Both of the first two are measurable today, on this bench, because there is a
**GPS-disciplined stratum-1 NTP server at 192.168.178.23** on the same segment
(the gateway, which is also the default the `ntp` command asks, is
192.168.178.1). That single fact is what makes this phase tractable and is why
it is being planned now rather than filed as an aspiration: the reference to
measure against already exists, and R6 already speaks to it.

---

## 1. What the tree has already, checked rather than assumed

* **A DCF-77 receiver that works.** Reception at the clock's location is
  excellent -- immediate sync, every time (user, 2026-09-01). This retires
  what would otherwise have been this phase's largest risk. Phase 17's D5
  recorded two 300 s runs that accepted no frame at all at a *different*
  antenna position, and a design that needs a frame a minute rather than one a
  night would have been a much harder ask against those numbers. It is not
  against these.

* **A portable frame decoder** (`drivers/dcf77_decode.c`) that consumes
  timestamped samples rather than registers, is driven by synthetic streams in
  `dcf77selftest` on every target, and already timestamps each transition at
  *the first sample that saw the new level* rather than when the 25 ms
  debounce completed -- so the systematic `+DEBOUNCE_MS` error every naive
  implementation carries is already absent from the decoder's own widths and
  gaps.

* **`frame_complete(d, mark_ms)`** already receives the correct instant: the
  start of the first pulse after the missing 59th, which is exactly when the
  decoded minute begins.

* **An IP stack with UDP**, four bindings, and a `udp_send()` that is already
  called from inside a receive callback by `net udpecho` -- so a server that
  replies from within `udp_input()` has precedent, not a new pattern.

* **An SNTP client** (R6) whose arithmetic is already in signed 64-bit
  milliseconds specifically so that widening the clock is a change of unit
  rather than of shape.

* **External interrupt infrastructure**: `devirq_attach()` /
  `arch_irq_enable()` (`kernel/devirq.c`, `arch/riscv/common/trap.c`), today
  carrying UART0 (IRQ 33). `IO_IRQ_BANK0` is **21**, confirmed against the
  Pico SDK's `intctrl.h` -- the same header that gives `UART0_IRQ` 33, which
  this tree independently arrived at, so the numbering is corroborated rather
  than trusted.

* **A 1 µs monotonic counter**: TIMER0, read in two instructions by
  `kernel/time.c`.

* **A working NTP client, hardware-verified against the reference this phase
  will measure against** (R6, and `tests/hw/test_ntp.py`). P0 needs no new
  transport, only a log.

* **A warning about `long`.** It is 32 bits on RV32 and RP2350. R6 shipped a
  correct clock with a truncated *printed* offset because an `int64_t` went
  through `%ld`, and `kernel/printk.c` supports one `l` and no `%lld`. Every
  quantity this phase adds -- microsecond timestamps, ppm estimates,
  dispersion in µs -- is wider than 32 bits or derived from something that is.
  Assume nothing prints itself correctly; `net/ntp.c`'s `fmt_interval()` is
  the pattern.

* **The GPIO register block** for the DCF pin already exists in
  `drivers/dcf77_rp2350.c` (IO_BANK0 / PADS_BANK0 / SIO). The interrupt
  registers it does not yet touch are at IO_BANK0 + 0x230 (`INTR0`), + 0x248
  (`PROC0_INTE0`), + 0x278 (`PROC0_INTS0`).

### And one defect, found while surveying for this phase

`drivers/dcf77_service.c`'s `commit()` sets `g_radio_at_ms = now_ms` -- the
time of the `dcf77_service_feed()` call that noticed -- because
`dcf77_take_time()` hands back a civil time and nothing else. The decoder's
own `mark_ms` is thrown away.

That makes every radio-set clock in this tree **systematically slow**, by
`DEBOUNCE_MS` (25 ms) plus up to one sample interval, before the receiver's
own delay is counted at all. It is a real error, it is present today, and
fixing it is an afternoon (P1). It is written here rather than filed as a bug
because P0 has to measure the *before* first, or the fix cannot be shown to
have worked.

---

## 2. The error budget, which is the actual subject of this phase

Five terms, in the order they are worth attacking. Only one of them is what
the original proposal reached for first.

| # | term | today | after this phase | can software fix it? |
|---|---|---|---|---|
| 1 | commit-path bias (`now_ms`, not `mark_ms`) | ~25-35 ms, systematic | 0 | **yes, trivially** (P1) |
| 2 | sample quantisation | ±4-8 ms, *varying with load* | ~±2 µs | yes (P3) |
| 3 | propagation, Mainflingen → here | ~1 ms | calibrated out | yes, as a constant (P4) |
| 4 | receiver group delay + AGC-dependent edge jitter | **measured: -65.5 ms bias, ~3.4 ms jitter** | bias calibrated out (P4); jitter remains | **only the mean** (P4) |
| 5 | holdover between syncs | datasheet ~2.6 s/day; **measured 40 ms/day** | ~1 ms/day | yes (P5), but it buys far less than assumed |

**Term 5 is the one nobody lists, and the first hour of P0 has already
shrunk it.** The RP2350's TIMER0 counts from a 12 MHz crystal whose
*datasheet tolerance* is of order ±30 ppm -- 30 µs per second, 2.6 s per day,
which is where this table's original figure came from.

**Measured, 2026-09-01, preliminary:** 63 samples over 3733 s against
192.168.178.23, with the clock frozen after one initial step, gave a
least-squares slope of **0.3 ppm** and offsets that stayed inside +2 to +6 ms
for the whole hour. The slope itself is not yet resolved -- 0.3 ppm over
3733 s is 1.1 ms of drift, which is smaller than the sample scatter, so an
hour cannot separate it from zero. What the hour *does* establish is an upper
bound, and a strong one: a 30 ppm crystal would have drifted 112 ms across
that run, and nothing of the sort happened. **This board's crystal is better
than about 1 ppm**, so free-running drift is on the order of tens of
milliseconds per day rather than seconds.

That is a different design problem from the one this phase was proposed
against. Holdover still has to be handled honestly -- P6's dispersion must
still grow -- but it is no longer obviously the dominant term, and P5's
frequency correction may turn out to buy much less than the ratio in §4
suggests. **A multi-day run is what settles it**, and P0 is now producing one.
The table below keeps the datasheet figure alongside the measurement, because
the point of P0 is that the two are different things. `drivers/dcf77_service.c` syncs
once nightly at 03:17 (`CONFIG_DCF77_AUTO_HOUR`) and does nothing in between.
A server built on that would be seconds wrong by evening, and how precisely
each individual sync landed would be irrelevant beside it. Note that ±30 ppm
is a *datasheet order of magnitude*: P0 measures this board's actual figure,
because a measured 4 ppm and a measured 40 ppm imply very different discipline
intervals and very different honest dispersions.

**Term 4 is the floor.** A 77.5 kHz narrowband AM receiver has a group delay
set by its filter, on the order of tens of milliseconds, varying between units
and moving somewhat with AGC state. The *mean* is a constant and can be
calibrated away against the stratum-1 server; what remains is the jitter, and
nothing in software removes it -- it can only be averaged over many minutes,
which the discipline loop does anyway. **This is why the achievable figure is
the 5-50 ms in the original proposal and not better**, and it is why terms 1
and 2 matter: they are the terms that are currently *comparable to or larger
than* the floor, which is the definition of worth fixing.

**Term 2's real problem is not its size, it is its variance.** The decoder is
fed at ~1 kHz when the display is dark and ~125 Hz when it is scanning
(`drivers/pico_clock_green_rp2350.c`'s own note), and phase 19's R5 recorded
in detail how much the radio disturbs the same loop. A fixed bias can be
calibrated out; one that changes with what the board is doing cannot.

### What the network then costs -- measured, 2026-09-01

NTP's offset calculation subtracts the server's own processing time and
cancels any delay that is *symmetric*. What it cannot see is asymmetry. Over
WiFi that is retransmissions, power-save wake-ups and queueing.

This was going to be an estimate. It is not, because R6's client made it
measurable the day it landed. An `rp2350-wifi` board against the
GPS-disciplined stratum-1 at 192.168.178.23, over the radio:

| consecutive syncs | offset | round trip |
|---|---|---|
| 1 (after a reboot, DS3231 holdover) | +967 ms | 9 ms |
| 2 | +1 ms | 6 ms |
| 3 | +0 ms | 7 ms |
| 4 | +0 ms | 7 ms |

**The whole network path costs about a millisecond**, not the 20-50 ms that
was assumed when this phase was proposed. The offsets are at the resolution
limit of a clock that only counts milliseconds, so the true figure is somewhere
below what the board can currently express -- which is itself an argument for
P2.

So the served accuracy is bounded by term 4 by a wide margin: **the DCF path
dominates the WiFi path by one to two orders of magnitude.** The design
conclusion -- spend the effort on the receiver side and do not over-engineer
the network side -- is now measured rather than argued, and P6 can treat the
transport as effectively free provided it timestamps honestly at receipt and
at send.

---

## 3. The four ideas, evaluated

### 3.1 "Delay is basically a constant, measurable against the stratum-1" — yes, and it is the keystone

Correct, and it is the single most useful idea in the set. Propagation from
Mainflingen is ~300 km of ground wave, about 1.0 ms, and it does not change.
The receiver's group delay is a property of one module that changes only
slowly. Their sum is one constant, and a constant is exactly what a reference
you already own can measure.

The measurement is cheap because both halves already exist: R6's client asks
192.168.178.23 what time it is; the DCF service already records the instant of
every accepted frame. Logging the difference for a few hours gives a mean (the
calibration constant) and a standard deviation (**the honest accuracy claim,
and the number that goes in the server's root dispersion**). This is P0 and
P4, and it is why they bracket everything else.

One caution worth recording now: the constant must be stored as a **board
configuration value**, not a literal in the decoder. It belongs to a
particular receiver module and antenna, and a second board with a second
module will have a different one.

### 3.2 "Can we use PIO state machines for high-precision signal timing?" — yes, and it is not needed

PIO can do it. A state machine can wait on a pin edge and push a cycle count,
and with DMA into a ring it costs zero CPU and is immune to interrupt latency
and to whatever the display and the radio are doing. That last property is a
real attraction on *this* board specifically, given what phase 19's R5 found.

But it answers a question the signal does not ask. A GPIO interrupt reading
TIMER0 timestamps an edge to about a microsecond including handler latency,
against a receiver whose own edge jitter is *milliseconds*. That is already
three orders of magnitude better than the thing being measured. PIO would buy
precision that term 4 discards on arrival.

**Recommendation: interrupt plus TIMER0 (P3), with PIO explicitly held in
reserve.** The trigger that would revive it is measured, not hypothetical: if
P3's interrupt handler is shown to disturb the display or to cost real CPU
under a noisy line (an AM receiver off-tune can produce thousands of edges a
second), PIO+DMA is the answer and this section is the record of why. Until
then it is complexity that no measurement has asked for -- and phase 17 §3 is
the precedent, where four planned display mitigations were dropped as "not
built and not needed" because the measurement said the cure was one constant
in the decoder.

### 3.3 "The pseudo-random phase modulation, said to allow ~1 µs" — real, and out of reach of this hardware

The mechanism is real and the order of magnitude is roughly right. DCF-77
carries, alongside the AM second marks, a phase modulation of the carrier: a
512-chip pseudo-random binary sequence transmitted each second, shifting the
carrier phase by a small angle at a chip rate derived from the carrier itself.
Correlating the received signal against the known sequence yields a timing
reference far sharper than the AM envelope, because a correlation peak over
512 chips is narrow where an AM envelope edge is soft. Commercial PZF
receivers quote tens of microseconds to the transmitter. The user's "1 µs" is
optimistic but names the right technique.

**It cannot be done with the receiver this board has, and the reason is not
CPU.** The phase information is in the 77.5 kHz carrier. What
`CONFIG_DCF77_OUT_GPIO` carries is the *demodulated AM envelope* -- the module
has already thrown the phase away, and `drivers/include/drivers/dcf77.h`
records the module as a four-wire part (VDD/GND/OUT/PON) with no analog or IF
output to tap. There is nothing on the pin to correlate against.

Two things would make it possible, and both are hardware projects:

* **A receiver that exposes the carrier or an IF**, sampled at ≥155 kSa/s and
  correlated in fixed point. The RP2350's ADC can sample fast enough and its
  second core could carry the correlation, so the *compute* is plausible --
  but it needs an analog front end, an antenna and a receiver that do not
  exist here, and it is a signal-processing phase rather than a milestone.
* **A PZF-class receiver module**, which does the correlation itself and hands
  over a serial time string plus a PPS edge. At that point the interesting
  work has been bought rather than done, and the board is doing what it would
  do with a GPS module.

**Closed, with the reason recorded so it stops recurring**: the limit is the
absence of a carrier tap, not the absence of arithmetic. If a receiver with an
analog output ever appears on the bench, this section is the place that says
what to do with it.

---

### 3.4 A GPS receiver's PPS, wired to the board — adopted, and it supersedes §3.1

*Proposed 2026-09-02, after P0 had run and shown that the instrument was
becoming the limit.*

§3.1 called measuring against a network stratum-1 the keystone, and for P0 it
was. It is not the best instrument available, and the better one is a pulse
per second from a GPS module on a free pin of the board itself.

**The two signals are complementary in exactly the right way.** DCF-77's frame
says *which* second it is. PPS says *exactly when* that second began, to tens
of nanoseconds. So the calibration stops being a statistical fit and becomes a
subtraction: timestamp the DCF mark, timestamp the nearest PPS edge, and the
difference **is** the path delay -- receiver group delay plus propagation plus
whatever software still adds.

**And both timestamps come off the same TIMER0 through the same GPIO
edge-capture path**, so nothing about the comparison touches a network. No NTP,
no WiFi asymmetry, no round trip to halve, and no reference server that can
quietly drop to stratum 3 overnight while answering every query (§5's P0 note).
The measurement noise falls from the **3.7 ms** P0 measured to
**microseconds** -- about three orders of magnitude, and it is the difference
between inferring the radio's jitter by quadrature and reading it directly.

**It is a transfer standard, not a time source, and the distinction is the
whole phase.** This work exists because DCF-77 is independent of the internet;
a permanently attached GPS is simply a better clock and would make all of it
ornamental. So: attach, calibrate, remove -- which is how a metrology lab uses
one. The plan states this so that nobody later "improves" the appliance by
leaving the GPS in and quietly deletes the reason for the phase.

**The driver survives the removal, and that is deliberate** (user, 2026-09-02).
A GPS time source is a legitimate capability for this tree to own -- a future
node with sky view and no longwave reception wants exactly this code, and a
server disciplined by *both* would be strictly better than one disciplined by
either. What this phase declines is *depending* on it, not *having* it.

**NMEA is read, not skipped.** The arithmetic needs only the edge, but a
module without lock may emit a free-running or degraded pulse and the edge
alone cannot say which. A front-panel LED is not a software gate. A small NMEA
parser -- the fix-quality and satellite-count fields are enough -- makes the
calibration self-validating, and it is the half of the driver a future
GPS-disciplined node would need anyway.

**Pins, checked against the clock persona's own map:** GP3, GP4, GP5, GP8,
GP19, GP20 and GP21 are unclaimed. GP20/GP21 are a UART1 pair, so NMEA lands
on a real UART rather than a bit-banged one, and PPS takes GP19. Nothing has
to move.

**Confirmed before adoption:** the module gets lock at the receiver's own
location (user, tested). That was the one practical risk worth checking first
-- DCF-77 wants an orientation away from interference and GPS wants sky view,
and those two optima had no obligation to coincide.

## 4. What the server has to say about itself

The design principle for the whole milestone, and the reason the protocol half
is not merely mechanical: **a server that advertises a confidence it cannot
justify is worse than one that admits it is unsynchronised**, because clients
believe stratum numbers.

* **Stratum 1** only while the clock is genuinely disciplined by the radio.
  Reference id `DCF`.
* **Root dispersion grows with holdover.** The value is
  `sync_uncertainty + drift_rate × age`, where both terms are *measured*
  numbers from P0 and P4 rather than constants someone liked the look of. With
  an uncorrected ±30 ppm that is ~108 ms per hour; with a frequency correction
  good to 1-2 ppm it is ~4-7 ms per hour. **That ratio is the entire argument
  for P5 expressed as one number.**
* **Leap indicator from the frame's own A2 bit**, which the decoder already
  parses (bit 19) and currently discards. A leap second announced on the air
  reaches NTP clients as an announced leap second -- one of the few places
  where being a DCF-disciplined server is *better* than being a GPS-fed one,
  since the announcement arrives in band.
* **Unsynchronised is a legitimate answer.** If the radio has been quiet long
  enough that dispersion exceeds what the phase decides is useful, the reply
  says LI 3 / stratum 16 rather than dressing holdover up as authority.
* **No kiss-o'-death rate limiting** in the first version: this serves a home
  segment, and the honest thing is to say so rather than to implement a
  mechanism nothing will trigger.

---

## 5. Milestones

**P0 — measure, before changing anything. DONE 2026-09-02.** With R6's client and the
stratum-1 at 192.168.178.23: log, for at least 24 hours, (a) the offset the
client reports against the reference, and (b) the instant of every accepted
DCF frame against the local clock. From those two, derive **this board's
actual crystal error in ppm** and **the current DCF path's mean offset and
standard deviation**. Host-side script under `tests/hw/`, board side a
`/proc` file or a `(dcf-log)` primitive -- whichever needs least new
machinery.
*Verify:* two numbers written into this document, with their sample count and
duration. Everything after this is judged against them; a phase that skips
this one is guessing about term 4 and term 5 both.

**P0's result, 13.4 hours, 753 samples, 689 after rejecting the ones whose
round trip made them worthless:**

| | measured | this phase assumed |
|---|---|---|
| crystal | **-0.460 ± 0.010 ppm** (40 ms/day) | ±30 ppm (2.6 s/day) |
| DCF path bias | **-65.5 ms**, stable to ±1.4 ms across the night | 10-100 ms, unknown |
| DCF path scatter | **5.0 ms sd** | "the floor", pessimistically tens of ms |
| our own measurement noise | 3.7 ms sd | not considered -- and now the limit, which is why §3.4 adopts PPS |

Four findings, in the order they change the phase.

* **The crystal is 65x better than its datasheet tolerance, and resolved.**
  -0.46 ppm ± 0.01 is nine sigma from zero, so this is a real number and not
  an upper bound like the first hour's. **§4's argument for P5 does not
  survive it.** That argument was a ratio: ~108 ms/hour uncorrected against
  ~4-7 ms/hour corrected. The true uncorrected figure is **1.7 ms/hour**, so
  the correction buys roughly one part in forty of what the plan claimed.

* **The drift is a straight line.** Hourly residual means stay inside ±1.7 ms
  across the whole run with no systematic wander and no temperature
  signature. So P5's frequency half is not a tracking loop: **measure the
  constant once and apply it.** A tracking loop remains the honest thing for a
  board whose ambient changes -- one night at stable indoor temperature says
  nothing about summer, or sunlight, or a different corner -- so P5 keeps the
  ability to re-measure. It just stops being the phase's centre of gravity.

* **The receiver's group delay is a genuine constant.** Hourly means span
  -64.3 to -67.1 ms, a 2.8 ms spread over thirteen hours. §3.1 hoped this
  would be true; it is. **P4's calibration constant is +65.5 ms**, and after
  P1 removes its own ~25-35 ms the residual should be the propagation plus the
  receiver alone, around 30-40 ms.

* **The measurement is now limited by the instrument, not by the radio.** The
  5.0 ms DCF scatter contains our own 3.7 ms, because every DCF figure is
  computed *through* an NTP offset. In quadrature the radio's own jitter is
  about **3.4 ms** -- an order of magnitude better than §2's pessimistic
  reading of term 4, and close enough to a millisecond clock's own resolution
  that **P2 and P3 are now what stand between this and a better number**,
  rather than refinements on top of a receiver that was going to dominate
  anyway.

**The reference itself degraded partway through that run, and the result
survived it.** 192.168.178.23 lost its GPS at t=9435 s and spent most of the
night climbing its fallback chain -- stratum 3, then 5, 7, 9, 11 -- while
answering every query and looking healthy. Only 137 of 757 samples were taken
against a true stratum 1. Checked separately once the analyser learned to
grade by stratum:

    stratum 1: n=133  dcf mean -65.9 ms  sd 4.9
    stratum 3: n=367  dcf mean -65.5 ms  sd 5.2

**0.4 ms apart**, so the fallback chain held UTC well enough not to move the
answer, and the headline above stands on the clean subset by itself. The
crystal figure is the one to treat with more care: -0.46 ± 0.01 ppm comes from
the full 13.4 h, while the stratum-1 subset spans only 2.6 h and gives
-0.26 ± 0.10 ppm on its own. The long fit is far better determined and a
network-disciplined reference does not *drift*, so -0.46 stands -- but it is a
figure that leans on the degraded stretch in a way the DCF number does not.

The lesson is the cheap one: **a reference that fails does not stop
answering.** Nothing in the run looked wrong at the time. `collect_p0.py`
grades by stratum now, and does it before the round-trip filter, because a
degraded server can be fast and wrong.

The DCF error was also, after filtering, **negative in every one of 685
samples** (-83 to -48 ms). That is the physically necessary sign -- the path
can only ever deliver time late -- and the unfiltered set contained values up
to **+259 ms**, which is impossible. The delay filter removed exactly the
physically impossible readings without being told any physics, which is the
best evidence available that it selects on the right quantity.

**P1 — hand out the mark timestamp. DONE 2026-09-02, measured.** `dcf77_take_time()` returns the decoder's `mark_ms`
alongside the civil time, and `dcf77_service_feed()` records *that* as the
instant the radio time was true, instead of the `now` of whichever sample
happened to notice.

**The prediction, written down before the board is reflashed**, because a fix
that moves the number by an unexplained amount has not been understood:

* The error being removed is `now_ms - mark_ms` at the moment the frame was
  taken. A transition is confirmed only once `DEBOUNCE_MS` (25 ms) has passed
  at the new level, and the service learns of it on its next sample -- about
  every 8 ms on this persona, since the panel is scanning. So the correction
  is **25 ms + uniform(0, 8 ms)**, mean ≈ 29 ms.
* **The bias should move from -65.5 ms to about -36 ms**, and land between
  -33 and -40. Anything outside that is a second effect nobody has accounted
  for, and is more interesting than the fix.
* **The scatter should shrink a little too**, which is the secondary
  prediction and the easier one to get wrong. The removed sampling jitter is
  uniform over 8 ms, sd ≈ 2.3 ms. Taking it out of the radio's measured
  3.4 ms leaves ≈ 2.5 ms, so the *reported* sd -- which also carries our own
  3.7 ms of NTP noise -- should fall from **5.0 ms to about 4.5 ms**. A small
  move, and worth stating precisely so that a large one is recognised as a
  surprise.

What P1 explicitly does **not** touch: the receiver's own group delay, which
is the ~36 ms this leaves behind and P4's to calibrate.

**A second instance of the same bug, found while making the change.**
`dcf77_sync()` in `drivers/dcf77_rp2350.c` -- the `(dcf-sync N 1)` diagnostic,
which is the command a person actually uses to set a clock by hand -- called
`time_set_utc(&got)` with the frame's minute start and *no carry-forward at
all*, asserting that no time had passed since the second it names. By that
point a good deal had: the debounce, the loop iteration that noticed, and then
six lines of console output, which at 115200 baud is tens of milliseconds on
its own. The service path had always carried this forward correctly; the
hand-operated path never did. Now fixed the same way.

*Verify:* P0's harness re-run against the same reference, and the two
predictions above checked as predictions rather than as observations.

**Result, 1.81 h, 74 accepted frames, against the stratum-1 baseline of
-65.9 ms / 4.9 ms sd:**

| | predicted | measured |
|---|---|---|
| bias | -33 to -40 ms | **-41.7 ms** |
| shift | +29 ms | **+24.2 ± 0.7** |
| scatter | 4.5 ms | **4.5 ms** |

**The scatter prediction landed exactly. The bias shift did not**, and the
5 ms gap is significant at this sample size, so it is worth saying what is and
is not known about it.

What is certain: the correction is real, large, and in the predicted
direction, and it is essentially `DEBOUNCE_MS` on its own. +24.2 ± 0.7 against
a hard floor of 25 ms is about one sigma below that floor -- consistent with
it, not a violation -- which says the sampling term contributed almost
nothing.

What does not reconcile: if sampling jitter were negligible the scatter should
not have improved at all, and it improved by exactly the predicted amount.
Recovering 4.9 -> 4.5 ms needs a uniform jitter about 6.7 ms wide, which would
have added ~3.4 ms to the mean and given +28. The two measurements cannot both
be explained by one sampling interval.

The most likely reconciliation is that they are not measuring the same thing:
the runs are on different days, 74 frames against 133, and **the receiver's
own delay may differ between them**. Whether that delay moves with signal
strength is a question P4 was already written to ask, and it now has a second
reason to. Two things follow for the milestones after this one: the ~8 ms
sampling figure taken from `dcf-monitor`'s ~125 Hz does not describe the
*app's* feed rate and should be measured rather than assumed, and a 5 ms
discrepancy is exactly the size that a millisecond clock and a network
reference cannot settle -- which is what P2, P3b and P4 exist to fix.

**P2 — a microsecond wall clock. DONE 2026-09-02.**
§3.4's PPS comparison is expressed in microseconds and cannot be recorded in a
millisecond clock; P0 also found its own noise floor sitting where the
clock's resolution is (a 3.7 ms residual against 5.0 ms of total scatter). `kernel/time.c`'s `g_base_epoch_ms` /
`g_base_mono_ms` become microseconds, with `time_get_utc_us()` /
`time_set_utc_us()` beside the existing calls, which keep working.
`rtc_time_t.ms` stays as it is -- the display, the DS3231 and `date` have no
use for microseconds.
*Verify:* existing suite unchanged (this is a widening, not a behaviour
change); a new selftest asserting round-trip through the µs setters preserves
sub-millisecond values.

**As built.** `kernel/time.c`'s pair became `g_base_epoch_us` /
`g_base_mono_us`, with `time_epoch_us()` / `time_set_epoch_us()` as the
primitive and the existing `rtc_time_t` accessors built on top. `rtc_time_t`
still carries milliseconds and deliberately still does -- widening it would
touch every caller for the benefit of two, and the display, the DS3231 and
`date` have no use for microseconds.

The monotonic half mattered as much as the epoch half: the old code read
`time_get_ms()`, throwing away three digits of a counter that has them
(TIMER0 is a 1 µs read), so a set-then-read round trip lost sub-millisecond
detail even when both ends had it.

`net/ntp.c` moved with it, which is where the resolution first buys something:
the four timestamps NTP subtracts were each quantised to a millisecond before
any arithmetic ran, against a measured 6-9 ms round trip. Its `fmt_interval()`
now renders microseconds and picks its scale by magnitude -- three decimal
places of a millisecond for a sync against a running clock, days and a clock
time for a board that has never been told what year it is. The 32-bit `long`
hazard that made that function necessary is unchanged and is why it still
splits the value into fields rather than printing it.

*Verified:* `timeselftest` on both QEMU targets, in the suite (304/304). Every
assertion in it would have passed on the millisecond clock except the two that
matter -- a sub-millisecond remainder surviving a set-then-read, and 250 µs of
separation being representable at all. Those are exactly what P3b's PPS
comparison needs and what the old representation silently rounded to zero.

**What P2 does *not* change:** P0's broadcast wire format, which still reports
milliseconds. Widening it means fetching the board back from wherever its
reception is, and the offsets it carries are dominated by WiFi asymmetry
rather than by that quantisation -- P4's comparison is against PPS, not
against this.

**P3 — edge capture on the DCF pin. DONE 2026-09-02 (bench verify outstanding).** §3.2 held
this at arm's length because a GPIO interrupt already timestamps a thousand
times finer than the receiver's jitter, so the precision had nowhere to go.
§3.4 gives it somewhere to go: the same mechanism, on a second pin, is what
captures PPS, so P3 is no longer a refinement on the DCF side but the shared
foundation both references stand on. `IO_IRQ_BANK0` (21), both edges,
`devirq_attach()`; the handler reads TIMER0 and pushes `(level, t_us)` into a
small bounded ring with a drop counter. The service snaps the decoder's
`mark_ms` to the nearest captured edge.
*Deliberately additive:* `drivers/dcf77_decode.c` keeps its millisecond,
sample-driven logic untouched, because its quality score's glitch term is
*sample*-based and is the one measurement that varies continuously with
antenna orientation (phase 17 D5) -- converting the decoder to edge-driven
microseconds would invalidate every D5 number as a baseline for the sake of
precision that P4 shows we cannot use anyway.
*Verify:* the ring's drop counter stays at zero under normal reception; the
spread of mark-to-mark intervals narrows measurably against P0's baseline; and
`dcf-monitor` before and after shows the display's frame cadence unchanged --
if it does not, §3.2's PIO fallback is what that failure means.

**P3b — the GPS/PPS reference (§3.4). DONE, bench-verified 2026-09-02.** A `drivers/gps_pps_rp2350.c` with two
halves that are useful separately:

* **PPS capture**, through P3's own edge ring on a second pin (GP19). One more
  `devirq` source, the same TIMER0 read, the same bounded ring with a drop
  counter. If P3 is built, this is a few dozen lines.
* **An NMEA reader** on UART1 (GP20/21) -- enough of `$GPGGA`/`$GPRMC` to know
  fix quality, satellite count and UTC second. The time is a cross-check
  rather than the product; the *lock* is what gates the calibration, because a
  module without it may still emit a pulse (§3.4).

*Verify:* with the receiver and the GPS both attached, PPS edges arrive at
1.000 s intervals measured on TIMER0 with a spread of microseconds, not
milliseconds -- which is simultaneously a test of the capture path and of the
lock; and NMEA reports a fix whose UTC second matches the second the PPS edge
falls in. A PPS train that is *regular* but disagrees with NMEA by a whole
second is the interesting failure and this check catches it.

**As built, 2026-09-02.** `drivers/edgecap.c` is the shared half, and shared
is the whole architecture: RP2350 has *one* interrupt for all of GPIO bank 0,
so two drivers cannot each attach a handler and the dispatch has to live in
one place. That is why §6 called P3 and P3b one piece of work, and it would
have been discovered the expensive way by building them a month apart.

`drivers/gps_pps_rp2350.c` is the second user. The DCF service is the first:
it registers `CONFIG_DCF77_OUT_GPIO`, keeps a short history of pulse starts,
and snaps a completed frame's mark to the captured edge nearest the
millisecond one the decoder produced. **The decoder itself is untouched and
stays sample-driven** -- its quality score's glitch term is sample-based and
is phase 17 D5's baseline. What changed is only the instant a frame is
stamped with.

Two things were deliberately kept safe. No candidate within half a second
means the capture missed it, and the millisecond mark is used unchanged --
which is the pre-P3 behaviour, so this cannot make anything worse. And only
pulse *starts* are timed on both pins: the other edge carries the pulse width,
a property of the receiver or the GPS module rather than of the second.

Visible without a console, because the board that does this measurement lives
where its reception is: `/proc/gps` reports fix, satellites, the UTC second,
the PPS interval and a trustworthiness verdict; `/proc/dcf77` gains
`mark_from_edge`, `edges` and `edges_dropped`.

**And visible without a *terminal*, which is a different requirement.** The
GPS is a transfer standard that gets carried to a windowsill and moved around
until it locks, and during that hunt nobody is at a console. So the clock
persona lights `CLOCK_IND_COUNTDOWN` for it -- immediately below the DCF and
network lamps, so the three ways this clock can learn the time read as a
group. It borrows the DCF lamp's language rather than the RTC lamp's, because
a sky view is precisely the kind of service that comes and goes: off when
nothing is talking, slow blink while NMEA arrives but the pulse is not yet
trustworthy, solid on fix-plus-pulse, and a fast blink for the one fault a
person can actually act on -- a pin shut off for oscillating. The gate behind
the lamp is `gps_pps_trustworthy()`, the same one a calibration consults, so
the panel reports the software's verdict rather than inviting a person to
form their own.

**A third instrument, added because the first bench session needed it.** When
the byte counter stops moving, "the module lost power" and "our receiver is
wedged" look identical, and the board is on a windowsill rather than next to a
console. So `/proc/gps` also carries `rx_idle_ms` (silence as a duration
rather than a counter someone has to sit and watch), the four PL011 error
counters, and `rx_fr`/`rx_cr` read live. `u_getc()` now reads the *whole*
UARTDR rather than its low byte, counts the error bits that ride along with
each character, and clears the latched copy in UARTRSR -- without that clear
one overrun during boot makes every later reading say "overrun" and the
register stops being evidence of anything.

It paid for itself immediately, on a failure that was entirely ours: counters
frozen, `rx_fr = 0x1c7` -- RXFF set, so the FIFO was *full* rather than empty
and nothing was draining it, while `rx_err_frame = 0` across 401 bytes proved
the baud was right and `rx_err_break = 0` proved the module's TX still idled
high. Not a dead module: the probe script had sent Ctrl-C to get a clean
prompt, and Ctrl-C exits `clock_app_run()`, which is the only caller of
`gps_poll()`. **The console cannot be used to measure this board**, because
touching it stops the loop under measurement; read `/proc/gps` over 9P
instead. A healthy receiver reads `rx_fr = 0x197`, with RXFE set. Note that
`rx_idle_ms` alone does not settle it -- it is computed in `gps_status()` at
read time and keeps advancing whether or not the poll loop runs.

**P3b VERIFIED ON THE BENCH, 2026-09-02.** `state=locked`, nine satellites,
`pps_interval_us` reading 999998-1000001 across repeated samples -- a spread of
about a microsecond against a criterion that asked only for microseconds
rather than milliseconds -- with `pps_dropped=0` and `pps_storms=0`.

**Getting there cost an evening to one wrong constant, and the way it misled
is worth more than the fix.** `edgecap`'s handler acknowledged `0x88888888`,
which is EDGE_HIGH alone, while every other line in the file correctly used
`EDGE_BOTH` (0xC; confirmed afterwards against the RP2350 datasheet, where
EDGE_LOW is bit 2 and EDGE_HIGH bit 3). INTR is write-1-to-clear, so an
unacknowledged falling edge leaves the bank interrupt permanently asserted and
the handler re-entering forever. The first falling edge a pin ever saw wedged
it for good.

What made this expensive is that it *presented as a plausible property of the
hardware*. A 1 Hz pulse looked like a sustained 1 kHz oscillation, and the
reported edge rate corroborated it: `storm_rate` averaged from a window start
that predated the burst by up to a second, so it always landed just above the
2000/sec threshold. Two independent-looking numbers agreed, and both came from
the same bug. On that basis the module was diagnosed as misconfigured, and
UBX-CFG-TP5 was written, acknowledged, saved to flash, and extended to a
second timepulse index -- none of which could have worked, because nothing was
wrong at that end.

Three things broke the loop, in order of how much they were worth:

* **Parsing the module's replies.** `ubx_tp5_sent` counts frames *queued*; it
  had been read as proof of delivery, which it never was. UBX-ACK-ACK is the
  only thing that separates "refused", "never arrived" and "accepted and
  behaved unchanged anyway". Getting `ubx_acks=4, ubx_naks=0` while the pin
  kept oscillating is what proved the fault was local.
* **Asking the owner what was actually wired.** "GP19 is on the module's PPS
  pin; the module has only PPS, RX, TX, GND, VCC" eliminated the last
  hypothesis that let the module be at fault.
* **The user's own observation** that it "goes for 1 second in non-storm mode,
  then back to storm" -- which is the falling edge of a 1 Hz pulse, stated
  plainly, before it was understood.

The rate is now measured over the most recent 64 edges rather than since the
window began, so the number describes the edges that caused the trip. A
measurement that cannot be wrong in a way that looks right is worth more than
a fix.

**Two pin lessons, both learned the hard way and both now structural.** The
first flash of P3b lit the display for a second and then went dark, twice:
GP19 was registered with the edge capture while still in its power-on state,
and a floating input with both-edge interrupts enabled is an interrupt storm
that starves everything else -- the clock display being this board's canary
for exactly that. Two fixes, at different levels. `edgecap.c` now has a storm
guard: a pin exceeding `EDGECAP_STORM_PER_SEC` gets its `INTE` cleared and is
flagged, because a board that cannot be reached to be fixed is a bricked
board, and no driver's wiring mistake should be able to take the system down.
And `gps_pps_rp2350.c` configures the pad *before* arming capture, which is
the actual bug.

The second: **PPS polarity is a board fact** (`CONFIG_GPS_PPS_ACTIVE_LOW`),
configured rather than inferred -- the same call phase 17 made for the DCF
pin. A push-pull `TIMEPULSE` idles low and pulses high; an open-collector one
idles high through a pull-up and pulses low. So "the pulse" is a rising edge
on one module and a falling edge on the other, and guessing wrong does not
produce noise, it produces a timestamp of the *wrong instant* -- the end of
the pulse rather than its start, folding the module's pulse width into a
measurement that has no business containing it. The pad's pull follows the
polarity, so that a disconnected module reads as "no pulse" rather than as a
permanent one, and either way the line has *a* pull, which is what stops an
unconnected pin oscillating in the first place.

**Explicitly not in P3b:** disciplining anything from GPS. The board reads it,
timestamps it, and reports; nothing sets a clock from it. §3.4's transfer-
standard argument is a design constraint, not a preference, and the easiest
way to honour it is for the code that *could* do it not to exist yet.

**P4 — calibrate the constant, against PPS rather than against statistics.
DONE 2026-09-04. `CONFIG_DCF77_DELAY_US = 37886`, +/- 62 us.**

1073 frames over 17.8 hours against a local GPS reference. The three questions
this step existed to ask, answered:

* **Is the delay constant, or does it move with signal strength?** No
  measurable dependence: r = +0.043 across 760 logged samples. That is a weak
  test rather than a null result, and worth saying so -- reception was
  uniformly excellent all night, so the quality score only ever spanned 6.2 to
  7.0 and the independent variable barely varied. It took a fix to ask at all:
  the logged score was a mean accumulated since boot, frozen at a constant 69
  while the measurement's own scatter tripled.
* **Is it stable across a temperature cycle?** Yes, to about +/- 0.6 ms. Two-hour
  block medians run 37383 to 38598 us across the night with no monotonic trend
  -- scatter around a fixed value, not drift.
* **What is the distribution?** Symmetric, which contradicts this section's own
  prediction of a late tail from a slow envelope edge. 5681 us below the median
  against 5752 above; 2537/2864 at the deciles. The tail seen in the first
  hours was a small-sample artefact, and the mean is therefore the better
  estimator after all.

**The cross-check is the strongest result here.** The phase measurement (PPS
alone, saying when a second began) and the full epoch comparison (PPS for the
instant, NMEA for the second) are separate code paths over the same frames,
and they agree to **one microsecond**: +37886 against -37887, with
`gerr_secbad = 0` confirming all 1073 second labels decoded correctly.

The NTP route reads -40.6 ms against those -37.9, so **the ~3 ms systematic
disagreement is in the network path**, not the radio -- as the board's owner
suspected when asking why a local GPS was being checked against a remote
server at all. §P4 wanted a disagreement between methods found here rather
than in P6; it was, and it was attributed.

Supporting health over the same run: 1078 frames accepted of 1079 seen,
128588 edges captured with **zero dropped**, and a WLAN link that stayed up for
17.8 unbroken hours after the scheduler's missing timed sleep was added.



As built: `gps_pps_offset_us()` returns the offset of any instant from the PPS
edge that began its second, refusing to answer when the nearest pulse is more
than half a second away -- at that distance there is no telling which second it
belonged to, and a number wrong by exactly one second is far worse than no
number. `dcf77_service` calls it for each accepted frame whose mark landed on a
real edge, and only then: the millisecond fallback is quantised at 25-35 ms by
the debounce, which is larger than the entire quantity being measured and would
poison the mean rather than merely widen it.

`/proc/dcf77` carries the aggregate (`pps_n`, `pps_mean_us`, `pps_sd_us`,
`pps_min_us`, `pps_max_us`) as sum and sum-of-squares rather than a stored
series, since a spread is wanted and not a history. The per-sample value goes
out through the P0 log as a *trailing* field, appended rather than inserted so
a log spanning the firmware change parses correctly on both sides of it, and
`collect_p0.py` reports the delay with a standard error and the
`CONFIG_DCF77_DELAY_US` it implies. Both routes to the same delay are printed
together on purpose -- the NTP comparison against a network reference and the
PPS comparison against a satellite pulse are independent, and a disagreement
larger than their two uncertainties means one of them is wrong. §P4 wanted that
found here rather than in P6.

Sample rate is one per minute: the receiver is powered from init and the
decoder runs continuously, independent of the sync state machine, so an
overnight run gives ~700 samples. At the ~4 ms of radio jitter P1 measured
that is a standard error near 150 us -- microseconds rather than milliseconds,
which is what this step's verification asks for.

**P4 — calibrate the constant, against PPS rather than against statistics.**
With P1, P2, P3 and P3b in, the constant stops being the mean of a noisy
distribution and becomes a directly measured interval: for each accepted
frame, the DCF mark's timestamp minus the timestamp of the PPS edge that began
the same second. Store it as a board configuration value
(`CONFIG_DCF77_DELAY_US` in `cmake/board-rp2350-clock.cmake`), not a literal in
the driver -- it belongs to one module and one antenna, and a second board with
a second receiver will have a different one.

The microsecond resolution buys questions P0 could not ask, and they are worth
asking while the GPS is attached because afterwards they cannot be:

* Is the delay **constant, or does it move with signal strength?** The
  decoder's own per-second quality score is already recorded; correlating the
  two costs nothing and would show an AGC dependence if there is one.
* Is it **stable across a temperature cycle**, over the same night that
  characterises the crystal?
* What is the **distribution** rather than the mean -- symmetric, or a tail on
  the late side as a slow envelope edge would give?

*Verify:* the constant is reported with an uncertainty in microseconds rather
than milliseconds, and a run with it applied has the calibrated DCF time
agreeing with PPS to within the jitter P4 itself measured. **P0's NTP
comparison is re-run alongside as an independent cross-check** -- two methods
that disagree by more than their stated uncertainties mean one of them is
wrong, and finding that out here is much cheaper than finding it out in P6.

**P5 — discipline, not a nightly step.** The phase's real work. Every accepted
frame is a phase measurement; a run of them is a frequency measurement.
Two-state loop -- phase offset and a ppm correction applied to the
monotonic-to-wall-clock conversion -- with outlier rejection (marginal
reception delivers occasional confidently wrong frames, and a naive mean is
exactly what they destroy), **slewing rather than stepping** once the clock is
roughly right, and an explicit holdover state that keeps serving while growing
its own dispersion. The DS3231 is written *rarely* (hourly, say), not on every
discipline step: it is the power-cut backup, not the clock.
*Verify:* against P0's harness, the clock stays within the measured DCF
standard deviation of the stratum-1 across a multi-day run, including a
deliberate holdover -- antenna disconnected for some hours -- during which the
reported dispersion is checked to grow at the ppm rate P0 measured, and the
error is checked to stay inside it.

**P6 — the NTP server.** `net/ntp_server.c`, `udp_bind(123, ...)`, replying
from the callback the way `udp_echo_cb()` already does. T2 stamped on receipt,
T3 on send, both in µs. Stratum, refid, leap indicator, root dispersion and
the unsynchronised case exactly as §4 states. Gated per persona -- the clock
has a radio and a receiver, the gateway has neither.
*Verify:* `chronyc`/`ntpdate -q` from a Linux host against the board reports a
sane stratum, refid and dispersion; the offset it computes agrees with the
independent P0-harness comparison; and, with the antenna disconnected long
enough, the same client correctly sees the server go unsynchronised rather
than confidently wrong.

**P7 — documentation.** README's networking section gains a "serving time"
part with the *measured* accuracy, not an aspirational one; §3.3 in short form
so the phase-modulation question is answered in the place people will ask it;
and the discipline loop's behaviour described in terms of what a client sees.

---

## 6. Sequencing, and what each is worth alone

P0 first, always -- it is the only milestone that cannot be reordered, because
everything after it is judged against its numbers.

P1 is worth doing **whatever happens to the rest of this phase**: the clock is
systematically slow today, by an amount larger than anything else software can
control, and the fix is an afternoon.

P3 is worth doing on robustness grounds even if the server never ships: it
decouples DCF capture from an application loop that the display and the radio
already contend for, which is the same class of problem phase 22 and 23 exist
to address.

P5 is the phase. P6 without P5 is a server that lies after lunch.

P2 was a prerequisite for P6 and nothing else when this was written. §3.4
changed that: it is now on the path to P4, because a microsecond comparison
cannot be recorded in a millisecond clock. Do it early rather than late -- it
touches a load-bearing file and is cheaper while the tree is quiet.

**P3 and P3b are one piece of work**, and doing them together is what makes
P3b cheap: the edge ring, the `devirq` attach and the TIMER0 read are written
once and used on two pins. Building P3 alone and P3b later means writing the
same mechanism twice with a month in between.

**The GPS is borrowed, and the calibration is the only thing that needs it.**
Everything from P5 onward runs on the constant P4 stores, so the module can go
back on the shelf as soon as P4 is verified. Plan the bench time accordingly:
P2, P3, P3b and P4 want to happen in one stretch while it is attached.

---

## 7. Explicitly not in this phase

* **PZF / phase-modulation decoding** -- §3.3. The limit is the absence of a
  carrier tap on this receiver, not the absence of CPU.
* **PIO-based edge capture** -- §3.2. Held in reserve with a measured trigger,
  not rejected.
* **Leap-second smearing.** The board will step a leap second like everything
  else on the segment. Smearing is a policy that only makes sense when every
  client agrees on it, and nothing here can arrange that.
* **NTS, symmetric-key NTP, or any authenticated time.** Phase 18's threat
  model is inherited unchanged: this segment is trusted, and a time server
  that anyone on it can spoof is exactly as exposed as every other service
  here. Worth saying rather than leaving implied, because time is a more
  attractive spoofing target than most.
* **Broadcast or multicast NTP.** Unicast answers to unicast questions; a
  broadcast server is a different failure model for no gain at this scale.
* **Serving anything beyond the local segment**, and by extension any pool
  membership. This is a local, independent time source for when the internet
  is not there, which is the whole point of it.
* **~~A second reference (GPS) on the board.~~ Revised 2026-09-02 -- see
  §3.4.** The original entry said a GPS would make this "a better clock and a
  less interesting one", and that stands for a GPS the board *depends on*. It
  does not stand for one the board is *measured against*: a transfer standard
  attached for the length of a calibration and then removed leaves the
  appliance exactly as independent as before, and leaves P4's constant a
  measurement instead of a statistic.

  So the line is drawn at dependence, not at presence. **Out of this phase:
  disciplining the clock from GPS, serving time sourced from GPS, or shipping
  a persona that needs one to be correct.** In it: reading one, timestamping
  its PPS, and using it to calibrate the receiver.

  The driver is kept afterwards rather than deleted (user, 2026-09-02). A node
  with sky view and no usable longwave reception is a real future case, and a
  server disciplined by both sources would be strictly better than one
  disciplined by either -- neither of which this phase needs to build to make
  the code worth having.

---

## 8. What to implement from, what to read, and the line between them

**The specifications are on this machine at `~/gith/NTP`** -- `rfc5905.txt`
(NTPv4: protocol, algorithms, and Appendix A's code skeleton) and
`rfc4330.txt` (SNTPv4) -- and **chrony's sources at `~/gith/chrony`** (user,
2026-09-01). The RFCs are the source to implement from, for the licence reason
below and for a better one; chrony's structure maps onto this phase's
milestones almost one for one, and it has already made most of these
decisions carefully with its reasons visible in the code -- so it is worth
reading alongside them, particularly before designing P5.

| RFC section (`~/gith/NTP`) | what it settles |
|---|---|
| 5905 §7, §8 | data structures and the on-wire protocol -- the shared floor under client and server |
| 5905 §11.3 | the clock discipline algorithm, and the PLL/FLL trade §8.1 below reads against this phase's error budget |
| 5905 §12 | the clock-adjust process: slew versus step, and the threshold between them (P5) |
| 5905 Appendix A | the code skeleton, **Simplified BSD**, usable directly where it fits |
| 4330 §5, §6 | client operations (R6, done) and **server operations (P6)** -- what a server must copy, fill and echo |
| 4330 §8 | the kiss-o'-death packet, which R6 already parses and P6 chooses not to send (§4) |

| chrony (`~/gith/chrony`) | what it answers here |
|---|---|
| `refclock.h`'s `RefclockParameters` (`offset`, `delay`, `precision`, `max_dispersion`) | P4's calibration constant is not a special case -- it is what every refclock driver has. The field names are worth borrowing even where the code is not. |
| `refclock.c`, `refclock_pps.c` | how a hardware time source is folded into a discipline loop, and what a PPS-shaped edge is allowed to assert. Closest analogue to P3. |
| `regress.c` | **the one to read first for P5.** Robust regression over a sample history is exactly the phase-plus-frequency estimator this phase needs, and it is where chrony's answer to "occasional confidently wrong samples" lives. |
| `sources.c` | dispersion accounting over time -- §4's "root dispersion grows with holdover" done properly. |
| `local.c` | slew versus step, and the threshold between them. |
| `ntp_core.c` | server-mode packet handling for P6. |

### 8.1 What in RFC 5905 actually applies to a single refclock

Worth settling before P5, so it is not designed as "implement §11":

Most of the system process exists to reconcile **several network peers** --
§10's clock filter over a shift register of eight samples, and §11.2's
selection, cluster and combine algorithms. A DCF-77 receiver is *one* source
delivering *one* sample a minute, so selection, cluster and combine have
nothing to do and should not be built.

What does apply is **§11.3, the clock discipline algorithm**, and it is worth
reading for one design fact it states directly: NTPv4's discipline is a hybrid
of a phase-locked and a frequency-locked loop, because *"a PLL usually works
better when network jitter dominates, while an FLL works better when
oscillator wander dominates"*. Map that onto §2's error budget and it says
something concrete about this board: term 4 (receiver edge jitter, tens of
milliseconds, sample-to-sample) is the analogue of network jitter and is
**large**, while term 5 (crystal wander) is slow and systematic. That argues
for weighting the phase loop conservatively and letting the frequency estimate
accumulate over hours -- which is the same conclusion §5's P5 reaches from the
other direction, and it is reassuring that the two agree.

§12 (clock-adjust) is where slew-versus-step lives, and §13 (poll process)
mostly does not apply either -- the poll interval here is set by the
transmitter, at one frame a minute, not chosen by us.

For P6, the server side is much simpler: **RFC 4330 §6 (SNTP Server
Operations)** states exactly what a server must copy, fill and echo, §5 is the
client half R6 already implements, §8 is the kiss-o'-death packet, and RFC
5905 §7 and §8 give the data structures and the on-wire protocol underneath
both.

### 8.2 The licence boundary, and where it actually falls

chrony is **GPLv2**; LugalOS is **MIT**. The line is not "avoid chrony", it is
narrower and more useful than that:

* **The protocol is not chrony's, and never was.** NTP is
  [RFC 5905](https://www.rfc-editor.org/rfc/rfc5905), SNTP is
  [RFC 4330](https://www.rfc-editor.org/rfc/rfc4330). Packet layout, field
  semantics, the epoch, the 32.32 fixed-point timestamps, the offset and delay
  formulae, stratum and leap-indicator rules, kiss-o'-death codes -- all of it
  is an open standard that anyone may implement. Nothing about reading or
  implementing it touches chrony's licence, and `net/ntp.c` (R6) was written
  from the RFC before this tree knew chrony was on the machine.

* **RFC 5905's own code may be *used*, not merely read** -- which is the fact
  that settles this whole question. Its Code Components (Appendix A's skeleton
  included) are released by the IETF Trust under the **Simplified BSD
  License**, stated in the document's own boilerplate: *"Code Components
  extracted from this document must include Simplified BSD License text as
  described in Section 4.e of the Trust Legal Provisions."* Simplified BSD is
  MIT-compatible, so where Appendix A is the right answer this tree may take
  it directly, carrying that notice. That is a categorically stronger position
  than "read chrony carefully", and it is why the RFC is the *better* source
  here and not merely the safer one -- clean, demonstrable provenance, plus a
  document that explains its own reasoning where optimised C does not.

* **Ideas and algorithms are not the boundary; expression is.** Reading
  chrony to understand how a real discipline loop is shaped -- what state it
  keeps, how it rejects outliers, how dispersion is accounted over time -- is
  ordinary engineering practice and entirely fine. So is arriving at the same
  algorithm afterwards.

* **What is not fine: copying, or transliterating.** Lifting lines, or
  translating chrony's code function-by-function into this tree's style while
  keeping its structure, would relicense whatever it touched. Distinctive
  comments and hand-tuned constant tables are expression too.

**Asked and settled, 2026-09-01: should the chrony checkout simply be deleted,
to remove the question?** No, and the reasoning is worth keeping so it is not
re-asked. Reading GPL code creates no obligation -- copyright covers
expression, not ideas, and having a checkout makes one a *user* of chrony,
which its licence expressly permits. The clean-room isolation this instinct
comes from belongs to adversarial commercial reverse engineering, where the
point is to *prove* independent creation in litigation; it does not map onto
reading a well-known daemon to understand a control loop. Deleting also does
not remove the real risk, which is behavioural and identical whether the
source sits on disk or on a web page. And it has a cost: `regress.c` is the
best available answer to the one genuinely hard part of P5.

The stronger discipline, if one is wanted, is free: **write P5 from §11.3
first, and read chrony afterwards as a review.** Independent creation by
construction, and nothing is given up.

**The one obligation that is real:** if any of Appendix A's code reaches this
tree, its Simplified BSD notice must be carried in that file, and the README's
Third-Party section gains an entry -- beside the Microsoft UF2 tools, which is
the precedent for exactly this.

### 8.3 The practical rule

**Implement from RFC 5905 / RFC 4330 (`~/gith/NTP`), taking Appendix A's code
directly where it fits and carrying its Simplified BSD notice; use chrony's
source to check understanding, not to produce text.**

Where an idea comes from chrony rather than from the RFC, cite it in the
comment as the place the idea came from -- which is exactly what this tree
already does for `ntruchsess/arduino_uip#167` in the ENC28J60 driver, an
independent project whose *finding* is credited without a line of its code
being present.

This is worth the paragraphs. The project accounts for 227 KB of radio firmware
by SHA-256 and argues its blob story in the README; a GPL discipline loop
quietly inside an MIT kernel would be a considerably worse lapse than the one
it takes that much care over.

---

## 9. Risks, and what each would look like

**~~The measured group delay turns out to be large *and* unstable.~~ Did not
happen.** P0 measured a bias of -65.5 ms holding to ±1.4 ms across thirteen
hours and a jitter of about 3.4 ms. This was the risk the phase was most
exposed to and it is retired by measurement.

**~~The crystal turns out to be much worse than ±30 ppm.~~ The opposite
happened**, and it is worth keeping as a lesson rather than deleting: the risk
register anticipated the datasheet figure being optimistic and never
considered it being pessimistic by a factor of sixty-five. A tolerance is a
bound the manufacturer will honour, not an estimate of what a part does, and
this phase spent its first three sections reasoning from one as though it were
the latter. **The measurement is the only thing that settled it, which is
exactly the argument for P0 running first.**

**The new dominant risk: this is one board, one night, one temperature.** An
AT-cut crystal's frequency is parabolic in temperature, and thirteen hours of
stable indoor ambient characterises none of that. Looks like: a summer run, or
the board in sunlight, gives a materially different ppm and the fixed
correction P5 is now scoped around stops holding. Mitigated by P5 keeping the
ability to re-measure rather than baking the constant in, and by re-running P0
in a different season before anything depends on the figure.

**The edge interrupt disturbs the display.** Looks like: `dcf-monitor` shows
flicker after P3 that was not there before, or the ring's drop counter is
non-zero. §3.2's PIO fallback is the answer, and P3's verify step is written
specifically to catch this rather than to discover it later.

**Outlier rejection is harder than it looks.** Marginal reception produces
frames that are confidently wrong rather than obviously wrong -- the decoder's
two-frame pairing already filters the worst, but a phase measurement is more
sensitive than a date. Looks like: the disciplined clock occasionally jumps.
Mitigated by slewing rather than stepping and by a median over a window rather
than a mean; if it persists, the frame's own quality score (already computed,
already per-second) becomes a weight.

**The GPS becomes load-bearing without anyone deciding it should.** The likely
shape is not a decision but a drift: the module is attached for P4, everything
works better with it, and it never comes off -- at which point the phase has
quietly built a GPS clock with a longwave hobby attached. Looks like: P5 or P6
referring to PPS at all. §3.4 and §7 are the guard, P3b deliberately contains
no code that could discipline anything, and P4's verify keeps the NTP
comparison alive as a second method precisely so the GPS is never the only
thing that can validate the board.

**PPS is present but not trustworthy.** A module without lock may still emit a
pulse, and a regular pulse train is not evidence of a correct one. Looks like:
a calibration constant that is stable, plausible, and wrong by a whole second
or by whatever the module's free-running oscillator does. Mitigated by P3b
reading NMEA rather than trusting the edge, and by its verify step checking
the PPS second against the NMEA second rather than only checking the interval.

**The phase produces a good clock nobody queries.** The least technical risk
and worth naming: a stratum-1 server on a home segment is only useful if
something is pointed at it. P7's documentation is what makes it usable, and
`chronyc` against it in P6 is the first client either way.

---

## 10. The next bench session, in order

The board has to come back for P2/P3/P3b anyway (§6: those want one stretch
while the GPS is attached). Three things are queued behind that trip, and the
order matters because the first two are lost the moment it is power-cycled.

**1. Before rebooting it, read what the crash left.** `/proc/kmsg` and
`/proc/ps` over the out-of-band 9P channel on the second CDC port, then `net`.
The klog is RAM and does not survive. See `plan/open_issues.md` -- the board
stopped answering after 1.81 h of P1 logging on 2026-09-02, with no evidence
recoverable afterwards, and a second occurrence should not cost the same
nothing.

**2. Settle the `00:00` display with one command: `i2c scan`.** It separates
the two candidates, which have different remedies:

* **0x68 does not answer** -- the chip is absent, unwired or dead, and the
  panel is showing its seeded default because every read fails.
* **0x68 answers** -- then the chip is alive and has lost its oscillator, and
  the coin cell is the thing to replace. `drivers/i2c_rtc.c` never reads
  status register 0x0F, so OSF -- the flag that exists to say precisely this
  -- is discarded, and the resulting 2000-01-01 00:00:00 passes the driver's
  range check as a valid time.

Both fixes in `plan/open_issues.md` are worth making regardless of which it
turns out to be: honour OSF, and let the clock face fall back to the kernel
clock when the RTC read fails. A board that knew the time to a millisecond and
displayed `00:00` for a day is the argument for the second one.

**3. Install a device key, and do it from the console rather than by
reflashing.** `identity key --generate` persists in the identity record,
touches nothing else, and needs no passphrase. The alternative --
`tools/provision.py --key-generate --uf2` -- writes a *whole* record, so it
would take the board's WLAN credentials with it unless the passphrase is
supplied again, and only the derived PSK's fingerprint is known here. Use the
UF2 path for a board being provisioned from scratch; use the console for one
that is already working.

This became practical on 2026-09-02: `tests/hw/flash.py` now sends Ctrl-C
before deciding a console is dead, so the clock persona's shell -- which sits
behind an application that owns the terminal -- is reachable without guessing.

What the key buys: `/proc/dcf77log` over 9P/TCP, so the board's own
accumulators can be read remotely instead of only inferred from broadcasts
that WiFi drops. During P1 that loss ran around a third of samples. The
accumulators counted them all; nothing could reach them.
