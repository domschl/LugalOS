# Phase 34 — The P4 stops running at a tenth of its speed, on half its cores

**Status: planned, not started. Written 2026-09-16**, from measurements taken
the same day with both boards on the bench.

**Two parts, sequenced, one phase.** The clock (34.1–34.7) and the second HP
core (34.8–34.13). They are one phase because they share an instrument, a
board and a failure mode: both are invisible except in a measurement, and
`perft` is the measurement for both. They are sequenced because the clock is
9× for less risk than the core's 2×, and because a second core brought up on a
40 MHz board would have every timing conclusion re-taken the moment the clock
moved anyway.

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

**Why the clock comes first and the core second.**
`plan/phase27_esp32p4_bringup.md:2405` defers the second core, and its reason —
*"bringing up SMP on a platform whose single-core behaviour is not yet trusted
inverts the ordering this phase is built on"* — is the same reason it is the
second half of this one rather than the first. The sizes settle the order: 9×
from the clock, against at most 2× from the core, for less work and with no
scheduler, no stall register and no bring-up handshake involved. A second core
on a 40 MHz board would also mean re-taking every measurement it produced the
moment the clock moved.

Both halves are needed to get what this chip is. Perft on the P4 today prints
`cores used: 1` and takes 557 s at depth 4 whether one core or two is asked
for; the clock alone leaves it a fast single-core board with an idle core beside
it.

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

## 4. The second core: what this chip actually requires

Established from IDF and the toolchain, 2026-09-16, and split into what is
already true of this tree and what is missing.

### 4.1 Three things that are already right

* **`mhartid` is the core id.** IDF's `rv_utils_get_core_id()`
  (`components/riscv/include/riscv/rv_utils.h:91`) reads `mhartid` on every
  RISC-V target with more than one core, this chip included. So
  `arch/riscv/common/entry.S`'s existing `csrr t0, mhartid` dispatch is correct
  here, and the **QEMU-shaped** secondary path — `#if CONFIG_ENABLE_SMP &&
  !defined(CONFIG_BOARD_RP2350)`, park in `.Lsecondary_wait` on
  `g_smp_release` — is the structurally right one for this board. RP2350 needed
  its own entry because its core 1 lives in a bootrom; this one does not.
* **The atomics are real.** `-march=rv32imac_zicsr_zifencei`
  (`CMakeLists.txt:76`) includes `A`, so phase 22's spinlocks compile to `lr`/
  `sc` and `amo*` rather than to a library fallback. Nothing in
  `kernel/lock.h` needs a P4 arm.
* **The L1 data cache is shared between both cores.** `components/hal/esp32p4/
  include/hal/cache_ll.h` has `CACHE_L1_ICACHE0_*` and `CACHE_L1_ICACHE1_*`
  registers but exactly **one** `CACHE_L1_DCACHE_*`, and `soc_caps.h:174` sets
  `SOC_SHARED_IDCACHE_SUPPORTED`. So two cores writing the same structure see
  each other with no maintenance at all. **This is the single biggest reason
  this bring-up is cheap here**, and it is the one fact on this list that must
  be confirmed against the TRM before it is relied on — a wrong answer here is
  not a slow board, it is silent data corruption.

### 4.2 Five things that are missing

* **The core is stalled and held in reset.** Starting it is four writes and a
  ROM call, all in register blocks this tree already addresses:
  `PMU.cpu_sw_stall.hpcore1_stall_code` from `0x86` to `0xFF`, then poll
  `HP_SYSTEM_CPU_CORESTALLED_ST_REG`'s CORE1 bit; set
  `HP_SYS_CLKRST_SOC_CLK_CTRL0`'s `CORE1_CPU_CLK_EN`; clear
  `HP_SYS_CLKRST_HP_RST_EN0`'s `RST_EN_CORE1_GLOBAL`; and point it at an entry
  with `ets_set_appcpu_boot_addr` (**ROM `0x4fc000a8`**, from
  `esp32p4.rom.ld:57` — so unlike the clock half, the launch *does* get a ROM
  call). IDF's interrupt-matrix helper for the app CPU is an empty function on
  this chip, which is one fewer thing.
* **There is no secondary stack.** `_stack_secondary_top` exists only in
  `linker/qemu-rv64.ld`; `linker/esp32p4.ld` has none, so an SMP build of this
  board does not link today. (The ROM reserves `0x4ff3afc0..0x4ff3fba4` as a
  CPU1 stack — `linker/esp32p4.ld:16` already records it — but only while
  download mode is live. Ours is ours to place.)
* **Core 1 must configure its own CLIC.** `kernel/ticker.c:141` already knows
  the shape: the CLINT window at `0x20000000` is *"CLINT (self)"*, the other
  core's block is at `0x20010000`, and `kernel/ticker.c:380` notes each core
  has its own CLIC and calls `esp32p4_clic_timer_enable()` per-hart —
  *"unreachable today (`CONFIG_ENABLE_SMP` is off on this board) and correct
  when it is not"*. That prediction gets tested here.
* **Two per-core settings are pure performance and silent when wrong.** Core 1
  has its **own L1 instruction cache** (`ICACHE1`) and its **own branch
  predictor** (`soc_caps.h:185`, `SOC_BRANCH_PREDICTOR_SUPPORTED`; IDF enables
  it explicitly in `call_start_cpu1()`). Neither produces an error when left
  off. See §6 for what the first one does to a board executing from flash.
* **The flash-park contract does not exist on this board.**
  `drivers/flash_rp2350.c:146` calls `smp_flash_park_request()` before an erase,
  because a core fetching XIP while the MSPI is unavailable jumps into nothing.
  `drivers/flash_esp32p4.c` never asks — correct while there is one core — and
  `kernel/smp.c:693` stubs the whole trio to `return true` *"for every other
  target: there is no second core to park"*. Enabling SMP here turns that stub
  into a false statement. Phase 32 §2 built the RAM-resident flash path for
  exactly this hazard with one core; the second core reopens it from the other
  side.

## 5. Milestones

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
report two cores as slower than one"* (`user/chess/src/perft.c:428`).

Which is what makes perft a usable instrument for this phase. The two depth-4
runs of the *same* command differ by **5 ms across 557 s**, and the one-core
against two-core pair by 11 ms: run-to-run noise under 0.01 %, so a speedup
claim of any size at all will be unambiguous. The `cores used: 1` on every
line is the other half -- `CONFIG_ENABLE_SMP` is off on this board, so the
`cores` argument is clamped away before any work is split, and both columns
are the same single-core code.

**Done when:** `clocks` prints 40 / 20 / 20 / 10 MHz and those four numbers are
read from registers, not printed from constants.

#### Done, 2026-09-16 — and what it found immediately

`clocks` on the board:

```
[CLK] root      = XTAL (40 MHz, CONFIG_XTAL_HZ)
[CLK] CPU       = 40 MHz   (root / 1)
[CLK] MEM       = 20 MHz   (CPU / 2)
[CLK] SYS       = 20 MHz   (MEM / 1)
[CLK] APB       = 10 MHz   (SYS / 2)
[CLK] dividers  = cpu 1 (frac 0/0)  mem 2  sys 1  apb 2
[CLK] ROM says  = 40 MHz   (bookkeeping: ets_get_cpu_frequency)
[CLK] CLINT     = 41626307 Hz   (ticker's 2 ms window, at boot)
[CLK] CLINT     = 40000029 Hz   (1 s window, now)
```

**The divider set is the check, not the frequencies.** (1, 2, 1, 2) is exactly
what IDF's `rtc_clk_cpu_freq_to_xtal(to_default)` programs, and it is what
makes §1's cascade reading verifiable rather than plausible: read as four
parallel taps off HP_ROOT_CLK the same registers give 40-20-**40-20**, and the
documented power-on default is 40-20-**20-10**. A wrong model would have
printed a wrong answer here instead of agreeing with the one published figure
this board has.

**§1 needed two corrections, both found by writing the reads:**

* **The dividers are a cascade, not four taps.** CPU = ROOT/cpu, MEM =
  CPU/mem, SYS = MEM/sys, APB = SYS/apb. §1 said "separate dividers", which is
  true and misleading.
* **The source mux is not in HP_SYS_CLKRST.** It is `LP_CLKRST_HP_CLK_CTRL`
  (LPAON + 0x1000 + 0x40), a different peripheral in a different power domain.

**And one milestone instruction was wrong.** 34.1 as written named
`ets_clk_get_cpu_freq` (ROM `0x4fc00554`). That symbol is the one of the four
this phase needs whose **address moves between ROM revisions** — `0x4fc00554`
in `esp32p4.rom.ld` against `0x4fc00560` in `esp32p4.rom.eco0_4.ld`. The code
uses `ets_get_cpu_frequency` (`0x4fc00040`) instead, which is identical in
both. `ets_update_cpu_frequency` and `ets_set_appcpu_boot_addr` are identical
in both as well, so 34.4 and 34.9 are unaffected — but the check is now on
record and should be made for any further ROM call this phase adds.

#### The defect 34.1 found on its first run

**The preemption tick is running at 95.99 Hz against the 100 Hz it is asked
for**, because `kernel/ticker.c`'s 2 ms boot measurement of the CLINT is
4.07 % high. Measured 6192 ticks over 64.51 s; predicted from the bias, 96.09
Hz. Full evidence and the leading explanation are in `plan/open_issues.md`.

Two things follow for this phase. **It must be fixed before 34.4**, which
plans to read the CLINT's source off a change in the ticker's measured figure
— a reading that is worthless while that figure carries a 4 % systematic
term. And **it is the argument for this milestone existing at all**: the bias
is at least four days old, survived a full phase, and was invisible until
something printed two measurements of the same clock side by side.

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

#### Done, 2026-09-16

**It is chapter 11, "Reset and Clock", not chapter 8.** Corrected here because
the wrong pointer costs the next reader the same search.

##### The TRM check, field by field

Rendered and counted, against the register headers and against
`clk_tree_ll.h`'s getters. Three sources, no disagreements:

| TRM register | offset | field | bits | agrees with IDF |
|---|---|---|---|---|
| 11.2 `ROOT_CLK_CTRL0` | 0x0004 | `CPUICM_DELAY_NUM` | [3:0] | yes |
| | | `SOC_CLK_DIV_UPDATE` (WT) | [4] | yes |
| | | `CPU_CLK_DIV_NUM` | [12:5] | yes |
| | | `CPU_CLK_DIV_NUMERATOR` | [20:13] | yes |
| | | `CPU_CLK_DIV_DENOMINATOR` | [28:21] | yes |
| 11.3 `ROOT_CLK_CTRL1` | 0x0008 | `MEM_CLK_DIV_NUM` | [7:0] | yes |
| | | `SYS_CLK_DIV_NUM` | [31:24] | yes |
| 11.4 `ROOT_CLK_CTRL2` | 0x000C | `APB_CLK_DIV_NUM` | [23:16] | yes |
| 11.67 `LP_CLKRST_HP_CLK_CTRL` | 0x0040 | `HP_ROOT_CLK_SRC_SEL` | [1:0] | yes |
| 11.47 `ANA_PLL_CTRL0` | 0x00BC | `CPU_PLL_CAL_END` (RO) | [2] | yes |

Two things the TRM adds that IDF's accessors do not make visible:

* **The dividers do not take effect when written.** TRM §11.2.4.1: *"Updates
  to `HP_SYS_CLKRST_ROOT_CLK_CTRL0/1/2/3_REG` will take effect only after
  `HP_SYS_CLKRST_SOC_CLK_DIV_UPDATE` is set."* That is what IDF's
  `clk_ll_bus_update()` is, and §1's ordering requirement is unimplementable
  without it. A divider written and not committed is the failure mode where
  nothing appears to happen.
* **`1: CPLL_CLK (360 MHz)`** — the TRM's own encoding for the source mux,
  which settles §1's figure from the primary source rather than from IDF's
  frequency table. Worth pinning because the surrounding nomenclature
  disagrees: the register header comment says `2'd1: cpll_400m` and the
  adjacent gate is named `HP_CPLL_400M_CLK_EN`. **The 400 is a name; 360 is
  the documented frequency for this silicon.**

**And what the TRM does not contain at all: CPLL's analog configuration.**
Searching the whole 3701-page text for `REGI2C`, `I2C_CPLL` or `CPLL_CAL`
returns nothing. The PLL's divider lives on an analog I2C bus that the TRM
does not document, so for that one register ESP-IDF is not a cross-check --
it is the only source. That asymmetry is why the answer to Q1 below is
hedged where the others are not.

##### The three questions, answered from the board

`clocks` now prints the evidence for all three:

```
[CLK] CPLL raw  = 18 25 50 08 62 80   (regi2c 0x67, registers 0..5)
[CLK] CPLL      = div 8  ref_div 0  -> 320 MHz (IDF programs 9 for 360, 10 for 400)
[CLK] pll cal   = cpu 1  sys 1  sdio 0  plla 0  mspi 0   [ANA_PLL_CTRL0 = 0x00000044]
[CLK] flash     = src 0 (XTAL)  pll_clk_en 1  core_clk_en 1  [PERI_CLK_CTRL00 = 0x2800c00c]
[CLK] regulator = HP dbias 24 (RO)  dbias_sel 1  [HP_ACTIVE_HP_REGULATOR0 = 0xc6677180]
[CLK] dbias cal = efuse 9 -> IDF would program 25; board has 24  (DIFFERENT)
```

**Q1 — is CPLL running, and at what frequency? Running, yes. At what
frequency, unknown, and the register cannot tell us.**

There is no readable CPLL power bit: `PMU_TIE_HIGH_XPD_CPLL` and every
related bit is **WT**, write-triggered, so the register performs actions and
reports no state. The answer therefore comes from `CPU_PLL_CAL_END`, which is
RO, resets to 0, and reads **1** — a PLL that has been calibrated has been
brought up, and nothing in this kernel calibrates anything, so **the boot ROM
brought CPLL up before handing over.** `SYS_PLL_CAL_END` is 1 too.

The frequency is the hedged part. The divider reads 8, which under IDF's
`clk_ll_cpll_get_freq_mhz()` is 320 MHz — *a value IDF never programs*, since
`clk_ll_cpll_set_config()` writes 9 for 360 and 10 for 400 on this revision.
And that function carries the comment **"div7_0 bit2 & bit3 is swapped from
ECO1"**: a field whose write encoding is documented to differ from its bit
order is not one to read a megahertz figure out of. The raw dump
(`18 25 50 08 62 80`, six distinct bytes from six addresses) establishes that
the bus is answering truthfully; it does not establish what the answer means.

**What follows for 34.4:** it must *program* the CPLL divider and recalibrate
rather than adopt whatever the ROM left, and **it must verify the result by
measurement, not by reading the field back.** 34.1's
`esp32p4_clint_measure_hz()` is that measurement, and this is the second job
it has now earned.

**Q2 — what has the ROM left the regulator at? One step below what IDF would
program, and control is already handed over.**

`DIG_REGULATOR0_DBIAS_SEL` reads 1 — its reset default, and the same value
IDF's `rtc_clk_init()` ends up setting, so that part of the sequence is
already satisfied. `HP_DBIAS_VOL` reads **24**, which is exactly
`HP_CALI_ACTIVE_DBIAS_DEFAULT`, IDF's *uncalibrated fallback*.

But this part **is** calibrated: eFuse `active_hp_dbias` (BLK1 word 4,
[19:16], the block `drivers/efuse_esp32p4.c` already reads the MAC from)
reads **9**, and IDF's `get_act_hp_dbias()` computes `9 + 16 = 25`. So the
ROM left the regulator at the generic default and never applied this chip's
own calibration.

**This makes 34.3 small, and that is the milestone's payoff.** It is not a
port of IDF's REGI2C analog bring-up. It is one field: raise HP_ACTIVE dbias
from 24 to 25, the value this chip's eFuse asks for, before any CPLL
frequency is selected. What remains open for 34.3 to decide is the DCDC —
IDF enables it and programs `dcm_vset` in the same sequence, and nothing here
has established whether that matters at 360 MHz or only for sleep modes.

**Q3 — what clocks the MSPI? The crystal, directly, and not MEM_CLK.**

`FLASH_CLK_SRC_SEL` reads 0 = XTAL. The field selects between XTAL, CPLL and
SPLL (`SOC_FLASH_CLKS`) — it is **not** downstream of the CPU/MEM/SYS/APB
cascade at all. Nothing in ESP-IDF ever writes this field either; a grep of
the whole tree finds it only in the register header. Whatever the ROM left is
what runs, and the ROM's own banner agrees: `SPI mode:QIO, clock div:2`, i.e.
40 MHz / 2 = 20 MHz.

Two consequences, pulling in opposite directions:

* **Good for 34.5.** Raising the CPU clock does not change the flash clock,
  so MSPI timing tuning drops out of that milestone's risk list entirely —
  the flash keeps running at exactly the speed it runs at now. §3's worry
  about "a wrong MSPI timing at the new MEM clock" does not apply.
* **Bad for 34.6, and it sharpens the warning.** The flash stays at 20 MHz
  while the CPU goes to 360. Every L2 miss that reaches the MSPI will cost
  **nine times more CPU cycles than it does today**, and today is where the
  measurement that justified `CONFIG_L2_CACHE_KB = 128` was taken.

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

### 34.7 — Documents: the clock

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

### 34.8 — What core 1 needs, established before it runs

Read-only, and the answers decide 34.10's checklist rather than IDF deciding it.
Extend 34.1's `clocks` command to print, from registers:

1. **Is `ICACHE1` enabled**, at reset and after the ROM has handed over?
2. **Is the branch predictor on**, for core 0 and for core 1?
3. **Is `CORE1_CPU_CLK_EN` already set and `RST_EN_CORE1_GLOBAL` already
   clear?** IDF checks both before writing, because a debugger may have done it
   already; on a board with no debugger attached the answer is evidence about
   what the ROM leaves.
4. **Is the L1 D-cache shared**, confirmed on the TRM page rather than inferred
   from IDF's register list (§4.1's one must-verify).

**Done when:** the four answers are recorded here with their register evidence.

### 34.9 — Core 1 executes one instruction

Mirror RP2350 X3 exactly, and for the reason `kernel/smp.c:104` gives rather
than out of symmetry:

> The first attempt skipped this step and sent core 1 straight into
> `secondary_main()` […] It wedged, and because everything downstream is
> silent when it does, there was no way to say which half had failed.

So: a counter in `.bss` that core 0 reads back, and nothing else. No scheduler,
no trap handler, no `printk` on core 1. Launched by an explicit shell command,
not at boot — the same decision `smp_release_secondaries()` made for RP2350,
and for the same reason: a board that boots is a board that can be reflashed.

**Done when:** the counter moves, proving the stall release, the clock and
reset, the boot address, the stack and core 1's first instructions in one step
that cannot be confused with a failure above it.

### 34.10 — Core 1 in the kernel

`_stack_secondary` in `linker/esp32p4.ld`; `CONFIG_ENABLE_SMP` on for the
preset; entry.S's existing secondary path; per-hart CLIC and trap init; and the
two silent settings from §4.2 — `ICACHE1` and the branch predictor — enabled by
core 1 for itself, before it executes anything that is not already resident.

**Done when:** `smp_harts_online()` returns 2, the boot log shows
`[SMP] hart 1: in the kernel, no task yet (pid -1)` (the `-1` is
`secondary_main()`'s identity fix working; a `0` there is the bug it was written
for), and `tests/hw/test_esp32p4.py` is still green with the second core up.

### 34.11 — The flash-park contract, before two cores meet an erase

A gate, not a feature, and sequenced here for the same reason 34.3 precedes
34.4: the dangerous half goes first, alone.

Either give the P4 the parking protocol `drivers/flash_rp2350.c:146` already
has, or establish that this board's flash path is safe with a second core
executing XIP and say why. What is not acceptable is leaving
`kernel/smp.c:693`'s `return true` in place on a board where it is false.

**Done when:** a filesystem write soak — many writes under load with core 1
running, not one erase — completes with `/flash0` intact.

### 34.12 — perft on two cores: correctness first, then speed

**This is the success criterion for the second half of the phase**, and perft is
the instrument for the reason `user/chess/src/perft.c:118` gives:

> Perft is the honest first use of a second core, and the reason is the test
> table below: the node counts are exact and published, so a parallel run is
> either right or wrong with no argument about it.

In order, and the order matters:

1. **Correct.** `(perft 4 2)` returns `96 passed depths, 0 errors` and
   `cores used: 2`. A wrong node count means the split dropped or double-counted
   root moves and nothing about timing is worth reading.
2. **Faster.** Against 34.6's single-core-at-360 MHz figure, not against the
   40 MHz baseline — otherwise the clock's 9× and the core's 2× are reported as
   one number and neither is checkable.
3. **Honestly.** Expect **less than 2×**. `perft.c:129` splits root moves
   round-robin because *"root moves have wildly different subtree sizes"*, and
   says of it: *"Interleaving does not balance it perfectly — nothing static
   does — and the measured speedup is reported rather than claimed."* Report the
   number that comes out.

**A result below 1× is a diagnosis, not a disappointment.** It means core 1 is
fetching XIP with a cold `ICACHE1` and starving core 0 at the MSPI — see §6 —
and it is fixed in 34.10, not by tuning the split.

### 34.13 — Documents: the second core

* `plan/phase27_esp32p4_bringup.md` §7's *"The second HP core"* deferral gets
  its forward reference, as E4's PLL deferral does in 34.7.
* `kernel/ticker.c:380`'s *"unreachable today … and correct when it is not"*
  becomes reachable; correct or not, say which.
* `kernel/smp.c:690`'s *"there is no second core to park"* stops being true of
  every non-RP2350 target and the comment must stop saying so.
* A memory for the P4 core-1 launch sequence — the stall code, the two CLKRST
  bits, the ROM boot-address call, and the two silent per-core settings. It is
  five register writes that took a day to locate and would take a day again.

## 6. What could go wrong, stated in advance

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
* **A core 1 with a cold instruction cache makes two cores slower than one.**
  This is not hypothetical arithmetic: with `.text` in flash since phase 32, a
  core fetching uncached does not merely run slowly, it saturates the MSPI that
  core 0 is fetching through as well. The symptom is `(perft n 2)` slower than
  `(perft n 1)` — which is precisely the report that started this phase, arriving
  for real the second time. 34.8 asks the question and 34.10 closes it.
* **`smp_flash_park_request()` returns `true` today and would be lying.**
  Nothing fails at the call site; a filesystem does, later, once. 34.11 is a
  gate for that reason.
* **Less than 2× is the expected honest outcome**, because the root-move split
  is static and the subtrees are not equal. A phase that reports 1.7× has
  succeeded; one that reports 2.0× should be checked for a node count nobody
  verified.
* **This is where phases 22, 23 and 31 get tested on second silicon.** The
  locking, the hart records and the concurrency hierarchy were all built and
  reviewed against RP2350 — a different interrupt controller, no CLIC, no shared
  L1 D-cache. `plan/phase31_concurrency_hierarchy.md` is the thing to re-read
  before 34.10, not to re-derive after it.

## 7. Explicitly not in this phase

* **No lazy-SMP chess search on the P4.** X8b did that for RP2350, and it is a
  different kind of proof: a search sharing a transposition table has no exact
  answer to check against. Perft is the criterion here precisely because it
  does.
* **No work stealing, no dynamic balancing, no migration tuning.** The
  round-robin root split and pinned tasks of X8a, unchanged. If 34.12's measured
  speedup makes a better split worth having, that is the next phase's evidence.
* **No PSRAM.** Unchanged from phases 27 and 32.
* **No dynamic frequency scaling, no sleep modes, no DVFS.** One frequency,
  chosen at build time, set once at boot. IDF's `esp_pm` exists and is a
  different project.
* **No MSPI timing tuning unless 34.5 proves it necessary.** If flash
  round-trips at 360 MHz, the ROM's configuration is adequate and porting
  `mspi_timing_tuning` buys nothing measurable.
* **No change to the RP2350 or QEMU paths.** One board's clock tree and one
  board's second core. The blast radius is `cmake/board-esp32p4-nano.cmake`,
  `linker/esp32p4.ld`, `kernel/time.c`'s and `kernel/smp.c`'s P4 arms, the
  ESP32-P4 preset, and one new source file per half. `arch/riscv/common/entry.S`
  is touched only if 34.10 finds its existing secondary path insufficient —
  §4.1 expects it is not.
