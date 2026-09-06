# Phase 27 — A second silicon, and nothing clever on it yet

**Status: in progress, 2026-09-06. E0-E3 done. E4's timer works and is measured; the `priostress` crash it exposed is root-caused (a blocking printk in `task_exit()`, kernel/sched.c, not P4 code) and the fix is not yet written.** This is the first of three
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
| ESP32-C6-MINI-1-N4 Wi-Fi 6 co-processor, **SDIO only** (C6 IO18–23 ↔ P4 GPIO14–19; reset on GPIO54) | 29 at the earliest, possibly never |
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

*(2026-09-05, E0 §7: confirmed against the schematic rather than the vendor
wiki. C6 `IO18`–`IO23` — that chip's fixed SDIO-slave pins — go to P4 nets
GPIO14–GPIO19 with 51 kΩ pull-ups, and there is no SPI alternative routed. The
P4 can also hard-reset the C6 over GPIO54, which is what an ESP-Hosted
bring-up needs.)*

## 2. What ports for free, and why

The honest good news, established by reading rather than hoping:

* **The atomics compile unchanged.** The P4 datasheet §4.1.1.1 gives
  "standard RV32IMAFCZc extensions", so the A extension is present and
  `arch/riscv/include/arch/atomic.h` needs no `#if` at all. This was the first
  thing checked, because it was the ESP32-C3's disqualifying property.
  *Confirmed on silicon 2026-09-05, E1: `misa = 0x40901125`, bit 0 set. No
  longer a reading of a datasheet.*
* **The toolchain is the one already installed.** `riscv64-elf-gcc` 16.2.0
  (Homebrew) compiles `-march=rv32imafc_zicsr_zifencei -mabi=ilp32` and
  `-march=rv32imac_zicsr_zifencei`; verified by test compile, 2026-09-05. No
  `riscv32-esp-elf`, no IDF toolchain, no new install. IDF enters this tree
  only as *reference source* and, much later, if the C6 firmware ever gets
  built. *Equally true of the distribution `riscv64-elf-gcc` on Arch: the
  same 695-byte binary, byte-for-byte the same size, built on both hosts.*
  *E2 took it the rest of the way, 2026-09-06: `cmake/toolchain-esp32p4.cmake`
  is `cmake/toolchain-rp2350.cmake` with one string changed, and it builds a
  226 KB kernel that boots. This has stopped being a claim about a test
  compile.*
* **`mtime` is a real memory-mapped CLINT**, at `0x20000000` with the
  standard `mtimecmp`/`mtime` register set (E0 §2) — against RP2350's SIO
  block at `SIO_BASE + 0x1a4`. *Half-true as originally written, and corrected
  2026-09-05: the counter is ordinary and slightly better than RP2350's (it
  has an atomic-read sampling mode), but its **interrupt** is not — it arrives
  through the CLIC as interrupt 7, so it is not free of §3.4's work.*
  *E2 did not use it, and E4 should decide deliberately rather than inherit
  that. `kernel/time.c` reads the **system timer** (TRM ch. 16) instead, for
  two reasons the TRM is explicit about: its rate is stated (XTAL/2.5, 16
  MHz average, "the timer counter is incremented by 1/16 µs on each CNT_CLK
  cycle") whereas the CLINT chapter never says what clocks `mtime`; and the
  CLINT's counter is **core-local** — one per HP core — which is the wrong
  instrument for a wall clock on a chip where §7's two-core question is live.
  The risk to avoid in E4 is running the tick off `mtimecmp` while the clock
  runs off the system timer: two independent time bases in one kernel is how
  drift becomes unexplainable. Either arm the tick from a counter this kernel
  already reads, or measure the two against each other and write the ratio
  down.*
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
  *Too comfortable, and E2 corrected it on 2026-09-06 — twice over. The 768 KB
  is not one usable region while `load-ram` is delivering the image: the ROM's
  own download-mode buffers occupy `0x4ff296b8`-`0x4ff40000` and split it. And
  the comparison with `rp2350-sensor` was not like for like, because on RP2350
  the kernel's 227 KB of `.text` lives in flash and costs no RAM at all, while
  a RAM-loaded image pays for it. The first link overflowed; the second, with
  `.bss` and the stack moved into the low half (NOLOAD only, so the loader
  never writes there), left 284 KB of heap. Both constraints are E6's to
  remove — a flash-booted image executes in place and is not sharing memory
  with a loader. See E2.*
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

*(2026-09-05: done. `esp32-p4_technical_reference_manual_en.pdf`, 23.5 MB, is
in `~/gith/esp/datasheet/` beside the datasheet. It is marked **PRELIMINARY**,
so the ordering above still stands and IDF stays the second opinion rather
than the first. One correction it already forced is recorded in §3.4.)*

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

*(2026-09-05, E0 §5: the board in hand is **v1.3**, so this hazard is live
rather than hypothetical. It takes the `hw_ver1` register set, the
**non-standard** CLIC — threshold in a memory-mapped register, `mintstatus` at
`0x346`, no `mintthresh` — and `rv32imafc` without Zb. E3 is written against
that variant.)*

### 3.4 The interrupt controller

CLIC, with `mtvec` mode 3, a jump table at `MTVT` (CSR 0x307), 32 external
lines at offset 16, and a priority threshold rather than a simple enable mask
(`csr_clic.h`). `arch/riscv/common/trap.c` currently has RP2350's
Hazard3-specific paths behind `#if defined(CONFIG_BOARD_RP2350)`; the P4 gets
a peer, not a rewrite.

~~Worth trying first, though: the datasheet claims CLINT compliance *as well*.
If plain direct-mode `mtvec` with a timer interrupt works, E3 can land before
E4 and the tick is available while the interrupt controller is still being
understood.~~

***2026-09-05, E0 §8 — this shortcut does not exist.*** The TRM's `mtvec`
description is unambiguous: *"MODE ... Only CLIC mode 0x3 is available.
(RO)"*. The field cannot be written to anything else, so there is no
direct-mode or vectored-CLINT `mtvec` on this core. And the timer interrupt is
itself a CLIC interrupt — pending in `clicintip[7]`, enabled by
`clicintie[7]` — so it does not arrive at all until the CLIC is up.

The interrupt controller therefore comes **before** the tick, not after: E3
and E4 are swapped in §4. The datasheet's "Compliant with CLINT" refers to
the memory-mapped timer and software-interrupt block (E0 §2), not to a
`mtvec` mode. A reasonable inference from a datasheet line, wrong on the
silicon, caught by reading the TRM before writing code — which is the whole
argument for E0 existing.

### 3.5 U-mode: the one genuine go/no-go — **passed, 2026-09-05**

*(E0 §1: `misa` reads `U = 1` ("User mode implemented"), `S = 0` ("Supervisor
mode implemented"). M+U with PMP and no S-mode — the RP2350 shape exactly, so
`CONFIG_NOMMU=1 CONFIG_MODE_M=1` is right. The paragraphs below are kept as
written because the reasoning is what made this the first question asked.)*

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
gets re-argued from there. *(It is not absent. E5 still has to prove it on
silicon — a CSR bit that says a mode exists is not the same as a task running
in it — but the phase is not at risk.)*

One rider from the TRM, recorded here because it will look like a bug
otherwise: the core is compatible with **privileged spec version 1.10**, not
1.11 or 1.12. Read CSR layouts from the TRM, never from current
documentation.

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

**DONE, 2026-09-05.** The TRM is at
`~/gith/esp/datasheet/esp32-p4_technical_reference_manual_en.pdf` (23.5 MB,
marked PRELIMINARY), beside the datasheet. Answers below; line references are
into the extracted text of that PDF, section numbers into the TRM itself.

**Sources used, in order of authority:** the TRM; the connected board, read
with `esptool`/`espefuse` v5.4.0 (via `uv tool run`, nothing installed
system-wide, read-only commands only); the Waveshare schematic
(`ESP32-P4-NANO-schematic.pdf`, rendered and read as an image — its PDF text
layer carries the labels but not the connectivity); and IDF headers last,
only where the TRM is silent.

#### 1. Does the P4 implement U-mode? — **YES. The phase proceeds.**

`misa` is explicit (TRM §2, register description): **"U — User mode
implemented = 1. (RO)"**, and beside it **"S — Supervisor mode implemented =
0. (RO)"**. The CPU feature list adds "User (U) privilege mode execution" and
"Standard physical memory protection (PMP) configurable up to 32 regions and
custom attributes (PMA) configurable up to 16 regions".

So the P4 is **M+U with PMP and no S-mode** — precisely the RP2350 shape, and
`CONFIG_NOMMU=1 CONFIG_MODE_M=1` is the right configuration. Everything phase
12 built has a home here.

Two riders, neither a problem:

* The core is "Compatible with RISC-V Privileged Architecture, **Version
  1.10**" — an older privileged spec than the 1.11/1.12 most current
  documentation assumes. CSR *details* should be read from the TRM, not from
  a modern spec, and this is a standing hazard for the whole phase.
* `mintstatus.UIL` is "Hardwired to 0x0 as **user mode interrupts are not
  supported**". We do not want U-mode interrupt delegation, so this costs
  nothing — but it is worth knowing before someone tries.

#### 2. CLINT, `mtime`, and the interrupt controller

The core-local blocks are memory-mapped, not at the usual CLINT address
(TRM §2.8.3, Table 2.8-2):

| Block | Range |
|---|---|
| CLINT (self) | `0x20000000` – `0x2000FFFF` |
| CLINT (other core) | `0x20010000` – `0x2001FFFF` |
| CLIC (self) | `0x20800000` – `0x2080FFFF` |
| CLIC (other core) | `0x20810000` – `0x2081FFFF` |

*(Note for anyone reading IDF instead of the TRM: `soc.h` labels
`0x20000000`–`0x28000000` as `SOC_DEBUG_LOW/HIGH`, "Debug region, not used by
software". That comment is misleading — this is the core-local register
window.)*

Timer registers, offsets from the CLINT base (TRM §2.9.3.5, registers
2.101–2.107):

| Register | Offset |
|---|---|
| `mtimecmplo` / `mtimecmphi` | `0x4000` / `0x4004` |
| `mtimeloadlo` / `mtimeloadhi` | `0x4008` / `0x400C` |
| `mtimectl` | `0x4010` |
| `mtimelo` / `mtimehi` | `0xBFF8` / `0xBFFC` |

Three things that shape `kernel/ticker.c`'s P4 arm:

* **The counter does not run until told to.** `mtimectl.MTIME_EN` enables it,
  and *"This bit is implemented only in Core 0"* — Core 1 can reach Core 0's
  CLINT to start or pause it, but its own `mtimectl` does nothing. This is
  structurally the same surprise RP2350 had, which `ticker.c` already
  documents ("mtime does not run on its own"), so the shape is familiar.
* **`mtimectl.MTIME_SAM` gives an atomic 64-bit read.** It configures
  sampling so that reading one half latches the other. That is strictly
  better than `ticker.c`'s existing `do { hi; lo; } while (hi != hi)` loop,
  and the P4 arm should use it rather than copying the RP2350 idiom.
* **The timer interrupt is a CLIC interrupt.** Pending state is
  `clicintip[7]`, enable is `clicintie[7]`; the software interrupt is
  interrupt 3. Each CLIC unit carries 32 external interrupts plus these 2.

#### 3. What the ROM leaves configured — **partly unanswered, by design**

The TRM documents the boot *mode* selection but not what the ROM's own code
leaves behind. Two things are settled:

* **Boot mode is strapped on GPIO35/36/37/38** (TRM §12.2.2, Table 12.2-2).
  SPI Boot is the default; Joint Download Boot covers USB-Serial-JTAG, UART,
  SPI-slave and USB-OTG download.
* **The ROM's second-stage bootloader offset is `0x2000`** on this part
  (IDF `components/esp_rom/esp32p4/esp_rom_caps.h`,
  `ESP_ROM_BOOTLOADER_OFFSET_FLASH`) — not `0x0` as on the C3.

The rest — clock tree, cache, MSPI state at hand-over — is not in the TRM and
should not be guessed. **It is E1's experiment, which is what E1 is for.**
Recording the question as still open is the honest outcome here; inventing an
answer from IDF's bootloader source would be inference dressed as fact.

#### 4. Direct boot — **available on this chip, mechanism still to confirm**

`espefuse summary` on the attached board reports `DIS_DIRECT_BOOT = False`,
so direct boot is *enabled*. The magic value and entry convention are not in
the TRM and are not in esptool's source either; E1 establishes them
empirically, with the `0x2000` image-format route as the fallback.

The rest of the security fuse block is virgin, which matters more than it
looks: `SECURE_BOOT_EN = False`, `SPI_BOOT_CRYPT_CNT = Disable`,
`DIS_DOWNLOAD_MODE = False`, `DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE = False`,
`DIS_PAD_JTAG = False`. **Download mode cannot be locked out by anything we
flash, so the board is not brickable by a bad image** — strapping into Joint
Download Boot always recovers it. Nothing in this phase should burn a fuse.

#### 5. The board in hand

Read from the attached board, 2026-09-05:

* **ESP32-P4 revision v1.3**, dual core + LP core, 400 MHz, 40 MHz crystal.
  MAC `80:f1:b2:d2:f0:53`.
* **Flash: 16 MB**, GD25Q128ESIG (`manufacturer c8, device 4018`), quad-IO.
* Connected over the **P4's own USB-Serial-JTAG** (`/dev/cu.usbmodem5B610420061`).
  The board's *other* USB socket goes to a CH343P USB-UART bridge and was not
  enumerated; both exist and they are not the same port.

**v1.3 is the finding that changes code.** §3.3 warned that revision decides
the programming model, and this die lands on the older side of the split:

* **Non-standard CLIC.** IDF gates on `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`,
  and for revisions below v3 records: *"The ESP32-P4 implements a non-standard
  version of the CLIC: the interrupt threshold is configured via a
  memory-mapped register instead of a CSR; the `mintstatus` CSR is at
  **0x346** instead of 0xFB1 as per the official specification"*
  (`components/soc/esp32p4/include/soc/interrupt_reg.h`). There is no
  `mintthresh`. E3 must be written against the non-standard variant.
* **Register set `hw_ver1`**, not `hw_ver3`
  (`components/soc/esp32p4/register/`).
* **`-march=rv32imafc`**, not `rv32imafcb` — no Zb on this die. We compile
  `rv32imac_zicsr_zifencei` (a subset of both), so this costs nothing, but it
  is why we should not reach for bit-manipulation intrinsics.

The kernel reads and logs the revision at boot and refuses an unexpected one
(§3.3). It is now known which value that check must accept.

#### 6. The console, and a gift

**P4 UART0 is GPIO37 (TX) / GPIO38 (RX)** — `U0TXD_GPIO_NUM 37`,
`U0RXD_GPIO_NUM 38` in `components/soc/esp32p4/include/soc/uart_pins.h`, and
the schematic wires exactly those two to the CH343P bridge (its TXD to
GPIO38, its RXD to GPIO37).

These are the ROM's *own* default UART pins. So **the ROM's boot messages
come out of the CH343P port with nothing configured**, which hands E1 an
instrument before E1 has written one — and, more importantly, tells us
whether the CPU reached the ROM at all when our own code is silent. Plug in
the second USB socket before starting E1.

Note that GPIO37/38 are also two of the four boot-strapping pins. That is
normal on this family and is not a conflict, but it is why the bridge's
DTR/RTS lines matter.

#### 7. The P4 ↔ C6-MINI interconnect — **SDIO, confirmed, with no SPI escape**

From the schematic (rendered and read directly; the text layer does not carry
connectivity). The part is an **ESP32-C6-MINI-1-N4**, and the wiring is:

| C6-MINI pin | C6 signal | P4 net |
|---|---|---|
| 24 | IO18 | GPIO14 – GPIO19, one each |
| 25–29 | IO19, IO20, IO21, IO22, IO23 | (all with 51 kΩ pull-ups to 3V3) |
| 8 | EN / CHIP_PU | **GPIO54**, via 0 Ω R54 |
| 5 | IO2 | **GPIO6**, via 0 Ω R52 |
| 30, 31 | U0RXD, U0TXD | routed as named nets; destination not resolved |

C6 `IO18`–`IO23` are that chip's fixed SDIO-slave pins, and the 51 kΩ
pull-ups are the SDIO bus terminations. **So §1's "SDIO only" is confirmed for
the data path** — there is no SPI alternative on this board, and any future
Wi-Fi work needs a real SDIO host driver.

Two findings worth having anyway: the P4 can **hard-reset the co-processor**
over GPIO54, which is exactly what an ESP-Hosted bring-up needs and is not
something to discover later; and GPIO6 reaches C6 IO2, which is free for a
handshake. Whether the C6's UART lands on P4 pins — which would have offered a slow but
far simpler transport — is **now closed, and the answer is no**: `C6_U0RXD`
and `C6_U0TXD` go to pin header **P2** (pins 19 and 20), alongside `C6_IO9`
and the C6's USB pair, not to the P4. They are there to flash and talk to the
co-processor from outside. So SDIO really is the only P4-to-C6 path, and any
future Wi-Fi work needs a genuine SDIO host driver with no fallback.

#### 8. What E0 changes in this plan

Recorded here rather than silently edited, since the corrections are the
point:

1. **§3.4's shortcut is dead, and E3/E4 swap.** `mtvec.MODE` is **read-only
   at `0x3`**: *"Only CLIC mode 0x3 is available. (RO)"*. There is no
   direct-mode or vectored-CLINT `mtvec` on this core, and the timer
   interrupt arrives through `clicintie[7]`. So the tick **cannot** land
   before the interrupt controller does. The milestones below are reordered:
   **E3 is now traps and CLIC, E4 is now time.**
2. **§3.3's hazard is live, not hypothetical** — this die is v1.3 (§5).
3. **§1's SDIO row is confirmed** and gains pin numbers (§7).
4. **§3.5 is settled** and the go/no-go is passed (§1).

No code was written. Everything above is reading, plus four read-only
commands against the board.

### E1 — `tools/minimal_esp32p4.c`

**DONE, 2026-09-05.** Everything the milestone asks for, on hardware.

**Our code runs on the ESP32-P4 and drives UART0.** Proven by echo:
`PROBE123\r` sent, `PROBE123\n\r` returned -- ten bytes for nine, with the
`\n` *before* the `\r`, which is the exact signature of this program's
`if (c == '\r') uart_putc('\n'); uart_putc(c);` and cannot be produced by a
hardware loopback. That is E1's "done when", and it is met.

**And the board now says all of it unprompted, on one command, with nobody
touching it** -- see "What Linux answered" below, which is where the
outstanding observation, the reset question and the flash backup all got
settled.

Files: `tools/minimal_esp32p4.c`, `tools/minimal_esp32p4_entry.S`,
`tools/minimal_esp32p4.ld`, `tools/build_minimal_esp32p4.sh`. 695 bytes of
text, running from L2MEM at `0x4ff00000`.

#### What E1 settled that E0 could not

* **The boot question never had to be answered.** E1 was specified to decide
  direct-boot versus image-format empirically. It turned out to need neither:
  `esptool load-ram` delivers an image over the download protocol straight
  into L2MEM and jumps to it, so the first program ran with **no flash write
  at all**. That is strictly better than the plan assumed -- nothing can brick
  the board, the factory image is untouched, and iteration costs one command.
  The flash boot path is now E6's problem, where it belongs, rather than a
  prerequisite for seeing any output.
* **The ROM leaves UART0 fully configured** -- clocked, muxed, 115200 8N1 on
  GPIO37/38 -- so `minimal_esp32p4.c` writes bytes and nothing else. Contrast
  `tools/minimal_rp2350.c`, which needs clk_peri, three unresets, two pad
  muxes and a baud divisor first. E2 must still configure the UART itself; a
  kernel that inherits a bootloader's leftovers breaks the first time it is
  booted differently.
* **The ROM's RAM map**, which E0 left explicitly open, is recorded in IDF's
  `components/bootloader/subproject/main/ld/esp32p4/bootloader.memory.ld.in`:
  `0x4ff296b8`-`0x4ff3afc0` shared buffers (live during download mode, so
  this is the one that constrains us), `0x4ff3afc0`-`0x4ff3fba4` CPU1 stack,
  `0x4ff3fba4`-`0x4ff40000` ROM .bss/.data. The linker script keeps
  everything below `0x4ff28000`.
* **The board is a factory-demo board.** Flash holds a Waveshare ESP-IDF app
  that fails an I2C probe and then watchdogs every five seconds. Its crash
  dump was useful before we wrote a line: `MTVEC : 0x4ff00003` -- low bits
  `0b11`, CLIC mode 3 -- is live-silicon confirmation of E0 section 8's
  finding that `mtvec.MODE` is read-only at CLIC. Its constant chatter on
  UART0 is also the first thing to suspect when esptool reports "serial
  noise".

#### One real trap, one retraction, and the mistake underneath both

**The mistake first, because it produced the other two.** Loading was done by
ad-hoc scripts that took an *image* path, while only the build script ran
`elf2image`. So from 16:55 onward every load re-delivered a **stale image**
built before any of the afternoon's edits: the source was changed four times,
rebuilt each time, and the board kept running the first binary. Symptoms —
a program that echoed correctly but never printed, and that ignored changes
which should plainly have made it print — were diagnosed twice, confidently,
and both diagnoses were wrong. The timestamps settled it in seconds, and
should have been the first thing checked rather than the last.

`tools/p4run.py` now takes an **ELF** and regenerates the image every time, so
there is no stale artifact left to load. That is the fix; the discipline
("check what you actually flashed before theorising about why it misbehaves")
is the lesson, and it is the same shape as phase 24's `pps_storm_rate`: a
measurement that could only ever say one thing.

**Retracted — the drain loop never hung anything.** An earlier version of this
section reported that an unbounded `while (uart_has_char()) uart_getc();`
wedged the board. It cannot have: no build containing that drain was ever
loaded. The board was running the original drain-free program the whole time.
The bound in `minimal_esp32p4.c` is kept anyway — an unbounded drain on a
misconfigured UART *is* an infinite loop, and the cost of the bound is one
comparison — but it is a precaution, not a fix for anything observed.

**Unresolved — whether resetting over the native USB port corrupts UART0.**
The evidence is genuinely mixed and this needs settling on Linux:

* *For:* a freshly-built program writing `'A'` continuously, loaded after a
  USB-Serial-JTAG reset, produced a saturated stream of `0x05` at 115200, and
  a sweep of 22 host baud rates from 9600 to 1152000 decoded none of it.
* *Against:* on that same path, minutes later, the echo test was byte-perfect.
  A corrupted UART does not echo cleanly.

The likelier explanation is now the spew program itself: it writes to the TX
FIFO as fast as the CPU allows, gated only by a `TXFIFO_CNT` field this tree
has not yet verified against hardware, so FIFO overrun is at least as good a
suspect as the reset path. **Re-test on Linux with a fresh image before
believing either story.** Until then the safe habit costs nothing: use the
CH343P port for everything.

**Stands — a heartbeat that stops when observed is not an instrument.** The
banner is emitted while esptool still owns the port and is therefore always
lost; a periodic "still alive" line compensates. Gating it on "no character
received yet" made it useless, because merely *opening* the host port is
enough to put a stray byte in the RX FIFO. Unconditional now. This one is
solid: it is why the board looked dead for most of an afternoon while it was
in fact echoing perfectly.

#### The workflow, as it actually is

```
tools/build_minimal_esp32p4.sh run     # build, elf2image, reset into download
                                       # mode, load-ram, listen. No buttons.
tools/p4run.py --probe                 # is our program still running?
tools/p4run.py --run                   # back to whatever is in flash
```

On macOS it was three steps with a button press at each end, because the
built-in CH34x driver does not carry the modem-control lines. That was a host
limitation, not a board one -- see below.

#### What Linux answered — 2026-09-05

The work moved to a Linux laptop to get the board resettable from software.
It is, and the other two open questions fell out of the same session.

**1. Software reset works, and so does software *download mode*.** This was
the priority, because E8's unattended hardware suite is impossible without
it. Linux's `cdc_acm` carries the modem-control lines that macOS's built-in
CH34x driver would not, so U6 does exactly what the schematic says. The
verdict comes from the ROM naming its own boot mode, not from inference:

```
run (RTS pulse)               2812 bytes  reset: True   rst:0x1 (POWERON),boot:0x30f (SPI_FAST_FLASH_BOOT)
download (RTS + DTR strap)     128 bytes  reset: True   rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))
```

`tools/build_minimal_esp32p4.sh run` is now one command, start to finish,
with nobody touching the board. **The macOS finding was about the host, not
the board** -- worth stating plainly, because "the auto-reset circuit exists
and does not help" was written down twice and was wrong about the cause both
times.

**2. The CSR dump, off silicon.** The last outstanding E1 observation. Every
one of these was an E0 claim derived from documents; the silicon agrees with
all of them:

| CSR | Read | Says |
|---|---|---|
| `misa` | `0x40901125` | MXL=1 (RV32); **A set** (bit 0); I, M, F, C; **U set** (bit 20); **S clear** (bit 18); X set |
| `mtvec` | `0x4fc00b03` | low bits `0b11` — CLIC mode 3, §3.4 |
| `mhartid` | `0x00000000` | hart 0 |
| `mstatus` | `0x00000080` | MPIE set, MIE clear, MPP = U |

**The A bit is the one that matters.** §2's first bullet — "the atomics
compile unchanged", the property whose absence disqualified the ESP32-C3 —
was a datasheet reading until now. It is a measurement. `misa` bit 20 set
with bit 18 clear is the other one: M+U with no S-mode, which is the
assumption phase 12's whole U-mode driver model rests on (§3.5), and `mtvec`
`0b11` confirms E0 §8 and the factory demo's crash dump independently.

`misa` bit 23 (X) is also set, i.e. non-standard extensions are present.
Nothing in this phase wants them; recorded so that a later reader does not
mistake it for corruption.

**3. Flash is backed up, and E6 is unblocked.** The sync errors that defeated
three attempts on macOS did not recur: 16,777,216 bytes at 921600 in 241 s
over the CP2102. **Verified afterwards with `esptool verify-flash`, which
compares a digest computed *on the chip* rather than re-reading down the same
path that produced the file** — a re-read can reproduce its own transfer bug
and look like agreement. It matched.

Kept at `~/gith/esp/p4nano-factory-flash/`, outside the repository, with a
README recording chip revision (ESP32-P4 v1.3), flash part (GigaDevice
`c8 4018`, 16 MB), MAC, and the restore command. **E6's precondition is
met.**

#### The ports, and a portability lesson worth more than the fix

The tooling had to be rewritten, and not for a small reason: **its port
detection was wrong in a way that would have looked like broken hardware.**

`p4run.py` chose ports by device-name glob — `/dev/ttyUSB*` for the CH343P,
`/dev/ttyACM*` for the native USB-Serial-JTAG. That mapping is a macOS habit
and it does not survive the move. On Linux the CH343P enumerates through
`cdc_acm` as `/dev/ttyACM0`, which is exactly the pattern the script had
learned to refuse; and `/dev/ttyUSB0` here is a CP2102 wired to UART0.
Detection now goes by USB VID:PID, and `--ports` prints what it found.

The roles then split, which the old single-port model could not express:

| cable | VID:PID | here | role |
|---|---|---|---|
| CH343P (on-board) | `1a86:55d3` | `/dev/ttyACM0` | reset lines, and UART0 RX |
| CP2102 (external, on UART0) | `10c4:ea60` | `/dev/ttyUSB0` | console, full duplex |

The CH343P's **host-to-board TX path is dead on this board**. esptool
diagnosed it in one line where a session of guessing would not have:
*"Download mode successfully detected, but getting no sync reply: The serial
TX path seems to be down."* Its RX and its modem lines are fine, so it keeps
the reset job and the console moves to the CP2102. `--port` / `--reset-port`
and `LUGALOS_P4_PORT` / `LUGALOS_P4_RESET_PORT` express the split; a stock
board with a working CH343P uses one cable for both, which is the default.

**The split turned out to be a capability, not a workaround.** The reset
port's RX is still UART0, so it can be held open and read *through* the
load — and the banner and CSR dump, printed once in the instant the ROM
jumps to us while esptool still owns the port it loaded over, survive. On a
single-cable host they are lost; that is why `minimal_esp32p4.c` re-announces
itself on a heartbeat. Item 2 above was captured this way.

**Two bugs found by disbelieving a clean-looking result**, both in code
written this session:

* `--reset-test` reported *"this host's USB-serial driver is not carrying the
  modem-control lines"* about a host that resets the board perfectly. It
  flushed the input buffer *after* driving the reset lines, and the ROM
  banner arrives inside the sequence's own final 50 ms dwell — so the one
  line that says whether the reset happened was thrown away every time.
  Flush before driving, never after.
* `--probe` reported *"something echoes, but not with our signature"* about
  a reply sitting in plain sight in its own debug output. It compared the
  whole read against `PROBE123\n\r` with `==`, and the heartbeat — correctly
  unconditional — almost always wraps a beat line around the echo. Searched
  for, not compared against.

Both had the shape the stale-image mistake had: a confident negative verdict
from a measurement that could not have returned anything else. The habit that
caught them was the same one that eventually caught that — read the raw bytes
the instrument collected before believing the verdict it printed on them.

#### The GPIO toggle, and how to observe one on a board with no LED

**`ESP32-P4-NANO-schematic.pdf` settles it: this board has no user LED.** The
only `LED1` is a 5 V power indicator, hardwired through R1 to `VCC_5V` with no
GPIO anywhere near it, and `LED0`/`LED3`/`LEDMOD` are pins of the IP101GRI
Ethernet PHY. So "toggle a GPIO and watch it blink" was never available here,
and a toggle with nothing attached proves only that a register accepted a
write.

**The pin: GPIO20, on header P1 pin 13.** Chosen by elimination against the
schematic and datasheet, because most of this chip's pins are spoken for:
GPIO7/GPIO8 are the I2C the factory demo fails on; GPIO34–GPIO38 are
strapping pins (datasheet Table 3-1) and GPIO36 carries R41, a 10 kΩ pull-up;
GPIO14–GPIO19 are the C6's SDIO and GPIO54 its reset; GPIO37/38 are the
console. GPIO20–GPIO23 are the clean ones — each appears exactly twice in the
netlist, at the P4 and at header P1, with no passives between.

**Observed by reading the pad back, in two stages, because the first stage
alone proves less than it looks.**

```
gpio20  = drive 1 reads 1, drive 0 reads 0  PASS
gpio20  = released, pull-down reads 0, pull-up reads 1  PASS (reading the pad, and the pin is free)
```

The first line drives the pin and reads `GPIO_IN` back, both ways round —
both ways, because a stuck-high pin passes a test that only ever checks for
1, which is the same defect as a heartbeat that cannot fail.

The second line exists to falsify the obvious objection to the first: that
`GPIO_IN` might simply be echoing `GPIO_OUT`, in which case the whole thing
proves that a register remembers what was written to it. So the output is
disabled with `GPIO_OUT` **left high**, and the pad is handed to a weak
internal pull — down, then up. A mirror would read 1 both times. It read 0
then 1, so `GPIO_IN` is the physical pin.

That second stage also, for free, confirms the schematic reading by
measurement: a pin that follows a ~45 kΩ internal pull in *both* directions
has nothing else driving it, which is exactly what "GPIO20 touches only the
P4 and header P1" predicts. The netlist claim and the electrical claim now
agree.

On the heartbeat, GPIO20 carries a square wave at half the beat rate, so P1
pin 13 tells anyone with a meter what the console tells anyone with a
terminal.

**Registers, all from the TRM chapter 10 rather than inferred** (§3.2's rule,
and phase 24's `0x88888888` is why): GPIO Matrix at `0x500E_0000` —
`OUT_W1TS` `0x08`, `OUT_W1TC` `0x0C`, `ENABLE_W1TS` `0x24`, `ENABLE_W1TC`
`0x28`, `IN` `0x3C`, `FUNCn_OUT_SEL_CFG` `0x558+4n`; IO MUX at `0x500E_1000`
— `IO_MUX_GPIOn_REG` at `0x04+4n`, `MCU_SEL` [14:12], `FUN_DRV` [11:10],
`FUN_IE` [9], `FUN_PU` [8], `FUN_PD` [7].

Two of those needed the TRM and would have been got wrong from the plan's
earlier notes alone:

* **`MCU_SEL` resets to 0, not to the GPIO function.** Function 1 is the GPIO
  function "for all pins", and selecting it is required — `GPIO_ENABLE` and
  `GPIO_OUT` on their own drive nothing. The original E1 note listed only
  `GPIO_OUT_W1TS` and `GPIO_ENABLE_W1TS` as "confirmed and ready", which
  would not have toggled anything.
* **`GPIO_FUNCn_OUT_SEL` is 9 bits and the "plain GPIO" index is 256**, not
  the 128 older Espressif parts use. It happens to be the reset value
  (`0x100`), so inheriting it would have worked — which is precisely why it
  is written explicitly instead, on the same principle that makes E2
  configure the UART rather than live off the ROM's leftovers.

`tools/minimal_esp32p4.c` is 1229 bytes of text, up from 695.

#### Original specification


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

**DONE, 2026-09-06.** Booted to a shell on the first load, on hardware.

```
[    0.000] [Timer] System timer up: XTAL/2.5 = 16 ticks/us (1 us resolution).
       LugalOS Lisp Machine v0.15.0 (build 490.61b8ab1f+)
[    0.020] [Priv] Execution Mode: Machine Mode (M-mode)
[    0.055] [Dev] Registry: rtc(absent), sensor(absent), eeprom, usb(absent),
                             uart, uartslip, uartdemux
[    0.064] [PAlloc] Page allocator: 71 pages of 4096 bytes at 0x4ff79000 (284 KB)
[    0.142] [Ticker] ESP32-P4: the timer interrupt is a CLIC source (E3);
                     preemption stays off
[    0.157] [UART] Driver running as task #1, reachable via chan_call("uart", ...)
LugalOS Interactive Console Shell (`lsh`)
lsh> cat /proc/meminfo
RAM: 768 KB total at 0x4ff00000
  Image (text+data+bss): 362 KB
    .data 340 B, .bss 134 KB
  Boot Stack: 16 KB, peak 6692 bytes
  Heap: 284 KB managed of 284 KB
  Storage: /flash0/ (not mounted), /sd0/ (not mounted), /ram0/ (not mounted)
```

`ls /proc` lists all fourteen files, `lisp` gives a REPL that evaluates, and
`uartstats` reports `write_calls=185` — which is the part worth checking
rather than assuming: it says the console is going through
`chan_call("uart", ...)` to a driver task, not silently falling back to
direct hardware access the whole time.

**The clock was measured, not trusted.** Two `date` calls 60 seconds apart by
the host's clock came back 60 seconds apart on the board (12:02:11 →
12:03:11), and every `[timestamp]` in the log above tracks the interval the
host actually waited. `date` has one-second resolution, so this bounds the
error at under ~1.7% — nowhere near a precision claim, and not meant to be
one, but it rules out the gross divisor mistake that is the thing worth
ruling out here. Phase 24's `TICKS_TIMER0_CYCLES` bug ran a whole system at
43% speed and hid for months because nothing ever checked; E4 owns the
precise version.

The boot log also shows the two lies this milestone found, already fixed —
see "What the new board caught in the old code" below.

#### What E2 built, and the three places the specification was wrong

Files: `cmake/toolchain-esp32p4.cmake`, `cmake/board-esp32p4-nano.cmake`,
`linker/esp32p4.ld`, `drivers/uart_esp32p4.c`, an `esp32p4` preset, and
`CONFIG_BOARD_ESP32P4` arms in `CMakeLists.txt`, `arch/riscv/common/trap.c`,
`kernel/ticker.c`, `kernel/time.c`, `kernel/board.c`, `kernel/main.c`,
`kernel/shell.c`, `kernel/meminfo.c`, `fs/vfs_server.c`,
`drivers/flashdisk.c` and `user/lisp/lisp.c`. 226,692 bytes of text, 154,448
of `.bss`, and a 290,816-byte heap.

**The toolchain file is a copy of the RP2350 one with a different
`LUGALOS_TARGET`, and that is the result rather than the shortcut.** The
same generic RISC-V cross-compiler this tree has always used builds for the
P4 with no new tool, no ESP-IDF, and no vendor GCC. §2 predicted it; E1
proved it on silicon; E2 is where it stopped being a prediction about a
standalone program and became a property of the build system.

**Everything except the UART was a subtraction.** The arms added to the seven
shared files are, with one exception, statements that this board has nothing
there — no block device, no netif, no interrupt controller yet, no identity
backend. Each is an explicit `#elif defined(CONFIG_BOARD_ESP32P4)` rather
than a fall-through, and that is the whole point: every one of those `#else`
branches was written meaning "QEMU", and a third target reaching them would
have probed virtio-mmio addresses, written a PLIC that is not there, and read
a CLINT that is not there either. The absence of a controller is now a
statement in `trap.c`, not a gap.

##### 1. Where the kernel goes — the specification said 768 KB and the ROM says otherwise

L2MEM is 768 KB, and it is not one region. This image is delivered by
`esptool load-ram`, so while it is being written the boot ROM's own
download-mode code is live in the same memory — `0x4ff296b8`–`0x4ff40000`,
which E1 already recorded. The first working layout put everything above
that and got **136 KB of heap**, under the 256 KB floor
`plan/phase15_memory_reclamation.md` §6 asks every real board to hold. The
floor fired, which is what it is for.

The rule that fixes it is about *when*, not *where*: once load-ram jumps to
`_start` the ROM's loader is finished and its buffers are dead memory. So a
NOLOAD section — one with no bytes in the image file, written for the first
time by `entry.S` — is perfectly safe down there, while a section carrying
bytes is not. `linker/esp32p4.ld` therefore splits:

| Region | Range | Contents |
|---|---|---|
| `LOWRAM` | `0x4ff00000`–`0x4ff40000` (256 KB) | `.bss`, boot stack. **NOLOAD only.** |
| `RAM` | `0x4ff40000`–`0x4ffc0000` (512 KB) | `.text`, `.rodata`, `.data`, then the heap |

That is 290,816 bytes of heap and a `/proc/meminfo` that can honestly say
768 KB. Two `ASSERT`s keep the rule true, and a third checks the floor.

**The boot stack sits at the top of `LOWRAM`, not immediately above `.bss`,**
and this is the one place the layout is doing something the other scripts do
not. A stack destroys whatever is below `_stack_bottom`; below `.bss` that is
live kernel state, which is the exact failure `linker/rp2350.ld` documents
twice. Putting it at the top leaves ~106 KB of unused region as the thing an
overflow reaches first. It is not a guard page — nothing faults — but it is
free.

Two consequences worth knowing. `AT()` was the first attempt and it moves the
*load* address, not the placement, so the stack stayed where it was and grew
a spurious LMA; an explicit address expression is what actually moves a
section, and it costs `ld`'s own region overflow check, which is now an
`ASSERT` instead. And `kernel/meminfo.c` needed a P4 arm: `image_bytes` is
computed everywhere else as `_bss_end - _ram_start`, which assumes the image
is one contiguous run from the bottom of RAM. Here it is two runs, and
without the fix `/proc/meminfo` would have reported the `.bss` half and
silently omitted 227 KB of resident kernel text — on the one target where the
text *is* resident.

**The 227 KB is temporary and is E6's to reclaim.** On RP2350 the kernel's
text lives in flash and costs no RAM at all; here it is RAM-resident because
the image is RAM-*loaded*, which is E2's entire boot story. Booting the same
image from flash puts it back into XIP.

##### 2. `LUGALOS_ENABLE_LISP` does not exist, and inventing it was the wrong fix

E2 asked for the image to be built with `_CC`, `_ED` and `_LISP` off. The
first two are real flags and are off. The third is not a flag at all, and the
measurement that forced the question also answered it: on the first link,
`user/lisp/lisp.c` alone carried **181,468 bytes of `.bss`** and the image
overflowed its region by 2352 bytes.

Adding an `ENABLE_LISP` flag would have put a new gate across a dozen call
sites in common code — `kernel_main`, the shell, `vfs_server` — that no other
target ever exercises, which is a gate that rots. And it would have been
solving a *pool sizing* problem with a *feature* switch. `user/lisp/lisp.c`
already has the mechanism: `NODE_POOL_SIZE` is board-scoped, 1024 on RP2350
against 4096 on QEMU, and the RP2350 figure is not a guess — it is what phase
13's S4 arrived at empirically on hardware, on a board with 520 KB. This
board has 768 KB and the same shape of budget, so it gets the same number.
`.bss` fell from 181 KB to 30 KB and the Lisp machine is still a Lisp machine
on the new silicon, which seemed worth more than five fewer kilobytes.

##### 3. The UART is the only thing here that is really new

`drivers/uart_esp32p4.c`, and it configures hardware the boot ROM has
already configured. That is deliberate. E1 established that the ROM hands
over a working UART0 and `minimal_esp32p4.c` therefore wrote bytes and
nothing else — correct for a program proving the chip was alive, and
unacceptable for a kernel. "It works because of what the loader left behind"
is a property of one boot path, and this image is meant to acquire more of
them.

**The source clock is the crystal, not the PLL**, and that turned out to
matter more than expected. XTAL_CLK is 40 MHz on this board and does not move
when the CPU clock does, so the console's baud rate does not depend on the
clock tree at all: E2 gets a shell without bringing the PLL up, and nothing
later can take the console away by changing a CPU frequency. The cost is
0.008% baud error against 0.00% — four orders of magnitude inside what a UART
tolerates.

**Three registers had to be transcribed and two of them were nearly wrong.**
`UART0_SYS_CLK_EN` is bit 18 of `SOC_CLK_CTRL1` and `UART0_APB_CLK_EN` is bit
7 of `SOC_CLK_CTRL2`. The first draft of this file had them at 24 and 25,
reasoned from where UART0 sits in the peripheral list — and the two registers
order their fields differently from each other, so there is no position to
infer. They were caught by rendering the TRM's own bit diagrams (Registers
11.7 and 11.8, pages 1108 and 1113) and counting field labels down from bit
31, then checking against IDF's generated header. This is §3.2's rule paying
for itself inside one afternoon, on the first file written under it.

Two more facts from the same reading, both of which would have cost a session
each:

* **The `_SYNC` registers need a commit.** `CLKDIV_SYNC` and `CONF0_SYNC`
  live in the UART core's clock domain, and a write does nothing until
  `UART_REG_UPDATE_REG` bit 0 is written and self-clears. It does not fail
  loudly — the readback shows what you wrote and the hardware keeps using the
  old value.
* **`TXFIFO_CNT` is now verified.** TRM Register 45.21, bits [23:16], against
  a 128-byte FIFO. E1 left it as an open suspect for a garbled-output report
  it could not explain; it is a real count against a real capacity and cannot
  silently overrun. (The TRM's prose for that field says "the number of valid
  data bytes in RX FIFO" — a copy-paste slip in the manual, not a second RX
  counter.)

The driver is **polled**, with no interrupt path at all, because there is no
route from a peripheral to a handler until E3 brings the CLIC up. Both
blocking primitives spin with `sched_yield()`, which is the fallback
`drivers/uart_16550.c` already takes when its ISR slots are busy. E3 adds the
ISR beside them.

**It is also the third copy of the uart task/batching machinery in this
tree**, and that is recorded rather than fixed. Two copies were defensible —
the RP2350 one carries a USB CDC mirror and a demux bypass the QEMU one does
not. A third makes the shared part worth factoring, but E2 is the wrong
moment: extracting it now would change two drivers that work, on two boards,
to make room for a third that has never run. **E3 is where that debt comes
due**, because that is when all three have the same shape and the comparison
is real.

##### What E2 deliberately did not do

* **No preemption.** `ticker_init()` refuses on this board and says why. The
  P4's timer interrupt arrives through the CLIC as `clicintie[7]`, so there
  is no tick until E3, and E4 arms the comparator through it. `g_enabled`
  stays false, `kernel_main()` never calls `irq_restore(IRQ_ENABLE_BIT)`, and
  the system runs cooperatively — which is what E2 asked for. Arming a
  comparator whose interrupt cannot be delivered would report preemption as
  enabled on a system that never switches.
* **But there *is* a clock.** `kernel/time.c` reads the system timer's UNIT0
  (TRM ch. 16) — a free-running 52-bit counter, no comparator, no interrupt.
  Reading a counter needs none of E3, and the alternative is what the QEMU
  RV32 branch of that same file records having shipped once: a "clock" that
  counted its own calls, so every caller measuring a time budget was really
  measuring how often it was polled. The rate is XTAL/2.5 = 16 ticks/µs (TRM
  16.4), derived from `CONFIG_XTAL_HZ` rather than written as a bare 16.
  Worth noting for E4 and for phase 29: XTAL/2.5 is not an integer ratio, so
  the hardware alternates ÷2 and ÷3 and any single reading can be one tick
  (62.5 ns) out.
* **No `/flash0`.** `flashfs.bin` is built and goes nowhere: the QEMU form is
  a 512 KB embedded C array, which will not fit in a RAM-loaded image, and
  the RP2350 form needs a flash map, which is E6. `flashdisk_get_device()`
  returns NULL, `/flash0` stays unmounted, and `df` says so.
* **No `lugalos.uf2`.** The QEMU builds emit one that nobody flashes, which
  is harmless there because nothing on those targets is flashed at all. This
  board has real flash and a real image format coming in E6, so a file
  carrying an RP2350 family id would be an artifact that looks flashable and
  is not. There is no build-time ESP image either: `tools/p4run.py`
  regenerates it from the ELF on every load, because E1's worst afternoon was
  spent re-delivering a stale one.

##### The trap vector, and 48 bytes of insurance

`arch/riscv/common/entry.S` now aligns `trap_vector_entry` to 64 bytes on
this board and keeps 16 everywhere else. In CLIC mode `mtvec[5:2]` are
reserved, so a base that is not 64-byte aligned can come back truncated — and
a truncated vector base points into the middle of the trap handler, which
presents as an unexplained hang on the first exception rather than as
anything to do with alignment. Nothing observed this; it costs 48 bytes of
padding once, and the failure it prevents is one of the expensive kind.

Interrupts stay off, but exceptions do not: in CLIC mode only *interrupts* go
through `mtvt`, so a bad instruction or a misaligned load still vectors to
`mtvec` and produces the same register dump every other target produces. That
is the difference between a bug and a board that stopped.

##### What the new board caught in the old code

A third target is a falsification device, which is §0's whole argument for
this phase. It took two boots to produce two findings, and neither is about
the P4:

* **`[AT24C32] 4KB I2C EEPROM detected at 0x57!`** — from a board with no I2C
  controller configured at all. The non-RP2350 backing for that driver is a
  synthetic 4 KB RAM buffer, which cannot fail a probe, so the detection test
  always succeeded and the "No EEPROM detected (Using synthetic 4KB RAM
  buffer)" branch was unreachable. It has been printing a chip that is not
  there on every QEMU boot for as long as the driver has existed. `detected`
  stays true — `/dev/eeprom`, `eeprom-read`/`eeprom-write` and the identity
  store all work against that buffer, so the device *is* present — but it now
  says what it is.
* **`[USB CDC] Host Pass-Through Gateway Online (/dev/ttyUSB0 / /dev/ttyACM1)`**
  — from a build that compiles `drivers/usb_cdc.c` down to stubs, and it named
  the two device paths that on this board are the actual cables carrying the
  console and the reset lines. So the kernel appeared to be claiming the port
  its operator was reading it on. Worse, `probe_usb_cdc()` returned 0
  unconditionally, so the registry listed `usb` as **present** — and `usb` is
  a bindable console (`console-bind "usb"`, `init.lisp`). Binding the terminal
  to a device that discards every byte is the one way to lose a console with
  no message to say what happened. There is now a `usb_cdc_present()`, and the
  probe reports what it says.

Both are exactly the class `kernel/board.c`'s own probe comments were written
about — "the registry said present, the kernel log said nothing had been
detected, and both were telling the truth about different things" — and both
survived on two targets for months because on QEMU nothing else is real
either, so a fictional device does not stand out. On a board where
`/dev/ttyUSB0` is a cable you are looking at, it does. The registry now reads
`rtc(absent), sensor(absent), eeprom, usb(absent), uart, uartslip,
uartdemux`, and the QEMU suite is 359/359 either way.

##### The workflow

```
cmake --preset esp32p4 && cmake --build --preset esp32p4
tools/p4run.py build/esp32p4/lugalos.elf --baud 921600 --interactive
```

`--baud` applies to the load transfer only, and it is new in E2 for an
arithmetic reason: the image is ~227 KB against E1's 1.2 KB, which is about
40 seconds at 115200. The console stays at 115200 throughout, because the
kernel sets its own rate the moment it starts and the ROM's is only in force
until then. `--cmd` (repeatable) types a line at the shell and prints what
comes back, which is what lets this milestone's own done-condition be checked
without a human at the keyboard — and what `tests/hw/` will use in E8.

### E3 — Traps and the CLIC

*(Was E4. E0 §8 established that this must come first: `mtvec.MODE` is
read-only at CLIC mode, and the timer interrupt itself arrives as
`clicintie[7]`, so there is no tick without an interrupt controller.)*

CLIC setup at `0x20800000`, `mtvt`, and the P4 arm in
`arch/riscv/common/trap.c` beside the existing Hazard3 one. Then
`kernel/devirq.c` routing a real peripheral interrupt — UART RX is the
natural first, since the console already wants it.

**Written against the non-standard CLIC** (E0 §5): interrupt threshold in a
memory-mapped register rather than a CSR, and `mintstatus` at `0x346`.

*Read TRM §2.9.2.1 before starting. It says the thing that makes this
milestone unavoidable rather than merely due:* "Only the CLIC mode of
operation is supported by the HP core, i.e. `mtvec.MODE` is hardwired to
0x3. Hence, **the basic RISC-V interrupt handling scheme and the associated
CSRs (such as `mie`, `mip`, `mideleg`, `uie`, and `uip`) are unavailable.**"
*So every `set_csr(mie, ...)` in this tree is a no-op on this silicon — it
does not fail, it simply does nothing, which is the failure mode that looks
like a dead peripheral. `kernel/ticker.c`'s E2 arm refuses for exactly this
reason rather than arming a comparator and hoping.* The
standard variant belongs to v3 silicon we do not have; if a v3 board ever
arrives, this is the file that learns about it, behind the revision check
§3.3 asks for.

Done when: a UART RX interrupt reaches a handler; a deliberate illegal
instruction produces the same diagnostic dump the RP2350 build produces.

**E3 also inherits a debt E2 deliberately did not pay.** `drivers/uart_esp32p4.c`
is the third copy of the uart task/batching machinery in this tree
(`uart_16550.c`, `uart_rp2350.c`, and now this one). Two copies were
defensible — the RP2350 one carries a USB CDC mirror and an A3b demux bypass
the QEMU one does not. Three is where the shared part is worth factoring,
and E2 was the wrong moment to do it: extracting a common core then would
have meant changing two drivers that work, on two boards, to make room for a
third that had never run. It has run now. Once E3 gives this file its ISR,
all three have the same shape and the comparison is real — that is when to
decide, and the decision may still be "leave them", but it should be a
decision.

*(Exceptions already work, incidentally: in CLIC mode only interrupts go
through `mtvt`, so `mtvec` — set by `entry.S`, 64-byte aligned there since E2
— already catches a fault. What E3 adds is the interrupt half.)*

**DONE, 2026-09-06.** Both halves verified on hardware.

```
lsh> uartstats
[UartStats] write_calls=21 irqs=6 rx_wakes=2
lsh> uartstats
[UartStats] write_calls=32 irqs=8 rx_wakes=3
lsh> cat /proc/meminfo
  ... twelve lines of output ...
lsh> uartstats
[UartStats] write_calls=65 irqs=17 rx_wakes=5

lsh> trapselftest
[Trap Selftest] Executing an illegal instruction under a probe...
[Trap Selftest] PASS: the exception reached the trap vector and execution resumed.

lsh> trapselftest fatal
[Trap Exception] Cause: 0x2, epc=0x4ff4a356, tval=0xc0001073, inst=0xc0001073
[Trap Register Dump] a0=0x8, a1=0x8, sp=0x4ff3f980, ra=0x4ff4a356
[Fatal] System halted due to unhandled exception.
```

`rx_wakes` is the number that answers the done-condition, and it is a new
counter (`uart_irq_rx_wakes()`, on all three console drivers) added because
the obvious evidence is not evidence. A console that answers keystrokes
proves nothing about *how* it noticed them: every one of these drivers keeps
a `sched_yield()` polling fallback underneath its ISR, for the paths that
cannot block, and the two are indistinguishable from the far end of the
cable. A count of interrupts that *woke a blocked reader* is the difference.
`uart_irq_count()` came first and was not enough on its own — a driver whose
TX blocks on an interrupt and whose RX polls produces a healthy total and
still spins on every key.

#### What E3 built

* **`arch/riscv/common/trap.c` gains a CLIC arm** beside the Hazard3 and PLIC
  ones, as §3.4 asked: `mcliccfg`, the memory-mapped threshold, the
  per-interrupt `clicintattr`/`clicintctl`/`clicintie` bytes, and `mtvt`.
* **`arch/esp32p4_intr.h`** — the two things that cannot live inside
  `trap.c`: `esp32p4_intmtx_route()`, because only a driver knows its
  peripheral is matrix source 31, and the CLIC line allocation itself,
  because a line is a shared resource and two drivers picking the same number
  would each get the other's interrupts with no evidence in either file.
* **`drivers/uart_esp32p4.c` gains its ISR**, RX and TX, in the shape
  `uart_16550.c` already had: one waiter slot per direction, the peripheral's
  interrupt-enable bit set only for the duration of a wait.
* **`trapselftest`** in the shell, on every target. The recoverable form runs
  an illegal instruction under `arch_probe_begin()`; `trapselftest fatal`
  does not recover, and exists because "the same dump the other builds
  produce" is something a person has to look at.
* **`devirq_dispatch()` returns a value now.** Unhandled means something
  different here than elsewhere — see below.

#### The four things this milestone found

**1. `mcause` is not a cause code on this chip.** In CLIC mode `mcause`
carries the state `mret` restores: `MPP` in [29:28], `MPIE` at 27, and the
previous interrupt level `MPIL` in [23:16]. The code itself is `EXCCODE`,
[11:0] — widened from the base ISA's [30:0] precisely to make room. So
`trap_handler()`'s "everything except the top bit" mask is wrong here, and
wrong in the way that costs an afternoon: the first UART interrupt this
kernel ever took on the P4 arrived as `0x38000010`, which is `MPP=3`,
`MPIE=1`, `MPIL=0`, and in the low twelve bits **interrupt 16** — the
console, correctly routed, correctly delivered, and matching nothing. It fell
through to the "Interrupt received" diagnostic, returned without touching the
peripheral, and a level-triggered source re-entered immediately. *An
interrupt controller that works perfectly and a console that prints one line
forever look identical from the other end of the cable.* This also matters to
**E4**: `code == 7` would never have matched either, so a timer arm written
against the old mask would have produced a comparator that fires and a tick
that never counts.

**2. An unhandled interrupt is a wedged board here, not a lost event.** Every
external interrupt on the P4 is level-triggered off the peripheral, so a
source with no handler is still asserted when the handler returns.
`devirq_dispatch()` therefore returns 0/-1 now, and the CLIC arm masks the
line on a miss. The PLIC and Hazard3 arms ignore the result, because
claim/complete and pending-bit semantics already drop an unclaimed interrupt
— this is the one controller where "unhandled" needs an action.

**3. `mcliccfg.MNLBITS` decides whether an enabled interrupt fires at all.**
With `MNLBITS` at IDF's value of 3, an interrupt's level comes from
`clicintctl[7:5]`, whose reset value is 0; the TRM's rule is that levels
"less than or equal to the effective threshold are not allowed to preempt",
so at threshold 0 a routed, enabled, pending, unmasked interrupt still never
fires until its `clicintctl` is written. `p4_clic_init()` sets `MNLBITS` to 0
— the row of Table 2.9-2 where every interrupt is level 255 and the failure
cannot happen — *and* `arch_irq_enable()` writes `clicintctl` to `0xff`
anyway, which is the maximum under either encoding. Correct under both, and
deliberately not dependent on which.

**4. The fatal dump's `inst=` field had a hardcoded address window.**
`0x10000000`–`0x20082000` covers RP2350's XIP flash and QEMU virt's RAM. The
P4 links at `0x4FF00000` and fell outside it, so every fault on this board
would have printed `inst=0x00000000` — indistinguishable from a genuine zero
word. Found by reading the line while writing the demonstration, not by
seeing the zero, which is the only way that one gets found.

#### What E3 deliberately did not do

* **No hardware vectoring.** Every line is enabled with `clicintattr.SHV = 0`,
  which the TRM defines as "the CPU will jump to the address configured in
  `mtvec`" — the vector `entry.S` already installs, which saves a frame and
  calls `trap_handler()`, which reads `mcause`. One entry point for
  exceptions and interrupts, as on the other three targets. `mtvt` is still
  set, to a 48-entry table of pointers to that same vector: not the
  mechanism, but insurance, on the same principle that makes
  `drivers/uart_esp32p4.c` configure a UART the ROM already configured.
  Leaving a CSR the core can jump through as whatever the loader left in it
  is the thing this phase keeps arguing against.
* **No interrupt levels or nesting.** One flat priority, matching what the
  PLIC and Hazard3 arms already do. `mstatus.MIE` stays clear inside a
  handler and `mintstatus.MIL` reaching 255 means nothing could preempt one
  anyway.
* **No timer.** The CLIC half of the tick exists — `trap_handler()`
  dispatches cause 7 the way every other target does — but the CLINT
  comparator, its `MTIME_EN` and its `MTIME_SAM` are E4. `kernel/ticker.c`
  still refuses, and its message now names E4 instead of E3.
* **`mstatus.MIE` is set in `trap_init()`, not left to `main.c`.** `main.c`
  only enables interrupts if `ticker_init()` succeeded, which on this board
  it does not. Waiting for the ticker would have meant an interrupt that is
  routed, enabled, pending and unmasked at the controller and still never
  delivered — this board's speciality, since `mstatus.MIE` is the one
  interrupt CSR here that does still work. Same shape the RP2350 arm already
  used.

#### The debt E3 was asked to decide about

The three copies of the uart task/batching machinery now genuinely do have
the same shape, which is what E3 was supposed to establish. **The decision is
to extract the task body and wire protocol, not the facade, and not in this
milestone.**

The reasoning, since "leave them" was an allowed answer and this is not it:

* The **hardware halves have converged** and should stay separate — they are
  three different peripherals and nothing about them wants sharing.
* The **task body and wire protocol** (`'H'`/`'R'`/`'W'`, `chan_serve_wait`,
  `chan_register_task`, `uart_task_alive`) are now the same in all three,
  ~60 lines, reaching the hardware only through two function pointers. That
  is a clean extraction with no board-specific residue.
* The **facade is not**. `uart_rp2350.c`'s `uart_putc()`/`uart_flush()` have
  the USB CDC mirror woven through them, so extracting the batching would
  mean either a hook in shared code that exactly one board uses, or a
  two-of-three extraction — which is the shape that was already defensible
  before E2 added a third file.

Not in this milestone because it is a refactor of two working drivers on two
boards, and this commit is an interrupt controller; those do not belong in
the same change. It belongs in its own commit, verified by the QEMU suite for
`uart_16550.c` and by an RP2350 board for the other, and it is not a
prerequisite for E4.

*(2026-09-06, after this milestone: it has one.
`plan/phase30_driver_framework.md` §G4 owns this extraction, and the question
it grew out of turned out to be larger than the UART. Asked whether a second
silicon family means this tree needs a HAL, the answer is no — the seams
`devirq.h`, `arch/trap.h`, `ticker.c` and `arch/vmm.h` took a whole new chip
with **no client changes at all**, which is what E2 and E3 were the test of.
What is duplicated is one layer up: nine copies of the driver-as-task
lifecycle and seven of the U-mode domain construction, none of which contain
a register. Phase 30 §1 has the general form — four categories of
hardware-facing code, opposite treatment for each — and the rule that would
have fired here on its own: extract at the third implementation, never the
second. Phase 30 is sequenced before phase 28.)*

### E4 — Time

*(Was E3.)*

`kernel/ticker.c` gains a P4 arm on the CLINT at `0x20000000` (E0 §2).
`time_init()`, the monotonic clock, `ticker_init(100)`, preemption,
`sched_init()`, and the existing task tests.

Three specifics from E0 §2, so they are not rediscovered at the bench:
`mtimectl.MTIME_EN` must be set or the counter never moves, and only Core 0's
copy of that bit does anything; the timer interrupt is CLIC interrupt 7; and
`mtimectl.MTIME_SAM` gives an atomic 64-bit read, so the P4 arm should use it
rather than copying the RP2350 path's re-read loop.

Three more from E3, now that the controller underneath exists:

* **The CLIC half is already done.** `trap_handler()` dispatches cause 7 to
  `ticker_count_tick()`/`ticker_next()` on this board exactly as on every
  other one, and `arch_irq_enable()` refuses IDs outside 16–47 on purpose —
  arming interrupt 7 is E4's business and needs its own call, not a device
  driver's. What is missing is only the CLINT end.
* **`mcause` masking is already fixed, and was not obvious.** E3's finding 1:
  the code is `EXCCODE`, bits [11:0], because CLIC mode puts `MPP`, `MPIE`
  and `MPIL` in the same register. Written against the old mask, `code == 7`
  would never have matched, and the result would have been a comparator that
  fires and a tick that never counts.
* **Two time bases, still.** `kernel/time.c` reads the system timer
  (XTAL/2.5) while the tick would run off the CLINT's `mtime`, whose source
  the TRM does not state. E4 owns reconciling them, or deciding they need not
  be reconciled, but it should not discover the question at measurement time.

Done when: `uspin.elf` is preempted, and the tick rate measured against a
host stopwatch over ten minutes is within the same tolerance the RP2350 build
holds.

**NOT DONE, 2026-09-06 — the mechanism works and one test does not.**
Preemption is real and measured on this board, and `priostress` crashes it
deterministically. That second fact is recorded here rather than deferred,
because a preemption timer with a reproducible crash under two busy tasks is
not a finished milestone.

```
lsh> clicdump
[CLIC] mtime      = 0x000000003f36e39c
[CLIC] mtimecmp   = 0x000000003f38fa00
[CLIC] mtimectl   = 0x00000001
[CLIC] mcliccfg   = 0x00000001  clicinfo = 0x00600030  mintthresh = 0x00000000
[CLIC] irq7  ip=0 ie=1 attr=0xc0 ctl=0xff  (timer)
[CLIC] spin0 (nothing else READY): ticks +200 over 2 s, MIL seen=0, MIE now=1
[CLIC] spin1 (a READY task waiting): ticks +200 over 2 s, MIL seen=0, MIE now=1

lsh> preempttest
[Preempt] ticks=11 flag=1 -- PREEMPTED (a task ran without anyone yielding)
```

`preempttest` returns in 104 ms. Before the fix below it ran for 150 seconds
and reported `ticks=1 flag=0`.

#### The ten-minute measurement, and what it says

Two `clicdump` samples 616.265 s apart by the *host's* clock, which is the
only reference here independent of the board:

| | measured | vs host |
|---|---|---|
| board timestamps (systimer, `kernel/time.c`) | 616.239 s | **−42.7 ppm** |
| preemption ticks counted | 61549 in 616.265 s = 99.8742 Hz | **−1258 ppm** |

**The wall clock is good and the tick is slow, and the gap between them is
not a defect in either.** `ticker_next()` rearms *relatively* --
`set_deadline(now() + interval)` -- and `now()` is read inside the handler,
some microseconds after the deadline actually expired. Every period is
therefore the interval plus one interrupt latency. The arithmetic closes:
61549 ticks over 616.265 s is 10.0126 ms per tick against a programmed
10.0000 ms, so the latency is **12.6 µs**, or about 500 cycles of a 40 MHz
core -- which is what a 32-register trap frame, `trap_handler()`,
`p4_drop_intlevel()`'s `mret`, `ticker_next()` and `sched_yield()` cost when
the CPU is running at a tenth of its rated speed because nothing has brought
the PLL up (see "deliberately did not do").

So the figure is dominated by this board's clock speed, not by its timer. The
same code on a 400 MHz core would show roughly 126 ppm, and the RP2350 build
has always had the same relative-rearm behaviour with a faster CPU under it.
**This is a scheduling timer, not a clock**: nothing in this tree reads
`ticker_ticks()` for elapsed time, `kernel/time.c` is what tells the time,
and that is the number measured at −42.7 ppm.

Making the tick itself drift-free means rearming *absolutely* (`mtimecmp +=
interval` rather than `now() + interval`), which is a change to the shared
`ticker_next()` and brings a catch-up hazard: after any long masked stretch
an absolute comparator fires repeatedly until it has made up the lost ticks.
That is a decision about all four targets, not an ESP32-P4 bring-up detail,
and E4 leaves it alone deliberately.

#### The finding this milestone exists for

**In CLIC mode, `mstatus.MIE` is not the interrupt gate.** Taking an interrupt
sets `mintstatus.MIL` to that interrupt's level, and the effective threshold is
`max(mintthresh.TH, mintstatus.MIL)`. Exactly one instruction lowers `MIL`
again: `mret`, which restores it from `mcause.MPIL`.

That is fine for a handler that returns. This kernel's timer handler does not
— preemption works by calling `sched_yield()` *inside* the handler, so control
leaves through `ctx_switch()` and the `mret` that would restore `MIL` stays
stranded in a stack frame, unwound only when that task is resumed. Every
interrupt at level 255, which is all of them here, stays masked for as long as
the switched-to task runs. The tick that would preempt it is masked too, so
nothing brings it back; only a cooperative yield eventually unwinds the chain.

The symptom was a preemption timer that ran at exactly 100 Hz and preempted
nothing:

```
spin0 (nothing else READY):   ticks +200 over 2 s, MIL seen=0
spin1 (a READY task waiting): ticks   +0 over 2 s, MIL seen=255
```

`mstatus.MIE` reads 1 in both. **That is why no existing test on any other
target could have caught this**: on RP2350 and on QEMU, `MIE` is the whole
story, and there is no `MIL` to leave behind.

The fix is `p4_drop_intlevel()` in `arch/riscv/common/trap.c`: an `mret` that
returns to the next instruction, with `mcause.MPIL` cleared first so the level
drops to zero, `mstatus.MPIE` cleared so the rest of the handler stays masked,
and `MPP` forced to M so privilege does not change. One `mret` per interrupt on
this board. The larger alternatives — a trampoline that returns into a stub
which then yields, or the CLIC's own `mnxti` — are not needed while this
kernel has one flat interrupt level.

#### Three more things the milestone found

**1. `mtime` runs at 40 MHz, and that had to be measured.** The TRM documents
every register of the CLINT block and never names its clock source, and
ESP-IDF is no help: **it does not use the CLINT on this chip at all**, ticking
FreeRTOS off the systimer instead, and its own `CLINT_BASE` constant
(`0x02000000`) is the generic RISC-V one, unrelated to this window. So the P4
arm measures the rate against `kernel/time.c` at init, exactly as the RP2350
arm already does, and reports `40005494 Hz, measured` — the 40 MHz crystal,
within this measurement's own resolution.

**That also answers the two-time-bases warning E3 left for this milestone.**
`mtime` and the systimer are two dividers off one crystal (the systimer takes
XTAL/2.5), not two oscillators — and the tick interval is *derived from*
`time.c` at init, so if the systimer is wrong the tick is wrong in the same
proportion. One discrepancy, not two.

**2. `MTIME_EN` resets to 1.** E0 §2 said "the counter does not run until told
to ... structurally the same surprise RP2350 had". It is not: the RP2350's tick
generator really does reset disabled, this one resets running. Written
explicitly anyway, for the reason `drivers/uart_esp32p4.c` gives about the
console.

**3. `MTIME_SAM` is a trap, not a feature, and E0 recommended it.** The plan
said this arm "should use it rather than copying the RP2350 path's re-read
loop". Read the wording again: *"Sample the other half of the system count on
reading MTIMELO **or** MTIMEHI"*. Either read latches the other, so a pair of
reads gets one live value and one latched value, and the *next* call reads a
half latched during the previous call — the two halves of a returned value no
longer come from the same instant. Invisible for the first 107 seconds of
uptime, because until the low half wraps the high half is 0 and a stale 0 is
indistinguishable from a fresh one. The P4 arm uses the ordinary re-read loop.

*(Recorded because it was also a wrong diagnosis for a while: `MTIME_SAM` was
removed as the suspected cause of the preemption failure, the fixed build
behaved identically, and the real cause was `MIL`. The removal was right on its
own merits and wrong as an explanation.)*

#### What E4 built

* **`kernel/ticker.c` gains a P4 arm** on the core-local CLINT at
  `0x20000000`: `mtimecmplo`/`hi` at `0x4000`/`0x4004`, `mtimelo`/`hi` at
  `0xBFF8`/`0xBFFC`, `mtimectl` at `0x4010`, the rate measured rather than
  assumed, and the comparator written high-half-first so it can never briefly
  hold a value in the past.
* **`esp32p4_clic_timer_enable()`**, because `mie` does not exist here and
  `arch_irq_enable()` refuses IDs below 16 on purpose — 3 and 7 arrive from the
  CLINT rather than the interrupt matrix, and a driver reaching them by
  arithmetic on its own line number is a bug that would present as a tick
  nobody armed.
* **`p4_drop_intlevel()`**, above.
* **`clicdump`** in the shell, P4-only. It reads the CLINT and CLIC state, and
  then samples `mintstatus` from *inside* two non-yielding loops — one with
  nothing else READY and one with a READY task waiting. That second phase is
  the regression test for this milestone's bug: it is the only state in which
  the fault is visible, and no dump taken at the prompt can reach it.

#### The open failure: `priostress` faults, deterministically

```
lsh> priostress
[  118.266] [Sched] Created task #4 'stressA' (stack 4ff7e000, 8 KB)
[  118.288] [Sched] Created task #5 'stressB' (stack 4ff80000, 8 KB)
[  140.817] [Sched] Task #4 'stressA' exited

[Trap Exception] Cause: 0x1, epc=0x0, tval=0x0, inst=0x00000000
[  140.826] [Trap Register Dump] a0=0x0, a1=0x0, sp=0x4ff81f50, ra=0x0
[  140.832] [Fatal] System halted due to unhandled exception.
```

Reproduced twice, identical to the byte. Cause 1 is an instruction access
fault; `epc` and `ra` are both zero, so a kernel task executed `ret` with
`ra = 0`. The reported `sp`, `0x4ff81f50`, is inside **stressB's** stack
(`0x4ff80000` + 8 KB) and is exactly 64 bytes — one `ctx_switch` frame — above
`0x4ff81f10`. So `ctx_switch()` restored a frame whose `ra` slot held zero and
returned into it, nine milliseconds after stressA exited.

**This is not a broken test.** `priostress` runs on RP2350 silicon in
`tests/hw/test_rp2350.py` and in the QEMU suite, and passes in both; it takes
2.5–3 s there against 22 s here, purely because this board has no PLL.

What is known, and what is not:

* `palloc_free()` does **not** zero pages — it only clears bitmap bits — so
  the zeroed frame cannot come from a free. `palloc_pages()` does zero, and
  `task_create_full()` then paints `STACK_POISON_WORD`, so a fresh task stack
  is poison rather than zeros either.
* The one place this tree *does* leave zeros in a 16-word context frame is
  `task_create_full()`'s priming: it zeroes all sixteen slots and then writes
  only `[0]` (ra), `[1]` (s0) and `[2]` (s1). A restore starting four words
  into that frame reads `ra = 0`. That is a lead, not a diagnosis.
* `task_exit()` calls `palloc_free()` **while holding `g_sched_lock`**, which
  `sched_reap()`'s own comment says must not happen: *"palloc_free() takes
  palloc's own lock (S4), and nesting the two would create a lock ordering
  this kernel has no reason to have."* Found while reading this path. Whether
  it is related to the fault is unproven — it is a real defect regardless.

The trigger is specific: a task exiting while another same-tier,
never-yielding task is mid-preemption. That combination is newly reachable
here because this is the first target where preemption runs on a CPU slow
enough for the window to be wide.

**E4 is not signed off until this is understood.** The preemption mechanism
below is correct and measured; this is a scheduler-lifetime bug that
preemption on slow silicon has exposed, and it may well not be P4-specific
code at all.

**Root cause found, 2026-09-06. The fix is not written yet, and the reason is
in "what the first fix attempt taught" below.**

**`task_exit()` makes a blocking IPC call before it has begun exiting.** Its
first act is

```c
printk("[Sched] Task #%d '%s' exited\n", t->pid, t->name);
```

which reaches the console through `uart_flush()` → `chan_call()` →
`task_block()`. So a task announcing its own death **switches away in the
middle of dying**: it sits BLOCKED, still owning its stack, with none of its
exit bookkeeping done, and a second task can walk into `task_exit()` behind
it. The comment above that line already knew half of this — *"deliberately
outside it, because printk() can block"* — and treated blocking as the reason
to move the call, rather than as the hazard.

The reap slot immediately below states the assumption that breaks:

> One slot suffices because a task can only exit while running, and the next
> task reaps before anything else can exit.

Confirmed by controlled experiment rather than by reading: **deleting that one
`printk()` makes `priostress` pass**, repeatably —
`done=1,1 total_ticks=2252,2252 -- FAIR`.

The diagnostic run that showed it:

```
[  140.802] [Sched] Task #5 'stressB' exited
[Trap Exception] Cause: 0x1, epc=0x0, tval=0x0, inst=0x00000000
[  140.804] [Sched] Task #4 'stressA<-- truncated: still printing
[  140.811] [Trap Context] pid=5 'stressB' state=RUNNING
[  140.884] [Sched Table]   #4 'stressA' BLOCKED  stack=0x4ff7e000+2P prio=3
[  140.892] [Sched Table]   #5 'stressB' RUNNING  stack=0x4ff80000+2P prio=3
```

Both stress tasks inside `task_exit()` at once, one BLOCKED in its own exit
message, the other returning to address zero.

**This is not ESP32-P4 code.** It is `kernel/sched.c`, and it is reachable on
every target that preempts. The P4 is simply the first board slow enough to
hold the window open every time: `priostress` runs two equal-length,
same-tier, never-yielding tasks that finish within milliseconds of each other,
and without a PLL this CPU spends long enough inside the exit path for the
second task to arrive. On RP2350 the same test passes — which is evidence
about timing, not about correctness.

#### What the first fix attempt taught

Moving the announcement into `sched_reap()` — where the task is already dead,
the stack already freed, and the printing task an ordinary healthy one — is
the obvious fix, reads well, and **hangs the board**.

`sched_reap()` is called from the top of `sched_yield()`, and `sched_yield()`
is called by the timer interrupt. A blocking `printk()` there is a blocking
IPC call from interrupt context, which is strictly worse than the bug it
replaces. Reverted.

So the constraint is sharper than "move it somewhere else":

* not in the dying task, before its bookkeeping — that is the bug;
* not in `sched_reap()` — reachable from the tick;
* not through `printk()` at all on any of those paths, because every route to
  the console on this board can block: even `uart_debug_puts()` reaches
  `uart_hw_putc_blocking()`, which calls `task_block()` when a task exists and
  the waiter slot is free, and `sched_yield()` otherwise.

A correct fix therefore needs either a genuinely non-blocking console write
for kernel-critical paths, or a deferred announcement drained from a context
that is guaranteed to be an ordinary task. Both are changes to shared code on
four targets and belong in their own commit with a full suite run behind them
— which is the same argument `plan/phase30_driver_framework.md` makes about
not folding refactors into bring-up milestones.

#### What is committed meanwhile

The diagnostics that found it, because they are worth more than the afternoon
they cost:

* **`[Trap Context]`** in the fatal trap dump — which task, by pid, name and
  state. The dump used to be registers only, and "whose stack is `0x4ff81f50`?"
  took a reproduction run to answer by arithmetic.
* **`[Trap Frame]`** — the sixteen words below `sp`. `ctx_switch()` restores
  `ra` from the word at the incoming sp, so those words are the frame that was
  just restored; printing them turns "the frame was corrupt" from an inference
  into a reading.
* **`[Sched Table]`** — every task's state, parked sp, stack and priority at
  the moment of the fault. This is the one that solved it: it showed two tasks
  inside `task_exit()` simultaneously.
* **`sched_check_incoming()`** — refuses to switch into a task whose parked sp
  has a zero return address, halting with the table intact instead of jumping
  to zero. It never fired here, and that was itself the decisive negative
  result: it ruled out a corrupt parked sp and pointed at corruption of a
  *running* task.
* **An overlap check in `sched_reap()`** — refuses to hand a range back to the
  allocator while a live task's stack overlaps it. Also never fired, also
  informative: `palloc_free()` does not zero memory, so a wrong free is silent
  until the pages are handed out again, and this makes it loud instead.

Three of the five exist to say "not this", and they did.

#### What E4 deliberately did not do

* **No PLL.** The CPU still runs off the crystal, so this board is slow — a
  volatile 64-bit loop of 400 M iterations takes about 150 seconds, which is
  what made `preempttest` look like a hang for most of an afternoon. Bringing
  the PLL up is not needed by any milestone here and would change every timing
  measurement in this phase; it belongs with E6 or later.
* **No interrupt nesting.** One flat level, as on every other target. Now that
  `MIL` is understood, levels would be implementable — and are still not wanted.
* **No use of `mnxti`.** See the fix note above.

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

**The board's real-time clock, if it is ever done, is done here — not in
E4.** Asked during E4 whether the NANO's on-board RTC was in scope, and the
schematic answers a different question than the one asked
(`ESP32-P4-NANO-schematic.pdf`, read 2026-09-06):

* **There is no RTC chip.** Nothing on I2C, nothing like the DS3231 the
  RP2350 boards carry. What the board has is the **P4's own LP/RTC domain**,
  and two parts feeding it.
* **Y2, a 32.768 kHz crystal**, on GPIO0/GPIO1 through 0 Ω links R32/R36,
  with 22 pF loading caps and a 10 MΩ feedback resistor. Those two pins are
  `XTAL_32K_N` and `XTAL_32K_P` (datasheet pin table), so this is
  `XTAL32K_CLK` — a real crystal source for `RTC_SLOW_CLK`, not the internal
  150 kHz RC. **Populated, and never selected by any software this project
  has run.**
* **H3, a two-pin battery header** — a header, not a cell holder, and empty
  as shipped. Pin 1 is `ESP_VBAT` (the chip's `VDD_BAT`, pin 102), pin 2 is
  GND, with 1 µF across it. `ESP_3V3` feeds `ESP_VBAT` through **D2, a
  B5819WS Schottky, anode at 3V3** — so the rail powers the LP domain while
  the board is on, and blocks back-feed so a cell on H3 holds it when the
  board is off.

**Which means the headline benefit does not exist on this board as it
stands.** With H3 empty, `VDD_BAT` dies with `ESP_3V3` and the LP domain
retains nothing across a power cycle. A cell has to be fitted before any of
this is worth writing, and that is a hardware decision, not a scheduling one.

And even fitted, it is not a drop-in for the `rtc` device slot. The LP block
is a **counter with two alarm comparators** (LP Timer, `0x50112000`), not a
calendar chip: keeping wall-clock time across a reset means holding an epoch
offset somewhere in the retained domain and reading the count against it.
That is a real design and a small one, but it is a *different* design from
`drivers/i2c_rtc.c`, which owns the `rtc` registry name and the `date`
persistence path — so it is either a second implementation behind one
registry name (`plan/phase30_driver_framework.md` §1 category C, exactly the
duplication that phase just catalogued) or a new device class.

E7 is the right home because E7 is the milestone with a persona that wants a
timestamp at boot, and it comes after E6's flash and after phase 30's
framework. **Not E4**, for the reason §0 of this document gives about doing
one thing at a time: E4's done-condition is a tick rate measured against a
host stopwatch over ten minutes, E4 already carries the two-time-bases
warning (systimer vs CLINT), and a third oscillator in a fourth clock domain
in the same milestone gives every anomaly another candidate cause.

*(Found on the way: `drivers/i2c_rtc.c` printed "No DS1307/DS3231 RTC module
found at 0x68" on every target without an I2C controller — a probe that never
happened, the same class of claim as the two E2 caught, and on this board the
wrong conclusion drawn from a true sentence. Fixed to say there is no
controller.)*

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
already work: skipped when no board is attached, never failed. `README.md`
gains the P4 as a supported target. `plan/open_issues.md` receives everything
parked along the way.

**Its precondition is already met, which was not a given.** This milestone
needs a board that can be reset, reloaded and returned to flash with nobody
in the room, and on macOS that looked impossible. On Linux `tools/p4run.py`
does all three (E1, "What Linux answered"), so the suite can drive a load
itself rather than asking a human for BOOT+RESET. `tests/hw/README.md`
already carries the flashing procedure from E1 — done ahead of this
milestone, since the procedure was what E1 spent its time establishing.

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

*(2026-09-06: one document exists already, and it is not one of these.
`plan/phase30_driver_framework.md` was written after E3, out of the question
E3's uart debt forced — whether a second silicon family means this tree needs
a HAL. It does not; it needs the layer above the registers, which is a
different thing. **Phase 30 is sequenced before phase 28**, for the reason
stated there.)*

### Phase 28 — Ethernet on the P4

**Phase 30 comes first.** `plan/phase30_driver_framework.md`, written after
E3 and sequenced ahead of this one. Phase 28's MAC/PHY driver is the largest
new driver on the roadmap and it will be a task like every other driver here
— so it is either written against the extracted driver-task framework once,
or written the long way and migrated afterwards. Phase 30 §0.4 makes the
argument; the short form is that this driver is the reason the ordering is
worth anything.

That phase also answers the question E3 raised and did not settle — whether a
second platform means a HAL — and the answer shapes how phase 28's driver
gets written: the EMAC's registers stay per-chip and unshared (phase 30 §1,
category A, the same rule §3.2 of this document already states), while
everything above them comes from the framework.

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
