# Phase 25 — a GPS/PPS stratum-1 server on wired Ethernet

**Status: a note, not a plan.** Written 2026-09-04 while phase 24's
verification run was in progress, so that what that phase cost to learn is
available to the next one rather than re-derived. Nothing here is committed
to; the milestone breakdown comes later.

**Superseded as a target, 2026-09-05 — kept as the source material.** The
server described here is now **phase 29**, on an ESP32-P4 rather than on an
RP2350 with an ENC28J60, reached through
[`plan/phase27_esp32p4_bringup.md`](phase27_esp32p4_bringup.md) (the platform)
and phase 28 (Ethernet). See that document's addendum for the full argument.

The short form is that this note's own §3 made the case: it identified
**software timestamping** as what actually limits precision next, and named
the hard half honestly — *"neither the ENC28J60 nor the W5500 has hardware
transmit timestamping"* — which is what pushed it toward interleaved mode as a
compensation for missing silicon. The ESP32-P4's EMAC has IEEE 1588v2
timestamping in the MAC, both directions. So the platform change does not
merely move this design to faster hardware; it removes the constraint that
shaped it.

Nothing here is discarded. §2 (what phase 24 established), §5 (the method
lessons, which are platform-independent and outlive both boards) and §6 (the
open questions) carry over to phase 29 unchanged. §3 and §4 are the parts the
new hardware rewrites: §4's argument for wired Ethernet over WiFi still holds
and holds more strongly, since the P4's MAC is on-die. What does *not* carry
over is `drivers/edgecap.c` and `drivers/gps_pps_rp2350.c` — the P4 has GPIO
and timer ETM, so PPS capture can happen in hardware with no ISR in the path.

**The RP2350 boards stay exactly where they are.** Phase 24's DCF-77 clock and
the GPS/PPS reference are what phase 29 gets measured *against*. §5 of this
note is the reason: never let the thing under test also be the referee.

## 1. What this is, and what it is not

A second board whose only job is to serve time accurately: **GPS/PPS as the
reference, ENC28J60 wired Ethernet as the transport, no display, no radio.**

**The clock stays DCF-77.** That is not a consolation prize. Phase 24 measured
its receiver's delay at +37.886 ms ± 0.062 and its jitter at 2.03 ms, and a
clock that keeps time to a couple of milliseconds from a longwave carrier,
through a night with no network and no sky, is exactly the appliance it was
meant to be. It also remains the *interesting* one: a leap second announced on
the air arrives in band, which no GPS-fed server gets for free (§4 of phase
24). Phase 25 is a different instrument for a different job, not a replacement.

Out of scope on present evidence: NTS, IPv6, leap smearing, serving beyond a
home segment.

## 2. What phase 24 established that this phase can simply use

* **PPS capture is already good to a microsecond.** `drivers/edgecap.c` timed
  1 Hz pulses at 999998-1000001 us across repeated samples, and captured
  128588 edges in one 17.8-hour run with **zero dropped**. The interrupt path
  is not the limiting factor and does not need rethinking.
* **PPS and NMEA together are a complete local stratum-0 reference**
  (`gps_epoch_us()`): the pulse says exactly *when* a second began and nothing
  about which one, the sentence says which and is far too coarse to say when.
  Either alone is useless; the pairing is the whole trick.
* **A module's timepulse is not a given.** This NEO-M8N arrived emitting 1 kHz
  and acknowledged a 1 Hz `UBX-CFG-TP5` while continuing to do so, which took
  a reply parser to establish. Assume nothing; read back.
* **The discipline loop, the wall clock's ppb correction and slewing, the
  honest-dispersion policy and the NTP server itself** are all source-agnostic
  already. `discipline_feed()` takes an offset and an instant; it has never
  known what a longwave carrier is. Feeding it PPS instead of DCF-77 is close
  to the whole port.

## 3. What actually limits precision next, and it is not the GPS

With the radio's 2 ms of jitter gone, the error budget is dominated by
**software timestamping**. Phase 24's server stamps T2 in the UDP callback and
T3 just before `udp_send()` -- both correct as far as they go, and both on the
far side of a cooperative scheduler, an interrupt path, an SPI transfer to the
ENC28J60 and a whole IP stack. That is tens to hundreds of microseconds of
jitter sitting on top of a reference good to one.

So the phase's real work is probably: **timestamp at the interrupt, not at the
callback.** Capture TIMER0 in the Ethernet RX ISR, carry it up with the frame,
and let the server use it as T2. The same in reverse for T3 is harder --
neither the ENC28J60 nor the W5500 has hardware transmit timestamping -- and
NTP's *interleaved* mode exists precisely for that case: report the previous
packet's actual departure time in the next reply. That is the standard answer
and worth reading RFC 9769 before designing anything bespoke.

**Order matters here.** Building the wired path first and only then measuring
would repeat phase 24's most expensive mistake in a new place: the thing to do
first is measure what the current software path costs, against the PPS that is
already trustworthy, and let that number decide how much of the above is worth
building.

## 4. Why Ethernet, specifically

Not preference -- measurement. WiFi cost phase 24 roughly a millisecond of
added round trip, and rather more than that in incidents: a link that dropped
overnight and could not rejoin (`kernel/sched.c`'s missing timed sleep, found
2026-09-04), a radio whose idle polling starves the clock's display, and a join
failure at the antenna position that was never explained. A wired MAC on SPI0
removes an entire class of them, and the driver already exists
(`drivers/enc28j60_rp2350.c`, phase 19 R4).

The CYW43 also competes for CPU in a way that matters to a timing loop -- see
`rp2350_display_vs_radio_cpu` -- and a dedicated server has no reason to pay
that.

## 5. Lessons that are about method, not about time

These cost the most and generalise furthest.

* **The reference has to be better than the thing being measured.** Phase 24
  judged a microsecond clock against an NTP path carrying ~3 ms of bias and
  5-8 ms of noise, and the interesting part was unresolvable until GPS became
  the yardstick. Decide what the reference is *before* deciding what counts as
  a good result.
* **A measurement that can be wrong in a way that looks right is worse than no
  measurement.** `pps_storm_rate` reported ~2000/sec because that was its trip
  threshold, not because anything ran at 2 kHz; a whole evening's theories were
  built on a number that could only ever have said that. Every derived
  statistic needs asking: what values can this *not* produce?
* **Confirm register bit layouts against the datasheet, never by inference.**
  `0x88888888` where `0xCCCCCCCC` was meant -- EDGE_HIGH without EDGE_LOW --
  left every falling edge unacknowledged and presented as a 1 Hz pulse
  oscillating at 1 kHz. A repeated-nibble mask is invisible in review. See
  `rp2350_datasheets_local`.
* **Instruments before mechanisms.** `/proc/clock`, `/proc/gps`, the UBX ACK
  parser and `gps_err_*` each turned a guess into a fact within minutes of
  existing. The ACK parser in particular ended a chain of wrong theories about
  the user's hardware by proving the module heard us perfectly.
* **A test that asserts a sign will pass on a useless implementation.** The
  first frequency estimator random-walked by 2000 ppb against a 460 ppb
  quantity and passed its selftest, because the test checked that `freq_ppb`
  moved the right way. Assert the recovered *value*.
* **Never let the thing under test also be the referee.** GPS disciplines
  nothing on the clock persona and must not, or the verification proves only
  that a loop agrees with itself.
* **The board lives where its reception is.** Anything only reachable over a
  console cable is unreachable in practice; that is why `/proc` grew
  `pon_asserted`, `out_level` and the rest. It applies equally to a server in a
  cupboard next to a switch.

## 6. Open questions for whoever picks this up

1. What *is* the current software timestamping jitter, measured against PPS?
   Everything in §3 is contingent on that number.
2. Does the ENC28J60's SPI transfer time vary enough to matter, or is it
   constant enough to calibrate out the way the DCF-77 delay was?
3. Is interleaved mode worth implementing, or does a calibrated constant plus
   an honest dispersion get close enough for a home segment?
4. Should this board also serve the clock -- i.e. does the clock become a
   *client* of it, giving a two-source household with the radio as the
   independent check on the satellite? That is the arrangement that would have
   caught phase 24's problems fastest.
