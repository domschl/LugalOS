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

**Q3 — what clocks the MSPI? The crystal, directly, at 40 MHz, and not
MEM_CLK.**

```
[CLK] flash     = src 0 (XTAL) / 1 = 40 MHz  pll_en 1 core_en 1
```

`FLASH_CLK_SRC_SEL` reads 0 = XTAL and `FLASH_CORE_CLK_DIV_NUM` reads 0,
which is divide-by-one — not the register's reset default of 3, so the ROM
set it deliberately. **The flash already reads at 40 MHz, the fastest this
source can give it.** Three independent things agree: that divider, the boot
image header (`Flash freq: 40m`, esptool's default, which our `elf2image`
call does not override), and the ROM's own banner.

The field is **not** downstream of the CPU/MEM/SYS/APB cascade at all, and
nothing in ESP-IDF ever writes it; a grep of the whole tree finds it only in
the register header.

**Two things this milestone got wrong first, both worth recording.**

* **The ROM banner's `clock div:2` is not relative to the crystal.** Read
  that way it says 20 MHz, and this document said 20 MHz. On these parts the
  banner's divisor is against an 80 MHz reference, so `div:2` *is* 40 MHz.
  The register settles it and the banner does not.
* **The source encoding is the TRM's, not the order of an IDF array.**
  `SOC_FLASH_CLKS {XTAL, CPLL, SPLL}` is a set, not a mapping. The field's
  real values, from the TRM's own description, are `0: XTAL_CLK`,
  `1: SPLL_CLK (480 MHz)`, `2: CPLL_CLK (360 MHz)`, `3: Invalid` — 1 and 2
  the other way round. The printed table was wrong and read correctly only
  because this board reads 0. Precisely the inference
  `esp32p4-register-provenance` exists to prevent, made anyway.

Three consequences:

* **Good for 34.5.** Raising the CPU clock does not change the flash clock,
  so MSPI timing tuning drops out of that milestone's risk list entirely —
  the flash keeps running at exactly the speed it runs at now. §3's worry
  about "a wrong MSPI timing at the new MEM clock" does not apply.
* **Still bad for 34.6, at half the magnitude first claimed.** The flash
  stays at 40 MHz while the CPU goes to 360, so every L2 miss that reaches
  the MSPI costs **nine times more CPU cycles than it does today** — and
  today is where the measurement justifying `CONFIG_L2_CACHE_KB = 128` was
  taken. The absolute latency is half what the 20 MHz error implied; the
  ratio, which is what hurts, is unchanged.
* **There is a second lever, and it is now identified rather than
  hypothetical.** 40 MHz is the ceiling *from this source*. Pointing
  `FLASH_CLK_SRC_SEL` at SPLL (480 MHz) or CPLL (360 MHz) with a divider
  reaches higher — SPLL/6 is 80 MHz — and that is an independent knob from
  anything else in this phase. **It is not scheduled here**: it changes flash
  timing, which is the one hazard §3 named for the MSPI, and it should be
  pulled only if 34.6 shows XIP actually dominating. Recorded so that the
  option is known when that measurement exists.

### 34.2a — The tick bias, fixed *(added 2026-09-16; 34.1 found it, 34.4 needs it gone)*

Not planned. `plan/open_issues.md` carried it after 34.1 measured the
preemption tick at 95.99 Hz against the 100 Hz asked for, and it is here
because 34.4 plans to read the CLINT's clock source off a change in the
ticker's measured figure — worthless while that figure carries a 4 %
systematic term.

**The diagnosis was a measurement, not a reading of the code.** The same 2 ms
window, on the same silicon:

| window | when | reads |
|---|---|---|
| 2 ms | at boot, from `arch_ticker_init()` | 41 118 086 Hz |
| 1 s | from a shell command | 40 000 029 Hz |
| **2 ms** | **from a shell command** | **39 999 500 Hz** |

The third row is the whole answer: **the window length was never the
problem.** A 2 ms window is accurate to 12 ppm when the code taking it is
warm. What differs at boot is the instruction fetch — since phase 32 this
code executes from flash, `t0 = now()` is sampled *before* the first
`time_get_us()`, and that first call's cold fetch therefore lands inside the
tick window and outside the microsecond window. The two windows stop being
the same length and the ratio is the measurement. 4.07 % of 2 ms is 81 µs,
which is what a cold XIP call costs on this board.

**The fix is four calls.** Warming both paths before opening the window
restores the symmetry the code was always written for. No longer window, no
retry loop, no averaging — the systematic term is removed rather than
diluted, which is what `open_issues` asked for.

**Verified on the board:**

| | before | after |
|---|---|---|
| boot measurement | 41 118 086 Hz | **39 996 508 Hz** |
| preemption tick, 64.51 s | 95.9925 Hz | **99.8982 Hz** |

And the second column lands on `plan/phase27_esp32p4_bringup.md:1384`'s
**99.8742 Hz**, measured on this board before XIP existed. That closes the
story: the residual 0.1 % is `ticker_next()`'s relative rearm, which phase 27
already identified and quantified, and the 4 % was phase 32's move to flash
acting on a measurement nobody re-checked afterwards.

**RP2350 was checked and is not affected.** Its boot line reads
`10000 ticks of a 1000000 Hz clock, measured` — exactly its 1 MHz source.
It is accidentally warm (the RUNNING poll above the measurement already calls
`time_get_us()`) and its `now()` is two SIO reads rather than a systimer
handshake. The same warm-up was added there anyway, as insurance and so the
two arms read alike, and the 1 000 000 Hz was re-checked afterwards.

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

#### Done, 2026-09-16 — one field, as 34.2 predicted

Most of `rtc_clk_init()` turned out to be already satisfied, which is what
34.2 bought:

* `DIG_REGULATOR0_DBIAS_SEL` reads 1 — and the TRM's encoding is *"0:
  Regulated by Hardware automatically, 1: Regulated by Software"*, so control
  is already where IDF's sequence leaves it.
* `HP_ACTIVE_HP_REGULATOR_XPD` reads 1 — the regulator is already on.
* Both dbias fields read 24, IDF's *uncalibrated* fallback, on a part whose
  eFuse trim asks for 25.

So the milestone is one write: `HP_ACTIVE_HP_REGULATOR_DBIAS` 24 → 25, at
boot, before any clock is raised against it. Applied in
`esp32p4_regulator_apply_efuse_dbias()`, which never lowers the setting and
leaves an unburnt eFuse alone.

```
[PMU] HP_ACTIVE dbias 24 -> 25 (this chip's eFuse trim; regulator indicates 24)
[CLK] regulator = dbias ctrl 25 (R/W)  indicated 24 (RO)  sel 1 (software)  [0xce677180]
```

**The two fields disagree, and that is reported rather than resolved.** They
are different fields, both in the same register, and the TRM distinguishes
them: `[31:27]` R/W *"Regulates the voltage … the higher the value, the
higher the voltage"*, `[13:9]` RO *"Indicates the current voltage of the HP
system regulator"*. The control took the write — the register went
`0xc6677180` → `0xce677180` — and the indicator did not move. **This is not a
failed write**, but it does mean the resulting voltage is unconfirmed from
software. Settling it needs a meter on the core rail.

**`DIG_DBIAS_INIT` is not the missing commit, and trying it made things
worse.** Bit 15 is WT and the TRM calls it *"Initializes the PVT voltage
configurations"*, which reads exactly like the trigger that would make the
indicator follow. Setting it drove the indicator to **20** — down, away from
the trim being applied. Reverted in the same session; the effect does not
survive a reset. Recorded so nobody spends the same reboot.

**Verified at 40 MHz, which is the point of doing this alone:**

* `tests/hw/test_esp32p4.py` — 20/20.
* `(perft 3 1)` — 21 949 ms then 21 941 ms, 0 errors.

**And that pair produced a methodological finding this phase needs.** 21 941
against 21 949 is 0.04 % run-to-run — but both are **2.9 % slower than
34.1's 21 333 ms**, measured on the same source at the same clock with the
same voltage. The difference is the *image*: `.text` has grown ~1.7 KB across
these milestones, and against a 128 KB L2 cache holding 186 KB of code, which
lines collide depends on link-time addresses.

> **Rule for 34.6, and for 34.12 after it: compare one image at two clocks,
> never two builds.** Perft's run-to-run noise is under 0.05 %; its
> build-to-build noise is ~3 %, which is large enough to swallow or invent a
> result. The existing rule about in-guest timing is not sufficient on its
> own.

**Still open, deliberately: the DCDC.** `rtc_clk_init()` also enables it and
programs `dcm_vset`; this does neither. The P4's internal buck needs an
external inductor, and the NANO schematic's `EN_DCDC` / `FB_DCDC` /
`VDDPST_DCDC` nets with a 470K 1 % feedback divider read far more like an
*external* regulator IC than the chip's own converter — and "reads like" is
not a basis for enabling a buck. The voltage this phase needs arrives via the
LDO regardless, and the board is stable on that path today. If 360 MHz proves
unstable or hot, this is the first thing to revisit, with the board's
inductor identified first.

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

#### Done, 2026-09-17 — and it is 80 MHz, not 90

```
[CLK] CPU at 80 MHz, measured (table entry '90 MHz' assumes CPLL 360; this board's is 320)
[CLK] root      = CPLL (320 MHz, from its own divider)
[CLK] CPU = 80   MEM = 80   SYS = 80   APB = 80 MHz
[CLK] ROM says  = 80 MHz
[CLK] CPU meas   = 80000139 Hz   (mcycle, 100 ms window)
```

**The switch answered the two questions 34.2 could not.**

**CPLL runs at 320 MHz, and the register was telling the truth all along.**
34.2 hedged because the divider read 8 — a value ESP-IDF never programs, in a
field IDF's own source says has two bits swapped from ECO1. Selecting CPLL
with `cpu_div = 4` and measuring settled it: 80 MHz out means 320 MHz in,
which is exactly `xtal × div / (ref_div + 1)` for div 8. **The formula is
right, the field is truthful, and the boot ROM simply configures this PLL to
a frequency IDF has no table entry for.** `clocks` now derives the root from
that divider instead of printing a nominal, so every figure under it is
correct rather than assumed.

**The CLINT follows the CPU clock.** `kernel/ticker.c:181` has carried the
question since E4 — *"Nothing states what clocks this counter"* — because at
40 MHz the crystal and the CPU were the same number. They are not any more:
the ticker measured `79 964 535 Hz` at boot. Two consequences worth
recording. The self-measurement in `ticker_init()` is not a nicety on this
board, it is what keeps preemption at 100 Hz across a clock change — and it
did, unchanged, which is 34.2a's fix working under the first real test. And
mtime's low half now wraps every 53 s rather than 107, so the `MTIME_SAM`
hazard `kernel/ticker.c` documents arrives twice as often as it used to.

**A bug caught by measuring rather than announcing.** The first version passed
`CONFIG_CPU_FREQ_MHZ` to `ets_update_cpu_frequency()`, and `clocks` duly
reported `ROM says = 90 MHz` against a CPU running at 80. Every ROM delay
loop — flash, cache, PMU — would have been calibrated 12.5 % wrong. *Long*
rather than short, so nothing would have broken and nothing would have shown
it. `esp32p4_cpu_freq_set()` now measures first and hands the ROM the result;
20 ms once at boot removes the class.

**Speedup: 10 617 ms against 21 941 ms, 0 errors — 2.07× on a 2.00× clock
change.** The 3.3 % excess is not super-linear scaling, it is the
build-to-build term 34.3 measured independently at 2.9 %: this is a
cross-build comparison, and 34.4's own deliverable is a config that changes
at compile time. Two independent sightings of the same artefact now, which
is the argument for 34.6 needing a **runtime** switch rather than a rebuild —
noted there rather than built here.

**What it says about XIP, early and provisionally.** Perft scaled linearly
with the clock while the flash stayed at 40 MHz, so at this ratio the L2 is
still absorbing the miss cost. That is one data point at 2×, not a prediction
for 9×, and 34.6 is still where it gets settled.

**Consequence for 34.5.** `CONFIG_CPU_FREQ_MHZ` names an IDF table entry
whose nominal assumes CPLL is 360; this board's is 320, so the three entries
actually yield 80, 160 and 320 MHz. Reaching 360 means **programming CPLL**
(div 8 → 9) and recalibrating — which 34.2 said 34.4 might have to do and
34.4 deliberately did not, to keep one change per milestone. 320 MHz is on
the table as a legitimate stopping point if reprogramming the PLL proves
unattractive: it is 8× rather than 9×.

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

#### Partly done, 2026-09-17 — CPLL is at 360, the CPU is at 180, and 360 is blocked

**CPLL reprogramming works.** 320 → 360 MHz, three REGI2C writes and a
calibration cycle — the first analog-bus *writes* in this tree. `clocks` reads
div 9 afterwards and the CPU frequency derived from it matches what `mcycle`
measures, so the whole chain is consistent.

Two things were needed that reading IDF did not make obvious, and both cost a
reboot each to find:

* **`LP_I2C_ANA_MST_CLK160M` bit 0 is 0 at reset**, and IDF sets it before any
  PLL configuration (inside its `ANALOG_CLOCK_ENABLE()` macro). *Reads work
  without it* — every register dump in 34.2 was taken with it at 0 and
  returned repeatable, sensible bytes — but **calibration does not**. The
  first attempt hung in the CAL_END poll with a silent console.
* **The ROM's `uart_tx_wait_idle` (0x4fc00078) hangs this kernel.** It is the
  obvious way to drain the console before the switch, it is what IDF calls,
  and its address is identical in both ROM variants. It still hangs: this
  kernel reconfigures UART0 in `drivers/uart_esp32p4.c`, and the ROM routine
  polls for a condition that never arrives against that setup. Replaced with
  a bounded drain on the TXFIFO_CNT field the driver already uses.

**Every wait in this path is now bounded**, which is the only reason the
first of those was diagnosable at all. A PLL that will not calibrate reports
a step number and the board stays on the crystal with its console.

**The console garbles at the switch, and that is expected rather than
broken.** APB goes 10 → 90 MHz while a character is in the transmit path. The
baud rate is immune — the UART is crystal-clocked, which is §2's whole point —
but the bytes in flight are not. This looked far worse than it was: the first
360 MHz attempt printed one line and then apparently nothing, which reads
exactly like a board that died at the switch, and it had not. Hence the drain.

##### 180 MHz: verified

```
[CLK] CPLL 320 -> 360 MHz (rc=0)
[CLK] CPU at 180 MHz, measured
CPU = 180  MEM = 180  SYS = 180  APB = 90 MHz     ROM says 180
CPU meas = 180 001 310 Hz     ticker measured 179 997 001 Hz
```

* `tests/hw/test_esp32p4.py` — **20/20**.
* `(perft 3 1)` — **4843 ms against 21 941, 0 errors: 4.53× on a 4.5× clock.**

**Still linear, and that is the interesting part.** Perft has now scaled with
the clock at 2× and at 4.5× while the flash stayed at 40 MHz. At a 4.5:1
CPU-to-flash ratio the L2 is still absorbing the miss cost, so §3's XIP worry
has not begun to bite. It is one more data point, not a prediction for 9×, and
34.6 still settles it.

For scale: the RP2350 does this suite in 6465 ms. **The P4 has overtaken the
other board**, having started the phase 3.3× behind it.

##### 360 MHz: blocked, and the diagnosis is voltage

It dies a few instructions after the switch, **deterministically** — three
resets, identical. Bisected with characters written straight into the UART
FIFO (the technique `xip_esp32p4.c` documents for its own blind window):

```
ABCDEFG     A..E all four dividers committed
            F   console drained
            G   THE MUX SWITCH ITSELF SURVIVED
            H   never reached
```

**The discriminator that matters: 180 MHz runs MEM at 180 too, and is fine.**
So this is not the memory path, not the flash path, and not MSPI timing —
those are identical between the working and failing cases. It is the core, at
360, and nothing else.

Which points straight back at what 34.3 could not close: **the dbias control
field reads 25 and the regulator's indicator still reads 24.** The most likely
reading is that the chip's own voltage trim never took effect and 360 MHz is
being attempted at the uncalibrated voltage.

**What reaching 360 would take.** The part of IDF's `rtc_clk_init()` that 34.3
deliberately skipped: the `I2C_DIG_REG` analog writes (block 0x6D —
`FORCE_RTC_DREG` reg 10 bit 0 and `FORCE_DIG_DREG` reg 10 bit 1 to 1,
`XPD_RTC_REG` reg 13 bit 2 and `XPD_DIG_REG` reg 13 bit 3 to 0) that put the
digital regulator under register control, and possibly the DCDC. The fields
are identified and the REGI2C write path now exists, so it is perhaps thirty
lines.

**It is not done here, and that is a decision rather than an omission.** It is
a core-voltage path on hardware, the TRM documents no part of REGI2C, and a
wrong bit fails as brownout or overvolt rather than as a failed test — which
is precisely why §3 called this the hazard that makes this a phase. What must
**not** happen is raising dbias above the eFuse's 25 to brute-force a boot:
that is overvolting past the vendor's calibration point, and it is the one
move here that damages a part rather than merely failing.

##### 360 MHz reached — it was the DC-DC, not the LDO

The analog work was authorised and done, in two stages, each regression-tested
at 180 MHz before being tried at 360.

**Stage one: the digital regulator under register control.** The four
`I2C_DIG_REG` and four `I2C_BIAS` writes from `rtc_clk_init.c` — ported in
IDF's order with fields from `regi2c_dig_reg.h` and `regi2c_bias.h`.
`FORCE_DIG_DREG = 1` is the load-bearing one: it takes the regulator voltage
off analog control and puts it on the register 34.3 had been writing.

Read back from the board, the writes land exactly as intended:

```
DIG_REG[10]=0x03   FORCE_RTC_DREG=1, FORCE_DIG_DREG=1
DIG_REG[13]=0x42   XPD_RTC_REG=0, XPD_DIG_REG=0
BIAS[4]=0x00       all four force-on overrides cleared
```

**And 360 MHz still failed.** Which is the most useful negative result in the
phase: with the regulator demonstrably under register control at this chip's
own eFuse trim, the LDO cannot run this core at 360. The remaining gap is not
a missing register write, it is voltage.

**Stage two: the external DC-DC.** IDF's `rtc_clk_init()` finishes by moving
the core onto an external converter and switching the LDO off, and it does so
**unconditionally on every ESP32-P4 boot** — not behind a Kconfig gate. The
evidence that this board is built for it:

* `Kconfig.dcdc` describes an external part ("TI-TLV62569/TLV62569P"), not an
  SoC block;
* the NANO schematic carries `EN_DCDC`, `FB_DCDC` and `VDDPST_DCDC` with a
  470K 1 % feedback divider, and MPS regulators (MP1605/MP1658);
* **Waveshare's own shipped firmware**
  (`~/gith/esp/ESP32-P4-Platform/firmware/brookesia`) overrides neither the
  CPU frequency nor anything in the DCDC menu, so the board ships running
  exactly this sequence.

`dcm_vset` is IDF's own `max(PVT, 27)`, and it is deliberately not adjusted
for the MPS parts: 27 is the figure the stock firmware this board ships with
uses against this board's own feedback network. Deriving a different number
from a different regulator's datasheet would be the mistake, not the caution.

**And it resolves 34.3's mystery as a side effect.** `HP_DBIAS_VOL` was never
a dbias readback. IDF reads that field as `pvt_hp_dcmvset` — a PVT-derived
*floor for the DC-DC setting* — which is why it never followed a dbias write,
and why `DIG_DBIAS_INIT` moved it to 20 when 34.3 poked it. `clocks` prints
control and indicator separately for exactly this reason; the two were never
the same quantity.

##### 360 MHz: verified

```
[CLK] CPLL 320 -> 360 MHz (rc=0)
[CLK] CPU at 360 MHz, measured
CPU = 360   MEM = 180   SYS = 180   APB = 90 MHz      ROM says 360
CPU meas = 360 000 920 Hz      ticker measured 360 112 000 Hz
```

* `(perft 3 1)` — **2563 ms against 21 941, 0 errors.**
* `/flash0` mounted, heap 372 KB, boot stack peak 11 484 B — unchanged.

**8.56× on a 9.0× clock change**, and that shortfall is the first sign of §3's
XIP concern arriving: about 3 % of it is the build-to-build term 34.3
measured, and the rest is a 9:1 CPU-to-flash ratio starting to cost what 2:1
and 4.5:1 did not. It is a small effect at the top of a 9× win, and 34.6 is
where it gets characterised rather than guessed at.

For scale: this suite took **21 941 ms** on this board at the start of the
phase and **6465 ms** on the RP2350. It now takes 2563 ms.

**Where this leaves the phase.** The clock half is done at its ceiling —
360 MHz is the maximum this silicon revision offers, since the 400 MHz ladder
needs rev ≥ 3.0. 34.6 re-measures what the clock invalidated, and the L2
cache trade is now genuinely live rather than theoretical.

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

#### Done, 2026-09-17

The rule 34.3 wrote — *one image at two clocks, never two builds* — is what
this milestone is built on, and honouring it needed a tool: `cpufreq <mhz>`,
a runtime switch with a downscale path (mux **first** going down, per
`rtc_clk_cpu_freq_to_xtal()`'s own warning about divider constraints). Every
number below except the L2 pair is one image, so the ~3 % build-to-build term
is out of the picture entirely.

##### The XIP penalty: 241 ms, and it is not the cache

`(perft 3 1)` at all four clocks, one image:

| CPU | perft | speedup | clock ratio | efficiency |
|---:|---:|---:|---:|---:|
| 40 MHz | 21 037 ms | 1.000× | 1.00× | 100 % |
| 90 MHz | 9 438 ms | 2.229× | 2.25× | 99.1 % |
| 180 MHz | 4 831 ms | 4.354× | 4.50× | 96.8 % |
| 360 MHz | 2 552 ms | **8.243×** | 9.00× | **91.6 %** |

Fitting `T(f) = A·(40/f) + B` gives **A = 20 796 ms clock-bound and
B = 241 ms clock-independent**, and that two-parameter model predicts the two
intermediate points to within 0.7 %. The 241 ms is the flash-bound term, and
it is the whole of §3's worry made arithmetic:

* at 40 MHz it is **1.1 %** of runtime — which is why phase 32 could move
  `.text` into flash and measure no cost, and was right to;
* at 360 MHz it is **9.5 %** — the same absolute stall, nine times more
  expensive in CPU cycles.

So the phase delivers 8.24× of a theoretical 9×, and the missing 8.4 % has a
name and a size rather than being a shrug.

##### The L2 trade: 128 KB stays, and the answer is unambiguous

The same four-point sweep, rebuilt with `CONFIG_L2_CACHE_KB = 256` (which
also needs `RAM` cut from 384K to 256K, dropping the heap from 372 KB to
244 KB):

| CPU | 128 KB | 256 KB | difference |
|---:|---:|---:|---:|
| 40 MHz | 21 037 ms | 21 042 ms | +5 ms (+0.02 %) |
| 90 MHz | 9 438 ms | 9 438 ms | **0** |
| 180 MHz | 4 831 ms | 4 831 ms | **0** |
| 360 MHz | 2 552 ms | 2 552 ms | **0** |

Three of four are bit-identical and the fourth differs by 5 ms in 21 s. The
fitted flash term is **241 ms for both**. Doubling the L2 changes nothing,
at any clock.

**Which also says what the 241 ms is.** It is not capacity — a cache twice
the size would have reduced capacity misses and did not touch it. What is
left is compulsory misses and the MSPI's fixed latency on them, neither of
which more cache can avoid. If that term is ever worth attacking, the lever
is flash *speed* (34.2's second lever: `FLASH_CLK_SRC_SEL` at SPLL/6 for
80 MHz), not cache size.

**So commit `37902c0`'s conclusion survives.** Its "under 0.1 % between 128
and 256 KB" was measured at 40 MHz where a miss was nearly free, and this
milestone existed because that might have been luck. It was not. **128 KB and
the 372 KB heap both stay**, now on evidence taken where it matters.

##### The tick, and the latency it implies

**99.9716 Hz over 124.5 s — −284 ppm**, implying an interrupt latency of
**2.84 µs**.

Against `plan/phase27_esp32p4_bringup.md:1384`'s 99.8742 Hz / −1258 ppm /
12.6 µs at 40 MHz — but **that is not a clean pair and should not be read as
one**: phase 27's image was RAM-resident, before phase 32 moved `.text` to
flash. This phase's own prediction ("roughly 126 ppm" on a 400 MHz core) is
missed by about 2×, in the direction the XIP measurement above predicts, since
the trap path now takes its instruction fetches through the same flash.

What the figure does establish cleanly is that **34.2a's fix holds under a
9× clock change**: the tick was asked for 100 Hz and delivers 99.97, with the
rate self-measured at boot at whatever the CLINT turns out to be.

##### The EMAC: 2000 echoes, zero loss

Phase 28 chased ~1 % frame loss to the RMII pad drive strength, and §6 asked
whether CPU/DMA contention for L2MEM changes shape at 9×.

**2000 packets transmitted, 2000 received, 0 % loss**, rtt avg 0.586 ms, at
360 MHz. The sample size is the point — `measure-before-declaring-fixed`
requires it, and a 1 % rate would have lost about twenty of these. The full
suite is 20/20 at 360 MHz alongside it, link negotiation included.

##### Comments corrected, as the done-condition requires

* `drivers/emac_esp32p4.c` said *"This kernel has never configured the P4's
  clock tree ... this file does not know it"*. It does now. The MDC divider
  is unchanged and the comment explains why that is the choice vindicating
  itself: CSR/124 gives 161 kHz at SYS 20 MHz and 1.45 MHz at SYS 180, both
  far inside IEEE 802.3's 25 MHz cap, so a 9× system clock needed no edit
  beyond the prose.
* `kernel/ticker.c` carried *"Nothing states what clocks this counter"* as an
  open question since E4. **It follows the CPU clock**, and the file now says
  so with the measurements that established it.
* The same file's "107 seconds" wrap window is 40 MHz arithmetic; at 360 MHz
  mtime's low half wraps every **11.9 s**, so the `MTIME_SAM` hazard it
  documents arrives nine times as often.

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

#### Done, 2026-09-17 — and the clock half of phase 34 closes here

* `plan/phase27_esp32p4_bringup.md`'s E4 deferral (*"No PLL … it belongs with
  E6 or later"*) carries its forward reference. The deferral was right: E4
  would have spent the phase on a clock tree instead of a console.
* `drivers/emac_esp32p4.c` and `kernel/ticker.c` were corrected in 34.6,
  where the measurements that falsified them were taken.
* `README.md` gains the figure, because it now has one worth stating: this
  board went from the slowest target in the tree to the fastest during a
  single phase.
* **`esp32p4-l2-cache-steals-ram` needs no change.** 34.6 asked its question
  at 360 MHz and got the same answer, so the memory's claim still holds — the
  one outcome that required nothing to be rewritten.
* Memory `esp32p4-clock-and-regulator` records what cost the most to derive:
  that 360 MHz needs the external DC-DC rather than the LDO, that
  `HP_DBIAS_VOL` is a PVT floor and not a dbias readback, that
  `LP_I2C_ANA_MST_CLK160M` must be set before calibration though reads work
  without it, and that the ROM's `uart_tx_wait_idle` hangs this kernel.

**The clock half is complete.** 40 → 360 MHz, 8.24× measured on perft, at the
ceiling this silicon revision offers. What remains of phase 34 is the second
core.

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

#### Done, 2026-09-17

`smpinfo`, read-only, on the board:

```
[SMP] icache    = ibus0 open  ibus1 open   [L1_ICACHE_CTRL = 0x00000000]
[SMP] branchpred = this hart: RS 1  BFE 1  BTB 1   [MHCR = 0x00001030]
[SMP] core1 clk  = off (reset default off)   [SOC_CLK_CTRL0 = 0xe6df97af]
[SMP] core1 rst  = HELD (reset default held)  [HP_RST_EN0 = 0x00000100]
[SMP] core1 stall= code 0x00 (0x86 stalls, 0xff runs)  stalled=0
[SMP] L1 dcache  = shared by both cores (TRM Figure 9.3-3)
```

**1. The instruction buses are both open**, `SHUT_IBUS0` and `SHUT_IBUS1`
clear. That is the bus path, not the cache enable, and the two are not the
same question — so 34.10 will call `Cache_Enable_L1_CORE1_ICache` (ROM
`0x4fc004e4`, identical in both ROM variants) explicitly rather than infer
from an open bus, which is what IDF does and costs one call.

**2. The branch predictor is a CSR, and it is on for core 0.** `MHCR`
(0x7c1) reads `0x1030` — RS, BFE and BTB all set, so the ROM enabled it. It
is **per-hart state**, so core 1 must set it for itself exactly as it must
set its own `mtvec`; core 0 having it changes nothing for core 1. This is the
quieter of the two silent-when-wrong settings and the reason 34.12 reads a
sub-1× result as a diagnosis rather than a disappointment.

**3. Nothing has touched core 1's clock or reset.** `CORE1_CPU_CLK_EN` is
clear and `RST_EN_CORE1_GLOBAL` is set, both at their documented reset
defaults. IDF checks these before writing because a debugger may have got
there first; on this board, with nothing attached, they are exactly as the
silicon left them.

**4. Core 1 is held by reset, not by the stall.** `HPCORE1_SW_STALL_CODE`
reads `0x00` — neither `0x86` (stall) nor `0xff` (run) — and
`CORE1_CORESTALLED_ST` reads 0. So the launch reduces to *enable the clock,
release the reset, set the boot address*; the unstall write IDF makes is
belt-and-braces here rather than load-bearing. 34.9 will still make it, since
a launch that depends on a stall register reading 0 is a launch that breaks
the first time something else writes it.

**And the one that had to come off the TRM page: the L1 data cache is
shared.** §4.1 flagged this as the single must-verify, because a wrong answer
is silent data corruption rather than lost speed. **TRM Figure 9.3-3 "Cache
Structure"** (page 910) draws it directly: HP CPU0 and HP CPU1 above, an L1
row containing **icache, icache, dcache** — two instruction caches, one data
cache — with the data bus tapped by both cores. 16 KB each for the icaches,
64 KB for the shared dcache, above a unified L2.

So cross-core data coherence is free on this chip. Two cores writing the same
structure see each other with no maintenance, which is what makes 34.10 small
and 34.14's shared transposition table a non-issue.

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

#### Done, 2026-09-17 — first time this core has ever executed our code

```
[SMP] CORE1_ALIVE -- counter 0 -> 7198790 in 100 ms
[SMP] core1 clk  = ENABLED   core1 rst = released   stall code 0xff
```

**No stack, and that is the milestone doing its job.** `linker/esp32p4.ld` has
no `_stack_secondary` (§4.2) and giving core 1 one is 34.10's work, so the
probe is four instructions of assembly with no calls: load, add, store,
branch. This milestone can therefore fail for exactly one reason, which is
the entire argument `kernel/smp.c:104` makes about X3.

**The rate is a second result, and a better one than "it runs".**
7 198 790 increments in 100 ms is **71.99 M/s**, which against a 360 MHz clock
is **5.00 cycles per iteration** of a four-instruction loop — about what a
store-to-load dependency through the data cache should cost.

So core 1 is not merely alive, it is running **at full speed with a working
instruction cache**. That matters because §4.2 listed `ICACHE1` as one of two
settings that are silent when wrong, and the arithmetic settles it: an
uncached fetch of every instruction over a 40 MHz MSPI would be two orders of
magnitude slower, not 5 cycles. **The ROM leaves core 1's instruction cache
usable.**

34.10 will still enable it explicitly. The cost is one ROM call and the
alternative is depending on a ROM behaviour nothing in this tree asked for —
but the number above means a sub-1× result in 34.12 now has one fewer
candidate explanation, which is what 34.8 and 34.9 exist to produce.

The launch was IDF's `start_other_core()` order, and 34.8's reading of it
held: the stall was already released (`0x00`, neither stall nor run), so it
was the **reset** bit that had core 1 parked. The unstall write is made
regardless and `smpinfo` afterwards shows all three moved — clock enabled,
reset released, stall code `0xff`.

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

#### Done, 2026-09-17

**The hazard was real and specific.** `drivers/flash_esp32p4.c` protects the
core doing the writing — `.ramfunc` routines, ROM pointers resolved before
entry, and `mstatus.MIE` masked. **`mstatus.MIE` is per-hart.** Masking it on
core 0 says nothing whatsoever about core 1, which since 34.10 runs an idle
task whose code is in flash like everything else. An erase would have pulled
the instruction stream out from under it, and the failure would not have been
an error — a fetch from a busy flash chip returns whatever it drives, and the
CPU executes that.

**Extracted rather than rewritten.** X7's protocol was inside
`kernel/smp.c`'s `#if CONFIG_ENABLE_SMP && defined(CONFIG_BOARD_RP2350)`
block along with the SIO FIFO handshake and everything else RP2350-shaped. It
now sits in its own block guarded for *both* boards that execute from flash.
The reasons differ — RP2350 turns XIP off outright, the P4 leaves the mapping
alone but the chip stops answering reads for tens of milliseconds — and the
mechanism does not: core 1 parks in a `.ramfunc` spin, acknowledges from RAM,
and is released after. Both boards have `.ramfunc`; QEMU keeps the stubs.

`flash_p4_erase_sector()` and `flash_p4_write()` now ask, and **refuse the
write** if core 1 does not park — the same decision `flash_rp2350.c:146`
makes, and for the reason it gives: a refused write is something a caller can
report, a hopeful one is a board that stops mid-erase.

**The soak.** 120 rewrites of a file on `/flash0`, with `harts online = 2`
before and after, read-back verified every 20:

```
   20 writes,  21 s, readback OK        100 writes, 103 s, readback OK
   40 writes,  41 s, readback OK        120 writes, 123 s, readback OK
   60 writes,  62 s, readback OK
   80 writes,  82 s, readback OK
RESULT: 120 rewrites in 123 s, 0 park-refused
/flash0 15% used -- unchanged
```

Each rewrite erases and programs both the data cluster and the directory
entry, so this is roughly 240 erase/program windows with a second core live
and **not one park refusal**. `flashtest`'s erase/program/read-back also
passes with core 1 running.

##### Why the soak rewrites rather than creates, and the bug that decided it

The first attempt created new files and nearly all of them failed. That is
**not** this milestone's hazard, and the control is the whole reason it can
be said so plainly — from a fresh boot, both ways:

| | create new file | rewrite existing |
|---|---|---|
| `harts online = 1` | `#f` | `#t`, content verified |
| `harts online = 2` | `#f` | `#t`, content verified |

**Identical single-core.** File creation on `/flash0` is broken independently
of the second core and of everything phase 34 has touched; `/proc/df` shows
the filesystem 15 % full, so it is not space either. Filed in
`plan/open_issues.md` with the leading suspicion (a full root directory) and
the observation that the create path returns a bare `#f` with no diagnostic,
which is itself worth fixing before guessing.

So a creation soak would have measured that bug instead of the park protocol.
Rewrites exercise the same erase and program path — which is the window the
park protects — without the confound.

One other thing the soak surfaced, also filed: the shell's `write` command
builds and evaluates a Lisp call, and after ~60 invocations the Lisp string
pool is exhausted. Harmless interactively, a real limit for scripted use, and
it cost one of the 120 writes. The filesystem was unaffected — the next
checkpoint read back clean.

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

#### Done, 2026-09-17 — 1.91×, and the node counts are exact

One image, one boot, `harts online = 2` verified before measuring; only the
`cores` argument differs between rows:

```
(perft 4 1)   96 passed depths, 0 errors (cores used: 1, 61 007 ms)
(perft 4 2)   96 passed depths, 0 errors (cores used: 2, 31 980 ms)
(perft 4 1)   96 passed depths, 0 errors (cores used: 1, 61 015 ms)
(perft 4 2)   96 passed depths, 0 errors (cores used: 2, 31 970 ms)
```

**Correctness first, and it holds.** 96 depths against published node counts,
**0 errors**, `cores used: 2`. That is the whole reason perft was chosen over
the search: the answer is exact, so a parallel run is either right or wrong
with no argument about it. The split drops no root move and double-counts
none.

**Then speed: 1.908×**, from 61 011 ms to 31 975 ms. Run-to-run spread is
8 ms and 10 ms — under 0.03 % — because this is one image at one boot, which
is the rule 34.3 wrote and 34.6 built the tooling for.

**That is better than this milestone predicted, and the prediction deserves
correcting rather than quietly forgetting.** §34.12 said *"expect less than
2×"* on the grounds that `perft.c`'s round-robin split is static and root
moves have wildly different subtree sizes. Both remain true; at depth 4 they
simply cost less than expected — 4.8 % against a perfect 2×.

**Depth is what decides it, and depth 3 shows the other end.** The same two
cores on the same boot:

| suite depth | 1 core | 2 cores | speedup |
|---|---:|---:|---:|
| 3 | 2 574 ms | 2 038 ms | 1.26× |
| 4 | 61 011 ms | 31 975 ms | **1.91×** |

The split is **per position, per depth**: `run_perft_cores()` allocates worker
records, spawns a pinned task, joins and frees for each of the 96 depths the
suite walks. Most of those are tiny — a depth-1 position is a few dozen nodes
— so at depth 3 the overhead is a large fraction of the work and the small
positions cap what is achievable. At depth 4 the big subtrees dominate
(kiwipete 4.08 M nodes, position-6 3.89 M, strange bugs 2.1 M) and the same
overhead amortises away. Neither number is wrong; they measure different
mixes, and the honest summary is the pair.

##### What phase 34 has done to this workload

```
 40 MHz, 1 core   557 153 ms      the board as this phase found it
360 MHz, 1 core    61 011 ms      9.13x
360 MHz, 2 cores   31 975 ms     17.42x
```

The depth-4 suite ran in nine and a half minutes at the start of the phase and
runs in thirty-two seconds now. (The 9.13× single-core figure is above the
9.00× clock ratio; that is cross-build comparison against a 40 MHz image from
before several milestones of `.text` growth, and 34.3 measured that term at
~3 %. The two-core figure inherits the same caveat. Both are quoted against
the phase's starting point rather than as clean same-image ratios, which the
rows above them are.)

##### One thing that went wrong, and it was the harness

The first attempt at this measurement produced a log that stopped mid-run and
looked exactly like a hung board. It was not: the driver was run with a
10-second idle timeout, and **a single depth-4 position produces no output for
longer than that** while it works. The driver declared the run finished
fifteen lines in, fired the remaining commands into the line editor, and
stopped listening; the board ran all of them and returned to its prompt.

The fix already existed — the driver used for 34.4's measurements waits for
the suite's own `PERFT Results` line rather than for silence, precisely
because silence is not an end-of-run signal here. Worth recording because the
failure mode is *indistinguishable from a hang* at a glance, and this phase
has met a genuine one.

### 34.14 — Lazy SMP: chess on both cores

*Added by amendment, 2026-09-17. This was in §7 as out of scope on the
grounds that a search sharing a transposition table has no exact answer to
check against. That is true and it is not a reason to skip it: **chess is the
only workload in this tree that a second core actually helps**, and a phase
that brings up SMP without running the one application that wants it has
proved a mechanism rather than delivered a capability.*

Perft stays the correctness criterion — 34.12 is unchanged and comes first,
because a search is only worth timing once the move generator is known to be
right on two cores.

**The code already exists.** X8b (`plan/phase23_multicore_scheduling.md:780`)
built Lazy SMP for RP2350: a helper searches the same root to its own
schedule, shares only the transposition table, and its result is discarded.
`(chess [cores])` and `(chess-selftest [cores] [tt_kb] [depth])` are the
entry points. None of it is RP2350-specific, so this milestone is about
*enabling and measuring*, not writing.

**One thing this board makes cheaper than the other one.** §4.1 established
that the P4's two HP cores **share their L1 data cache**. A transposition
table written by one core and read by the other needs no maintenance at all
here — the hazard that usually makes shared-TT Lazy SMP delicate is absent.

**Measure time-to-fixed-depth, and X8b's account says why in detail.** Its
first metric was "how deep in a fixed 2 seconds", and that metric said the
helper was *pure overhead* — identical mean depth, ~8 % fewer nodes searched
by the primary. Time to a fixed depth showed **1.60×** on RP2350. Use the
metric that was right, and quote the median of at least three runs the way
X8b did.

**Done when:** `(chess-selftest 2 ...)` agrees with the one-core result on
the positions it checks, and time-to-fixed-depth at two cores is recorded
against one core at 360 MHz — median of three, with the speedup reported
rather than claimed. A result at or below 1.0× is a finding too, and the
first thing to check is whether the helper is pinned to the second hart at
all.

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
