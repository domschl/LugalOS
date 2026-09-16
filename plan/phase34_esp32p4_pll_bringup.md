# Phase 34 — The P4 stops running at a tenth of its speed

**Status: planned, not started. Written 2026-09-16**, from measurements taken
the same day with both boards on the bench.

**Milestone scheme: `34.1`, `34.2`, … — numbers, not a letter.**
`plan/phase32_esp32p4_execute_in_place.md` predicted this: *"The letter pool is
now effectively exhausted; phase 29 will need a scheme rather than a letter."*
Phase 29 took `O`, which really was the last one. So this phase prefixes its
milestones with its own number, and every phase after it can do the same
without ever running out. It is also strictly better than a letter in the one
place these tokens are actually read — a commit subject. `U5` tells you
nothing about where to look; `34.5` tells you exactly.

## 0. The measurement, and why this is the largest single win available

Both boards, same suite, same source, single core, 2026-09-16:

| | clk_sys | `perft` depth-3 suite | "strange bugs" d3, 62 379 nodes | cycles/node |
|---|---|---|---|---|
| RP2350 (Hazard3) | 150 MHz | 6 465 ms | 89 625 nps | 1674 |
| ESP32-P4 | **40 MHz** | 21 333 ms | 25 819 nps | **1549** |

**The P4 is 3.3× slower than the RP2350 and 8 % more efficient per clock.**
Both of those are the same fact: nothing in this kernel has ever configured the
P4's clock tree, so the CPU runs from the bare crystal.
`drivers/emac_esp32p4.c:123` says so in the course of explaining why it picks
the largest MDC divider on offer, and `plan/phase27_esp32p4_bringup.md:1724`
recorded the decision at the time:

> **No PLL.** The CPU still runs off the crystal, so this board is slow — a
> volatile 64-bit loop of 400 M iterations takes about 150 seconds […]
> Bringing the PLL up is not needed by any milestone here and would change
> every timing measurement in this phase; it belongs with E6 or later.

That was right then and is wrong now. Every milestone since has been measured
on a board running at a tenth of its rated speed, and the comparison above is
the first one that makes the cost legible rather than theoretical.

**For contrast, the other board has always done this.**
`arch/riscv/rp2350/boot_header.S:315`–`350` brings PLL_SYS up to a 1500 MHz VCO
for a 150 MHz `clk_sys`, in assembly, before C exists. The P4 has no
equivalent and never had one.

**Why this comes before the second HP core.** `plan/phase27_esp32p4_bringup.md:2405`
defers the second core, correctly. It is worth noting how much it defers: 9×
from the clock against 2× from a second core, for less work and with no
scheduler, no cache-coherence and no bring-up handshake involved. Perft on the
P4 today prints `cores used: 1` and takes 557 s at depth 4 whether you ask for
one core or two, because there is only one — and even a perfect second core
would leave this board behind an RP2350.

## 1. What the chip actually offers

Three facts, and the first is a disappointment worth stating up front.

**360 MHz, not 400.** This board is **ESP32-P4 revision v1.3**
(`plan/phase27_esp32p4_bringup.md:393`). IDF's
`rtc_clk_cpu_freq_mhz_to_config()`
(`components/esp_hw_support/port/esp32p4/rtc_clk.c`) splits on exactly that
boundary: `ESP32P4_SELECTS_REV_LESS_V3` covers *"ESP32-P4 revisions 0.x and
1.x"* and offers **90 / 180 / 360 MHz** from a 360 MHz CPLL. The 100/200/400
ladder — and the CPLL recalibration that raises 360 to 400 — belongs to
revisions ≥ 3.0. So the ceiling here is **9×**, and the datasheet's 400 MHz is
not available on this silicon.

**Four clocks move, not one.** HP_ROOT_CLK feeds CPU, MEM, SYS and APB through
separate dividers. The power-on reset configuration is **40 – 20 – 20 – 10
MHz** respectively (IDF `rtc_clk_cpu_freq_to_xtal()`'s own comment), and the
constraints are hard: **MEM ≤ 200 MHz, APB ≤ 100 MHz**. Selecting a CPU
frequency therefore means selecting three other dividers, and getting them
wrong does not produce an error — IDF's comment on the ordering is explicit:

> […] an intermediate state will occur, in the intermediate state, the
> frequency of APB/MEM does not meet the timing requirements. If there are
> peripherals/CPU access that depend on these two clocks at this moment, some
> exception might occur.

Upscaling order is APB → SYS → MEM dividers, each followed by a bus update,
then the CPU divider, and **the source mux last**. Downscaling is the exact
reverse. This is not a style preference; it is the difference between a
working board and an intermittent one.

**The ROM will not do it for you.** This is the change from phases 27 and 32,
where `Cache_FLASH_MMU_Set` and the cache operations meant the risky work was
a ROM call with a known address. `components/esp_rom/esp32p4/ld/esp32p4.rom.ld`
exports **nothing** from `rtc_clk_*`, no REGI2C helper and no PMU helper. The
three relevant symbols are:

```
ets_get_cpu_frequency  = 0x4fc00040   /* what the ROM believes, not what is */
ets_update_cpu_frequency = 0x4fc00044 /* tell the ROM what it now is        */
ets_clk_get_cpu_freq   = 0x4fc00554
```

`ets_update_cpu_frequency` is a *bookkeeping* call — it retunes the ROM's own
microsecond delay loops — and it must be made, because ROM code this kernel
still calls would otherwise delay by a ninth of what it intends.

## 2. What survives the switch — and why this tree is unusually well placed

Three things that would normally make this dangerous are already handled, none
of them by accident:

* **The console is XTAL-clocked and keeps working.** `drivers/uart_esp32p4.c:25`
  drives UART0 from XTAL_CLK deliberately, and says why in terms that read as
  though written for this phase: *"Selecting it means the console's baud rate
  does not depend on the clock tree at all -- so E2 gets a shell without
  bringing the PLL up, and no later milestone can take the console away by
  changing a CPU frequency."* This is the single most valuable fact
  in this phase: unlike `esp32p4_xip_map()`, which had to run blind because
  its own `.rodata` was in the window it was mapping, **this switch can be
  made with a live console and `printk` on both sides of it.**
* **The wall clock is XTAL-clocked.** `kernel/time.c` selects the systimer's
  XTAL source explicitly (`P4_SYSTIMER_CLK_SRC_SEL`, *0 = XTAL*) and derives
  `SYSTIMER_TICKS_PER_US` from `CONFIG_XTAL_HZ`. So `time_get_ms()`,
  `time_get_us()` and therefore `time_delay_us()` (`kernel/time.c:227`, a spin
  on `time_get_us()`) are all immune. **No delay anywhere in the P4 drivers is
  cycle-calibrated** — every one goes through `time_delay_us()`. There is no
  hidden loop counter to recalibrate.
* **The preemption tick measures itself.** `kernel/ticker.c:285` computes
  `measured_hz` against the microsecond timer at init rather than trusting
  `TICK_HZ`, and refuses with a diagnostic if mtime is not advancing. Whatever
  the CLINT turns out to be clocked from, the tick interval self-corrects.

Only two consumers of `CONFIG_XTAL_HZ` exist in the tree and both are
XTAL-sourced peripherals. Nothing reads a CPU frequency constant, because
there has never been one.

**A question this phase answers for free.** `kernel/ticker.c:181` records that
*"Nothing states what clocks this counter — the TRM documents every register of
the block and never names its source"*, and guesses 16 MHz before measuring.
Today XTAL and CPU are both 40 MHz, so the measurement cannot distinguish them.
After 34.4 it can: if `measured_hz` moves, the CLINT follows the CPU clock; if
it stays, it is XTAL. Either answer retires an open comment. Note the
consequence of the first one — at 360 MHz the low half of mtime wraps every
11.9 s rather than every 107 s, which would have made the `MTIME_SAM` bug that
cost E4 an afternoon appear nine times sooner.

## 3. The hazard that makes this a phase and not a patch

**Voltage.** IDF does not simply move a clock mux. `rtc_clk_init()`
(`components/esp_hw_support/port/esp32p4/rtc_clk_init.c:30`) first performs an
analog-domain sequence, and only then touches the CPU frequency:

1. REGI2C writes forcing the RTC and digital regulators on (`I2C_DIG_REG`),
   and the bias block's forced power-downs off (`I2C_BIAS`).
2. `get_act_hp_dbias()` — a **per-chip efuse calibration value**, whose comment
   says it exists *"to ensure that the hp_active_voltage is close to 1.15 V"* —
   written to the PMU's HP_ACTIVE regulator.
3. DCDC enabled, `dcm_vset` programmed, `DBIAS_SEL` set to hand dbias control
   to the PMU, then a **1 ms settle**.
4. *Then* `rtc_clk_cpu_freq_set_config()`.

Whether all of that is required here depends on what the boot ROM already
left behind, and **that is not yet known**. It is the first thing 34.2 and
34.3 exist to find out, because the two possible answers scope this phase very
differently: either the ROM already holds the regulator where CPLL frequencies
need it and this is a handful of HP_SYS_CLKRST writes, or IDF's PMU/REGI2C
path has to be ported and 34.3 is the bulk of the work.

**The failure mode is the bad kind.** An under-volted or mis-ordered clock
switch does not fault cleanly. It corrupts occasionally, under load, warm —
and phase 33 exists precisely because this project has already spent a phase
on intermittents. The board's only console is UART0 and its only recovery is
the ROM's download mode. Hence the ordering below: the regulator question is
settled, in its own milestone, before any clock moves; and the first frequency
tried is the lowest one, not the highest.

**Flash is being executed from, at a MEM clock that is about to move 9×.**
Since phase 32 the CPU fetches `.text` over the MSPI. IDF tunes MSPI timing
when flash runs fast (`mspi_timing_tuning`); this kernel has never touched it
and has never needed to. A wrong MSPI timing at the new MEM clock presents as
garbage instructions, which is indistinguishable from a corrupt image — the
same diagnostic trap `arch/riscv/common/xip_esp32p4.c` documents. 34.5's
done-condition therefore includes a filesystem read-back, not just a prompt.

## 4. Milestones

### 34.1 — The baseline and the instrument

No clock changes. Two deliverables.

**A `clocks` shell command** that reports CPU / MEM / SYS / APB as *read back
from HP_SYS_CLKRST*, plus `ets_clk_get_cpu_freq()` (ROM `0x4fc00554`) and the
ticker's own `g_measured_hz`. The point is that after 34.4 there must be a way
to tell "the switch worked" from "the switch appeared to work" without
inferring it from a stopwatch.

**The baseline, recorded in this document**, so the comparison later is against
a number rather than a memory. Measured 2026-09-16, already in hand:

```
(perft 3 1)   75 passed depths, 0 errors (cores used: 1,  21 333 ms)
(perft 3 2)   75 passed depths, 0 errors (cores used: 1,  21 332 ms)
(perft 4 1)   96 passed depths, 0 errors (cores used: 1, 557 153 ms)
(perft 4 2)   96 passed depths, 0 errors (cores used: 1, 557 164 ms)
(perft 4 2)   96 passed depths, 0 errors (cores used: 1, 557 169 ms)
"strange bugs" depth 3: 62 379 nodes, 25 819 nps
```

**These are the board's own in-guest figures, not host round-trips** --
`run_perft_tests_cores()` prints its own elapsed millisecond count precisely
because timing from the host is *"how the first X8 hardware run managed to
report two cores as slower than one"* (`user/chess/src/perft.c:414`).

Which is what makes perft a usable instrument for this phase. The two depth-4
runs of the *same* command differ by **5 ms across 557 s**, and the one-core
against two-core pair by 11 ms: run-to-run noise under 0.01 %, so a speedup
claim of any size at all will be unambiguous. The `cores used: 1` on every
line is the other half -- `CONFIG_ENABLE_SMP` is off on this board, so the
`cores` argument is clamped away before any work is split, and both columns
are the same single-core code.

**Done when:** `clocks` prints 40 / 20 / 20 / 10 MHz and those four numbers are
read from registers, not printed from constants.

### 34.2 — Read the clock tree; do not infer it

No writes. Transcribe from the TRM
(`~/gith/esp/datasheet/esp32-p4_technical_reference_manual_en.pdf`, chapter 8,
HP_SYS_CLKRST) the CPU/MEM/SYS/APB divider and source-mux fields, and the CPLL
control and status registers. **Render the page and count the bits** —
`esp32p4-register-provenance` is a memory in this project because two
clock-gate bits were nearly wrong from inference, and this phase is entirely
clock-gate bits.

Cross-check each field against IDF's `clk_ll_cpu_set_src()`,
`clk_ll_cpu_set_divider()`, `clk_ll_{mem,sys,apb}_set_divider()` and
`clk_ll_bus_update()` in `components/hal/esp32p4/include/hal/clk_tree_ll.h`.
Agreement between two independent sources is the bar; either alone is not.

Answer three questions in writing, in this document:

1. **Is CPLL already running**, and at what frequency, when the ROM hands over?
2. **What has the ROM left the HP regulator at** — is `DBIAS_SEL` already
   handed to the PMU, and what is the HP_ACTIVE dbias value?
3. **What clocks the MSPI**, and does it follow MEM_CLK or a separate source?

**Done when:** the three answers are recorded with their register evidence, and
34.3's scope is decided by them rather than assumed.

### 34.3 — The regulator, before anything moves

Gated on 34.2's answer 2.

*If the ROM already holds the regulator where CPLL frequencies need it*, this
milestone is a written justification with register readbacks and nothing else.

*If it does not*, port IDF's sequence from `rtc_clk_init.c:30`–`90`: the REGI2C
writes, the efuse dbias read, the PMU regulator and DCDC configuration, the
`DBIAS_SEL` handover and the 1 ms settle. Verify at the **current 40 MHz** —
registers read back as intended, and the board survives a full
`tests/hw/test_esp32p4.py` plus a `(perft 3 1)` soak unchanged. A voltage
change with no frequency change should be invisible; if it is not, that is
found here rather than blamed on 34.4.

This milestone is sequenced before the first frequency change for the same
reason phase 32 put U3 before U4: the dangerous half goes first, alone, where
its failures cannot be attributed to anything else.

**Done when:** the regulator is in a state that is *known* (not inherited) and
stated, and the board is unchanged at 40 MHz.

### 34.4 — 90 MHz: the smallest switch that proves the path

The lowest CPLL-derived step, CPU divider 4 from the 360 MHz CPLL. 2.25×, which
is enough to be unmistakable in perft and small enough that MEM/SYS/APB stay
close to their current values.

Do the switch **after the console is up**, not in boot. This is the one
advantage this phase has over phase 32, and it should be spent: a failed
switch prints, and a board that stops printing localises the failure to a
single function.

Make the target a board config (`CONFIG_CPU_FREQ_MHZ`, one of 40 / 90 / 180 /
360 in `cmake/board-esp32p4-nano.cmake`), so that a bad step is one constant
away from bisecting and 40 remains selectable as a control.

Call `ets_update_cpu_frequency()` afterwards so ROM delay loops stay honest.

**Done when** all four hold:
* `clocks` reports the new CPU/MEM/SYS/APB from registers;
* `ets_clk_get_cpu_freq()` agrees;
* `(perft 3 1)` comes back **correct** (0 errors — perft is a correctness suite
  first) and about 2.25× faster than 34.1's baseline;
* `g_measured_hz` is recorded, answering §2's CLINT question.

### 34.5 — 360 MHz, and the dividers that come with it

The full step, with MEM/SYS/APB chosen against the MEM ≤ 200 / APB ≤ 100
constraints and programmed in the upscaling order from §1.

Two things need re-verifying here that 34.4 does not stress:

* **Flash.** Mount `/flash0`, read the whole filesystem back, and compare —
  `stdlib.lisp` loading at boot is necessary but not sufficient. If MSPI timing
  needs tuning at the new MEM clock, this is where it shows.
* **Ethernet.** `drivers/emac_esp32p4.c`'s MDC divider is `CSR/124` where CSR is
  the system clock, chosen as *"the largest divider the field offers"* precisely
  because the clock was unknown (`drivers/emac_esp32p4.c:128`). It stays legal
  at every frequency in this phase — IEEE 802.3's cap is 25 MHz and this cannot
  approach it — so the divider needs no change, but that reasoning should be
  re-stated in the file now that the clock *is* known, and the link should be
  brought up and exercised before the milestone closes.

**Done when:** 360 MHz is live, `tests/hw/test_esp32p4.py` is green, `/flash0`
round-trips, the EMAC passes traffic, and `(perft 4 1)` returns 0 errors.

### 34.6 — Re-measure everything the clock invalidated

A 9× CPU makes several recorded conclusions stale. Each of these was measured
honestly and is now measured at the wrong frequency:

* **The XIP penalty.** At 40 MHz the P4 is *ahead* of the RP2350 per clock
  (1549 vs 1674 cycles/node) because a flash miss costs almost nothing in CPU
  cycles. At 360 MHz it costs nine times as many. **The speedup will be less
  than 9× and the shortfall is the measurement** — report it, do not round it.
* **The L2 cache size.** `CONFIG_L2_CACHE_KB = 128`
  (`cmake/board-esp32p4-nano.cmake:195`) rests on commit `37902c0`'s finding
  that *"(perft 3) node rates across six positions differ by under 0.1 %
  between 128 KB and 256 KB"* — taken at 40 MHz. Against 186 KB of `.text`,
  that result may invert now. Re-run the same comparison at 360 MHz. If 256 KB
  wins materially, the 249 KB of heap phase 32 bought is genuinely back on the
  table and the trade has to be made again on the new numbers.
* **The interrupt latency.** `plan/phase27_esp32p4_bringup.md:1384` derives
  12.6 µs, *"about 500 cycles of a 40 MHz core"*, from the tick's −1258 ppm.
  Repeat the ten-minute measurement; the prediction is roughly 126 ppm.
* **The EMAC's frame loss.** Phase 28 chased ~1 % to the RMII pad drive
  strength. CPU/DMA contention for L2MEM changes shape at 9×, so re-run the
  3000-frame echo. Per `measure-before-declaring-fixed`: quote the sample size
  or do not quote the rate.

**Done when:** each of the four has a new number beside its old one, and any
that moved has its source file's comment corrected rather than left describing
a board that no longer exists.

### 34.7 — Documents

* `plan/phase27_esp32p4_bringup.md` §"What E4 deliberately did not do" gets a
  forward reference: the deferral was correct and has been paid.
* `drivers/emac_esp32p4.c:123`'s *"This kernel has never configured the P4's
  clock tree"* becomes false the moment 34.4 lands. Fix it in the same commit
  that makes it false, not in this milestone.
* `kernel/ticker.c:181`'s open question about the CLINT's source gets its
  measured answer.
* README states no clock figure for any board today. If 34.6's result is worth
  advertising, the P4's introduction at `README.md:93` is the one place for it.
* Memories: `esp32p4-l2-cache-steals-ram` needs 34.6's result if the trade
  changes; a new one is warranted for the clock-switch ordering and the
  regulator sequence, because both are the sort of thing that is expensive to
  re-derive and invisible in the code.

## 5. What could go wrong, stated in advance

* **The regulator is the risk and it is not observable directly.** A board that
  is slightly under-volted at 360 MHz passes every test on the bench and fails
  in a week. The mitigations are 34.3's sequencing, 34.4's lower first step,
  and a soak that is longer than a test run — not a sharper eye.
* **The speedup disappoints and that is a real outcome, not a bug.** If XIP
  dominates at 360 MHz, the honest result may be 5× rather than 9×, with the
  remainder recoverable only by an L2 or MSPI change. Phase 32 U5 set the
  precedent for reporting a trade-off measurement rather than a headline.
* **Nine times more code runs per second, including the racy kind.** Phase 33
  spent a phase on intermittents. Timing-sensitive bugs that exist today and
  have never lost their race may start losing it — and the first symptom will
  look like "the clock change broke X". Keeping 40 MHz selectable
  (`CONFIG_CPU_FREQ_MHZ`) is what makes that question answerable in one reboot.
* **`ets_update_cpu_frequency()` is easy to forget.** Any ROM routine that
  delays — flash, cache, PMU — silently delays by a ninth of its intent
  without it. The failure is timing-dependent and will not point at itself.
* **The chip is v1.3 and 400 MHz is not on offer.** If a later measurement
  wants the last 11 %, it wants different silicon, not a better sequence.

## 6. Explicitly not in this phase

* **No second HP core.** Still `plan/phase27_esp32p4_bringup.md` §7's deferral,
  and this phase makes it *less* urgent rather than more: 9× arrives first, and
  the remaining 2× is the harder, riskier half. `CONFIG_ENABLE_SMP` stays off
  on this board and `(perft n 2)` keeps honestly printing `cores used: 1`.
* **No PSRAM.** Unchanged from phases 27 and 32.
* **No dynamic frequency scaling, no sleep modes, no DVFS.** One frequency,
  chosen at build time, set once at boot. IDF's `esp_pm` exists and is a
  different project.
* **No MSPI timing tuning unless 34.5 proves it necessary.** If flash
  round-trips at 360 MHz, the ROM's configuration is adequate and porting
  `mspi_timing_tuning` buys nothing measurable.
* **No change to the RP2350 or QEMU paths.** One board's clock tree.
  `cmake/board-esp32p4-nano.cmake`, `kernel/time.c`'s P4 arm and one new source
  file are the blast radius.
