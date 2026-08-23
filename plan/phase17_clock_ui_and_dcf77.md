# Phase 17 — Pico-Clock-Green: on-device UI, indicator LEDs, and a DCF-77 time source

**Status: complete, 2026-08-23.** C1-C3 and D1-D5 all landed and are
hardware-verified on the Pico-Clock-Green board; C6's menu tests run on both
QEMU targets. Section 9 records a boot bug this phase's hardware exposed,
fixed and verified on both RP2350 personas.

Written 2026-08-22 from the user's feature sketch (buttons/menu, day-of-week,
no more auto-alternating temperature, DCF-77 receiver as a new time-source
driver), corrected and extended below.
Successor to [`plan/phase11_pico_clock_green.md`](phase11_pico_clock_green.md),
which delivered the display, the DS3231 read path and LDR auto-brightness and
deliberately deferred exactly the things this phase picks up ("Deliberately out
of scope", "Straightforward future additions").

Target: the `rp2350-clock` persona only (`cmake/board-rp2350-clock.cmake`,
`CMakePresets.json`'s `rp2350-clock` preset) on a Waveshare Pico-Clock-Green
baseboard populated with an **RP2350W** ("Pico 2 W"). Nothing here touches the
`rp2350-chess`, `rv32-nommu` or `rv64-mmu` personas except the shared, opt-in
CMake/config plumbing and the (target-independent) DCF-77 decoder core, which
is deliberately built and tested on QEMU too — see D1.

---

## 0. What changed relative to the user's sketch, and why

The sketch is adopted almost entirely. The corrections and additions are:

1. **"Three buttons ... up/down/enter"** — the board's third button is
   `SET_FUNCTION` (GP2), not "enter"; the vendor firmware treats it as
   *mode/enter* with a long-press second function, and uses UP/DOWN both as
   navigation *and* as direct shortcuts from the idle screen
   (`Pico-Clock-Green.c:198-340`). Kept that shape: **SET = enter/back, UP/DOWN
   = navigate, plus short-press shortcuts from the idle screen**, so the two
   most-wanted actions (temperature, date) need no menu at all. Long-press is
   real on this hardware and worth using (the vendor's own threshold is 300 ms;
   we use 400 ms).
2. **"One menu item could be show temperature for 2 secs"** — kept, but the
   *primary* path for temperature becomes a UP short-press from the idle
   screen. A menu that must be entered to see the temperature would be worse
   than what exists today; a single button press is better.
3. **Temperature accuracy** — the 1–4 °C offset is not a sensor defect that
   more code can fix: the DS3231 reports its own *die* temperature, inside the
   case, next to a self-heating LED matrix and an RP2350. So this phase adds a
   **user-settable calibration offset** (menu item, default −2 °C from a new
   board-file constant) rather than pretending to fix it, and displays °C as
   an integer. A one-decimal display is possible (the DS3231's register pair
   carries 0.25 °C steps and `ziku.h` has a 1-px `.` glyph) but is *not*
   planned: displaying a tenth of a degree that is systematically 2 °C wrong
   would be false precision.
4. **Day-of-week** — done with the board's **weekday indicator LEDs** (the
   vendor's `disp_buf` row-0 bit macros in `define.h`), not with text. That is
   what the hardware is for, it costs no display columns, and it stays visible
   while the digits show something else. The weekday is **computed from the
   date** (Sakamoto) rather than read from the DS3231's day-of-week register:
   that register is a free-running counter the RTC never validates against the
   date, and nothing in this tree writes it today (`drivers/i2c_rtc.c` doesn't
   touch register 3, and `rtc_time_t` has no weekday field). Computing it needs
   no ABI change and cannot disagree with the displayed date. The DCF-77 frame
   *does* carry a weekday, and D1 uses it as a free integrity cross-check
   against the computed one.
5. **DCF-77 `PON`/`OUT`/`VDD` semantics** — `VDD` is 3.3 V for the common
   modules and `OUT` is the demodulated 100/200 ms second pulse, as the user
   guessed. `PON` is a *power/enable* input, on almost all modules **active
   low** ("power on when low"), but polarity and `OUT` polarity both vary by
   module, so the driver **auto-detects both** and a documented 5-minute bench
   probe (D0) settles it before any decode logic is trusted. See §2.
6. **"Switch off display for sync?"** — yes, and the phase plans for it as the
   guaranteed-correct fallback. But the interference analysis in §3 says two
   *cheaper* mitigations should be tried first (a deterministic 1.000 ms row
   period, and 2 mA/slow-slew pads), and it also produces a better idea than
   full blanking: **second-gated multiplexing** — once locked to the DCF second
   grid, run the display only in the 400–900 ms window of each second, where no
   pulse edge can fall. That keeps the clock readable at ~half brightness
   *while* syncing. Full blanking stays as the fallback and as the reference
   measurement.
7. **"Feedback on special LEDs during sync"** — adopted, and the left-hand
   indicator column is the right place: the top indicator (vendor label
   "MoveOn", unused by us) becomes the **DCF status LED**. It also turns out
   that a *statically* lit LED costs nothing in interference (no shifting, no
   PWM — pure DC), which is what makes a visible "syncing" indicator compatible
   with a blanked display. See §3.
8. **Added, not in the sketch:** a `/proc/dcf77` status file; a
   decoder self-test that runs on QEMU (the hard part of DCF-77 is the frame
   decoder, and it should not be debugged by flashing a board and waiting for
   minutes of radio); a manual time-set menu (the clock persona has no SD card
   and, if the console is not attached, `date` is unreachable — today a
   never-set DS3231 can only be fixed over UART/USB); and an honest note that
   **there is nowhere to persist settings on this persona** (§5).

---

## 1. The board's remaining resources (checked, not assumed)

Pin budget for `rp2350-clock` as it stands after phase 11, from
`cmake/board-rp2350-clock.cmake`, `~/gith/pico-clock-green/define.h` and the
Pico 2 W pinout:

| GPIO | Header pin | Used by today | This phase |
|---|---|---|---|
| GP0/GP1 | 1/2 | UART0 console + 9P | unchanged |
| GP2 | 4 | *free in LugalOS* — baseboard `SET_FUNCTION` button | **C1: SET button** |
| GP3 | 5 | *free* — baseboard `SQW` (DS3231 square-wave out) | documented in a comment, no config key (C1) |
| GP4/GP5 | 6/7 | free, not on the baseboard | — |
| GP6/GP7 | 9/10 | I2C1 → DS3231 | unchanged |
| GP8 | 11 | free | — |
| GP9 | 12 | `CONFIG_LED_EXT_GPIO` heartbeat (no LED fitted) | unchanged |
| GP10-13 | 14-17 | display CLK/SDI/LE/OE | unchanged |
| GP14 | 19 | *free* — baseboard `BUZZ` buzzer | **C3: buzzer (optional)** |
| GP15 | 20 | *free* — baseboard `DOWN` button | **C1: DOWN button** |
| GP16/18/22 | 21/24/29 | display A0/A1/A2 row address | unchanged |
| GP17 | 22 | *free* — baseboard `UP` button | **C1: UP button** |
| GP19/20/21 | 25/26/27 | free | — |
| GP23/24/25/29 | — | **CYW43439 wireless** on the W (WL_ON/WL_D/WL_CS/WL_CLK) | see note |
| GP26 | 31 | LDR (ADC0) | unchanged |
| GP27 | 32 | free (ADC1) | **D2: DCF-77 `OUT`** |
| GP28 | 34 | free (ADC2) | **D2: DCF-77 `PON`** |

**Note on GP25 / `CONFIG_LED_ONBOARD_GPIO`.** On a Pico 2 **W** the user LED is
on the wireless module (`WL_GPIO0`), and GP25 is the CYW43439's chip select.
`drivers/uart_rp2350.c` currently sets GP25 as an output at init
(`LED_MASK`, `uart_rp2350.c:83`) even on this persona. It is only *driven* at
init and the wireless module is unpowered (`WL_ON` low), so this is harmless
today — but it is wrong, and it is a chip-select line being driven next to a
radio receiver we are about to add. **C1 drops `CONFIG_LED_ONBOARD_GPIO` from
the clock board file** (the heartbeat keeps GP9) rather than leaving a
misleading fact in a board file. Small, but exactly the kind of thing that
should not be discovered later while chasing DCF-77 noise.

**No spare pins are needed beyond GP27/GP28**, both of which are on the header
edge farthest from the display's GP10-13. Whether they are *physically*
reachable on an assembled Pico-Clock-Green (whether the Pico is socketed or
soldered, and whether the baseboard leaves the pads accessible) is the one
mechanical unknown — **verify before ordering a receiver**; GP19/20/21
(pins 25/26/27) and GP4/GP5/GP8 are equivalent electrical alternatives, and the
pin numbers live in the board file precisely so this is a one-line change.

---

## 2. DCF-77 receiver: what the four wires are, and how we find out for certain

The common modules (Pollin/HKW DCF1-style, the "DCF77 receiver + ferrite
antenna" breakouts, Canaduino/CANADUINO) present exactly the four pins the user
lists:

| Pin | Meaning | Plan |
|---|---|---|
| `VDD` | supply, typically 1.2–3.6 V for the bare receiver IC, 3.3 V for the breakouts (some 5 V-only variants exist) | **3V3(OUT), header pin 36.** If the module is marked 5 V-only, `OUT` may swing to 5 V — RP2350 GPIO is **not** 5 V tolerant, so a 2-resistor divider (10k/20k) or a 5 V→3.3 V level shifter is mandatory. Check the module's marking first. |
| `GND` | ground | header pin 38 (digital GND, **not** pin 33/AGND — AGND is the LDR's reference) |
| `OUT` | demodulated second pulse: the carrier is attenuated to ~15 % for 100 ms (bit 0) or 200 ms (bit 1) at the start of every second; second 59 has no pulse at all (minute mark). Often push-pull, sometimes open-collector, and **sometimes inverted** | GP27 (pin 32), input, Schmitt on (RP2350 pad default), internal pull-up enabled (harmless for push-pull, required for open-collector). If the module is open-collector and the lead is long, fit an external 10 kΩ to 3V3 — the internal ~50 kΩ pull-up is a good antenna for the very noise we are fighting. |
| `PON` | receiver enable / power-down. **Active low on nearly every module** ("power on when low"); a few are active high | ~~GP28 (pin 34)~~ — **not wired** (2026-08-23). The active receiver actually fitted has no enable input; its reference designs tie the corresponding pin to ground. `CONFIG_DCF77_PON_PRESENT=0` turns the whole path off: nothing is driven, no GPIO is reserved (GP28 is free again), and the warm-up wait shrinks to whatever has not already elapsed since boot — normally nothing, since the module has been running the whole time. The driver keeps the PON code for a module that does have one. |

**D0's bench probe settles both polarities in five minutes, before any decoder
exists** (see D0). The driver additionally auto-detects `OUT` polarity at
runtime — over any 4-second window the "pulse" state is the *minority* state
(≈15 % duty), so if the level we treat as active is high for more than half the
time, we invert and say so. `PON` polarity gets the same treatment: try the
assumed-active level, and if no edge at all arrives within the module's stated
settling time, flip it once and retry before declaring failure. With
`CONFIG_DCF77_PON_PRESENT=0` that second attempt is skipped — it would drive
nothing and re-measure the same pin — and `(dcf-hunt)` / `(dcf-poweron)`, which
are PON experiments and nothing else, refuse to run and say why.

**Warm-up.** Receivers need time after power-up before the AGC settles and the
output is meaningful — datasheets quote anywhere from ~1 s to ~30 s. The sync
state machine treats the first `CONFIG_DCF77_WARMUP_MS` (default 5000) after
asserting `PON` as "ignore everything", and D5 measures the real number for the
actual module rather than keeping the guess. With PON absent the count runs
from **boot**, not from the start of the sync, so in practice there is nothing
to wait for.

**Antenna.** The ferrite rod is a *magnetic* pickup, and the strongest field
near it will be the LED matrix's own row currents. Mount it on its lead as far
from the panel as the case allows, with the rod axis **horizontal, broadside to
Mainflingen** (a ferrite rod is directional: maximum signal when the rod is
perpendicular to the direction of the transmitter) — and, secondarily,
perpendicular to the display's current loops. Orientation is worth more than
any code in this document; D5's live signal monitor exists so this can be tuned
by hand in a minute instead of guessed.

**Aiming it, with actual numbers** (worked example for Munich; the method is
the same anywhere). A ferrite rod receives off its *broadside* — the sides of
the rod — and has deep nulls off each end, so the rod is aimed by pointing it
**across** the path to the transmitter, not along it:

| | |
|---|---|
| Transmitter | Mainflingen, near Frankfurt (77.5 kHz, DCF-77) |
| True bearing from Munich | **≈ 319°** (north-west) |
| Magnetic declination, southern Germany | ≈ 3.5–4° east |
| Compass heading to walk to | **≈ 315–316°** |
| Rod axis | perpendicular to that: **≈ 49° / 229°**, i.e. the SW–NE line |

So: lay the rod horizontally along the south-west/north-east axis, which puts
its broadside facing 319°. Rotating it towards 315° itself is the *worst*
case, not the best — that is the null, and it is a sharp one, which is why an
antenna that "should" work sometimes reads as a dead module.

Two practical consequences. A rod pointed into its null looks exactly like no
signal at all, so orientation should be ruled out before wiring is suspected.
And the null is sharp enough to be useful in the other direction: rotating
*into* it and back out is a quick confirmation that what you are receiving is
really Mainflingen and not a local interferer, which will not null out the
same way.

### Wiring diagram

```
   Pico 2 W (on the Pico-Clock-Green baseboard)          DCF-77 module
   ┌──────────────────────────────┐
   │ 32  GP27  (input) ───────────┼──────┬────────────►  OUT   (100/200 ms
   │                              │      │                    second pulse)
   │                              │     10k  ← only if the module's OUT is
   │                              │      │     open-collector
   │ 34  GP28  ── FREE            │      │
   │            (PON not wired)   │      │             PON   (no connection:
   │                              │      │                    tied to GND on
   │ 36  3V3(OUT) ────────────────┼──────┴────────────►  VDD   the module)
   │ 38  GND ─────────────────────┼───────────────────►  GND
   └──────────────────────────────┘
        ▲ header edge farthest from GP10-13 (display CLK/SDI/LE/OE)

   Already in use on the baseboard (phase 11 + C1):
     GP2  SET button      GP10 CLK   GP16 A0     GP26 LDR (ADC0)
     GP15 DOWN button     GP11 SDI   GP18 A1     GP6/GP7 I2C1 → DS3231
     GP17 UP button       GP12 LE    GP22 A2     GP14 buzzer
     GP3  DS3231 SQW      GP13 OE    GP0/1 UART console

   Ferrite antenna: on its lead, as far from the LED panel as the case
   permits, rod axis horizontal and broadside to the transmitter -- from
   Munich that is a rod lying along 49/229 deg (SW-NE), broadside facing
   319 deg. See "Aiming it, with actual numbers" above.
   If the module is a 5 V type: OUT → 10k/20k divider → GP27. RP2350 is
   not 5 V tolerant.
```

---

## 3. Interference: the real analysis, and what to do about it

The user's instinct ("the 77 kHz signal is probably heavily disturbed by
display multiplexing in the same frequency domain") is right about the risk and
worth being precise about, because the precision produces cheap fixes.

**What actually radiates.** Three sources, in descending order of expected
coupling into a ferrite rod:

1. **The LED row currents.** Up to 24 columns × one row switched on and off at
   the row rate, through the panel's own loops — tens of mA switched, a large
   loop area, right next to the antenna. This is a magnetic near-field source
   and the ferrite rod is a magnetic pickup. It dominates.
2. **The `OE` dimming pulse.** In dim conditions the driver already chops `OE`
   with a 330 µs pulse inside each row period (`DIM_ON_TIME_US`) — this is
   amplitude modulation of source (1) at the row rate, which spreads its
   spectrum rather than concentrating it.
3. **`CLK`/`SDI`/`LE` edges.** 32 bit-banged clock cycles per row, a burst a
   few microseconds long, repeated at the row rate. Broadband, but small loop
   area and small current.

**The frequency-domain fact that matters.** A signal repeating at exactly
*f<sub>row</sub>* has energy only at integer multiples of *f<sub>row</sub>*. A
DCF-77 receiver's front end is a quartz/ceramic-filtered narrowband channel —
tens of Hz wide around 77.5 kHz. So:

* At a **1.000 ms** row period the harmonics land at 77.000 kHz and
  78.000 kHz — **500 Hz either side of 77.5 kHz, outside the passband.** This
  is close to the best case available.
* At a **2 ms** row period the 155th harmonic lands on **77.500 kHz exactly** —
  the worst possible choice. Likewise 4 ms (310th), 0.4 ms, 1/155 of any
  multiple of 77.5 kHz.
* The current loop runs `pico_clock_green_scan_step(); time_delay_us(1000);`
  (`drivers/pico_clock_green_rp2350.c`), so the *actual* period is
  1000 µs **plus** however long the scan step took — call it 1.02–1.06 ms, i.e.
  *f<sub>row</sub>* ≈ 950–980 Hz, and its 79th–81st harmonic wanders in the
  76–78 kHz region and *drifts* (the I2C read once per second briefly stretches
  one period, smearing the comb). That is exactly how energy ends up in the
  passband.

**Mitigation ladder, cheapest first.** Each step is independently measurable
with the D3 signal monitor, and D5 records which ones were actually needed
rather than applying all of them on faith:

| # | Mitigation | Cost | Effect |
|---|---|---|---|
| M0 | **Force the Pico's own SMPS out of power-save mode** — see below. Upstream of everything else here, and on the evidence the single biggest factor for this module | one GPIO write on a Pico 2; **not reachable on a Pico 2 W** | Removes variable-frequency buck ripple that lands in the 77.5 kHz band |
| M1 | **Deterministic 1.000 ms row period**: deadline scheduling (`next += 1000; delay_until(next)`) instead of "do work, then sleep 1000 µs", so the comb sits on exact integer kHz and stops drifting | ~5 lines, no visible change | Moves the comb 500 Hz off the passband and stops it wandering into it |
| M2 | **2 mA drive, slow slew on CLK/SDI/LE/OE** (`PADS_BANK0` DRIVE=0, SLEWFAST=0 — the driver writes `0x5A` = 4 mA today; `0x4A` is 2 mA) | 1 constant | Cuts the edge-rate broadband content of source (3); no downside on 5 cm traces |
| M3 | **Antenna placement/orientation** (§2) | free, manual | Usually the single biggest factor |
| M4 | **Second-gated multiplexing**: once the second grid is known, scan only during **t+400…t+900 ms** of each second and hold the panel dark (`OE` closed, nothing shifted) across each pulse window | moderate — one flag in the scan step, driven by the decoder's phase | Display stays readable at ~50 % duty *while* syncing; receiver sees a quiet channel exactly when the edges matter |
| M5 | **Full quiet sync**: display completely dark for the whole sync (up to ~5 min), one **statically** lit indicator LED (row address set, one column bit shifted once, `OE` held open — DC current, zero switching, zero interference) as the "syncing" sign | small | The guaranteed-clean reference. Always available as the fallback and as the control measurement for M1/M2/M4 |

Note M5's trick is what makes "show the user something while the display is
off" possible at all: every indicator LED on this board is on the same
multiplexed shift register as the digits, so the only way to light one without
switching noise is to stop scanning and leave exactly one row latched.
Advancing that LED once per second (a single 32-bit shift burst, a few µs) can
be scheduled inside the quiet-safe window, giving a slow progress animation
that still costs essentially nothing.

**M0, found 2026-08-22 while identifying the module, and it may matter more
than the whole display question.** The Pico's on-board buck-boost regulator
(RT6150) runs in PFM/power-save mode at light load, producing variable-frequency
ripple on 3V3 rather than a clean fixed-frequency one. A Raspberry Pi Pico user
running this exact RC8000 module scoped it at roughly 50 kHz on a plain Pico and
10 kHz on a Pico W, and **got no DCF-77 reception at all until forcing the SMPS
into PWM mode** ([forum thread][rc8000-forum]). Our receiver is powered from
that same 3V3 rail and sits centimetres from that same inductor.

The catch, verified against the SDK's own board headers rather than assumed:

* `pico-sdk/src/boards/include/boards/pico2.h`: `PICO_SMPS_MODE_PIN 23` — on a
  plain **Pico 2**, driving GP23 high forces PWM mode. One register write.
* `pico2_w.h`: no such pin. Instead `CYW43_WL_GPIO_SMPS_PIN 1`, with GP23
  reassigned as `CYW43_DEFAULT_PIN_WL_REG_ON`. On a **Pico 2 W** the SMPS mode
  control hangs off the *wireless chip's* GPIO 1 — unreachable without a CYW43
  driver, which this tree does not have and which section 7 already lists as a
  separate, substantial phase.

So on a Pico 2 this is a one-line fix; on a Pico 2 W (which phase 11 records as
the populated part) it is not available at all, and the alternatives are:
**raise the load** so the regulator leaves PFM on its own — amusingly, running
the LED matrix may *improve* reception rather than harm it, inverting this
section's whole assumption and making it a thing to measure rather than assume;
**filter the module's own supply** (an RC or LC into its VDD, or a small
reservoir cap at the module); or **power the module separately**, from a battery
or a linear regulator, which also removes the coupling path entirely. No code is
added for M0 yet: it has no consumer until D5 measures reception, and this tree's
own rule is not to write facts into a board file before something reads them
(phase 11 L1).

**AGC caveat, stated up front so the measurement is read correctly:** receivers
have slow automatic gain control, so noise bursts *outside* the pulse window
still degrade the pulse shape *inside* it. M4 may therefore underperform its
duty-cycle arithmetic. That is a prediction to test in D5, not a reason to skip
it.

**Expected outcome (to be falsified in D5):** nightly quiet sync (M5) works;
M1+M2+M3 make live sync work often enough to be worth offering; M4 lands
somewhere in between. The default policy ships as *"nightly quiet sync, plus a
live sync the user can ask for"* and gets revised to whatever D5 measures.

---

## 4. Milestones

### Part A — on-device UI

#### C1 — Buttons, indicator LEDs, weekday display *(landed 2026-08-23 — builds on all four presets, 259/259 QEMU tests; NOT yet hardware-verified)*

* Board file: `CONFIG_CLOCK_BTN_SET_GPIO=2`, `CONFIG_CLOCK_BTN_UP_GPIO=17`,
  `CONFIG_CLOCK_BTN_DOWN_GPIO=15`, `CONFIG_CLOCK_BUZZER_GPIO=14`,
  and **no** `CONFIG_CLOCK_SQW_GPIO`: GP3 carries the DS3231's square-wave
  output and nothing here reads it, so it stays a comment in the board file
  rather than a config key, per phase 11 L1's own "no facts without a
  consumer" rule (it earns a key when something — a 1 Hz display tick, or a
  hardware second reference for the DCF decoder — actually consumes it).
  Forward the new keys through
  `cmake/gen_config.cmake`'s `_optional_keys` and `/proc/config`
  (`fs/vfs_server.c:722`), the same way the `CONFIG_CLOCK_*` display pins
  already are. Drop `CONFIG_LED_ONBOARD_GPIO` (see §1's GP25 note).
* Buttons are **polled inside the existing ~1 ms scan loop**, not from a new
  task: the cadence is already perfect, the GPIO registers are already in the
  clock task's hands, and a second task polling the same SIO window would
  duplicate a register grant for no benefit. Debounce 20 ms; classify
  short (< 400 ms) vs. long (≥ 400 ms) on release, plus auto-repeat at 5/s
  after 600 ms of hold for UP/DOWN (needed for the time-set screens).
* An 8-entry event ring, drained by the appliance loop, and exposed on the
  wire as one new op (`'K'` — pop one event, non-blocking) so a diagnostic can
  read buttons without owning the display.
* Indicator/weekday LEDs: port the vendor's `disp_buf` bit macros
  (`define.h:88-131`) **literally**, as named helpers rather than macros:
  `clock_set_weekday(1..7)` (row 0 of groups 0/1/2) and
  `clock_indicator(IND_DCF|IND_ALARM|IND_COUNTDOWN|IND_C|IND_F|IND_AM|IND_PM|IND_COUNTUP|IND_CHIME|IND_AUTOLIGHT, on)`
  (columns 0-1 of group 0, rows 0-7). The top indicator, vendor-labelled
  "MoveOn" and meaningless to us, is relabelled **DCF** in our code and
  documentation.
* Weekday from the date via Sakamoto's algorithm (§0.4), lit continuously on
  the idle screen.
* Diagnostic before any UI exists: `(clock-keys secs)` Lisp primitive printing
  each event to the console — so C1 is hardware-verifiable on its own.
* **Verify on hardware:** each button, short and long, repeat; every weekday
  LED; every indicator LED; and that adding the poll to the scan loop causes no
  visible flicker (it adds ~1 µs to a 1000 µs period — but phase 11's own
  history is that display timing regressions are found by looking, not by
  arithmetic).

**As landed**, with the deviations from the above spelled out:

* Board file gained `CONFIG_CLOCK_BTN_SET_GPIO=2`, `_UP_=17`, `_DOWN_=15`,
  `CONFIG_CLOCK_BUZZER_GPIO=14`, forwarded through `gen_config.cmake` and
  `/proc/config`. GP3/SQW stayed a comment, as planned.
  `CONFIG_LED_ONBOARD_GPIO` is **gone** from this board file; `uart_rp2350.c`
  and `/proc/config` treat the key as optional, so GP25 is no longer driven on
  a Pico 2 W.
* Buttons are active LOW with the internal pull-up, and the buzzer active
  HIGH — both read out of the vendor source rather than assumed, and cited in
  the board file. The buzzer pin is declared but **not driven**: it has no
  consumer until C3.
* Press classification happens on **release**, so a gesture produces exactly
  one event: hold SET for a second and you get one `LONG` when it comes back
  up, never a `SHORT` on the way there. A press that auto-repeated reports no
  short/long at all on release, for the same reason.
* Auto-repeat is UP/DOWN only. SET's long press is a distinct command
  (back/exit) and repeating it would fire that command over and over while the
  finger is still down.
* The 8-entry ring drops the **newest** event on overflow, not the oldest, so
  what remains is a prefix of what happened rather than a hole in the middle
  of it; the drop count is reported by `(clock-keys)`. Overflow needs a
  consumer that has stopped draining — nothing a finger can do reaches it at a
  1 ms drain.
* Wire ops: `'K'` pop-key (a poll, never a wait — blocking there would
  deadlock against the loop that produces the events), plus `'W'` weekday,
  `'I'` indicator, `'B'` key test and `'D'` LED walk. Every public entry point
  routes through the task when it is alive and falls back to direct hardware
  when it is not, matching `pico_clock_green_run()`; the task-side code calls
  `_hw_` variants so nothing ever `chan_call()`s its own endpoint.
* Indicator and weekday state lives **outside** the frame buffer and is
  re-applied by `pico_clock_green_clear()`. Otherwise every minute tick would
  wipe the weekday LEDs, since redrawing the digits clears the whole buffer.
  Glyphs cannot collide with them: `draw_glyph()` writes rows 1-7 only and
  preserves the low bits of the first group, and every layout starts at
  column 2 — which is what the vendor's `disp_offset` of +2 was always for.
* The weekday LED is lit on the idle screen from `time_weekday()`
  (kernel/time.c, sharing the calendar arithmetic the UTC clock and the DCF-77
  decoder now both use — `dcf77_weekday_from_date()` delegates to it rather
  than carrying a second copy).
* Diagnostics: `(clock-keys [secs])` prints the idle pin levels first and then
  every event with its held time; `(clock-leds)` walks all seven weekday LEDs
  and all ten indicators by name. Both drive the row scan while they run.
* The appliance loop polls the buttons and **discards** the events for now,
  rather than letting the ring fill — C2/C3 replace the discard with the
  idle-screen shortcuts and the menu.

#### C2 — Font, text rendering, and the idle screen *(landed 2026-08-23 — builds on all four presets, 259/259 QEMU tests; NOT yet hardware-verified)*

* `drivers/pico_clock_font.c`: a 5×7 font in **flash** (`const`, `.rodata` —
  zero `.bss`) covering `0-9 A-Z . - : ° / space`, in the same row-wise,
  LSB-is-leftmost format `GLYPH_DIGIT[]` already uses, with a per-glyph width
  table so text is proportional (4 px for `1`/`I`, 5 px normal, 2 px for `:`).
  `ziku.h` only has `A-F H L N P U`, so the rest is authored here.
* `draw_text(col, str)` with clipping, and `scroll_text()` for labels wider
  than the 24 visible columns (~60 ms/column).
* **Remove the automatic time↔temperature alternation** (the user's first
  request): the idle screen is `HH:MM` plus the weekday LEDs, and nothing
  changes on its own except the digits.
* Idle-screen shortcuts: **UP short** → temperature for 2 s (with the `°C`
  indicator lit) → back; **DOWN short** → `DD.MM` for 2 s, then `YYYY` for
  1.5 s → back. Any further press returns immediately.
* Temperature calibration: `CONFIG_CLOCK_TEMP_OFFSET_C` (default `-2`), applied
  in the display path only — `i2c_rtc_read_temperature_c()` keeps returning the
  raw die temperature, because a driver that silently returns a fudged sensor
  reading is a trap for every other caller.
* Optional 1 Hz colon blink, **default off** (the user's stated preference is
  for a calm display; the setting exists for whoever disagrees).

**As landed:**

* `drivers/pico_clock_font.c` + a driver-private `pico_clock_font.h`. All
  `const`, so ~360 bytes of `.rodata` and zero `.bss`. Digits, colon, minus
  and the combined °C glyph are the phase 11 originals, unchanged — the clock
  face's fixed column layout was validated against them. A-Z and the
  punctuation are authored here (ziku.h only ever had `A-F H L N P U`).
  Letters and digits are 4 columns, M and W five, `I` three, `:`/`.` two; `~`
  is the degree sign, since ASCII has none and the panel has no code page to
  negotiate.
* `clock_font_render()` produces a **column-major** bitmap — one byte per
  column, bit `r` = row r+1 — which is the transpose of the frame buffer's
  layout, on purpose: scrolling is then "blit a window at an offset", a slice,
  rather than a shift of all 32 buffer bytes per step. `draw_cols()` accepts a
  negative destination, so both ends of a scroll are the same expression as
  the middle, with no special case.
* Text writes only columns **2-23** — the panel is 24 wide and the first two
  are the indicator LEDs — so the LEDs are never disturbed by a redraw and
  never need re-applying. Scrolling runs at 60
  ms/column, the vendor's own pace.
* The automatic time↔temperature alternation is **gone**. Idle is `HH:MM` plus
  the weekday LEDs and changes only when the minute does. UP short →
  temperature (with the `C` indicator lit) for 2 s; DOWN short → `DD.MM` for
  2 s then `YYYY` for 1.5 s. **Any** press returns to idle immediately — not
  just the key that opened the screen, because waiting out a timeout to get
  the clock back is what makes an appliance feel broken. Long and repeat
  presses are ignored, and SET does nothing at all: it belongs to C3's menu
  and should not first learn a behaviour that has to be unlearned.
* `CONFIG_CLOCK_TEMP_OFFSET_C` (default −2) and `CONFIG_CLOCK_COLON_BLINK`
  (default 0) are board keys, forwarded to `/proc/config`. The offset is
  applied in the display path only.
* Diagnostic: `(clock-text "..." [secs])` renders any string, centred if it
  fits and scrolling if it does not, and reports its width on the console —
  because whether a letter is legible on this panel is not something a build
  can answer.

**Corrected after the first hardware run (2026-08-23):**

* The text area is **22 columns (2-23)**, not 24. Found by `DD.MM` losing its
  last column: a 22-column string centred in a claimed 24 starts at 3 and ends
  at 24, and column 24 is not on the panel. The existing clock face had been
  saying so all along — its rightmost digit is at column 20 and is 4 wide,
  ending at exactly 23.
* Letters went from 5 columns to 4 (M and W excepted). At five, every
  four-letter label in C3's menu — `TEMP`, `DATE`, `SYNC`, `LAST`, `AUTO`,
  `TSET`, `OFFS` — came to 23 columns against 22 available. One column, and it
  would have made every short label scroll, which is not a short label. They
  are 19-20 columns now, and matching the digits' width puts mixed strings on
  a common pitch. `12/24` is 24 and still scrolls; C3 should label that one
  `24H`/`12H` instead.
* **The DS3231's year was wrong, and it was our bug, not the chip's.** A clock
  the radio had just set correctly displayed `23.08` and `2055`. The cause was
  in `drivers/i2c_rtc.c`: `i2c_rtc_write_time()` `memcpy()`'d a native
  `rtc_time_t` onto the "i2c" channel, while the U-mode server decodes those
  bytes by hand with a **big-endian** `get_u16()` for the year. 2026 (0x07EA)
  arrived as 0xEA07 = 59911, and `(uint8_t)(59911 - 2000)` is 55 -- written as
  BCD 0x55 and read back as 2055. Month and day, being single bytes, were
  right the whole time, which is exactly what made it look like a bad chip.

  The read direction was broken more quietly: the server replies with 1+9
  bytes and the client demanded `1 + sizeof(rtc_time_t)` = 1+10, so the check
  *never* passed and every RTC read silently fell through to direct hardware
  access from whatever task asked. It worked, so nobody noticed -- but it
  meant the display task was driving the I2C bus itself and the i2c task was
  serving nothing.

  Both directions now go through an explicit nine-byte codec
  (`rtc_to_wire`/`rtc_from_wire`) shared by the client and the kernel-mode
  server, matching the U-mode server's existing layout. The same class of
  mistake is already recorded in the other direction on the EEPROM path
  (`i2c_usys_put_i32()`'s comment). A wire format is a format, not a struct.
* `printk()` never parsed the `-` flag, so `%-4s` printed itself literally
  instead of left-justifying -- found by `(clock-keys)` printing its own
  format string. Fixed in `kernel/printk.c`: `-` and string width now work for
  `%s`, `%c`, `%d`, `%u`, `%x`.
* With PON unwired, "Receiver powered down (PON released)" was describing a
  wire that is not there. One `powered_down_note()` decides that wording now,
  and `(dcf-pins)` says up front that GP28 is a free pin rather than the
  receiver's enable input.

#### C3 — Menu state machine *(landed 2026-08-23 — 46 menu cases green on both QEMU targets, 261/261 suite; NOT yet hardware-verified)*

* New file `drivers/pico_clock_app.c` (the appliance/UI layer) + a
  driver-private `drivers/pico_clock_internal.h`; the public header keeps only
  `init`/`run`/`task_start`/`read_light`. The 542-line driver should not grow a
  menu, a font and a radio decoder inside it.
* The state machine is written **pure**: `(state, event, now_ms) → (state,
  screen descriptor)`, with rendering and hardware access outside it. That is
  what lets C6 unit-test the whole menu on QEMU with synthetic key events,
  leaving hardware to verify only pixels and switches.
* Navigation: SET short = enter/confirm, SET long = back/exit, UP/DOWN =
  navigate or adjust, 10 s inactivity → idle. Each item's full name scrolls
  once on entry, then shows its short label.
* Items:

  | Label | Full name | Action |
  |---|---|---|
  | `TEMP` | TEMPERATURE | show until any key, ≥ 2 s |
  | `DATE` | DATE | `DD.MM` / `YYYY` |
  | `SYNC` | DCF SYNC NOW | D4's forced sync |
  | `LAST` | LAST SYNC | age (`2H15`, `3D`, or `NONE`) |
  | `SIG` | DCF SIGNAL | D3's live monitor |
  | `AUTO` | AUTO SYNC | off / nightly (+ hour) |
  | `BRT` | BRIGHTNESS | auto (LDR) / fixed 1-7 |
  | `TSET` | SET TIME | hour → minute → day → month → year, blinking field, UP/DOWN adjust, SET advances; writes the DS3231 with `sec=0` on the last confirm |
  | `OFFS` | TEMP OFFSET | −9…+9 °C |
  | `12/24` | HOUR FORMAT | drives the AM/PM indicator LEDs |
  | `EXIT` | | back to idle |

* Buzzer (GP14): a short click on each accepted keypress and a longer beep on
  a value committed. Polarity per the vendor (`gpio_put(BUZZ,1)` = on,
  `Pico-Clock-Green.c:413`) and assumed to be a self-oscillating (active)
  buzzer — **verify on hardware**; if it turns out to be passive, it needs a
  square wave and the feature becomes a small tone generator inside the scan
  loop, or gets dropped. Default: keypress click **off**, confirm beep **on**,
  both settable.

**As landed:**

* Three files, not two. `drivers/pico_clock_ui.c` is the pure machine and is
  built on **every** target, QEMU included — that is what makes C6 possible,
  exactly as `dcf77_decode.c` is. `drivers/pico_clock_app.c` is the loop:
  buttons in, I2C reads, pixels out. `drivers/pico_clock_green_rp2350.c` keeps
  the pins and the frame buffer, and `drivers/pico_clock_internal.h` is the
  seam between the last two.
* The machine is `(state, key, press, now) → state` plus
  `(state, inputs, now) → screen`. The `inputs` struct is what keeps it pure:
  the current time and temperature are *given* to it, never fetched, so a test
  can hand it 31 January 2026 without an RTC. Actions that touch hardware
  (commit a time, apply a brightness, beep) come back as a bitmask for the
  caller to perform.
* **C6 is done for the menu, here rather than later:** `clockuiselftest`
  drives 46 cases from synthetic key sequences on both QEMU targets —
  wrapping at both ends of the menu, SET-long-is-back everywhere, the 10 s
  timeout, an abandoned edit changing nothing, the committed offset showing up
  in the shortcut, 12-hour mode lighting PM, and a 31st that stops existing
  when the month moves under it.
* Every adjustable value **wraps** rather than clamping. On three buttons,
  reaching the end of a range and having nothing happen reads as a broken
  button. Brightness wraps through `AUTO` between 7 and 1, so one button
  reaches automatic.
* The inactivity timeout **abandons** an edit rather than committing it: a
  clock that sets itself to whatever was on screen when its owner walked away
  is worse than one that did nothing.
* `SET TIME` commits all five fields at once, on the last confirm, so a
  half-set clock is never written. The edit is in local time — that is what a
  person typed — and is converted to UTC exactly once, in `commit_time()`.
* Labels: `24H`/`12H` rather than the planned `12/24`, which is 24 columns and
  would have had to scroll every single time.
* Deviation from "the public header keeps only `init`/`run`/`task_start`/
  `read_light`": it also keeps the C1/C2 diagnostics (`keys`, `led_walk`,
  `show_text`) and the LED setters, because the Lisp primitives are outside
  the driver and those are their entry points.

**Corrected after the first hardware run (2026-08-23):**

* **Scrolling did not poll the buttons.** "TEMPERATURE" is 55 columns, which
  at the vendor's 60 ms/column is 77 steps -- 4.6 seconds -- and
  `clock_hw_scroll_text()` never called `buttons_poll()` while it ran, so
  presses in that window were not ignored but never *sampled*. It surfaced as
  "SET long doesn't back out of TEMPERATURE": it had backed out, and the menu
  was then deaf for the length of an animation. Scrolling now polls on the
  same cadence as everything else and the first event ends it immediately,
  leaving the event queued so the press still does its job. Pressing a button
  to skip an animation is what everyone expects anyway.
* **The full name no longer replays on the way out of an item.** It plays when
  you *arrive* at an item, which is what a name is for; being told it again at
  length as a reward for leaving was the other half of the same complaint.
* **Confirming now returns to the clock, not to the menu.** It used to land
  back on the item just finished, where SET reopened it -- the one screen in
  the UI where completing something left the same button pointed at doing it
  again (user, 2026-08-23). The rule is now uniform: SET short is forward
  everywhere and confirming is the end of the forward path, so it ends the
  errand; SET long is back one level everywhere. The two exits are not
  redundant -- confirm means "done, put my clock back" and long-press means
  "not that one, let me look at another" -- and each is worth a button.
  Changing two settings in a row costs a second trip through the menu, which
  for a clock is the right way round. Read-only items (TEMP, DATE) have
  nothing to confirm, so any press dismisses them straight to the clock, which
  is also how the same screens behave when reached by an idle shortcut.
* The confirm beep went 120 ms to **40 ms** (the buzzer is self-oscillating,
  so duration is the only lever there is), and a **BEEP** menu item turns it
  off. Turning it off deliberately does not beep on the way out.
* `SYNC`/`LAST`/`SIG`/`AUTO` are **not** in the menu yet. They need D3's
  monitor and D4's sync controller to mean anything, and an item that is in
  the menu and does nothing is worse than one that is not there. Adding them
  is an insertion into one table in `pico_clock_ui.c`.

### Part B — DCF-77

#### D0 — Bench probe *(no LugalOS code; do this first)*

Before writing a decoder, settle §2's unknowns with the module on the bench:
`VDD` = 3.3 V, `PON` tied low, `OUT` observed (scope, logic analyser, or the
crudest version — a LED and a resistor: a DCF-77 module's output blinks once a
second, visibly). Record: does `PON` low enable it? Is `OUT` idle-low with
~100/200 ms high pulses, or inverted? How long after power-up does the first
clean pulse appear? Write the answers into this document. If a pulse per second
cannot be seen at a window with the display unplugged, no amount of software
will help and the problem is the module, the supply or the location.

#### D1 — Portable decoder core *(done, 2026-08-22 — 9/9 synthetic cases pass on both QEMU targets; 255/255 suite)*

Landed as `drivers/dcf77_decode.c` + `drivers/include/drivers/dcf77_decode.h`,
built on **every** target (it consumes timestamped samples, never a register)
and wired into `tests/runner.py` as "DCF-77 Frame Decoder Against Synthetic
Frames (D1)" via a new `dcf77selftest` shell command.

Decisions worth recording:

* **Polarity is configured, not inferred** — `CONFIG_DCF77_OUT_ACTIVE_LOW`,
  a board fact beside the PON polarity, passed explicitly to `dcf77_reset()`
  so no caller can forget it is a decision. *Corrected on user feedback,
  2026-08-22*: D1 originally inferred it from duty cycle over a rolling
  window, which the user rejected as "not necessary and error prone if the
  signal is bad at start" — and they are right twice over. The module has a
  **polarity jumper** (currently on "neg.", so pulses go LOW), making this a
  hardware fact like any pin number; and inference needs a good signal to be
  correct, so it is least reliable exactly when a clock is starting up or an
  antenna is being moved, and guessing wrong there locks the decoder onto the
  gaps instead of the pulses. Duty cycle is still *measured* and reported —
  demoted to a warning that the jumper and the board file disagree, which
  `dcf77_probe()` and `dcf77_listen()` now print instead of silently
  overriding the setting.
* **Two-frame agreement, but not two *adjacent* frames.** A frame is believed
  when a later one decodes to exactly as many minutes later as our own
  monotonic clock says have passed (rounded; a crystal's error over the 30
  minute pairing window is nowhere near the ±30 s that would make that
  ambiguous). Compared via days-from-civil arithmetic rather than
  field-by-field, so day, month and year rollover need no special case.

  *Changed the same day, from the user's reception report*: with a bad bit
  every 5–10 seconds, requiring two **consecutive** clean 59-bit frames is
  hopeless — one bad bit anywhere costs the whole pair and the wait restarts.
  Checking against elapsed time is an equally strong agreement, arguably
  stronger since it constrains the interval as well as the two times. So a
  decoded frame now **survives sync losses and rejected frames**: it is a
  validated absolute time, and nothing that happens afterwards makes it
  wrong. Tested by "frames two minutes apart still pair (bad minute between)";
  "non-consecutive frames are rejected", "year boundary" and "leap day" still
  hold the rest of the rule down.
* **The weekday is a free integrity check.** A frame carries both a weekday
  and a date, so they are checked against each other — this catches
  corruptions all three parity bits can survive.
* **An unreadable bit resyncs rather than guesses.** A pulse whose width fits
  neither window, or spacing that fits neither one second nor two, drops sync
  instead of leaving a frame with one invented bit in it.
* **The "next minute" rule** is implemented where it is easy to get silently
  wrong: the frame completed at a minute mark describes the minute *starting
  at that mark*, so the accepted time in the tests is the third frame's, not
  the first's.

The ten cases: clean decode; inverted (configured) polarity; stale parity;
dropped pulse; spurious edge; weekday/date mismatch; non-consecutive frames;
frames two minutes apart pairing across a corrupted one; year boundary; leap
day. All allocate on the stack — no `.bss` growth on a persona whose heap
and `.bss` are the same budget ([[rp2350_memory_budget]]).

Original plan for this milestone, for reference:

#### D1 (as planned) — Portable decoder core *(QEMU-testable, no hardware)*

* `drivers/dcf77.c` + `drivers/include/drivers/dcf77.h`, built on **every**
  target (it touches no registers), gated by nothing.
* API, deliberately sample-driven so it is testable:
  ```c
  void dcf77_reset(dcf77_t *d);
  void dcf77_feed(dcf77_t *d, bool active, uint64_t now_ms); /* call ≥50 Hz */
  bool dcf77_take_time(dcf77_t *d, rtc_time_t *out);         /* validated frame pair */
  void dcf77_get_stats(const dcf77_t *d, dcf77_stats_t *out);
  ```
* Frame decode, per the DCF-77 specification: bit 0 = 0 (minute start);
  bit 15 = R (backup antenna in use); bit 16 = DST-change announcement;
  bits 17/18 = CEST/CET
  (exactly one must be set); bit 19 = leap-second announcement; bit 20 = 1
  (start of encoded time); bits 21-27 minute BCD + bit 28 even parity;
  bits 29-34 hour BCD + bit 35 parity; bits 36-41 day, 42-44 weekday,
  45-49 month, 50-57 year, bit 58 parity over 36-58; second 59 has **no**
  pulse, which is how the minute boundary is found (a ~2 s gap).
* Pulse classification: 60-150 ms → `0`, 150-300 ms → `1`, anything else →
  invalid second. Second spacing 1000 ms ± 150 ms; ~2000 ms → minute mark.
* **The transmitted time is the time of the minute that *starts* at the next
  minute mark** — a detail that produces a silent one-minute error if missed.
  The RTC is written at that mark, with `sec = 0`.
* Acceptance requires *all* of: both framing bits, all three parities, plausible
  field ranges, exactly one timezone bit, the frame's weekday matching the
  weekday computed from its own date (§0.4), **and** a second consecutive frame
  decoding to exactly one minute later. Two frames means a sync takes ≥ 2
  minutes; that is the right trade and it is what every serious DCF-77 clock
  does.
* `OUT` polarity auto-detection (§2) lives here, so it is covered by the tests.
* Statistics for D3: a 24-entry ring of per-second quality (0 = nothing seen,
  1-7 = graded by pulse-width plausibility, edge count and spacing error),
  plus counters (frames attempted/accepted, parity failures, spurious edges,
  mean |spacing − 1000 ms|).
* **Tests (`tests/runner.py`, both QEMU targets):** a `dcf77selftest` shell
  command feeding canned sample streams — a clean two-frame minute pair; one
  with a corrupted parity bit; one with a dropped pulse; one with a doubled
  edge (noise); one with inverted polarity; one crossing a month/year boundary;
  one with the leap-second/DST-announce bits set. Asserts accept/reject and the
  exact decoded time. This is the milestone that keeps the genuinely fiddly
  part of DCF-77 out of "flash it and wait five minutes".

#### D2 — RP2350 pin backend *(landed 2026-08-22; hardware-verified 2026-08-23 — a real DCF-77 pulse train, 117/117 plausible widths)*

**Done out of order, ahead of D1**, because the receiver was soldered before
the decoder existed: the module's two polarity unknowns are answered by
hardware, not by code, and every line of D1 is written against assumptions
this milestone turns into facts. It also produces the sample data D1's tests
will be built from.

What shipped:

* `cmake/board-rp2350-clock.cmake`: `CONFIG_DCF77_OUT_GPIO=27`,
  `CONFIG_DCF77_PON_GPIO=28`, `CONFIG_DCF77_PON_ACTIVE_LOW=1`,
  `CONFIG_DCF77_WARMUP_MS=5000`, forwarded through `cmake/gen_config.cmake`
  and reported by `/proc/config`.
* `LUGALOS_ENABLE_DCF77` (default `OFF`, `ON` in the `rp2350-clock` preset),
  gating `drivers/dcf77_rp2350.c` — **independent of
  `LUGALOS_ENABLE_PICO_CLOCK_GREEN`**, since the probe is most useful with the
  display *not* running.
* `drivers/dcf77_rp2350.c` + `drivers/include/drivers/dcf77.h`: pin bring-up
  (`dcf77_init()`, called from `kernel/main.c` and leaving the receiver
  powered **down**), `dcf77_power()`, `dcf77_raw_level()`, and
  `dcf77_probe()` — the D0-in-system bring-up diagnostic.
* `(dcf-raw [secs])` Lisp primitive (default 30 s), Ctrl-C abortable.

`dcf77_probe()` does four things in order: tries the board file's PON
polarity and then the other one, declaring the receiver alive only when the
pin actually moves; infers OUT polarity from the duty cycle (a DCF-77 pulse
train is ~10-20 % active by construction, so the *minority* level is the
pulse, and a duty cycle outside that band is reported as "this is noise, not
a signal"); prints a 4-second raw trace at 25 ms/char, one second per line,
which is the picture that explains a *failure* (clean = a short run of `#` in
the same place on every line; dead = all `.`; noise = confetti); then prints
every pulse with its width, its spacing against the 1000 ms grid, and the bit
that width decodes to, ending in a summary with an explicit verdict.

Two details that are easy to get wrong and are handled deliberately: a
transition is timestamped at the **first sample that saw the new level**, not
at the moment the 5 ms debounce completed, or every width and gap would carry
a systematic +5 ms; and a ~2000 ms gap is reported as `<< MINUTE MARK` rather
than as a spacing error, because second 59 carries no pulse at all — that gap
is the one that is *correct*.

**Found while landing this, not predicted:** `/proc/config`'s generated-content
buffer (`fs/vfs_server.c`) is sized against a measured worst case, and its own
comment records that 512 once silently truncated it. The five new `DCF77_*`
lines are 95 bytes, which took the worst case from 690 to 791 — past the 768
that was there. Bumped to 896, with the arithmetic recorded in the comment.
This would have shown up as a truncated `/proc/config` tail on hardware only,
since QEMU has no pins to report.

**First hardware run, 2026-08-22 — no signs of life, and the *shape* of the
failure is the useful part.** `(dcf-raw 60)` reported `0 edges, pin high 0.0%`
for both PON polarities. That is not the "nothing is connected" reading: the
OUT pad is configured with the internal pull-up enabled, so an open circuit
reads **HIGH**. A hard low means either something is actively sinking current
on GP27, or the pad configuration is not taking effect.

Separating those needs a measurement, not more decoding, so `dcf77_pin_report()`
/ `(dcf-pins)` was added: register readback (did the pad writes land), a
pull-up / no-pull / pull-down test on OUT, an **ADC voltage** for each — GP26/
27/28 are ADC channels 0/1/2, so the pad voltage is measurable rather than
inferable — a drive-and-read-back test on PON, and a verdict. The voltage is
what discriminates:

| Reading | Meaning |
|---|---|
| follows the pull (HIGH with pull-up, LOW with pull-down) | floating — the OUT wire is not reaching GP27 after all |
| ~0.3-0.8 V against the pull-up | an **unpowered** module: our pull-up is pushing current through its protection diode into a dead VDD rail, and a diode drop is what a diode drop looks like. Check VDD. |
| ~0 V whatever the pull | held low hard: either a powered module idling low (**normal** when it is not decoding), or GP27 shorted to ground — header pin 32 sits directly beside pin 33 (AGND), and pin 34 (PON) is on its other side, so one solder blob can ground both |
| HIGH against the pull-down | an inverted-output module idling high, or a short to 3V3 |

Read-only on OUT, deliberately: it never drives that pin, because if a powered
module is driving it low, driving against it is a deliberate short between two
output drivers. PON is already an output, so driving it both ways and reading
the pad back costs nothing new — and its pad drive was dropped to 2 mA
(`PAD_PON_OUT`), which both suits a static enable line, is quieter next to a
longwave receiver (section 3, M2), and limits the current if pin 34 really is
bridged to the AGND pad beside it.

Also worth recording, since it silently mangles output rather than failing to
build: `kernel/printk.c` supports neither a `-` flag nor a width on `%s` (only
a `.N` precision), so `%-4s` prints a literal `%`. Columns are padded in the
string literal instead.

**Second hardware round, 2026-08-22.** `(dcf-pins)` reported GP27 held at
**53 mV** against the internal pull-up, with the pin following neither pull;
PON drove cleanly to 3300 mV / 1 mV; the ADC self-check against the LDR was
plausible, so the voltages are real. 53 mV against a ~50 kΩ pull-up is ~65 µA
into roughly **800 Ω–1 kΩ to ground** — not a solder bridge (<1 Ω, ~0 mV; a
multimeter confirmed none), not a CMOS output driving low (~50 Ω, ~3 mV), not
an unpowered chip's ESD clamp (~500 mV). It reads like a series resistor into
a low, or an on-board pull-down. The user then measured **3.3 V at the
module's own VDD/GND pads**, so the module is powered: the fault is downstream
of supply.

That round also exposed a flaw in `dcf77_pin_report()` worth recording,
because it is the kind that produces confident, meaningless data: it measures
OUT with the receiver *disabled* (`dcf77_init()` leaves PON inactive and the
pull test runs first), so "idle low" was guaranteed by construction and said
nothing about whether the module is alive.

`dcf77_hunt()` / `(dcf-hunt [secs])` is the test that does say something, and
it needs no radio signal: sweep PON through **driven-low, driven-high and
floating**, watching OUT in each, digitally and with the ADC in the same loop.
If OUT's voltage moves when PON moves, the module is powered *and* PON reaches
it, and the remaining question is reception rather than wiring. Three things
this adds beyond the first probe:

* **A floating PON state**, which the first probe never tried. Some modules
  bias PON internally and expect a switch to ground rather than a push-pull
  drive, so high-Z is a real operating mode. Floating it also *measures* that
  internal bias (GP28 is ADC2), which says which way the module expects to be
  driven.
* **A swapped-wires check.** In the floating state neither pin is driven, so
  both can be watched safely — and the evidence actively suggests it: a PON
  *input* with an internal bias resistor reads exactly like the observed ~1 kΩ
  to ground, and GP28 reaching a clean 3300 mV means it is *not* fighting a
  driven output, consistent with sitting on an inactive open-drain OUT.
* **Analog min/max per window.** A line that moves without crossing a logic
  threshold reads as "dead" to any digital-only probe — precisely what a weak
  open-collector output into a 50 kΩ pull-up would produce. The verdict
  suggests an external 4.7–10 kΩ pull-up in that case.

Warm-up was also raised from 5 s to 15 s per state: the first probe's 5 s may
simply have been too short to be evidence of anything.

`dcf77_free_pin_survey()` / `(dcf-pinscan)` answers the remaining structural
doubt — whether that ~1 kΩ is the module or the *baseboard*. GP27/GP28 were
taken as free from the vendor firmware's pin list, which records only the pins
that firmware uses and is **not** proof the board leaves the others
unconnected. The survey compares OUT against the persona's other unused pins
(GP4/5/8/19/20/21) using pulls only, driving nothing, saving and restoring
every pad. GP9 is excluded: the heartbeat task drives it.

**Third hardware round, 2026-08-22 — and a bug of mine, found by the data.**
`(dcf-hunt)` showed OUT at 53 mV in all three PON states, unchanged, 0 edges
over 105 s. But the floating state measured something genuinely new: **PON
floats to 3084 mV with no pull of ours**, so the module pulls PON up
internally to its own supply. That proves the PON wire reaches the module and
that the module's supply is live — a second, independent confirmation of the
user's 3.3 V meter reading, from the other end of the wire.

`(dcf-pinscan)` then reported all six of this persona's genuinely-free pins
(GP4/5/8/19/20/21) as "something is on it". **That was my bug, not a board
fact**, and the RP2350 datasheet names it: **RP2350-E9** (affects A2 silicon).
With `IE` set, `OE` clear and the pad floating in the undefined logic region,
the pad *sources* ~120 µA and latches at roughly 2.2 V; the internal pull-down
(36–113 kΩ) is far too weak to overcome it, so a floating pin reads HIGH
however hard it is pulled down. The datasheet's own workaround is now
implemented in `read_level_pulled_down()`: keep `IE` clear while the pull-down
acts, set `IE` only for the instant of the read, clear it again.

Two things follow, and they point in opposite directions:

* The six "loaded" pins are almost certainly just **floating** — the survey's
  conclusion was wrong and the tool now says so, including that a pin reading
  high under a pull-down is weak evidence on this silicon while a pin held
  **low** against the pull-up is solid (E9's leakage only ever pushes a pad
  *up*, so it cannot manufacture a low).
* **GP27's 53 mV is therefore untouched by the erratum and remains real.**

Quantified against the datasheet's specified pull ranges (IOVDD 3.3 V: RPU
32–86 kΩ, RPD 36–113 kΩ — a factor of 2.7, so any derived resistance is a
range and printing one number would be false precision): 53 mV under the
internal pull-up means a path to ground of roughly **0.5–1.4 kΩ**. Not a short
(<1 Ω), not an unpowered chip's ESD clamp (~500 mV), and higher than a CMOS
driver's on-resistance. It reads like a series resistor into a driven low, or
a bias resistor.

`dcf77_pin_report()` gained the measurement that turns that into an
identification, passively and with nothing driven: an **enable input** is
biased with something large (≥100 kΩ — it only has to beat leakage), while an
**open-drain output** is pulled up with something small (4.7–10 kΩ — it has to
produce fast edges). Two orders of magnitude apart, and both are readable with
the ADC. Applied to the pin wired as PON, that says directly whether OUT and
PON are swapped.

**Fourth hardware round, 2026-08-22 — the module is identified and the fault
is localised.** The board is marked **RC8000**, which is the
DCF-1060N-800 / DCF-3850M-800 family (77.5 kHz, ferrite rod, sold for
Arduino/Pico use). Facts from its listings and from a Raspberry Pi Pico user
running the same module the same way ([forum thread][rc8000-forum],
[product page][rc8000-tiny]):

* **PON is active LOW** — confirms `CONFIG_DCF77_PON_ACTIVE_LOW=1`.
* Supply **1.1–3.3 V**, **85 µA maximum** total current.
* On enable, **OUT goes HIGH for a few seconds**, then starts pulsing.

Measured here, with the corrected tools:

* `(dcf-pinscan)`, E9 workaround in place: GP4/5/8/19/20/21 all **float**, and
  GP27 is the *only* pin held low. The baseboard is exonerated — there is no
  need to lift the Pico off it.
* GP28 under our pull-down sits at 37 mV, i.e. a path to 3V3 of **3.2–10 MΩ**.
  That is far too weak to be an open-drain output's pull-up (those are
  4.7–10 kΩ; they have to make fast edges) and is exactly what an input's bias
  resistor looks like on an 85 µA part. **OUT and PON are not swapped**, and
  GP28 really is PON.
* GP27 sits at 55 mV under our pull-up: a path to ground of **542–1457 Ω**.

The last number is the diagnosis, and the datasheet is what makes it damning:
**an 85 µA module cannot have a ~1 kΩ pull-down on its data output** — that
would draw 3.3 mA when the output went high, forty times the module's entire
budget. So GP27 is not sitting on a healthy data output. The remaining
candidates are all at the module end: a solder bridge across its OUT/GND pads,
a damaged output, or the wire landing on an **antenna terminal** — a 77.5 kHz
ferrite coil is hundreds of ohms of fine wire, the right order of magnitude to
imitate exactly this.

Also fixed here: `print_resistance()` reported megohm-scale results as
"9965.3 kohm", and — worse — the PON interpretation line was guarded behind
`pon_mv_pd > 100`, so it printed *nothing* in the low-voltage case that
identifies the pin. The guard suppressed the conclusion in precisely the
situation it was written to explain.

[rc8000-forum]: https://forums.raspberrypi.com/viewtopic.php?t=378829
[rc8000-tiny]: https://www.tinytronics.nl/en/communication-and-signals/wireless/rf/modules/dcf77-radio-clock-receiver-module

**Fifth round, 2026-08-22 — a second module, and a correction to the round
above.** The user swapped in a *different* receiver (PON strapped to GND at the
module, so GP28 is now unconnected) and `(dcf-pins)` reported **the same
53 mV, the same 522-1403 Ω**. Two independent modules do not share a fault
that specific, so the previous round's conclusion was wrong, and the argument
behind it was wrong in an instructive way:

> an 85 µA module cannot have a ~1 kΩ pull-down on its output

The arithmetic was right and the conclusion did not follow. A **pull-down
resistor** drains current continuously, which that budget really does forbid —
but an **output driver's on-resistance** drains nothing at all when it is
driving low into a high-impedance input. A micropower CMOS output stage with
0.5–2 kΩ of on-resistance is entirely normal. What was measured is a healthy
module driving its output low, which is its correct idle state.

So both modules are fine, both are powered, both are enabled, and neither
detects a carrier. **This is a reception problem, not a wiring problem** —
which is what M0 predicts, and this board is confirmed as a **Pico 2 W**,
where the regulator's PWM-mode pin sits behind the CYW43 and cannot be reached
from this tree.

`dcf77_listen()` / `(dcf-listen [secs] [load])` is the tool for that phase:
watch for minutes rather than seconds, print every pulse as it arrives and a
progress line every 10 s, so the ferrite rod can be moved while someone
watches a number change. Its `load` argument runs the **M0 experiment**
directly — `pico_clock_green_static_load()` lights one matrix row as pure DC
(shifted out once, latched, OE held open; no multiplexing, no PWM, no shift
clock), so the 3V3 load rises by tens of mA without a single switching edge.
That separates "more current on 3V3" from "the display is switching", which
simply running the display would not. If pulses appear only with the load on,
the regulator was the problem all along — and this phase's founding assumption
that the display is the enemy is exactly backwards.

`pico_clock_green_static_load()` is the same primitive section 3's M5 needs
for the quiet-sync indicator, so it is not a diagnostic-only detour.

**Sixth round — the user asked "maybe GP27 is somehow blocked?", and was
right to.** Round five concluded from "two different modules read identically"
that the modules were fine. That inference picked the wrong common cause: the
element shared by both experiments is **the Pico pin**, not the module, and
"GP27 is stuck low" explains the repeat just as economically. Every test in
this file up to that point was passive, and a passive test is structurally
unable to tell a stuck-low pad from a healthy pad watching a device drive low
— they look identical when you only ever look.

`dcf77_drive_test()` / `(dcf-drivetest)` answers it by driving the pin and
reading it back:

| Result | Meaning |
|---|---|
| ~3.3 V | pad healthy, nothing holding it — the module's output is high-Z |
| ~1–2 V | pad healthy, fighting the ~1 kΩ measured passively: a module output driving low, healthy on both sides |
| ~0 V | the pad cannot drive high: damaged pad, or a hard external short |

It is the only function in the file that drives OUT, breaking the rule the pin
report deliberately follows, so the budget is stated rather than assumed: 2 mA
drive (the weakest the RP2350 offers) against the measured 0.5–1.4 kΩ is about
3 mA of contention, held 50 ms — inside the RP2350's 12 mA per-pin limit and
inside what a CMOS output stage tolerates. With the module's OUT wire unplugged
it is cleaner still, and the printed verdict distinguishes the two cases.

The pin choice was also made genuinely free, since "use another GPIO" is the
obvious next move if the pad *is* dead: only GP26–29 reach the ADC, so
`PIN_IS_ADC()`/`MV_NA` now gate every voltage this file prints and the tools
degrade to digital-only on any other pin rather than reading a garbage
channel. `CONFIG_DCF77_OUT_GPIO` can name any pin; **GP28 keeps the voltage
diagnostics** and is free now that PON is strapped at the module.

**Seventh round — the pin is exonerated, and a hole in this file's own test
design is found.** `(dcf-drivetest)` run twice: with the module connected,
GP27 drives to **3115 mV**; disconnected, **3295 mV**. A 180 mV droop under a
2 mA drive is a real but weak path to ground that the driver overcomes easily
— a healthy pad and a healthy micropower output stage. GP27 is not blocked.

Re-reading the RC8000's documented behaviour against this file's own tests
then exposed a real gap: **on being enabled the module drives OUT HIGH for a
few seconds**, and `dcf77_hunt()` asserts PON and then waits out a **15 s
warm-up before it starts watching**. If the module does exactly what it is
supposed to do, the whole transient is over before the first sample is taken.
Every "OUT never moved" result so far is blind to the one event that would
prove the module reacts to PON at all — and that event needs no radio signal.

`dcf77_power_on_capture()` / `(dcf-poweron [secs])` closes it: de-assert PON,
wait 3 s for a genuine power-down, assert it, and sample from that same
instant with nothing skipped. Printed as a trace at 100 ms per character
(`#` = high for the whole cell, `+` = part of it, `.` = low), which renders a
multi-second startup high as an unmistakable run and later DCF-77 pulses as
one marked cell in every ten. Each cell is sampled continuously rather than
point-sampled, so a 100 ms pulse cannot fall between two looks. It requires
PON on its own GPIO and says so when there is nothing to toggle, rather than
reporting a flat line as if it were evidence.

**Eighth round — `(dcf-poweron 30)` flat for 30 s, and the test was testing
one of two possibilities.** PON *was* on GP28 for that run (confirmed by the
user), so the toggle was real. But the capture assumed **active low**: it parks
PON at the disabled level for 3 s, then asserts it. If this module enables on
**PON HIGH**, that run had it exactly backwards — the 3 s "power down" was
enabling it, and the entire 30 s sample window had it switched off. Nothing in
the evidence has ever excluded active-high for module #2, whose type is
unknown; active-low is documented only for the RC8000.

So the capture now runs **both polarities in one go**, no rewiring, since PON
is on a GPIO this code drives: phase 1 assumes active low, phase 2 assumes
active high, and the verdict compares them. A startup transient in exactly one
phase identifies the polarity outright; one in *both* means the line is
following PON rather than being driven by the receiver (OUT and PON shorted, or
OUT on the module's PON pad); none in either is the first genuinely earned
negative in this whole sequence.

A PON float measurement is still printed, but **only as information, not as a
gate** — a first attempt made it a gate, which would have manufactured a fresh
false negative in the other direction: an input that biases itself reads high
when floated (3084 mV on module #1), but an input with no bias at all is
perfectly legal and reads exactly like an unconnected pad.

`dcf77_mirror()` / `(dcf-mirror [secs])` answers the user's question — "can I
put an LED on OUT to see it flashing?" Not on this OUT: ~1 kΩ of output
on-resistance against an 85 µA budget will not light an LED, and trying loads
the output being observed. (A high-efficiency LED behind 10 kΩ draws ~0.3 mA
and is visible in a dim room; a 330 Ω one is not.) The instinct is right
though, so the line is mirrored onto the LED matrix instead — one row lights
while OUT is high, lit **statically** via `pico_clock_green_static_load()`, so
the mirror adds no switching noise of its own. Noted in the code: a lit row is
also tens of mA of extra 3V3 load, i.e. M0's variable, so the mirror is not a
clean M0 test — `(dcf-listen secs 1)` is.

**Ninth round — first signal, 2026-08-22.** A *third* module, a 3-wire
GND/VCC/SIG part with active electronics and its own reception LED, produced
a moving line on GP27 within seconds. `(dcf-poweron)`'s trace came back mostly
`#` with scattered `+` — **idle HIGH with brief low excursions**, i.e. an
**inverted output**: the pulses go low on this module. The user then found
reception improves markedly near a window, and the module's own LED confirms
it is decoding.

That retires the whole "is it wired right" phase. It also says something about
the first two modules worth keeping: both were micropower parts (~1 kΩ output
stage, 85 µA budget) that never produced an output under any PON polarity,
while the module that works is the one with active electronics — enough to
drive an indicator LED, which is precisely what the earlier ones could not do
when the user asked about wiring one up.

Two real bugs this exposed, both of the same shape — code trusting a default
where the hardware has an opinion:

* **`dcf77_listen()` and `dcf77_mirror()` never detected OUT polarity.** Only
  `dcf77_probe()` did; the other two took "pulses are high" on faith. On an
  inverted module that measures every *gap* as a pulse and reports ~800 ms
  widths with a confident, meaningless verdict. Both now run the same
  duty-cycle survey first (a DCF-77 pulse train is 10–20 % active by
  construction, so the minority level is the pulse) and print which level they
  settled on. `mirror()` additionally lights its row on the *pulse* rather
  than the idle level, which on this module is the opposite of what it did.
* **`(dcf-poweron)`'s "long high in both phases" verdict accused the wiring**
  ("OUT and PON shorted") of what is, for a 3-wire module, simply not having a
  PON pin. It now offers the two innocent explanations — no PON pin, or an
  inverted idle-high output — before the guilty one.

**Still to verify on hardware** (this is D0's checklist, now runnable in-system
as `(dcf-raw 60)` / `(dcf-pins)` / `(dcf-hunt)` / `(dcf-pinscan)` from the
shell, where the LED matrix is not scanning): PON polarity, OUT polarity, real
warm-up time, and whether there is a usable signal at this location at all.


* `LUGALOS_ENABLE_DCF77` CMake option (default `OFF`, `ON` in the
  `rp2350-clock` preset), `CONFIG_DCF77_OUT_GPIO=27`, `CONFIG_DCF77_PON_GPIO=28`,
  `CONFIG_DCF77_PON_ACTIVE_LOW=1`, `CONFIG_DCF77_WARMUP_MS=5000` — through
  `gen_config.cmake` and `/proc/config` like every other board fact.
* Pad setup: `OUT` input, Schmitt on, pull-up on, `ISO` cleared (the RP2350
  pad-isolation bit resets to 1 — the existing `0x5A` writes already handle
  this, and the same convention is reused); `PON` output, driven inactive at
  init.
* `dcf77_feed()` is called from the clock scan loop, once per row (~1 ms) — no
  new task, no new IRQ plumbing (RP2350 `IO_IRQ_BANK0` is not wired into
  `devirq` today, and 1 ms polling is ~100× finer than the 100 ms the
  measurement needs). The once-per-second I2C read stretches one sample period
  by ≈1 ms, which is immaterial against a 100 ms pulse; this is worth stating
  because phase 11 L4 shows the same I2C stall *was* fatal to something else.
* Diagnostics: `(dcf-raw secs)` — display blanked, prints per-second edge
  counts, measured pulse widths and the auto-detected polarity to the console.
  This is what D0's findings get confirmed against, in-system.

#### D3 — Signal monitor on the matrix *(landed 2026-08-23 — 61 menu cases green on both QEMU targets, 261/261 suite; NOT yet hardware-verified)*

* A full-screen mode (menu item `SIG`, and reachable from `(dcf-monitor)`):
  one column per second, appended on the right and scrolling left, height 1-7
  from the per-second quality score; column empty when nothing was seen. 24
  columns = the last 24 seconds. The DCF indicator LED blinks in step with each
  received pulse — the single most useful signal-strength feedback there is,
  since a human can see at a glance whether it is ticking once a second or
  stuttering.
* Runs with the display **on** by definition (it *is* the display), which makes
  it the natural instrument for §3's measurements: compare it against the same
  minute with M1/M2/M4 toggled, and against `(dcf-raw)` with the display dark.

**As landed:**

* A new `drivers/dcf77_service.c`: a decoder fed from the clock's existing
  ~1 kHz row-scan loop, holding its state between calls. **The receiver is
  listened to continuously**, not only during a sync. That is a deliberate
  departure from D4's planned `OFF -> WARMUP -> ACQUIRE ...` machine, and it
  is possible only because PON turned out to be unwired -- with the module
  always powered, the first three states had nothing to do. What it buys: the
  signal screen has live data the instant it is opened, and a sync requested
  after good reception commits immediately instead of waiting two more minutes
  for frames the decoder has already seen and verified.
* Listening always, committing only on request, is the line that keeps a radio
  from quietly overwriting a clock somebody set by hand. Nothing is written
  without an explicit request, and nothing at all on failure.
* `SIG` is the **first** item in the menu, so the monitor is SET, SET from the
  clock face. A finished clock has no console attached, so the menu is not one
  route to the antenna-aiming tool, it is the only one, and burying it five
  presses deep would have made the hardest thing to reach the one you need
  with both hands on a ferrite rod (user, 2026-08-23). `TEMPERATURE` and
  `DATE` moved to the end of the menu for the mirror-image reason: they are
  already a single press from the idle screen, and are in the menu for
  completeness rather than as the way anyone reaches them.
  `(dcf-monitor [secs])` is the same screen from the console, with a
  commentary the panel has no room for.
* The bar chart pins the newest second to the **right** edge and drops the
  oldest: the decoder keeps 24 scores and the panel has 22 text columns, and
  the old end is the right one to lose. A score of 0 draws nothing at all
  rather than a stub -- an empty column is the most legible mark on this
  display, and "the signal dropped here" is precisely what antenna tuning
  needs to see.
* Fixed after the first hardware run: the decoder's scores were refreshed
  *after* the frame was drawn, so the bars lagged 250 ms and the first frame
  of the signal screen always read `NO SIG`. They are fetched before the
  screen is decided now.
* The `SIG` screen is **exempt from the 10 s inactivity timeout**: it exists
  to be stared at while an antenna is moved, and being read-only, nothing can
  be left half-done in it.
* **The score is a meter, not a light** (user, 2026-08-23). As first written
  it was 7 unless spacing was more than 50 ms off, so every column read full
  in any reception good enough to work and only dropped once a pulse was
  already failing. Spacing and width are both like that: stable right up to
  the point of failure, then collapsing. The quantity that actually varies
  *continuously* with orientation is line noise, and the decoder was throwing
  it away -- transitions shorter than the 5 ms debounce were discarded without
  being counted. They are counted now, and the score is
  `7 - f(spacing error) - f(width error) - f(glitches)`, with the glitch term
  on a roughly logarithmic ladder (3, 6, 10, 18) because noise counts explode
  rather than creep. The first rung starts at three so that an edge which
  bounces once or twice on a perfect signal still reads 7 -- a meter that
  cannot show its own top division is no better than the light it replaced.
  A moving average was considered and rejected: averaging a binary signal
  gives a smoother binary signal, and the eye already integrates 22 bars.
* **Silence advances the chart.** Beyond a minute mark's 2 s, one empty column
  per second, so turning a rod into its null shows the display marching on and
  emptying rather than freezing -- which is indistinguishable from a crash.
* The DCF indicator LED now carries four meanings, on a cadence independent of
  screen redraws -- it follows the second pulse on the signal screen (the
  single most useful reception feedback there is), blinks slowly while
  syncing, sits solid if the last sync was under 48 h ago, and is otherwise
  dark.

#### D4 — Sync controller and integration *(landed 2026-08-23 — 78 menu cases green on both QEMU targets, 261/261 suite; NOT yet hardware-verified)*

* State machine: `OFF → WARMUP (PON on, CONFIG_DCF77_WARMUP_MS) → ACQUIRE (wait
  for a minute mark) → DECODE → VERIFY (second frame) → COMMIT (write DS3231 at
  the minute mark) → DONE | FAILED (timeout, default 5 min)`. Any button
  aborts. `PON` is released on exit.
* Display policy during sync, selectable and defaulting to whatever D5
  measures: `live` (nothing changes), `gated` (§3 M4), `quiet` (§3 M5, display
  dark with one statically lit DCF indicator).
* Nightly auto-sync at `CONFIG_DCF77_AUTO_HOUR:CONFIG_DCF77_AUTO_MIN` (default
  **03:17** — off the hour, where the band is quieter and every other radio
  clock in the house is not also listening; and at an hour that covers the
  European DST changeover, which happens at 03:00 CEST/02:00 CET).
* ~~**Timezone/DST is not our problem, deliberately**~~ — **superseded
  2026-08-23, see section 8.** The original plan was to store the German local
  time the frame carries and be done. The user's requirement that GPS and NTP
  join DCF-77 later changes the answer: those speak UTC, and a store of local
  time has no correct value during the hour that repeats every October. The
  kernel clock and the DS3231 now hold **UTC**; the frame's Z1/Z2 bits convert
  it on the way in, and `CONFIG_TIMEZONE` converts it back on the way out to a
  display. This clock is now correct anywhere, and still shows CET/CEST.
* Surfaces: menu items `SYNC`/`LAST`/`SIG`/`AUTO` (C3); the DCF indicator LED
  (off = never synced; slow blink = syncing; solid = last sync < 48 h; off
  again when stale); `printk` lines to the console; and a new **`/proc/dcf77`**
  synthetic file (`fs/vfs_server.c`) reporting state, last-sync ISO timestamp
  and age, last result, frames accepted/rejected, and the current quality
  score — so a 9P-attached host can watch the clock's radio health without a
  serial console. Lisp: `(dcf-sync [timeout])`, `(dcf-status)`,
  `(dcf-monitor [secs])`, `(dcf-raw [secs])`, `(dcf-selftest)`.
* When no valid frame is decoded before the timeout, **nothing is written** and
  the failure is reported. A DS3231 holding a slightly-wrong time is
  categorically better than one holding a garbage time.

**As landed:**

* **No `OFF -> WARMUP -> ACQUIRE` states.** PON is unwired, so the receiver is
  always powered and the decoder always listening (see D3). What remains is
  `idle -> syncing -> done | failed`, and a sync requested after good
  reception commits *immediately* off a frame pair the decoder has already
  verified, rather than waiting two more minutes to see it again. A decoded
  time is carried forward by the monotonic counter, so age costs only that
  counter's drift; the two-minute freshness limit is about trusting the radio
  is *still* there, not about arithmetic.
* Menu: `SYNC` (entering it *is* the request -- pressing SET on an item called
  "sync now" and then asking for confirmation would be asking twice), `LAST`
  (`NONE` / `45M` / `13H` / `03D`), `AUTO` (on/off). `SYNC` counts down while
  it waits, in minutes and then seconds: five minutes of an unchanging screen
  reads as a hang.
* Leaving the `SYNC` screen cancels a sync in progress. That is the plan's
  "any button aborts" -- walking away from the screen is the gesture, and a
  sync left running invisibly would be worse than not offering one.
* `SYNC` and `SIG` are exempt from the inactivity timeout; both are screens
  someone deliberately watches, and neither can leave anything half-done.
* Nightly auto-sync at `CONFIG_DCF77_AUTO_HOUR:CONFIG_DCF77_AUTO_MIN` (03:17
  local), keyed on the **calendar day** rather than a timer, so a clock that
  is itself being corrected cannot fire twice or skip a day. Checked once a
  second, not once per sample -- it is the only part of the service that reads
  a clock, and a timezone conversion has no business on the row-scan hot path.
* `/proc/dcf77` and `(dcf-status)` report the same picture. Both distinguish
  **`last_sync`** (when the clock was last changed) from **`radio_time`**
  (what the receiver most recently decoded), because the whole design rests on
  those being different things: the radio can be decoding perfectly while the
  clock has deliberately not been touched.
* **Display policy is moot.** The plan offered `live`/`gated`/`quiet` for the
  sync window; with continuous listening there is no sync window to gate. If
  D5 shows the display costs too much reception, the lever moves to the
  decoder's own duty cycle rather than to a mode, and that is a better place
  for it.
* Not done here: the `SYNC`-triggered display blanking (M4/M5) and the
  interference measurement itself, which is D5's job and needs the hardware.

**Precursor landed 2026-08-23**: `dcf77_sync()` / `(dcf-sync [secs] [set])`
joins the D2 pin layer to the D1 decoder and reports a decoded *time* rather
than pulse widths — no state machine, no auto-sync, no `/proc/dcf77`, no
display policy yet. It prints each completed frame with its accept/reject
reason as it happens, a per-second quality string every 10 s, and on failure
a breakdown that distinguishes "no minute mark" from "frames arrive but bits
flip". Writing the clock (kernel RTC + DS3231, the `set_date` idiom) is
opt-in and never happens on failure. D4 is now a controller around a
known-good decode path rather than around two unproven halves.

#### D5 — Hardware bring-up and the interference measurement *(cut to one measurement, 2026-08-23 — see the scope note below)*

Run in this order, recording the numbers in this document as they come in —
this is the part that cannot be reasoned into existence:

**Result of the first bring-up attempt, 2026-08-23 — the receiver was fine,
the kernel's clock was not.** The first run with the active receiver on a 2 m
lead showed a textbook pulse train at the wrong speed: pulses every 429 ms
with widths of 45 ms and 88 ms, instead of every 1000 ms with widths of
100 ms and 200 ms. Every width and every gap was scaled by the same factor,
which is the signature of a measuring problem rather than a radio one — 38
pulses spanned 16.287 reported seconds, and 16.287/38 = 0.42861 against
12/28 = 0.428571.

The cause was in `arch/riscv/rp2350/boot_header.S`, not in anything on this
phase's list: the TIMER0 tick generator was programmed with a read-modify-OR
(`lw / ori 12 / sw`) into a CYCLES register the bootrom does not hand over at
zero. It had bit 4 set, so `16 | 12` asked for 28 cycles per tick and TIMER0
counted at 12/28 MHz while `kernel/time.c` called each count a microsecond.
**Every clock on this board had been running at 42.9% of real time** — uptime,
`time_delay_us()` in every bit-banged driver, the matrix refresh (~53 Hz
rather than the intended 125 Hz), chess's search budget, and the clock face
itself. `kernel/ticker.c` had measured the same 2.33x in phase 15 and blamed
the RISC-V tick generator in a comment; it was the correct clock of the two.

Two lessons worth keeping: a peripheral register the bootrom touched is not
zero at reset, and DCF-77 is an unusually good calibration source — a
transmitter whose pulse spacing is a caesium standard makes any error in the
local time base immediately visible.

With the divisor fixed, the same antenna in the same place gives 117/117
plausible widths, spacing within ±20 ms of the 1 s grid, and clean 105/210 ms
clusters, with the display quiet (step 1's condition).

1. Baseline: display **unplugged/blanked**, receiver on a long lead, away from
   the case. Frames to first sync; per-second quality distribution over 5 min.
2. Same, display running normally, antenna in its final position. **The delta
   is the interference budget.**
3. Apply M1 (deterministic 1.000 ms period). Re-measure. *Prediction: this is
   the single biggest software win.*
4. Apply M2 (2 mA/slow slew). Re-measure.
5. Try M3 variations (orientation, distance) with the `SIG` screen live.
6. Implement/measure M4 (second-gated multiplexing) only if 2-5 leave live sync
   unreliable. Watch specifically for the AGC caveat in §3.
7. Confirm M5 (quiet sync) works and time it (expect 2-4 minutes).
8. Choose and document the shipped default policy.

Also verified here: the actual warm-up time, `PON`/`OUT` polarities matching
D0, power consumption with the receiver gated off, and that a failed sync
leaves the clock exactly as it was.

**Scope cut, 2026-08-23 (user).** The receiver ended up on a **2 m lead**,
which removes the coupling path this entire section was written about.
Magnetic near-field falls off as 1/r³, so moving the rod from ~5 cm to 2 m is
a factor of roughly 64,000 in field strength: the matrix's row currents are no
longer a plausible interferer, and M1/M2/M4/M5 exist only to fight them.

What distance does **not** fix is the cable. The module is powered from the
board, so the regulator's ripple and the display's current pulses reach it
*conducted*, along the supply and ground wires, and the 2 m OUT lead is itself
an antenna. That path is unchanged, so one measurement is still worth taking —
and one is now all of it.

Steps 3-8 are dropped unless step 2 shows something. Of the rest:

* the warm-up time and the gated-off power consumption are **moot**: PON is
  unwired and the module is always on;
* the `PON`/`OUT` polarities are **already confirmed** (D2, 2026-08-23);
* "a failed sync leaves the clock exactly as it was" stands, and is now a
  one-minute check: aim the rod into its null, `SYNC`, watch it report `FAIL`,
  and confirm the clock did not move.

**The measurement, as it now stands.** `(dcf-monitor secs 1)` blanks the panel
completely — zeros latched, OE closed, and the row scan not run at all, so
there is no shift clock and no switching current — while running the identical
decoder, scoring and summary as the lit version. Two runs of the *same* code
differing in one variable is a measurement; comparing the monitor against
`(dcf-raw)` would only have compared two instruments. Both runs end with a
**mean quality score**, which is the figure to compare: pulse counts saturate
(a pulse either arrives each second or it does not) while the mean moves
smoothly, because the score now does.

    (dcf-monitor 300)      ; display running
    (dcf-monitor 300 1)    ; display dark, same antenna position

**Measured 2026-08-23, same antenna position, only the display changed:**

| | display running | display dark |
|---|---|---|
| mean quality | 6.5 / 7 | 6.7 / 7 |
| pulses in 300 s | **309** | 300 |
| bad widths | 12 | 6 |
| glitches | 7 | 2 |
| sync losses | **24** | 10 |
| worst spacing | **145 ms** | 52 ms |
| frames seen / accepted | **0 / 0** | 2 / 1 |

**The display costs every frame.** Not a degradation -- the difference between
a clock that can set itself and one that never will. And the mechanism is
plain in one figure: 309 pulses in 300 seconds. The panel is not damaging
pulses, it is *inventing* about nine of them, each surviving the 5 ms
debounce. Every invented pulse makes two impossible gaps, which is the 24 sync
losses, one every twelve seconds; frame assembly needs 59 *consecutive* clean
seconds and so never gets close.

Two lessons, both about measurement rather than radio:

* **The mean quality score was the wrong headline.** It moved 0.2 -- inside
  the noise band this document had just declared -- while every frame in five
  minutes was destroyed. A mean measures *typical* pulse health, and the
  pulses were typically fine; the damage was rare and fatal. Frame assembly is
  a conjunction, not an average, so one bad second in twelve is total failure
  rather than an 8% loss. The summary now prints sync losses and frames first,
  flags a pulse count above one per second explicitly, and labels the mean as
  what it actually is.
* **The glitch counter did not see them either** (7 vs 2), because these are
  not sub-debounce events. That is not a flaw in the counter -- it is the
  counter correctly reporting that this interference is of a different kind
  than the one it was built to catch.

**The fix, and it is none of section 3's mitigations:** `DEBOUNCE_MS` was
5 ms, a switch-bounce figure with no business in a protocol whose shortest
feature is 100 ms. It is 25 ms now, in both the decoder and the driver's
diagnostics -- four times longer than the injected pulses and four times
shorter than a real one, so a genuine bit cannot be filtered out. Widths and
gaps are unaffected: transitions are timestamped at the first sample that saw
the new level, not when the debounce completes. `dcf77selftest` asserts both
halves of that promise -- a 20 ms spurious pulse is survived, a 60 ms one
still costs the frame, which is the honest limit of a filter that cannot tell
a short pulse from a short burst of noise.

**Re-measured after the debounce change, same antenna position:**

| | running (before) | running (after) | dark (after) |
|---|---|---|---|
| pulses in 300 s | 309 | **298** | 302 |
| sync losses | 24 | **6** | 13 |
| frames seen | 0 | **2** | 1 |
| worst spacing | 145 ms | **64 ms** | 73 ms |

**D5 is answered: the display is no longer distinguishable from dark.** The
panel-on run actually scored better than the dark one on both headline
figures, which is run-to-run variation and exactly the point -- the display's
contribution has dropped below the noise. M1 (deterministic row period), M2
(2 mA slow-slew pads), M4 (second-gated multiplexing) and M5 (quiet sync) are
**not built and not needed**, and this measurement is the justification rather
than an assumption. Section 3's analysis was sound about the mechanism and
wrong about the remedy: the cure was one constant in the decoder, not any
change to how the display is driven.

**What remains is reception, not interference.** Neither run accepted a frame,
and the antenna is admittedly in a worse position than the one that synced in
133 s. Two diagnostics were added so the next run says why rather than
requiring inference:

* **Longest clean run**, the number that actually decides whether a frame can
  assemble. A frame is 59 *consecutive* seconds, so a run of 58 is worth
  exactly what a run of 3 is. "6 sync losses in 300 seconds" reads like 98%
  success and can still mean no frame ever completes; "longest run: 34 s of 59
  needed" says the same thing usefully. In `/proc/dcf77` and `(dcf-status)`
  too.
* **Why frames failed** -- the parity / framing / range / weekday breakdown,
  printed only when frames were seen but not accepted, with an explicit note
  for the case where all four are zero (the frames decoded and simply have not
  paired yet, which needs one more good minute rather than a better antenna).

### C6 — Menu/UI unit tests on QEMU

`clockuitest` (shell command, both QEMU targets, in `tests/runner.py`): drives
C3's pure state machine with scripted key events and asserts the resulting
screen descriptors and settings — menu entry/exit, timeout-to-idle, long-press
back, time-set field advance and wrap, offset clamping. Hardware then only has
to prove that switches produce events and that the descriptor renders as
pixels, which is what hardware is uniquely able to prove
([[falsify_on_hardware_not_qemu]] is about not *substituting* QEMU for
hardware, not about declining to test the half that is portable).

### C7 — Documentation and housekeeping

* README's clock-persona paragraph: buttons, menu, weekday LEDs, DCF-77.
* A per-persona size baseline: `tools/sizereport-rp2350.json` is the *chess*
  build's, and nothing guards the clock persona's static RAM today. Add
  `tools/sizereport-rp2350-clock.json` and wire the `sizecheck` target to the
  active preset — otherwise this phase's new files grow `.bss` on the one
  persona nobody is measuring ([[rp2350_memory_budget]]: on RP2350, `.bss` and
  the heap are the same budget).
* `clockisotest`: every other RP2350 driver task has an isolation test proving
  its PMP domain confines it (`kernel/shell.c:123-139`); the clock task, added
  after that sweep, has none. Adding one is small and brings the clock persona
  up to the bar the chess persona already meets.

---

## 5. Settings persistence: there is nowhere to put them (yet)

Everything C3 makes adjustable — brightness mode, temperature offset, 12/24 h,
auto-sync on/off and hour, beep — is **RAM-only and lost on reboot**, because
this persona has no writable non-volatile storage:

* `/flash0/` is genuinely read-only (`drivers/flashdisk.c:23` returns −1 and
  says so), not merely unmounted.
* No SD card — GP10-13 are the display (phase 11 L0).
* The DS3231 has no user NVRAM (unlike a DS1307's 56 bytes), and its alarm and
  aging-offset registers are not scratch space.
* No AT24C32 EEPROM on this board: phase 11 L3's `i2c` scan found only `0x68`.
  (Worth re-running once — a `0x57` would hand us 4 KB for free.)

So: compile-time defaults live in the board file, the menu adjusts them for the
session, and **the time itself survives anyway** because the DS3231 is
battery-backed and a nightly DCF sync re-asserts it. If persistence turns out
to matter in use, the real fix is a small flash settings sector programmed via
the RP2350 boot ROM's flash API (XIP disabled, code running from RAM) — a
self-contained future milestone that would serve every persona, explicitly
**not** in this phase.

---

## 6. Sequencing, and what each milestone is worth on its own

```
C1 buttons + LEDs ─┬─► C2 font + idle screen ─► C3 menu ─┬─► C6 UI tests
                   │                                      │
                   └──────────────────────────────────────┴─► C7 docs/size/iso
D0 bench probe ────► D1 decoder core (QEMU tests) ─► D2 pin backend ─► D3 monitor
                                                                       │
                                                          D4 sync controller
                                                                       │
                                                          D5 measure + tune
```

C1-C3 and D0-D1 are independent and can proceed in either order; D3's monitor
needs C2's rendering; D4's menu entries need C3. **C1+C2 alone already deliver
the user's first three asks** (no auto-alternation, buttons, weekday) and are
worth shipping before any radio hardware arrives. D1 is worth doing before D0's
module even ships, since it needs no hardware at all.

Suggested release points: **0.13.0** after C3 (the clock becomes a real
appliance), **0.14.0** after D5 (the clock sets itself).

---

## 7. Risks and open questions

* **Are GP27/GP28 physically reachable on an assembled Pico-Clock-Green?**
  Mechanical, unknown until inspected. Alternatives listed in §1; the pin
  numbers are board-file constants precisely so this is a one-line change.
* **Module variant** (5 V vs 3.3 V, `OUT` polarity, `PON` polarity, warm-up).
  D0 settles it in minutes; the driver auto-detects the two polarities anyway.
* **Reception at the installation site.** DCF-77 indoors, in a metal-ish case,
  next to an LED panel, is genuinely marginal in some rooms — and no code fixes
  a bad location. D3's monitor exists so a bad location is *diagnosed* in
  seconds instead of blamed on the software.
* **M4 (gated multiplexing) may not work** because of receiver AGC (§3). The
  plan does not depend on it: M5 is the fallback and is not clever.
* **Buzzer type** (active vs passive) — unverified; C3's beep degrades to
  "dropped" if it turns out to need a tone generator.
* **`.bss` growth on a persona with no size guard** — C7 fixes the guard;
  until then, the new state is small by construction (font and glyph tables are
  `const`/flash; the decoder holds ~64 bytes of frame bits plus a 24-byte
  quality ring; the menu holds a few dozen bytes).
* **Scope creep into a full vendor-feature clone** (alarms, countdown,
  count-up, scrolling messages, °F). The indicator LEDs for all of those exist
  and will be tempting. Explicitly **out of scope** for this phase; the
  `IND_*` names are reserved so a later phase does not have to re-derive the
  bit layout.

---

## 8. The clock runs on UTC

*Added 2026-08-23, at the user's direction, and it changes section 4's D4
bullet — see the strikethrough there.*

The kernel wall clock is UTC. Local time is **computed, never stored**.

The reason is not tidiness. Three time sources are planned — DCF-77 (now),
GPS and NTP (later) — and two of the three speak UTC natively while the third
states its offset explicitly in every frame. Local time is the only one of the
representations with no correct value during the hour that repeats at the
October switchover: 02:30 happens twice, an hour apart, and a stored `02:30`
cannot say which. Store UTC and all three sources agree by construction, the
ambiguity disappears, and a DST change becomes a rendering decision rather
than a write.

**What holds what:**

| | |
|---|---|
| `kernel/time.c` | UTC milliseconds since 1970, pinned to the monotonic counter |
| DS3231 | UTC. It is storage for a UTC clock, not a display |
| DCF-77 frame | German local time + the offset (Z1/Z2). `dcf77_take_time()` returns UTC, converted with the offset the transmitter itself stated — no rule of ours is consulted |
| `date`, the clock face, PGN tags | local, rendered through `CONFIG_TIMEZONE` |

**The rule format** is POSIX TZ, the string an ESP32 or an Arduino already
takes, from the tz-list summary the user pointed at
(<https://mm.icann.org/pipermail/tz/2016-April/023570.html>). Default
`CET-1CEST,M3.5.0,M10.5.0/3` (Europe/Berlin). `kernel/timezone.c` parses it and
does the arithmetic; `Jn`/`n` julian rule forms are rejected rather than
half-implemented. Two details it gets right and hand-rolled code usually does
not: the POSIX offset sign is **west-positive** (`CET-1` means UTC+1), and a
rule's transition time is in the local time in force *just before* the switch —
so `M3.5.0` is 02:00 CET and `M10.5.0/3` is 03:00 CEST, both 01:00 UTC, which
is how the whole EU moves at once.

**Surfaces:** `date` (local, with the UTC underneath), `date <ISO>` (typed by a
human, so local in, UTC stored), `tz` / `tz <rule>`, `tzselftest`, and in Lisp
`(date)`, `(date-utc)`, `(tz)`, `(tz "...")`.

**Tested on QEMU**, not on hardware, because it is pure arithmetic:
`tzselftest` runs 29 cases — both sides of both European switchovers to the
minute, midwinter and midsummer, the new-year and leap-day crossings, a
southern-hemisphere zone whose summer spans the new year, fixed-offset and
UTC0 zones, every parse rejection, and a round trip over every 7 hours of 2026.
`dcf77selftest` gained a winter frame, the one case where Z2 rather than Z1 is
set and the answer differs by an hour. Both are in `tests/runner.py` on both
QEMU targets.

**One migration note:** a DS3231 written by an earlier build holds *local*
time, and will be read back as UTC — so the clock reads an hour or two fast
until the next `date` or `(dcf-sync … 1)` rewrites it. There is no way to
detect this in the stored value, which is precisely the argument for storing
UTC in the first place.

---

## 9. The appliance question: booting with no computer attached

*2026-08-23. Not a phase-17 feature, but found by phase-17 hardware, and the
lesson is general enough to be worth the space. Fixed and verified on **both**
RP2350 personas.*

**Symptom.** On a computer the clock came up; on a plain USB charger the panel
stayed dark. It also "broke on the first restart" -- a freshly flashed board
worked and the next power cycle did not, on *either* power source.

**Cause.** `linker/rp2350.ld` places the PMP-granted driver regions
(`.ustacksN`) in their own `NOLOAD` sections, **outside** the
`__bss_start..__bss_end` range `arch/riscv/rp2350/boot_header.S` clears. Seven
of the eight objects there are stacks, where that is correct. The eighth,
`drivers/usb_cdc.c`'s `g_usb_region`, is **state**: ring indices,
`ep2_configured`, `ep2_dtr`. None of it was ever initialised.

Two faults then lined up. `usb_cdc_putc()`'s guard tested garbage and let a
caller through with nothing enumerated; it then indexed its ring with an
**unmasked** garbage head, storing out of bounds into a PMP boundary. That
faults before any handler exists to report it, so the machine stopped inside a
`printk()` with no output and no reset.

**Why it looked like everything except what it was.** A BOOTSEL session runs
the bootrom's mass-storage code through that same SRAM and leaves values that
happen to fail the guard -- so every reflash "fixed" it and every cold start
broke it again. The power source was never the variable: every working run had
simply followed a flash.

**Fixes.**

* `boot_header.S` zeroes `_ustacks_start.._ustacks_end` alongside `.bss`. At
  boot, not in a driver: the memory belongs to the region layout, and the next
  driver to keep state there would otherwise hit this again.
* `usb_cdc_init()` clears `g_usb_region` itself too -- a driver should not
  depend on a linker section being cleared by another file, and that init runs
  again after a 1200-baud BOOTSEL touch, long after boot-time zeroing.
* `usb_cdc_putc()` masks both ring indices.

**Both personas were affected.** `usb_cdc.c` and `rp2350.ld` are shared, so
the chess board carried the same latent fault; it simply drew a benign SRAM
pattern most of the time. Its symptom was different and more confusing --
`usb` is probe index 2, so a hang there means the D+ pull-up is never asserted
and the host sees *no USB device at all*, which reads as a bad cable. Both
personas now boot repeatedly from a bare USB power adapter.

**Two real bugs found on the way, kept:** `uart_hw_putc()` called
`task_block()` when the TX FIFO filled, which before `sched_init()` is a stop
with nothing to wake it (`[Dev] Registry:` is the first boot message long
enough to overflow 32 bytes); and `usb_cdc_init()`'s enumeration handshake ran
a fixed 500 000 iterations with no early exit, now a 2 s deadline that leaves
as soon as the interface is configured.

**Audit afterwards, clean.** Every `.ustacksN` object other than
`g_usb_region` is a stack; `.utext`/`.blktext`/`.usbtext` are `> FLASH` and
execute in place; `.data` is in RP2350's copy table and the QEMU targets copy
it in `entry.S`, so `-1` sentinels are real; the QEMU linker scripts have no
allocated section outside `.bss`/`.stack`.

**How it was found, which is the transferable part.** Not by reading code --
four theories died in turn (a wedged I2C bus, `usb_cdc_task()`, an unbounded
EP4 drain, the UART FIFO), each disproved by a number from the hardware. It
was found by the boot beacon (C1): one buzzer click per `CLOCK_BOOT_MARK()`
plus a latching LED count, on a board with no console, no LED and no SD card.
Three properties did the work:

1. **Marks go before the call they guard**, so a hang leaves its own number
   lit rather than only proving the previous step finished.
2. **The instrument must not touch what it measures.** The beacon used
   `time_delay_us()`, which pumps `usb_cdc_task()` -- the very subsystem under
   suspicion -- hundreds of times per click, both masking the fault and
   misattributing its location. Replaced with a raw timer spin.
3. **It must be able to test itself.** Two marks back to back with nothing
   between them is what finally ruled the beacon out as the wall, after every
   earlier "localisation" had silently assumed a mark that clicks also
   returns.

The beacon is off by default with no call sites at rest; set
`CONFIG_CLOCK_BOOT_BEACON` and scatter `CLOCK_BOOT_MARK(n)` over whatever is
suspect.
