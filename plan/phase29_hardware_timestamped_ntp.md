# Phase 29 — A time server that knows when the packet actually left

**Status: planned, not started. Written 2026-09-16**, the day phase 28 closed,
while the EMAC's behaviour is still fresh and the board is still on the bench.

**Milestone letter: `O`.** A–N and P–Z are spoken for across `plan/`; `O` is
the last free letter, which is a fact about this project's history rather than
about this phase.

## 0. Why this phase exists, and why it is not merely phase 25 on faster silicon

`plan/phase25_gps_ntp_server.md` §3 did the important work already: it
identified what limits precision next, and it is **not** the reference.

> *"With the radio's 2 ms of jitter gone, the error budget is dominated by
> software timestamping. Phase 24's server stamps T2 in the UDP callback and
> T3 just before `udp_send()` — both correct as far as they go, and both on
> the far side of a cooperative scheduler, an interrupt path, an SPI transfer
> to the ENC28J60 and a whole IP stack."*

It then named the hard half honestly — *"neither the ENC28J60 nor the W5500
has hardware transmit timestamping"* — and reached for NTP's **interleaved
mode** as compensation for silicon that was not there.

The ESP32-P4's EMAC has IEEE 1588v2 timestamping **in the MAC, both
directions**. So this phase does not host phase 25's design on a faster part;
it deletes the constraint that shaped it. T2 and T3 stop being "when the
scheduler got round to it" and become "when the frame crossed the MAC".

That is the whole thesis, and it is falsifiable: if O1's measurement shows the
software path already costs less than the accuracy anyone can verify, the rest
of this phase is not worth building and should be abandoned in favour of
`plan/phase25_gps_ntp_server.md` §6's question 3 — *"does a calibrated
constant plus an honest dispersion get close enough for a home segment?"*

**Phase 28 was the prerequisite and it is paid for.** The P4 is a node on a
LAN whose descriptor rings and cache discipline have been proved on real
traffic (3500-frame loopback runs clean, 3000 echoes with zero loss). Phase
29 may touch `EMAC_SYSTEMTIMESECONDS_REG` without wondering whether the driver
underneath it is sound.

## 1. What is inherited, and what is genuinely new

Most of this phase already exists. That is the point of having done 24, 25,
27 and 28 first.

| Inherited, works, source-agnostic | Where |
|---|---|
| The discipline loop, ppb correction, slewing, holdover | `kernel/discipline.c` — `discipline_feed(offset_us, at_mono_us)` "has never known what a longwave carrier is" |
| The NTP server, with the honest-dispersion policy | `net/ntp_server.c` (phase 24 P6) — stratum 1 only while genuinely tracking, LI 3 / stratum 16 once holdover is dishonest |
| UDP, IP, ARP, the netif seam | `net/` (phase 19), unchanged by phase 28 |
| A working EMAC on real traffic | `drivers/emac_esp32p4.c` (phase 28) |
| `/proc/clock`, `/proc/gps`, `/proc/net` as instruments | phases 24, 19, 28 |
| PPS + NMEA as a complete stratum-0 pairing | `gps_epoch_us()` (phase 24) |

| Genuinely new in this phase | Why it is new |
|---|---|
| The MAC's PTP timestamp unit | Never touched; `0x700`–`0x718` is untouched register space |
| Per-frame RX/TX timestamps reaching the NTP server | The descriptors carry them; nothing reads them |
| GPS/PPS input **on the P4** | `drivers/gps_pps_rp2350.c` and `drivers/edgecap.c` are RP2350-only by construction |
| Disciplining a *hardware* clock rather than a software one | See §2.4 — this is the interesting design question |

## 2. The hardware, with provenance for every claim

Same rule as phases 27 and 28: a register value gets a **source**, not an
argument. Phase 28 lost most of a day to a line whose comment reasoned instead
of citing (`plan/open_issues.md`, the RMII drive-strength entry).

### 2.1 The timestamping unit exists on *this* silicon

* `SOC_EMAC_IEEE1588V2_SUPPORTED (1)` —
  `esp-idf/components/soc/esp32p4/include/soc/soc_caps.h:713`.
* The register block is at `EMAC_BASE + 0x700`…`0x718`
  (`TIMESTAMPCONTROL`, `SUBSECONDINCREMENT`, `SYSTEMTIMESECONDS`,
  `SYSTEMTIMENANOSECONDS`, `…UPDATE`, `TIMESTAMPADDEND`) and is **identical in
  `hw_ver1` and `hw_ver3`** — checked because this board is **rev v1.3**
  (esptool: *"ESP32-P4 (revision v1.3)"*), i.e. `hw_ver1` silicon, and phase 28
  established that the two register maps have to be diffed rather than assumed
  equal.

### 2.2 The descriptors already have somewhere to put a timestamp

`drivers/emac_esp32p4.c`'s `emac_desc_t` carries, and has since Z2:

```c
    volatile uint32_t ts_lo;    /* IEEE 1588 timestamp -- phase 29, not read here */
    volatile uint32_t ts_hi;
```

The rings run with `BUSMODE_ATDS` (enhanced, 32-byte descriptors), one
descriptor per 64-byte cache line, so enabling timestamping does not resize or
re-align anything. **This is the cheapest part of the phase and it is already
done.**

### 2.3 The constraint that shapes the verification plan: no PPS output pad

`esp-idf/components/esp_hal_emac/esp32p4/emac_periph.c:46`:

```c
#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) >= 300
    .ptp_pps_idx = EMAC_PTP_PPS_PAD_OUT_IDX,
#else
    .ptp_pps_idx = SIG_GPIO_OUT_IDX,     /* "cannot be connected" */
#endif
```

`SIG_GPIO_OUT_IDX` is IDF's marker for *"this signal has no route to a pad on
this target"*. **On rev < 3.0 — which is this board — the MAC's PTP clock
cannot drive a pulse-per-second output pin.**

That matters because the obvious verification — emit a PPS from the
disciplined PTP clock, capture it alongside the GPS PPS, and scope the
difference — **is not available to us**. O7 has to verify by another route,
and §5 says which. Finding this at planning time rather than at O7 is the
entire reason this section exists.

### 2.4 The addend register is a hardware discipline loop

`emac_hal.c`'s fine-update path computes an addend from a ppb correction:

```
          2^32 * PTPClk(MHz)
 SysClk = ——————————————————
                 addend
```

So the MAC's time base can be **frequency-corrected in hardware at ppb
resolution**, which is the same job `kernel/discipline.c` does in software
today by slewing a software clock. That raises the design question this phase
actually has to answer, and it is not obvious:

* **Option A — discipline the PTP clock.** Feed PPS error into the addend, let
  the MAC's clock *be* the truth, and have the NTP server read it directly.
  Timestamps are then inherently on the disciplined timescale.
* **Option B — leave the PTP clock free-running.** Treat it as a precise
  *interval* counter, keep `discipline.c` authoritative, and convert each
  hardware timestamp into system time through the existing offset/ppb model.

A is cleaner and throws away a working, tested loop. B keeps one timescale and
one discipline implementation, and pays a conversion per timestamp. **The plan
does not pre-judge this**; O5 is where it gets decided, on evidence, and the
decision is a done-condition in its own right the way Z6's was.

### 2.5 PPS input without an ISR

`SOC_GPIO_SUPPORT_ETM` and `SOC_TIMER_SUPPORT_ETM` are both set for the P4
(`soc_caps.h`), so a GPIO edge can be routed to a hardware timer capture with
no interrupt in the path — noted in `plan/phase27_esp32p4_bringup.md` §3 and
deliberately left to this phase. Phase 24 measured its RP2350 ISR path at
999998–1000001 µs and called it *"not the limiting factor"*, so **this is not
urgent**: it is one fewer variable, not a fix for a known problem. O4 may take
the ISR route first and ETM later, and should say which it did.

## 3. The order, which is this phase's largest risk

`plan/phase25_gps_ntp_server.md` §3, verbatim, because it is the rule most
likely to be broken by enthusiasm:

> *"Order matters here. Building the wired path first and only then measuring
> would repeat phase 24's most expensive mistake in a new place: the thing to
> do first is measure what the current software path costs, against the PPS
> that is already trustworthy, and let that number decide how much of the
> above is worth building."*

**O1 produces a number before O2 writes a register.** If that number is
already small compared to what §5's referees can resolve, this phase stops and
says so — which is a successful outcome, not a failure, and cheaper than
discovering it at O7.

Phase 28 supplies a second reason to respect this. Its ~1% frame loss survived
four wrong diagnoses because each was argued rather than measured, and the
lesson recorded in `measure-before-declaring-fixed` applies with more force to
a timing phase: **quote a sample size or do not quote a rate.**

## 4. Milestones

Each is independently useful and independently falsifiable. There is no QEMU
for any of this (§6), so "it builds" is never a milestone.

### O0 — Board facts, the GPS wiring, and the go/no-go

What silicon rev the board is (known: v1.3), what that costs us (§2.3), which
GPIO the GPS PPS will arrive on, and whether a u-blox module is available for
the P4 at all or has to be shared with the RP2350 clock. **The reference
cannot be borrowed from the thing being measured** (§5), so if there is only
one module, that is a scheduling constraint on the whole phase and it is
better known now.

**Done when:** the board file carries the GPS pins with the same provenance
discipline as the EMAC ones, and this document's §2.3 is either confirmed or
corrected against the TRM's own PTP chapter.

### O1 — What does the current software path actually cost?

Measure, on the P4, against PPS: the spread between the instant a client's
request crossed the wire and the instant `ntp_server.c` stamps T2, and the
same for T3. No new mechanism, only instrumentation.

**Done when:** a number with a sample size and a distribution, in this
document, and a decision recorded: continue, or stop here because the software
path is already good enough.

### O2 — The PTP clock runs and can be read

Enable `TIMESTAMPCONTROL`, set `SUBSECONDINCREMENT`, read `SYSTEMTIMESECONDS`
/ `SYSTEMTIMENANOSECONDS`, expose them in `/proc/clock` or beside it.

**Done when:** the counter advances at 1.000 s/s against the existing
monotonic clock over a ten-minute run, with the residual rate error reported
in ppb — a *value*, not a sign (`phase25` §5).

### O3 — Receive timestamps reach the stack

Turn on per-frame timestamping, read `ts_lo`/`ts_hi` out of the RX descriptor,
carry it up with the frame. `netif_t` has no field for this, and inventing one
is a category-D change: argue it in `plan/hardware_seams.md` §2 before making
it, exactly as phase 28 Z6 argued the category-C question.

**Done when:** a timestamp arrives with each frame, is monotonic across a
burst, and its difference from the software stamp matches O1's measurement —
two independent instruments agreeing is the check.

### O4 — GPS/PPS input on the P4

The pulse, captured. ISR first if that is quicker; ETM (§2.5) if it is not
much harder. Plus the NMEA pairing (`gps_epoch_us()`'s trick: the pulse says
*when*, the sentence says *which*).

**Done when:** `/proc/gps` on the P4 reports the same shape phase 24's does,
and the capture-to-capture interval is 1 s ± a stated bound across an
overnight run with a dropped-edge count.

### O5 — Transmit timestamps, and the timescale decision

TX timestamps out of the transmit descriptor, and the §2.4 decision made on
evidence: discipline the PTP clock, or convert from a free-running one.

**Done when:** both halves of an NTP exchange carry hardware timestamps, and
this document gains a section recording which option was chosen and *why the
other was not* — a decision either way is the done-condition, as in Z6.

### O6 — The server uses them

`ntp_server.c` takes T2 and T3 from hardware. The honest-dispersion policy
gets stricter, not looser: dispersion should now reflect a smaller, better
understood error, and must still grow during holdover.

**Done when:** a client sees stratum 1 with a root dispersion justified by
measurement rather than by a constant.

### O7 — Verification against an independent referee

See §5. This is the milestone the phase exists for and the one §2.3 made
harder.

**Done when:** the P4's served time is compared against the GPS stratum-1 at
192.168.178.23 **and** the DCF-77 clock, over a run long enough to quote a
distribution, with the comparison method stated including its own error.

### O8 — Tests and documents

`tests/hw/test_esp32p4.py` grows time tests that skip cleanly with no GPS
attached. `plan/hardware_seams.md` records whatever O3 did to `netif.h`.
`README.md` and the preset description gain the server.

**Done when:** the P4 suite passes at its new total with GPS attached and
skips-with-reason without; QEMU unchanged at 363/363; RP2350 unchanged at
25/25.

## 5. How it is verified, and who referees

`plan/phase25_gps_ntp_server.md` §5, which cost phase 24 several evenings:

> **"Never let the thing under test also be the referee."**

The P4 is the thing under test. The referees are the two instruments this
project already trusts and which share none of its hardware:

* the **GPS stratum-1 server at 192.168.178.23** (`lan_time_references`), and
* the **DCF-77 clock** — an independent physical source, on a different
  carrier, that a GPS fault cannot correlate with.

`plan/phase24_dcf77_precision_and_ntp_server.md` is the reason a two-source
household is worth the trouble: a leap second announced on the air arrives in
band, which no GPS-fed server gets for free.

**The comparison method needs its own error budget.** Comparing two servers
over the network re-introduces exactly the software path this phase is trying
to remove. State the method's uncertainty before quoting a result, or the
number means nothing — and note that §2.3 has denied us the cleanest method
(scoping a hardware PPS out of the MAC against the GPS PPS).

## 6. Explicitly not in this phase

* **PTP as a protocol.** This uses the MAC's *timestamping unit*; it does not
  speak IEEE 1588 messaging, elect a grandmaster, or implement a BMCA.
* **NTS, IPv6, leap smearing, serving beyond the home segment** — inherited
  unchanged from `plan/phase25_gps_ntp_server.md` §1.
* **An interrupt-driven EMAC.** Phase 28 Z6 established the driver is polled
  because `netif_t` forbids blocking in `poll()`. Timestamping reads a
  per-frame latched value, which polling does not disturb. If this phase finds
  it needs an ISR, that is a `netif_t` change and must be argued in
  `plan/hardware_seams.md`, not smuggled into a driver.
* **Interleaved mode.** Phase 25 reached for it to compensate for missing
  silicon. This board has the silicon. RFC 9769 is still worth reading before
  concluding it is unnecessary, but it is not a goal.

## 7. Open questions carried in

From `plan/phase25_gps_ntp_server.md` §6, still open, re-pointed at this
hardware:

1. **What is the current software timestamping jitter?** O1 exists to answer
   it, and everything after O1 is contingent on the answer.
2. Does the RMII/DMA path add variance that a *hardware* stamp still cannot
   see — i.e. is the MAC's stamp taken where we think it is?
3. Is a calibrated constant plus an honest dispersion good enough for a home
   segment, making O5–O6 unnecessary?
4. **Should the clock become a client of this server**, giving a two-source
   household with the radio as the independent check on the satellite? Phase
   25 noted this "is the arrangement that would have caught phase 24's
   problems fastest" — but see §5: a referee that is also a client is no
   longer independent.

## 8. What the next phase inherits

If this works: a stratum-1 server whose error budget is dominated by something
other than its own software, and a P4 whose EMAC is understood down to the
timestamp unit.

If O1 says stop: a measured number proving the software path was never the
limit, which retires a question that has been open since phase 24 — and that
is worth more than a server nobody can tell apart from the one already
running.
