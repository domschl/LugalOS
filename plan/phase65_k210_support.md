# Phase 65 — The K210 Maix Bit: a 64-bit LugalOS node without a fence

Status: CANCELLED 2026-10-29
Hardware platform Maix Bit v1 expectations not holding for LugalOS.

**Status: planned, not started. Written 2026-10-26.** A Maix Bit (Canaan
K210) is connected over USB and ready for testing and flashing
(`/dev/serial/by-id/usb-1a86_USB2.0-Serial-if00-port0` → `ttyUSB0`, the
onboard CH552 enumerating as a CH340-class bridge, VID:PID `1a86:7523`).

**Milestone scheme: `65.1`, `65.2`, …**

**Reference material**

- Kendryte standalone SDK (Apache 2.0): `~/Source/gith/kendryte/kendryte-standalone-sdk/`
  — `lib/bsp/include/platform.h` (memory map), `lib/bsp/crt.S` (boot),
  `lib/drivers/` (PLIC, CLINT, UART, FPIOA, GPIO, SPI, I2C, I2S, WDT, DVP, KPU),
  `lds/kendryte.ld` (memory layout), `cmake/compile-flags.cmake` (`-march=rv64imafc -mabi=lp64f -mcmodel=medany`).
- Arduino core for K210: `~/Source/gith/kendryte/arduino-k210/` (peripheral usage examples).
- Datasheets: `~/Source/gith/kendryte/datasheets/`
  (`Maix-Bit_1.09(Schematic).pdf`, `Sipeed Maix-Bit Specifications_EN V2.0.pdf`).
- NNCase (KPU toolchain): `~/Source/gith/kendryte/nncase/` (out of scope here; see §8).

The K210 has appeared in this project's plans since phase 14 ("14e — new
hardware platforms") and was repeatedly deferred: *no networking, wrong word
size for the existing RV32 targets, and an unusual core*. This phase makes it
a first-class LugalOS target: **`k210-maixbit`**, the tree's 64-bit,
no-MPU, dual-core, camera-and-AI-flavoured node with a generous 6 MB of
SRAM.

---

## 1. What the chip actually is (survey of the SDK and schematics, 2026-10-26)

Everything below was read from the Kendryte standalone SDK or the Maix Bit
schematic; items marked **(verify)** are measured on the silicon in 65.1
before any kernel code depends on them.

### 1.1 CPU and privilege model

| Item | Value | Source / note |
| :--- | :--- | :--- |
| Cores | 2× Canaan-designed "K210" RISC-V cores, 64-bit | spec: "RISC-V Dual Core 64bit" |
| Clock | 400 MHz standard (PLL0), 26 MHz crystal | spec; `sysctl.h` PLL0/1/2 |
| Base ISA | `rv64imafc` + `cmodel=medany` | SDK `compile-flags.cmake` |
| Atomics (A) | **Not in the vendor `-march` string, but `lib/bsp/include/atomic.h` `#error`s without `__riscv_atomic` and `locks.c` uses `amo`-based `__sync_*`** — the vendor core almost certainly implements LR/SC/AMO and the march string is simply wrong | probe `misa` + an LR/SC/AMO smoke test in 65.1 **(verify)** |
| FPU | Single-precision **F** only, **no D**; `lp64f` ABI | march string; `crt.S` sets `MSTATUS_FS` and uses `fssr`/`fmv.w.x` |
| S-mode / MMU | **None.** The BSP runs entirely in M-mode; `crt.S` zeroes `mideleg`/`medeleg` and returns with `mret` into M | probe in 65.1 **(verify)** |
| PMP | **None.** No `pmpcfg`/`pmpaddr` exists anywhere in the SDK headers | probe: writes to `pmpcfg0`/`pmpaddr0` must read back 0 **(verify)** |
| Interrupts | Standard RISC-V **PLIC** (`0x0C000000`) + **CLINT** (`0x02000000`: `msip` per hart, `mtimecmp` per hart, shared `mtime`) | `plic.h`, `clint.h` |
| Cycle counter | `rdcycle`/`mcycle` availability unknown | **(verify)** in 65.1 |

Consequences, stated up front:

* **This is the first LugalOS target with no memory protection at all.**
  Every existing target enforces domains — RP2350 (8-entry PMP), ESP32-P4/C6
  (16-entry PMP), rv64 (Sv39). On the K210, isolation is *advisory*: the
  kernel keeps its domain objects, locks and deputy checks (so the same code
  paths are exercised and a fault is still contained *cooperatively*), but
  nothing catches a wild store across a boundary. The node must say so
  honestly: `/proc/node` gains `domains: unenforced (silicon: no pmp)` —
  the same "tell the peer what the hardware cannot promise" principle as the
  phase-45 `tainted` flag, without implying unauditable code.
* **U-mode driver containment (the C6's radio trick, phase 45 §4.5) is
  impossible here.** There is no less-privileged mode to put a driver in.
* **64-bit M-mode is new.** The existing rv64 QEMU target runs S-mode under
  an MMU (`CONFIG_MODE_S=1`); the K210 bucket that already sits in
  `CMakeLists.txt` (`rv64gc_zicsr_zifencei`, `lp64d`, `CONFIG_MMU=1
  CONFIG_MODE_S=1`, `linker/qemu-rv64.ld`) is a stale placeholder with the
  wrong ISA *and* the wrong privilege model, and is rewritten in 65.3.
* **No D extension** means a double-precision instruction anywhere in the
  image is an illegal instruction. The kernel's zero-float rule already
  guarantees this; the build must not regress by inheriting the rv64 QEMU
  flags. We compile `-march=rv64imac_zicsr_zifencei -mabi=lp64` (integer
  registers only) so the *compiler itself* cannot emit float code —
  strictest-first, matching how RV32 targets ignore the ESP FPU. The K210's
  single-precision FPU stays off (`mstatus.FS = 0`); using it is a possible
  later, explicitly-flagged feature for user tasks, not for this phase.

### 1.2 Memory map (`platform.h`, `lds/kendryte.ld`)

| Region | Address | Size | Notes |
| :--- | :--- | :--- | :--- |
| CPU SRAM (cached) | `0x80000000` | 6 MB | alias `0x40000000` = uncached, same physical RAM |
| AI RAM (L3/LM) | `0x80600000` | 2 MB | dedicated to the KPU; usable as extra RAM when the KPU is idle **(verify coherency)** |
| AI / peripheral block | `0x40800000` | — | KPU (`0x40800000` … `0x40BFFFFF` per `kpu.h`) |
| FFT | `0x42000000` | 4 MB window | |
| ROM (128 KB) | `0x88000000` | 128 KB | BROM code |
| CLINT | `0x02000000` | — | `msip` `+0x0`, `mtimecmp` `+0x4000`, `mtime` `+0xBFF8` |
| PLIC | `0x0C000000` | — | standard RV PLIC |
| UARTHS | `0x38000000` | — | 8250-style "high speed" UART → console, via CH552 to USB |
| GPIOHS | `0x38001000` | — | 6 pins: onboard LED, button, JTAG pins |
| AXI/GDMA | `0x50000000` | — | DMAC |
| GPIO | `0x50200000` | — | 32 slow GPIOs |
| UART1–3 | `0x5021_0000`…`0x5023_0000` | — | 16550-style |
| SPI3 | `0x504C0000` | — | external SPI (SD card, displays) |
| I2C0–2 | `0x5028_0000`… | — | |
| FPIOA | `0x52000000` region (`0x502B0000`) | — | **pin mux: every function reaches every pin only through FPIOA** |
| SHA256 / AES | `0x502C0000` / `0x50460000` | — | hardware crypto, later |
| TIMERS 0–2 | `0x502D0000`… | — | PWM-capable |
| WDT0/1 | `0x50400000`/`0x50410000` | — | |
| OTP | `0x50420000` | 4 KB | 1 kbit eFuse-style per page; MAC/serial usage TBD |
| SPI0 NOR ("SPIFF") | `0x58000000` region | — | **attached: GD25LQ64/128 (8 or 16 MB) per schematic rev 1.09** — JEDEC ID probe decides; **not XIP** (§1.4) |

There is **no PSRAM and no MMU**; everything LugalOS runs from — code,
rodata, heap, framebuffer, Lisp pools — comes out of 6 MB of flat SRAM,
which by this tree's standards is enormous (the C6 port ships in ≈ 500 KB).

### 1.3 The Maix Bit board (spec sheet v2.0 + schematic rev 1.09)

| Item | Detail |
| :--- | :--- |
| Download circuit | Onboard **CH552T** USB↔UART (`1a86:7523`), up to 1.5 Mbaud, drives K210 reset/boot lines (one-click ISP, like an ESP toolchain). The currently attached board appears as `usb-1a86_USB2.0-Serial-if00-port0`. |
| SPI NOR flash | `GD25LQ64` or `GD25LQ128` (8/16 MB) on SPI0 |
| MicroSD slot | SD 1-bit (usable in SPI mode as first transport) |
| RGB LED | driven from GPIOs (nets `LED_R/G/B` ↔ `IO_12/13/14`, 4K7 series) — direct-driven tri-color, **not** WS2812 **(confirm against the physical board)** |
| User button | `USR` button + `RST` button |
| MEMS microphone | MSM261S4030H0, PDM/I2S (`MIC_BCK`, `MIC_WS`, `MIC_DAT3`) |
| DVP camera connector | 24P FPC, OV2640/OV7725 class (`DVP_*`, SCCB on 2K pullups) |
| LCD connector | 24P FPC "8-bit MCU LCD" (parallel 8080 bus: `LCD_CS/DC/WR/RST/D0–D7`) — **the Maix Bit itself has no panel**; a panel is an optional attachment |
| Light sensor | photoresistor on an analog input |
| All GPIOs | on 2.54 mm headers (breadboard-compatible) |

There is **no Ethernet, no Wi-Fi, no Bluetooth** on the board. As a
distributed node it reaches the rest of the system over **9P on the serial
link** (§5) and over SD-card storage; a C6-Zero companion over UART1/2 as a
Wi-Fi modem is a natural phase-45-protocol consumer but out of scope here.

### 1.4 Boot and flash model (the other big difference)

The K210 BROM **loads an image into SRAM and jumps there — there is no XIP**
(execution from SPI flash), unlike RP2350/P4/C6:

* Boot modes: ISP (serial, chosen by the CH552 boot lines), SD-card
  (`sdboot` needs the vendor stage-2 loader chain — we avoid it), and
  BROM-from-SPI-NOR at offset 0.
* The ISP/boot image format ("kfpad"/`.img`, magic `0x90000046`) is a flash
  header + section table + **LZ4-compressed sections** loaded to absolute
  SRAM addresses. Our images link flat at `0x80000000`; the host tool
  produces the header and does (or omits) the compression.
* Reference implementations to consult for the ISP protocol (sync/boot
  handshake at 115200 then switch to 1.5 Mbaud, per the vendor spec): the
  Sipeed `ktool`/`kflash` ecosystem. We implement our own
  `tools/k210run.py` / `tools/k210flash.py` (same stance as `c6run.py`: the
  only sanctioned way to touch the port, DTR/RTS boot-line handling
  included).
* **Fast dev loop:** `k210run.py` ISP-loads the ELF sections into SRAM and
  runs without writing flash — the exact `load-ram` loop the C6 port used,
  and the only flash write needed is when we choose to make the board
  standalone.
* **Flash-preservation policy** (mirrors `p9sync.py`): the vendor demo
  (MaixPy) occupying flash will be overwritten when 65.5 first writes a
  layout; no backup is required (same stance as the C6 demo, phase 45 §4.1).

---

## 2. Decisions

| # | Question | Decision |
|---|---|---|
| D1 | Privilege model | **M-mode-only kernel, cooperative domains** (the hardware offers nothing else). Probe first (65.1); if the probe contradicts the SDK and finds PMP/U-mode, that only *adds* enforcement, the design does not change. |
| D2 | ABI | `rv64imac_zicsr_zifencei` / `lp64`, integer-only registers, `mstatus.FS = 0` held by policy. F/D float is not used anywhere in the kernel (AGENTS.md zero-float rule); no `lp64f` anywhere in the image. |
| D3 | Feature honesty | `/proc/node`, boot banner and the 9P `attach` welcome report `domains: unenforced (silicon: no pmp)`. Peer policy (future auth work) can weigh it, exactly like `tainted`. |
| D4 | SMP | **Core 0 first, second core deferred** (65.9, optional). Single-core first boot mirrors every previous target; the vendor BSP itself is single-core and the core1 release path is under-documented (probe from the TRM + BROM behaviour when we get there; CLINT `msip` gives us IPIs for free). |
| D5 | Console | UARTHS through the CH552 on the single USB-C cable — same connector for power, console, and ISP flashing. Polled first (as the C6's), interrupt-driven later if anything needs the CPU back. |
| D6 | Network identity | The K210 node joins the distributed system over **9P/serial** (the RP2350's UART SLIP/demux link, which the C6 stubbed out, is revived on the K210); Wi-Fi via a C6 companion is *future work*, not this phase. |
| D7 | QEMU | There is **no K210 machine in QEMU** (only `k230`). All pure-function code is host-tested (ASan/UBSan harnesses) per the phase-26 rule; chip-specific code is tested by `tests/hw/test_k210.py`, which skips cleanly when the board is absent. |
| D8 | KPU / AI | Out of scope. The KPU + nncase story gets its own phase if it happens; this phase only reserves the AI-RAM region in the layout (§1.2) so it can be carved out later without a re-layout. |
| D9 | Ordering | Silhouette first (boot → shell → storage), personality second (camera/mic/LCD), SMP last. |

---

## 3. What makes this port *not* just another board file

1. **64-bit M-mode trap/switch entry.** `arch/riscv/common` has an M-mode
   path (RV32 targets) and an S-mode path (rv64 QEMU) but never 64-bit
   M-mode: `register_t`, trap frame, `switch.S`, satp/PMP assumptions in
   `mem_domain.c` and `umode.c` all need a look. `mem_domain.c` grows an
   "unenforced" mode that keeps the bookkeeping (so the deputy checks,
   ownership and racescan verdicts stay honest) and compiles the enforcement
   to `always-true` — loud in `/proc`, silent in cycles.
2. **PLIC + CLINT as our first "standard-platform" interrupt tree.** The
   PLIC driver is generic RV code (the same one rv32/rv64 QEMU want);
   `mtime` at 6 MB-sane rates is the tick. This is reusable: the P4/C6
   ports grew vendor trees (CLIC, matrix+PLIC_MX), here we finally get the
   textbook device and should structure it as `arch/riscv/k210/` +
   shareable `plic.c`/`clint.c`.
3. **Everything lives in 6 MB of SRAM, and flash is a disk.** The linker
   script is a flat SRAM layout (`linker/k210.ld`); the SPI NOR is a
   block device (`drivers/spi_nor_k210.c` + the existing `flashdisk.c`
   MBR/FS layering), not a place to execute from. The flash layout work
   follows `cmake/flash_layout.cmake` shapes: OS image (reflashable),
   config, identity sector (phase-21 record, `drivers/idstore_k210.c` —
   flash-backed like the C6's), data FS, `/sd0` on top of the real SD slot.
4. **FPIOA: the pin-mux driver is a prerequisite for everything GPIO-ish.**
   No RP2350-style "function is fixed per pin" — every signal (UART, SPI,
   SD, DVP, I2C, PWM, GPIO) must be routed through the FPIOA matrix, with
   power-domain (`PADO`/drive/voltage) setup. Small driver, but nothing
   else GPIO works before it, so it comes early.
5. **The ISP toolchain is a real host program.** Boot header, LZ4, CRC,
   baud switching, DTR/RTS boot-line choreography, and a console mux that
   never opens the port naively (AGENTS.md rule; the CH552 lines reset/boot
   the chip exactly like the C6's DTR trap). This is the phase's `c6run.py`
   and it gets the same "everything through the tool" discipline.

---

## 4. Kernel & arch work

* **`CMakeLists.txt`**: replace the stale `K210` bucket (D1/§1.1) with
  `LUGALOS_TARGET=K210`, `rv64imac_zicsr_zifencei`, `lp64`,
  `CONFIG_TARGET_RV64=1` (no `CONFIG_MMU`, no `CONFIG_MODE_S`),
  `linker/k210.ld`, new preset `k210-maixbit` + `cmake/board-k210-maixbit.cmake`
  + `cmake/toolchain-k210.cmake` (finds `riscv64-elf-gcc`, already installed
  and verified for `-march=rv64imafc -mabi=lp64f`, so `imac/lp64` trivially
  works) + `cmake/flash_layout_k210.cmake`.
* **`arch/riscv/k210/`**: `entry.S` (M-mode, clear `mie`, `FS=0`, zero BSS,
  per-hart stacks via `mhartid`, park hart 1 until 65.9), `clk_k210.c`
  (PLL0→CPU 400 MHz, 26 MHz XTAL, all `sysctl` clock trees for APB/UART/SPI),
  `clint.c`, `plic.c`, trap glue reusing `arch/riscv/common/trap.c` with a
  64-bit M-mode frame.
* **`drivers/uarths_k210.c`** (console; 8250-ish with the vendor's
   clock/divisor quirks), **`drivers/fpioa_k210.c`**, **`drivers/gpio_k210.c`**,
   **`drivers/spi3_k210.c`**, **`drivers/spi_nor_k210.c`**,
   **`drivers/sd_k210.c`** (SPI mode first, native SD mode later),
   **`drivers/wdt_k210.c`**, **`drivers/idstore_k210.c`**.
* **`kernel/time.c`**: the K210 bucket joins the CLINT-`mtime` family
   (the comment there already reserves a bucket for it); `mtime` runs at
   **XTAL/… — measure in 65.1** (it is *not* guaranteed to be the CPU
   frequency; the C6 port's lesson about trusting ROM timing applies).
* **Lisp/system sizing:** with 6 MB, the full profile runs — Lisp pools,
   `cas/` tree on flash, big heap (~2–3 MB), a QVGA-class framebuffer with
   room to spare. The `CONFIG_BOARD_K210` stub in `user/lisp/lisp.c`
   already returns `"k210"`.
* **racescan/lockcheck/runner integration**: new target joins
  `tests/runner.py` (`ninja -C build/k210-maixbit`), all host harnesses run;
  `tools/hw`-style suite `tests/hw/test_k210.py`.

---

## 5. Networking story (D6)

The K210 has no radio. It participates in the distributed system as a
**serial 9P node**:

* Revive/port the RP2350 UART SLIP + demux 9P link to UARTHS (and a real
  115200–921600 UART1/2 downlink option), `lugalos attach k210` via the
  host tools, additive `p9sync.py --k210` for `/sd0` staging over the SD
  card or the serial link.
* Phase-45's raw-frame modem protocol over the UART with a C6-Zero as the
  radio modem is the *documented* follow-up (it turns this board into a
  Wi-Fi node without touching the K210's silicon limits); deliberately not
  scheduled here.

---

## 6. Milestones

Each milestone ends with a repeatable verification and a commit subject
starting `65.N:`. No QEMU exists for this chip: pure logic is host-tested,
everything else is `tests/hw/test_k210.py`, which skips cleanly without the
board.

* **65.1 — Silicon truth probe.** Standalone C, no kernel, ISP-loaded
  (`tools/minimal_k210.{c,ld}` + entry.S + `tools/build_minimal_k210.sh`,
  same shape as the C6's 45.1/45.3a probes). Must measure and print:
  `misa`; whether `pmpcfg0`/`pmpaddr0` writes stick (expect: no PMP);
  whether U-mode is reachable (expect: not); LR/SC/AMO execution (expect:
  works despite the vendor march string — and if they *fault*, that is a
  major finding that changes §7); `mcycle` presence; BROM-default CPU
  frequency (cycle counter vs. `mtime`) and `mtime`'s frequency; JEDEC ID
  of the SPI NOR (8 vs 16 MB); PLIC/CLINT sanity (software interrupt via
  `msip` on hart 0). Verification: human-readable table in the milestone
  note; every "verify" in §1 resolved one way or the other **before kernel
  code is written**.

* **65.2 — Host tooling: `tools/k210run.py` / `tools/k210flash.py`.**
  BROM ISP protocol (boot handshake, LZ4 sections, CRC), port discovery by
  `/dev/serial/by-id` (`1a86:7523`), DTR/RTS boot-line choreography,
  115200→1.5 Mbaud switch, and a console watcher with the same
  never-open-naively rule as `c6run.py`. ELF→`.img` section extraction from
  the build. Verification: 65.1's probe loads and runs *through the tool*;
  `k210run.py --help` documented in `doc/admin/flashing.md`.

* **65.3 — First boot to `lsh` (single core).** Build-system bucket per §4,
  `linker/k210.ld`, entry.S, clock tree to 400 MHz, CLINT tick (100 Hz),
  PLIC skeleton, UARTHS console, FPIOA routes for the console pins, empty
  device table. Verification: boot banner + `lsh` prompt over the USB
  cable; `preempttest`, `lockselftest`, `priostress` from the existing
  suites pass; heartbeat stable across a minute (watchdog not yet armed —
  that is 65.7).

* **65.4 — The unenforced-domain mode.** `mem_domain.c` grows the D3 mode;
  every existing domain/U-mode self-test that *can* run without enforcement
  runs (deputy checks stay real, PMP-specific expectations compile out);
  `/proc/node` and the banner print `domains: unenforced (silicon: no pmp)`;
  `/proc/version` machine line. Verification: suite green on `k210-maixbit`;
  new host test that the "unenforced" verdict is *only* possible on K210 —
  no other target can silence its PMP by configuration.

* **65.5 — SPI NOR flash disk + flash layout.** `spi_nor_k210.c` (QE-bit
  handling for the GD25LQ family, quad mode optional), MBR +
  `flashdisk`/`flash_layout_k210.cmake` partitioning, OS-image header
  written by `k210flash.py` so the board **boots standalone** with no host
  attached. Verification: unplug the data cable (power only) — the board
  comes up to `lsh` from flash, verified with a serial log; reflashes
  cleanly 3× with `--verify`.

* **65.6 — LED, button, config + identity store.** RGB LED (verify the
  schematic's three-GPIO reading against the physical board; the Maix Bit
  kit's wiring is the source of truth), `USR` button with debounce through
  the existing gpio/input plumbing, `lugalos_config` in flash,
  `drivers/idstore_k210.c` (phase-21 record in a reserved NOR sector,
  same policy as the C6's idstore: never in `flash.manifest`, `identity
  provision` works and persists across power cycles and reflash). The RNG
  question is answered honestly: **no hardware RNG is documented for the
  general case** — probe what's available (the crypto blocks, timer-jitter
  fallback); if entropy is weak, `identity key --generate` refuses like the
  C6's RF rule, with a printed reason.

* **65.7 — Watchdog + unattended boot.** WDT0 armed once the scheduler
  runs (the C6's shape: a feeding task at normal priority, boot reports the
  reset cause); autostart (`/cfg/autorun`-style) works from flash.
  Verification: `wdt test` resets and recovers the board headless.

* **65.8 — SD card `/sd0` + serial 9P.** SPI3 SD in 1-bit SPI mode first
  (native SD mode is an optimization), FAT FS as `/sd0`, `p9sync.py
  --k210 --src build/k210-maixbit/sd_root` support (additive, idempotent),
  and the SLIP/demux 9P link on UARTHS: `lugalos attach` from the host,
  authenticated attach with the provisioned identity (the RP2350 serial
  story, ported). Verification: file round-trip both ways, `sd0` survives
  reboots, `tests/hw/test_k210.py` grows `test_sd`, `test_p9_serial`.

* **65.9 — Hardware test suite + persona pass.** `tests/hw/test_k210.py`
  collects everything from 65.3–65.8 (tick accuracy, PMP-absence verdict,
  NOR JEDEC/erase/write, SD, idstore, watchdog reset cause, 9P attach,
  memory report); `tests/runner.py` integration; board guide
  `doc/admin/boards/k210.md` (consoles, baud, flashing, layout — the
  esp32c6 guide's structure), `AGENTS.md` target matrix row
  (`k210-maixbit` preset, CH552 console 1.5 Mbaud after boot / 115200 ISP),
  `doc/admin/flashing.md` + `doc/admin/consoles.md` updates,
  `python3 tools/gendocs.py --all && --check` zero drift.

* **65.10 — Camera (DVP) and microphone (optional, gated on 65.9).** DVP
  capture into SRAM (OV2640/OV7725 over SCCB-via-GPIO or I2C), a
  `camshot`/`frames` shell story and a live-view on the parallel MCU-LCD
  connector if a panel is attached; PDM mic → WAV-on-SD (`mic` command
  with level meter). The 6 MB budget makes frame double-buffering trivial;
  the interesting work is DVP DMA + FPIOA routing and the sensor's SCCB
  init sequences. AI inference on the KPU explicitly **not** here (D8).

* **65.11 — Second core / SMP (optional).** Find the core1 release path
  (BROM state of hart 1, any sysctl remap/reset bits — 65.1 leaves hart 1
  parked and reports what it sees), run the existing SMP suite
  (`rv64-smp`/`rp2350-smp` precedent) with CLINT `msip` IPIs. The 64-bit
  SMP lock hand-off review (phase 63) applies. If the release path cannot
  be established reliably, this milestone is documented closed with a
  single-core verdict and the parking code stays.

---

## 7. Risks and their counters

| Risk | Why it's real | Counter |
| :--- | :--- | :--- |
| **No atomics on the core** despite the SDK using them | Vendor `-march` lacks `a`; a broken `__sync` fallback would silently be single-core-critical-section code | 65.1 executes `lr/sc/amoadd` before anything else; if absent, SMP is dead-on-arrival and even `spin_lock_irqsave` audit needs review — this can only be discovered first, not mid-phase |
| No PMP ⇒ a kernel bug corrupts everything | Silicon fact | cooperative domains + every isolation *logic* still tested on rv32/rv64/rp2350; the taint-style honest report (D3); nothing new is trusted, only enforcement is lost |
| ISP/flash protocol reverse-engineering eats the phase | BROM protocol is only documented via vendor tools | 65.2 exists alone, with the 65.1 probe as its test object; worst case, SD-card boot with the vendor stage2 chain is the documented fallback path |
| `mtime`/clock assumptions | C6 lesson: ROM "delays" lie (phase 45 §4.5) | 65.1 measures `mtime` vs cycle counter against wall time before the scheduler exists |
| Dual-alias RAM (`0x4000…` vs `0x8000…`) coherency foot-guns | DMA (DMAC/SPI/DVP) sees one alias, cache the other | one alias is the *kernel's* (cached, `0x800…`); DMA buffers are placed in the uncached window by a linker region + helper, mirroring the P4's cache-maintenance discipline but simpler; documented in the board guide |
| Vendor `rv64imafc` march ≠ our `rv64imac` with the toolchain's libc headers | none — we are `-nostdlib -ffreestanding` like every target | verified in 65.3 (the AGENTS.md freestanding rule already covers it) |
| Camera/LCD kit variance (panels ship and disappear) | Maix Bit kits differ | 65.10 is optional and panel-specific findings live in the board guide, not in core code |

---

## 8. Explicitly out of scope

* KPU/nncase neural-network inference (its own phase if ever; nncase is
  modern-Python-host tooling and a big surface).
* Wi-Fi via C6 companion (phase 45 protocol; a cable and a board away).
* Native SD-bus mode (SPI mode ships), MIPI-style QSPI upgrades, DVP
  autofocus camera features, FFT/SHA/AES hardware blocks (SHA-256 in
  hardware is a candidate later win for `idstore`/9P auth cost).
* MicroPython/MaixPy binary compatibility, Arduino compatibility layer.
