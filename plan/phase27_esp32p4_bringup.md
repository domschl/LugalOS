# Phase 27 — A second silicon, and nothing clever on it yet

**Status: planned, 2026-09-05.** Not started. This is the first of three
phases on the ESP32-P4 (Waveshare ESP32-P4-NANO); phases 28 and 29 are
sketched in the addendum and deliberately not designed here.

## 0. Why this phase exists, and why it is deliberately dull

Every persona this project has shipped runs on one family of silicon. That is
not a portability claim; it is an untested assumption wearing the costume of
one. `arch/riscv/` has three backends and two memory models, but all three
have only ever been aimed at Hazard3 and at QEMU's `virt`, and the seams that
look general are general in the way untested code is always general.

A second platform is how that gets falsified. The point of *this* phase is
that it does nothing else at the same time.

**The destination is a wired NTP server (phase 29), and the reason to want
the P4 at all is `SOC_EMAC_IEEE1588V2_SUPPORTED`** — see the addendum. But
`plan/phase25_gps_ntp_server.md` §5 already wrote the rule that governs the
order: *"The reference has to be better than the thing being measured."* A
brand-new platform and a precision instrument brought up in the same phase
give every timing anomaly two candidate causes, and phase 24 already paid for
that lesson once, over several evenings, with a 1 kHz pulse that presented as
1 Hz.

So phase 27's success condition is a P4 that is **trusted and boring**: it
boots, it keeps time, it takes interrupts, it isolates U-mode tasks, it has a
filesystem, and it runs a persona this project has already proven elsewhere.
Nothing on it is new except the silicon underneath.

**Milestone letter: `E`.** A–D, F, H–N and P–T, V–X are already spoken for
across `plan/`; E, G, O, U, Y and Z are free.

## 1. The board, and what is actually on it

Waveshare ESP32-P4-NANO. What matters here, and what does not:

| Part | Phase |
|---|---|
| ESP32-P4, dual RV32 HP core @400 MHz, 768 KB L2MEM | **27** |
| 16/32 MB in-package PSRAM | not 27 — see §7 |
| SPI flash (XIP through a two-level cache) | **27** (E6) |
| UART, USB Serial/JTAG, GPIO, I2C | **27** |
| IP101GRI Ethernet PHY on RMII (MDIO GPIO52, MDC GPIO31, PHY reset GPIO51) | 28 |
| ESP32-C6-MINI Wi-Fi 6 co-processor, **SDIO only** | 29 at the earliest, possibly never |
| MIPI CSI/DSI, H264, ISP, PPA, 2D-DMA, audio | not planned |
| LP core (RV32 @40 MHz), 32 KB LP SRAM | not planned |

Two facts about that table are load-bearing.

**The Ethernet is blob-free and the reference code is open.** IP101GRI is a
clause-22 PHY, so IDF's generic `components/esp_eth/src/phy/esp_eth_phy_802_3.c`
covers it, and the MAC has readable Apache-licensed reference in
`components/esp_hal_emac/esp32p4/` and `components/esp_eth/src/mac/esp32p4/`.
Nothing on the wired path needs reverse-engineering — unlike CYW43, where R5
had to derive gSPI from embassy-rs as an independent reference.

**The Wi-Fi is behind SDIO and nothing else.** The C6-MINI is a co-processor
speaking ESP-Hosted over a 4-bit SDIO slave interface. That is the CYW43
arrangement in principle — the blob runs on the other die, behind a bus — but
the transport is a genuine step up in difficulty from gSPI and from
`drivers/spisd_rp2350.c`'s SPI-mode SD. It is out of scope here and stays out
until the board has earned it.

## 2. What ports for free, and why

The honest good news, established by reading rather than hoping:

* **The atomics compile unchanged.** The P4 datasheet §4.1.1.1 gives
  "standard RV32IMAFCZc extensions", so the A extension is present and
  `arch/riscv/include/arch/atomic.h` needs no `#if` at all. This was the first
  thing checked, because it was the ESP32-C3's disqualifying property.
* **The toolchain is the one already installed.** `riscv64-elf-gcc` 16.2.0
  (Homebrew) compiles `-march=rv32imafc_zicsr_zifencei -mabi=ilp32` and
  `-march=rv32imac_zicsr_zifencei`; verified by test compile, 2026-09-05. No
  `riscv32-esp-elf`, no IDF toolchain, no new install. IDF enters this tree
  only as *reference source* and, much later, if the C6 firmware ever gets
  built.
* **`mtime` is standard.** The datasheet says "Compliant with RISC-V Core
  Local Interrupt (CLINT)". `kernel/ticker.c`'s RP2350 path is a special case
  built on SIO's non-standard mtime block at `SIO_BASE + 0x1a4`; the P4 gets
  the *ordinary* one, which makes the P4 path the simpler of the two.
* **PMP budget roughly quadruples.** "Up to 32 PMP regions and 16 PMA
  regions", against RP2350's 8 — of which `arch/riscv/common/pmp_probe.c`
  already records that entries 8/9/10 ship preconfigured (`pmpcfg2 =
  0x001f1f1f`) and are not free budget. The U-mode driver model built in
  phase 12 has been living inside a tight allowance; here it will not be.
* **The memory question does not arise.** 768 KB L2MEM
  (`SOC_DRAM_LOW 0x4FF00000` .. `SOC_DRAM_HIGH 0x4FFC0000`,
  `components/soc/esp32p4/include/soc/soc.h`) against RP2350's 520 KB, before
  PSRAM is even considered. The `rp2350-sensor` build's static footprint is
  173,544 bytes; it fits with room to spare.
* **Two HP cores.** Phases 22 and 23 are live rather than inert — but see
  §7: not in this phase.

## 3. What genuinely has to be written, and the shape of each

### 3.1 Boot, and the one decision that shapes everything else

The ROM does not hand control to a raw ELF. There are two routes, and E1
exists to find out which:

**Direct boot.** The P4's efuse summary
(`docs/en/api-reference/system/inc/espefuse_summary_ESP32-P4.rst`) carries
`DIS_DIRECT_BOOT`, default `False`. On the RISC-V parts this is the ROM path
that jumps straight into mapped flash with no image header and no second-stage
bootloader. If it works on this chip and this board, the whole `esp_image_header_t`
/ bootloader question evaporates and the flow becomes "objcopy to binary,
esptool write_flash, run" — which is very close to the RP2350's UF2 story.

**Image format.** Otherwise: an esptool-produced image, and a second-stage
bootloader — either IDF's, used as an opaque first stage that then jumps to
us, or our own. IDF's bootloader does real work (clock tree, cache, MSPI,
PSRAM), and re-deriving it is the single largest cost in this phase.

**This decision is E1's entire purpose and it is made empirically, not from
documentation.** Everything downstream — linker script, load addresses, flash
tooling, what state the CPU is in at `_entry_point` — depends on it.

### 3.2 Documents we do not have

We hold `~/gith/esp/datasheet/esp32-p4_datasheet_en.pdf`. **We do not hold the
ESP32-P4 Technical Reference Manual**, and the datasheet does not carry
register layouts. Phase 24's most expensive single bug was `0x88888888` where
`0xCCCCCCCC` was meant — EDGE_HIGH without EDGE_LOW, invisible in review —
and the lesson it produced was *"confirm register bit layouts against the
datasheet, never by inference"* (`rp2350_datasheets_local`). Half of this
phase's registers are not in any document we currently have.

E0 fetches the TRM. Until it is on disk, IDF's `components/soc/esp32p4/register/`
is the *secondary* source, and where the two disagree the TRM wins.

### 3.3 Chip revision is not a detail here

IDF carries two register sets — `components/soc/esp32p4/register/hw_ver1/`
and `.../hw_ver3/` — and `components/riscv/include/riscv/csr_clic.h` states
that the P4 uses the standard CLIC specification (with the `mintthresh` CSR)
only *"since REV2"*. So the interrupt controller's programming model depends
on which die is in hand.

The board's revision gets read and **logged at boot**, and the kernel refuses
to continue on a revision it was not built for rather than running on
plausible-looking wrong registers. This is the `pps_storm_rate` lesson in a
new place: a measurement that can only ever say one thing is worse than none.

### 3.4 The interrupt controller

CLIC, with `mtvec` mode 3, a jump table at `MTVT` (CSR 0x307), 32 external
lines at offset 16, and a priority threshold rather than a simple enable mask
(`csr_clic.h`). `arch/riscv/common/trap.c` currently has RP2350's
Hazard3-specific paths behind `#if defined(CONFIG_BOARD_RP2350)`; the P4 gets
a peer, not a rewrite.

Worth trying first, though: the datasheet claims CLINT compliance *as well*.
If plain direct-mode `mtvec` with a timer interrupt works, E3 can land before
E4 and the tick is available while the interrupt controller is still being
understood. That ordering is cheap to attempt and expensive to skip.

### 3.5 U-mode: the one genuine go/no-go

**Everything phase 12 built assumes M-mode plus U-mode.** All seven drivers,
`arch/riscv/common/umode.c`, `mem_domain.c`, the per-task PMP grants that
`ps` reports.

The P4 datasheet lists 32 PMP regions and 16 PMA regions, which is only
meaningful if there is an unprivileged mode for them to constrain, and IDF
runs everything in M-mode so it proves nothing either way. This is almost
certainly fine — but it is not *established*, and it is the one finding that
would change the phase from "port" to "do not port". E0 confirms it from the
TRM; E5 proves it on silicon by taking a deliberate fault.

If U-mode turns out to be absent, phase 27 stops at E4 and the whole P4 line
gets re-argued from there.

### 3.6 Flash, cache, and the write-while-executing problem

`/flash0` on the P4 means writing to the same SPI flash the CPU is executing
from through a two-level cache. RP2350 has the identical hazard and
`drivers/flash_rp2350.c` already solves it; the P4 version differs in the
mechanics (cache invalidate/disable, MSPI, code that must not be fetched from
flash while flash is busy) and not in the shape. Precedent exists; the
registers do not, yet.

### 3.7 What `edgecap.c` gets for free later

Not this phase, but worth recording while it is in view:
`SOC_GPIO_SUPPORT_ETM` and `SOC_TIMER_SUPPORT_ETM` are both set for the P4.
The Event Task Matrix can route a GPIO edge to a hardware timer capture with
**no ISR in the path at all**. `drivers/edgecap.c` is interrupt-based and
already cleanly `#if defined(CONFIG_BOARD_RP2350)`-guarded; phase 29 gets to
choose. Phase 24 measured the ISR path at 999998–1000001 µs and called it
"not the limiting factor", so this is not urgent — it is one fewer variable.

## 4. Milestones

Each milestone is independently useful and independently falsifiable, and
each ends with something observable on real hardware. There is no QEMU here
(§6), so "it builds" is never a milestone.

### E0 — Documents, board facts, and the go/no-go

Before a line of code. Fetch the **ESP32-P4 Technical Reference Manual** to
`~/gith/esp/datasheet/`, alongside the datasheet already there. Read the
High-Performance CPU chapter and answer, in writing, in this document:

1. **Does the P4 implement U-mode?** (§3.5. If no, the phase stops.)
2. What is the CLINT base address, and does `mtime`/`mtimecmp` behave as the
   privileged spec says?
3. What does the ROM leave configured — clock tree, cache, MSPI — at the
   moment it hands over?
4. What is the direct-boot mechanism on this part, and its magic?

Also: read the chip revision off the board and record it. Check the Waveshare
schematic for whether anything besides SDIO is routed between the P4 and the
C6-MINI (it changes phase 29's cost, and it is a five-minute answer now
versus a discovery later).

Deliverable: this section, filled in, with page references. No code.

### E1 — `tools/minimal_esp32p4.c`

A standalone bare-metal program in the exact spirit of
`tools/minimal_rp2350.c`: initialise UART0 (`0x500CA000`, from
`reg_base.h`'s `DR_REG_HPPERIPH1_BASE + 0xA000` — **confirm against the
TRM**), print a banner, toggle a GPIO, echo received characters. No kernel, no
linker script sharing, no CMake integration beyond its own target.

This is the milestone that answers §3.1 empirically: direct boot or image
format, what the clocks are doing, whether the cache needs touching before
flash is readable, and what `esptool` invocation actually puts bytes on the
board. It is also the artifact that will be reached for every time something
later stops working, exactly as `minimal_rp2350.elf` has been.

Done when: characters typed on the host echo back from the board, and the
flash-and-run command is written down in `tests/hw/README.md`.

### E2 — The kernel boots to a shell

`cmake/toolchain-esp32p4.cmake`, `linker/esp32p4.ld`, a `CONFIG_BOARD_ESP32P4`
arm in `CMakeLists.txt`, `cmake/board-esp32p4-nano.cmake`, and whatever
`arch/riscv/common/entry.S` needs to reach `kernel_main()` with a stack and a
cleared `.bss`.

Everything that does not need time, interrupts or isolation: `printk`,
`kernel/console.c` on UART0, the shell, `/proc`. Cooperative nothing — this
runs to the first prompt and stops there.

Built with `LUGALOS_ENABLE_CC`, `_ED`, `_LISP`, and every RP2350-only driver
flag **off**. The persona is subtraction, and the smaller the first image the
easier every one of these milestones is to debug.

Done when: a shell prompt over UART0, `ls /proc`, and `cat /proc/meminfo`
reporting a plausible 768 KB.

### E3 — Time

`kernel/ticker.c` gains a P4 arm on the standard CLINT (§3.4). `time_init()`,
the monotonic clock, `ticker_init(100)`, preemption, `sched_init()`, and the
existing task tests.

Done when: `uspin.elf` is preempted, and the tick rate measured against a
host stopwatch over ten minutes is within the same tolerance the RP2350 build
holds.

### E4 — Traps and device interrupts

CLIC setup, `mtvt`, the P4 arm in `arch/riscv/common/trap.c`, and
`kernel/devirq.c` routing a real peripheral interrupt (UART RX is the natural
first one, since the console already wants it).

Done when: a UART RX interrupt reaches a handler; a deliberate illegal
instruction produces the same diagnostic dump the RP2350 build produces.

### E5 — PMP, U-mode, and the isolation model

`pmp_probe()` reporting the P4's real entry count. `mem_domain.c` and
`umode.c` on the P4. One U-mode task, one grant, one deliberate
out-of-bounds access that faults.

The `umode_code_hazards` memory applies in full and is not optional: six real
bugs came out of that class of code on RP2350, including GCC emitting a jump
table outside the granted region that a `jal`-grep disassembly check does not
catch. `-fno-jump-tables` on every U-mode translation unit, and the whole
checklist walked before this milestone is called done.

Done when: `ps` shows per-task PMP isolation on the P4, and the deliberate
fault is caught and reported rather than hanging the board.

### E6 — `/flash0`

The P4 MSPI driver, the flashfs segment, `cmake/flash_layout.cmake` extended
so the linker and `drivers/flashdisk.c` still cannot disagree, and the
two-image flashing story (`README.md`'s "Two images, flashed independently")
reproduced with `esptool` in place of UF2.

`sizeof(pointer) after a heap move` is in scope for review here: this is the
first new code in a while to hold buffers whose lifetime the compiler does
not check.

Done when: a file written from the shell survives a power cycle.

### E7 — The sensor persona on the P4

The first *appliance*: BME280 on I2C, and `p9share`/SLIP-framed 9P over UART
uplink to the existing RP2350 gateway persona — the same three wires
`drivers/uart1_link_rp2350.c` already describes as "the gateway's cable to a
chess or clock board".

Deliberately **not** MQTT and **not** a network stack. The gateway already
publishes; the P4's job here is to be a node the existing household can see.
This exercises boot, time, interrupts, PMP, U-mode, flash, I2C and 9P — every
part of the port except the one phase 28 is for.

The `heap_stateless_user_programs` rule applies to anything new that runs.

Done when: the P4's BME280 readings appear in the gateway's namespace and
reach the broker, with the P4 having no network stack of its own.

### E8 — The hardware suite, and the documents

`tests/hw/` grows a P4 arm the way `test_rp2350.py` and `test_gateway.py`
already work: skipped when no board is attached, never failed. `tests/hw/README.md`
gains the flashing procedure from E1. `README.md` gains the P4 as a supported
target. `plan/open_issues.md` receives everything parked along the way.

The `hw_suite_is_chess_persona` lesson applies: whatever the suite needs
flashed, it needs *all* of it flashed, and it should say so when it isn't —
five ELF tests once failed and blamed everything except storage.

Done when: `uv run test_esp32p4.py` passes with a board attached and skips
cleanly without one.

## 5. Risks, and what each looks like

* **U-mode is absent** (§3.5). Looks like: E5 cannot set `mstatus.MPP` to
  U and every attempt returns to M-mode. Costs: the phase, and the P4 line.
  Mitigated by E0 answering it before E1 is written. *This is the one that
  ends things, and it is checked first.*
* **The ROM leaves less configured than hoped.** Looks like: E1 produces no
  UART output at all, and no way to tell whether the CPU is running. Costs:
  days, and possibly adopting IDF's bootloader as an opaque first stage.
  This is the most likely schedule risk in the phase. Mitigation: E1 is a
  standalone program precisely so this is discovered with nothing else in
  flight, and a GPIO toggle is the fallback instrument when UART is silent.
* **CLIC is fiddlier than CLINT-compliance suggested** (§3.4). Looks like:
  E3 lands, E4 stalls. Cheap because the ordering already anticipates it.
* **The board is a pre-REV2 die** (§3.3). Looks like: registers that read
  plausibly and behave wrongly — the worst failure mode this project knows.
  Mitigated by reading and logging the revision in E0/E2 and refusing to run
  on an unexpected one.
* **The TRM is not obtainable in the detail needed.** Looks like: register
  layouts derived from IDF headers alone. Tolerable, but it demotes every
  register write from "confirmed" to "inferred", which is exactly the state
  that produced phase 24's edge-mask bug. Record it honestly if it happens.
* **Scope creep toward the interesting parts.** PSRAM, the second core,
  Ethernet, the C6. Every one is more attractive than E6. §7 exists to be
  pointed at.

## 6. Testing, and the QEMU problem, stated plainly

**The 181 QEMU tests do not follow us.** There is no upstream QEMU machine
for any ESP32 part — `qemu-system-riscv32 -machine help` lists none (checked
2026-09-05). Espressif maintains a fork; its P4 support is newer than its C3
support and its maturity is unknown to us.

This matters more here than it would in most projects, because
`falsify_on_hardware_not_qemu` is a rule about QEMU *hiding* divergences that
hardware then reveals — it presumes QEMU is there as the first net. On the P4
there is no first net, and every milestone above is therefore
hardware-verified or unverified, with nothing in between.

Two consequences, both deliberate:

1. **Every milestone ends with an observation on silicon.** That is why none
   of them is "the code compiles" or "the abstraction is in place".
2. **The existing QEMU suite still has to pass.** Nothing in this phase may
   regress `rv32-nommu`, `rv64-mmu` or any RP2350 persona; the P4 arms are
   additive `#if` arms beside existing ones, never rewrites of shared code.
   The full suite runs before every merge, exactly as now.

Evaluating Espressif's QEMU fork is a reasonable *later* task and is not a
prerequisite. If it turns out to work well, it is a gift; planning around it
before anyone has run it would be planning around a hope.

## 7. Explicitly not in this phase

* **Ethernet** — phase 28. The reason the P4 was chosen, and still not now.
* **The NTP server** — phase 29.
* **Wi-Fi, the C6-MINI, ESP-Hosted, SDIO** — not scheduled. It needs an SDIO
  host driver and a protobuf-shaped control path, and the wired path makes it
  unnecessary for the intended appliance. Revisit only with a reason.
* **PSRAM.** 768 KB of L2MEM is more than any current persona uses. PSRAM is
  a cache/MMU configuration problem plus a `palloc` region question, and it
  earns its phase when something actually wants the memory.
* **The second HP core.** Phases 22–23 make this tractable, which is exactly
  why it should wait: bringing up SMP on a platform whose single-core
  behaviour is not yet trusted inverts the ordering this phase is built on.
* **The LP core, MIPI CSI/DSI, H264, ISP, PPA, audio.** No persona wants
  them.
* **USB Serial/JTAG as console.** `SOC_USB_SERIAL_JTAG_SUPPORTED` is set and
  it is a far simpler peripheral than `drivers/usb_cdc.c`'s full device
  controller — a good second console, and a distraction from a first one.
  UART0 is enough for every milestone here.
* **Retiring anything on RP2350.** Phase 24's DCF-77 clock and the GPS/PPS
  reference stay exactly where they are. Phase 25 §5: *"never let the thing
  under test also be the referee"* — the RP2350 household is what the P4 gets
  measured against, in this phase and much more so in phase 29.

## 8. Budget

E0 is reading, and it gates everything. E1 is the milestone most likely to
consume more than its share, and the one most worth spending it on. E2–E5 are
each a recognisable piece of work with a clear end. E6 is fiddly and has
precedent. E7 is mostly assembly of things that already exist. E8 is
housekeeping that pays for itself the first time a board misbehaves.

The phase is done when a P4 sits next to the RP2350 boards, reporting
temperature into the same namespace, and nobody has to think about it.

---

## Addendum — where this leads

Sketches, not plans. Written now so phase 27's ordering is legible; each gets
its own document when it is reached.

### Phase 28 — Ethernet on the P4

The IP101GRI over RMII, through `netif_register()` (`net/include/net/netif.h`),
which has already taken ENC28J60 and CYW43 and is the seam this plugs into.
MDIO on GPIO52, MDC on GPIO31, PHY reset on GPIO51.

Open reference, all of it: `components/esp_hal_emac/esp32p4/` and
`components/esp_eth/src/mac/esp32p4/` for the MAC, and
`components/esp_eth/src/phy/esp_eth_phy_802_3.c` for the clause-22 PHY. No
blob, no reverse-engineering, no vendor binary in `firmware/`.

Proven by MQTT over the wire — a path phase 26 already built and tested — so
that the milestone is "the existing stack works on new hardware" rather than
anything new. **No timing claims in phase 28.** The moment Ethernet works,
the temptation to measure will be considerable, and the whole reason for
three phases is to not do that yet.

### Phase 29 — The GPS/PPS stratum-1 server, on hardware that can do it

This is the rewrite of `plan/phase25_gps_ntp_server.md`, on the P4 rather
than on the RP2350 + ENC28J60 pair, and the argument for moving it is written
in phase 25 itself.

Phase 25 §3 identified the real limit as **software timestamping** — "tens to
hundreds of microseconds of jitter sitting on top of a reference good to one"
— and named the hard half honestly: *"neither the ENC28J60 nor the W5500 has
hardware transmit timestamping"*, which pushed it toward NTP interleaved mode
(RFC 9769) as a workaround for missing silicon.

The P4's EMAC carries `SOC_EMAC_IEEE1588V2_SUPPORTED`, and the PTP registers
are there to read in `components/soc/esp32p4/register/hw_ver1/soc/emac_reg.h`
(`EMAC_SYSTEMTIMESECONDS_REG`, `EMAC_SYSTEMTIMENANOSECONDS_REG` and the
update pair). Hardware timestamping, both directions, in the MAC. So phase 29
does not merely host phase 25's design on faster silicon — **it deletes phase
25's hardest open question**, and makes interleaved mode a choice rather than
a compensation.

Two things phase 25 established that carry over unchanged, and one that does
not:

* **Carries over:** `discipline_feed()` has never known what a longwave
  carrier is, and the discipline loop, the ppb correction, the slewing and the
  honest-dispersion policy are all source-agnostic already. And its §5 method
  lessons, which are about method and outlive both platforms.
* **Carries over:** the RP2350 GPS box stays alive as the independent
  yardstick. §5 again — the thing under test must not also be the referee.
* **Does not carry over:** `drivers/edgecap.c` and `drivers/gps_pps_rp2350.c`
  are RP2350 GPIO-interrupt code. The P4 has GPIO ETM and timer ETM (§3.7),
  so PPS capture can happen in hardware with no ISR at all. That is a design
  choice for phase 29, made with a measurement rather than in advance.

Phase 25's §6 open questions survive intact and get asked there, against an
instrument good enough to answer them.
