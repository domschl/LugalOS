# Phase 24 — A clock that serves time, and knows how wrong it is

**Status: planned 2026-09-01.** Succeeds `plan/phase19_ip_stack_and_ethernet.md`,
which is concluded: R6 built the NTP *client*, and this phase builds the
inverse -- a Pico-Clock-Green persona disciplined by DCF-77 that answers NTP
queries for the segment, as a local time source that survives an internet
outage.

**Scope.** Make the DCF-77 path as accurate as the signal allows, measure how
accurate that actually is against a GPS-disciplined stratum-1 server on the
same LAN, keep the clock disciplined between syncs rather than set once a
night, and then serve it. The protocol is the small half; §2 is about the
other one.

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

## 3. The three ideas from the proposal, evaluated

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
| our own measurement noise | 3.7 ms sd | not considered |

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

The DCF error was also, after filtering, **negative in every one of 685
samples** (-83 to -48 ms). That is the physically necessary sign -- the path
can only ever deliver time late -- and the unfiltered set contained values up
to **+259 ms**, which is impossible. The delay filter removed exactly the
physically impossible readings without being told any physics, which is the
best evidence available that it selects on the right quantity.

**P1 — hand out the mark timestamp.** Widen `dcf77_take_time()` to return the
decoder's `mark_ms` alongside the civil time; `commit()` uses it instead of
`now_ms`. Removes the ~25-35 ms systematic bias of §1.
*Verify:* P0's harness re-run; the mean offset moves by approximately
`DEBOUNCE_MS` plus half a sample interval, in the predicted direction.
Predicting the size *before* re-measuring is the point -- a fix that changes
the number by an unexplained amount has not been understood.

**P2 — a microsecond wall clock.** `kernel/time.c`'s `g_base_epoch_ms` /
`g_base_mono_ms` become microseconds, with `time_get_utc_us()` /
`time_set_utc_us()` beside the existing calls, which keep working.
`rtc_time_t.ms` stays as it is -- the display, the DS3231 and `date` have no
use for microseconds.
*Verify:* existing suite unchanged (this is a widening, not a behaviour
change); a new selftest asserting round-trip through the µs setters preserves
sub-millisecond values.

**P3 — edge capture on the DCF pin.** `IO_IRQ_BANK0` (21), both edges,
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

**P4 — calibrate the constant.** With P1-P3 in, re-run P0's comparison for a
day. The residual mean is (propagation + receiver group delay + whatever
capture bias is left); store it as a board configuration value
(`CONFIG_DCF77_DELAY_US` in `cmake/board-rp2350-clock.cmake`), not a literal
in the driver -- it belongs to one module and one antenna.
*Verify:* the calibrated DCF time and the stratum-1's agree to within the
standard deviation P0 measured, over a run long enough for that to mean
something.

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

P2 is a prerequisite for P6 and nothing else, so it can be deferred until
then -- but it touches a load-bearing file, so doing it while the tree is
quiet is cheaper than doing it under P6.

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
* **A second reference (GPS) on the board.** That would make it a better clock
  and a less interesting one; the premise here is that DCF-77 is the fallback
  when the network reference is gone.

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

**The phase produces a good clock nobody queries.** The least technical risk
and worth naming: a stratum-1 server on a home segment is only useful if
something is pointed at it. P7's documentation is what makes it usable, and
`chronyc` against it in P6 is the first client either way.
