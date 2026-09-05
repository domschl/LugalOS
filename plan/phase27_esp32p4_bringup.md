# Phase 27 — A second silicon, and nothing clever on it yet

**Status: in progress, 2026-09-05. E0 and E1 done; E2 is next.** This is the first of three
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
* **`mtime` is a real memory-mapped CLINT**, at `0x20000000` with the
  standard `mtimecmp`/`mtime` register set (E0 §2) — against RP2350's SIO
  block at `SIO_BASE + 0x1a4`. *Half-true as originally written, and corrected
  2026-09-05: the counter is ordinary and slightly better than RP2350's (it
  has an atomic-read sampling mode), but its **interrupt** is not — it arrives
  through the CLIC as interrupt 7, so it is not free of §3.4's work.*
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

### E3 — Traps and the CLIC

*(Was E4. E0 §8 established that this must come first: `mtvec.MODE` is
read-only at CLIC mode, and the timer interrupt itself arrives as
`clicintie[7]`, so there is no tick without an interrupt controller.)*

CLIC setup at `0x20800000`, `mtvt`, and the P4 arm in
`arch/riscv/common/trap.c` beside the existing Hazard3 one. Then
`kernel/devirq.c` routing a real peripheral interrupt — UART RX is the
natural first, since the console already wants it.

**Written against the non-standard CLIC** (E0 §5): interrupt threshold in a
memory-mapped register rather than a CSR, and `mintstatus` at `0x346`. The
standard variant belongs to v3 silicon we do not have; if a v3 board ever
arrives, this is the file that learns about it, behind the revision check
§3.3 asks for.

Done when: a UART RX interrupt reaches a handler; a deliberate illegal
instruction produces the same diagnostic dump the RP2350 build produces.

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

Done when: `uspin.elf` is preempted, and the tick rate measured against a
host stopwatch over ten minutes is within the same tolerance the RP2350 build
holds.

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
