# Phase 45 — The C6: a LugalOS node with a radio, and a tainted kernel

**Status: planned, not started. Written 2026-10-05**, after a feasibility
discussion (decisions in §1) and a read of the ESP-IDF 6.x tree under
`~/Source/gith/esp/esp-idf`. Nothing here has been run on silicon yet; every
"should" below is a reading, and §8 lists what the first milestones exist to
turn into measurements.

**Milestone scheme: `45.1`, `45.2`, …** (as in phase 34).

**Scope, in the order it will be built.**

1. **A stand-alone C6 node** (Waveshare ESP32-C6-Zero): boots LugalOS, drives
   its WS2812, joins WLAN (45.1–45.9).
2. **A sensor node**: BME280 on I2C, MQTT, the phase-26 sensor persona
   (45.10–45.11).
3. **A modem protocol** (our own raw-frame protocol) carried by whatever
   physical channel a host has (45.12–45.13).
4. **The ESP32-P4-NANO**: LugalOS on the P4 *and* on its C6-MINI, with the P4
   reaching the network through the C6 over SDIO (45.14–45.16).
5. **The C6-Zero as a Wi-Fi add-on for RP2350-terminal** boards (45.17).

Steps 3–5 reuse step 1's Wi-Fi core and are deliberately not designed in
detail until step 1 has worked; §7 records the constraints now so step 1 does
not foreclose them.

## 1. Decisions taken (2026-10-05)

| # | Question | Decision |
|---|---|---|
| D1 | May the Espressif Wi-Fi/PHY binary run in-process on the C6, M-mode, under LugalOS? | **Yes — and the kernel then reports itself as `tainted`** (§2). |
| D2 | P4 ↔ C6 data path | **Raw Ethernet frames** over the available channel (SDIO on the P4-NANO). 9P networking (`/net`-style export) is a later layer on top, not part of this phase. |
| D3 | ESP-Hosted compatibility | **No.** Our own frame protocol (§6). Hosted's only advantages (ready host drivers, tracking new chips) do not outweigh importing its stack and protobuf control path. ESP-Hosted remains a documented plan B only if the in-process blob proves unworkable (§9). |
| D4 | Order | Stand-alone sensor node first; expand from there. |

## 2. The `tainted` concept

A LugalOS node that links a binary it cannot audit says so, the way Linux
does with `/proc/sys/kernel/tainted`.

* A new build-time flag `CONFIG_TAINT_BLOB` (set by the `esp32c6` presets
  that link the Wi-Fi libraries; clear everywhere else, including every
  existing preset).
* `/proc/version` and the boot banner append `tainted: blob(espressif-wifi)`;
  `/proc/node` gains a `taint` line. Machine-readable so a gateway, MQTT
  announce, or a 9P client can see it without parsing prose.
* **Intent of the label:** a node without it has no unauditable code in its
  M-mode address space. A tainted node may still be *useful*, but a peer is
  entitled to weigh that — e.g. a future auth policy may refuse to hand key
  material to a tainted node. Phase 21's identity work is the place that
  would consume it; this phase only produces the flag.
* **Containment (best effort, not a security claim):** the blob shares our
  address space and runs in M-mode. PMP (16 regions) can at least keep the
  blob's *data* non-executable and our kernel's data out of its reach only if
  the blob is run in U-mode, which it was not written for. 45.3 decides
  whether the shim runs the radio tasks in U-mode with PMP windows; if it
  cannot be done cheaply the taint label stays the honest statement and the
  isolation is explicitly *not* claimed.
* The Ethernet-only P4 builds and all RP2350 builds stay untainted. On the
  P4-NANO the C6 *die* is tainted while the P4 is not (§7) — taint is a
  property of a node, and each die is its own node.

## 3. What the chip is, and what ports for free

From the datasheet and the P4 precedent (phases 27/34):

* **RV32IMAC, 160 MHz, 512 KB HP SRAM, 320 KB ROM, 16 KB LP SRAM, no FPU, no
  PSRAM, external SPI flash executed in place through the cache.** `A` is
  present, so `arch/riscv/include/arch/atomic.h` needs no change; no FPU
  matches the "FS = 0" rule for free. The installed `riscv64-elf-gcc` with
  `-march=rv32imac_zicsr_zifencei -mabi=ilp32` is expected to be enough
  (verify in 45.1; no IDF toolchain).
* **PMP/PMA, 16 regions; M and U modes.** Same privilege model as the P4.
* **Interrupts: the C6 has its own interrupt matrix and controller, not the
  P4's CLIC.** First real driver work; see 45.4.
* **Console: native USB Serial/JTAG on the C6-Zero's USB-C port**, plus UART0.
  No CP2102/CH34x bridge, so the P4's "RTS holds the chip in reset" hazard
  does not exist here — but `tools/` needs a `c6run.py` / `c6flash.py` that
  speaks to the USB-JTAG CDC, and the AGENTS.md rule (never open the port
  with a naive terminal tool) stays until proven unnecessary.
* **WS2812 on GPIO8**, driven by the RMT peripheral in the Waveshare
  example. Bit-banging is possible at 160 MHz but the timing is tight with
  interrupts enabled; RMT is the plan.
* **Flash:** a C6-Zero typically has 4 MB. Layout work follows
  `cmake/flash_layout_esp32p4.cmake`; the ROM bootloader loads a
  second-stage image the way it did for the P4 (45.1).

## 4. The one hard thing: the Wi-Fi blob

### 4.1 What is shipped (measured 2026-10-05, `size` on the archives)

| Library | text | data | bss | Source |
|---|---|---|---|---|
| `libnet80211.a` | 307 KB | 1.7 KB | 11 KB | `esp_wifi/lib/esp32c6/` |
| `libpp.a` | 185 KB | 4.3 KB | 3.5 KB | same |
| `libphy.a` | 45 KB | 0.4 KB | — | `esp_phy/lib/esp32c6/` |
| `libcoexist.a` | 11 KB | 0.8 KB | — | `esp_coex/lib/esp32c6/` |
| (`libmesh`, `libespnow`, `libsmartconfig`, `libwapi`) | not needed | | | |

Unlinked archive totals (~550 KB text); `--gc-sections` will shrink it, and
it runs XIP from flash, so the open questions are *RAM at run time* (static
plus the RX/TX buffer pools the blob allocates) and the IRAM-resident hot
paths IDF pins with `linker.lf`. 45.2 measures exactly that, before any
other Wi-Fi work.

### 4.2 The boundary is narrow and mostly open

* **The blob talks to its host through one table of function pointers**
  (`wifi_osi_funcs_t`, ~128 entries in `esp_private/wifi_os_adapter.h`):
  mutexes, semaphores, queues, event groups, task create/delete, timers,
  interrupt allocate/enable, `malloc`/`free`, time. IDF's own glue
  (`components/esp_wifi/esp32c6/esp_adapter.c`, 759 lines, Apache 2.0) fills it
  from FreeRTOS. **Our job is the same file, written for LugalOS primitives.**
* **The blob also calls chip ROM** (`pp`, `net80211`, `phy`, `coexist` ROM
  function tables). Resolved with `esp32c6.rom.*.ld` — open, shipped in
  `esp_rom/esp32c6/ld/`, and a property of the silicon.
* **WPA handshake lives outside the blob**, in IDF's `wpa_supplicant`
  component (open, `esp_supplicant/`), which wants SHA-1/PBKDF2/AES-CCMP
  primitives. Taking IDF's supplicant + a small crypto subset (or mbedtls
  pieces) is the largest unknown after the shim itself. **WPA2-PSK only**; no
  WPA3/SAE, no enterprise, no WPS (§8).
* **PHY calibration** needs init data (`esp_phy`), a country/regulatory
  setting, and NVS-like storage for calibration results. We need only
  "calibrate every boot" for a first pass; storing the result is an
  optimisation (45.9).

### 4.3 The shim, concretely (45.3)

LugalOS has no FreeRTOS. The shim must provide, with the *blocking and
ISR-context* semantics the blob expects:

| Blob wants | LugalOS primitive to map onto | Risk |
|---|---|---|
| mutex / recursive mutex | kernel lock *within the phase-31 wait-for graph rules* | Deadlock graph assumptions: the blob takes locks from its own task and from ISR-deferred paths. |
| binary/counting semaphores, `…FromISR` | kernel semaphores | ISR-safe variants must not allocate. |
| queues | a small ring queue with wake-up | Item-by-copy semantics. |
| task create (the blob creates ~2–4 tasks: `pp`, timer, event) | `task_create` with the stack sizes IDF gives | Stack size budget against 512 KB. |
| timers | one shim timer service on the existing tick | ms resolution is sufficient. |
| interrupt alloc/enable | 45.4's C6 interrupt driver | The blob installs its own MAC/PHY/BB interrupt handlers by *source number*. |
| `malloc`/`free`/`calloc` | `palloc`/heap | The blob allocates big, aligned, DMA-capable buffers. |

Everything above lives in `drivers/esp32c6_wifi_osal.c` (one file, mirroring
IDF's adapter) and is **not** compiled into any untainted build.

## 5. Milestones

Each milestone ends with a verification that can be repeated and a commit
subject starting `45.N:`. Hardware milestones have a QEMU-less verification
(there is no C6 in QEMU); pure-function parts are still built for every
target and tested in QEMU, per the phase-26 rule *nothing should be debugged
by flashing*.

### Stand-alone node

* **45.1 Boot and a heartbeat.** `cmake/board-esp32c6-zero.cmake`,
  `cmake/toolchain-esp32c6.cmake` (a copy of the P4 one with `-march=rv32imac`),
  preset `esp32c6`, linker script, ROM-bootloader image format
  (reuse or extend `tools/p4flash.py`'s image tooling; `esptool` is at
  `~/Source/gith/esp/esptool/`), `tools/c6flash.py`, `tools/c6run.py`.
  Verifies: `ninja -C build/esp32c6` builds; flashed board prints a banner
  and `misa` over USB-JTAG. *Not* yet any kernel service.
* **45.2 Spike: what does the blob cost?** Link `libnet80211`, `libpp`,
  `libphy`, `libcoexist` against a stub OSAL (every entry traps with its
  name), `--gc-sections`, map the result. Report: text/data/bss, how many OSAL
  entries are *actually reachable* from `esp_wifi_init → start → scan →
  connect`, and the RAM the buffer pools want. **Go/no-go point for §9.**
* **45.3 The OSAL shim.** The table in §4.3, with a host-side unit test of the
  queue/semaphore semantics (pure C, built for every target). Decides
  U-mode-versus-taint-only for the radio tasks (§2).
* **45.4 C6 kernel bring-up.** Interrupt matrix and controller, trap
  vector, the system tick, PMP setup, the kernel's scheduler, the VFS and
  the existing `lsh` console on USB-JTAG. This is the "LugalOS runs on the C6"
  milestone for the untainted core. Reuse `arch/riscv/` the way phase 27
  reused it for the P4.
* **45.5 LED.** WS2812 on GPIO8 via RMT; a `/dev/led` (or `/proc`-style)
  node and a shell command; `(led r g b)` in Lisp. First useful thing that
  works with no radio.
* **45.6 Wi-Fi bring-up: PHY + `esp_wifi_init`.** Calibration, `wifi_init`,
  `start`; `wifi scan` lists access points. First time the blob runs; every
  failure here is a shim or interrupt bug and is debugged with the 45.2 trace.
* **45.7 Associate (WPA2-PSK) and DHCP.** Supplicant integration, then the
  existing phase-19 stack with a new `netif` over the blob's `esp_wifi_internal_tx`
  / rx callback (the CYW43 `netif` in `drivers/cyw43_rp2350.c` is the
  template). Verifies: ping both ways, 9P over TCP/564 on the board, the
  existing `tests/hw` network checks adapted.
* **45.8 Unattended operation.** Credentials stored like the identity record,
  rejoin after AP loss (phase-26's R5 behaviour), watchdog.
* **45.9 Taint flag, `/proc/node`, tests.** §2 delivered (the flag could
  start earlier; it is listed here so it is verified). `tests/hw/test_esp32c6.py`
  written to the same skip-if-absent pattern as the P4 suite.

### Sensor node

* **45.10 I2C on the C6 + BME280.** C6 I2C driver under the existing shared
  `i2c` task API; reuse `drivers/bme280.c` unchanged (its compensation is pure
  fixed-point).
* **45.11 `esp32c6-sensor` persona.** The phase-26 persona (join WLAN, MQTT
  announce, periodic publish, will) on this board. Soak run. **End of the
  stand-alone scope: scenarios 1 and 2 are done here.**

### Expansion (planned only far enough to avoid foreclosing it — §7)

* **45.12 Modem protocol spec** (§6) and a host-side reference (Python) so it
  is testable without a second chip.
* **45.13 C6 as a modem:** the C6 persona that bridges the Wi-Fi `netif` to
  the modem channel (UART first: needs only the C6-Zero and a USB-UART or the
  RP2350's UART1).
* **45.14 P4: SDIO *function* host driver.** Different from
  `sdmmc_esp32p4.c`, which drives memory cards: IO_RW_DIRECT/EXTENDED
  (CMD52/53), function enumeration, card interrupt, reset via GPIO54.
* **45.15 C6: SDIO slave driver**, and the modem protocol over it.
* **45.16 P4-NANO networked through its own C6.** LugalOS on both dies,
  P4 `netif` over SDIO; the P4 stays untainted, the C6 is tainted (§2).
* **45.17 C6-Zero as an add-on for RP2350-terminal** over the serial link
  (UART1 downlink already exists, `uart1_link_rp2350.c`); bandwidth is the
  thing to measure first.

## 6. The modem protocol (sketch — to be specified in 45.12)

Not a design yet, but the properties that §7's constraints demand:

* **Channel-agnostic:** one framing for UART/SPI/SDIO. A frame is
  `type, length, payload`; types: `DATA` (an Ethernet frame), `CTRL` (scan,
  join, status — a few fixed commands, *not* protobuf), `EVENT` (link up/down,
  scan results).
* **Raw frames first (D2).** 9P and `/net`-style export come later *over the
  same channel* as another frame type or as a normal TCP session through the
  C6's IP stack.
* **Flow control and a reset handshake** designed together; SDIO has a
  hardware doorbell, UART needs credits.
* **Versioned**, since the P4-side firmware and the C6-side firmware are
  separate builds on one board and will not always be flashed together
  (the compatibility hazard the vendor doc warns about for Hosted applies to
  us in miniature).

## 7. Constraints carried from the expansion scenarios into step 1

1. **Keep the Wi-Fi core (OSAL + blob glue + supplicant) independent of the
   persona.** Standalone C6 = Wi-Fi core + IP stack + sensor persona;
   modem C6 = Wi-Fi core + modem bridge. Same library, no `#ifdef` forest.
2. **The `netif` boundary stays exactly the phase-19 one,** so the P4's
   future `netif` over SDIO is "another driver", not a change to the stack.
3. **P4-NANO wiring is fixed:** C6-MINI SDIO on C6 IO18–23 ↔ P4 GPIO14–19,
   reset on GPIO54, C6 UART0 on header P2 only (phase 27 §7). The
   C6-MINI's boot and flash path from the *P4's* side needs a plan (45.16):
   either the C6 image is loaded over its UART/USB pads on P2, or the P4
   flashes it through its download-mode pins. This is **not** discovered yet.
4. **The C6-MINI's factory (ESP-Hosted) firmware is expendable** (decision
   2026-10-05): no backup is required before overwriting it, and returning the
   board to ESP-Hosted is not a goal of this phase.

## 8. Explicitly not in this phase

* **BLE / 802.15.4 / Thread / Zigbee / Matter** — the C6's other radios.
  They need the same kind of blob and their own coexistence handling.
* **WPA3/SAE, WPA2-Enterprise, WPS, mesh, ESP-NOW, SmartConfig, SoftAP** — a
  later phase if wanted; STA with WPA2-PSK is the target.
* **Low-power modes, the LP core, light/deep sleep, TWT.** A battery-powered
  sensor wants them; this phase's sensor runs from USB power, like phase 26.
* **TLS**, as in phase 26.
* **9P networking / `/net`** — a later layer over the raw-frame channel (D2).
* **OTA** — wanted, not scoped. Flash layout (45.1) must leave room for a
  second slot so that adding it later is not a re-layout.

## 9. Risks and the fallback

| Risk | Why | What we do |
|---|---|---|
| **Shim bugs are timing bugs** | The blob assumes FreeRTOS scheduling and ISR semantics | 45.2 enumerates what's reachable; 45.3 tests the primitives in isolation on the host before any flash |
| **RAM** | 512 KB total, with the kernel, stacks, the IP stack and the blob's buffers | 45.2 measures *before* 45.3 starts; pool sizes are a config knob |
| **WPA supplicant + crypto** | Open but large; not yet inspected | Inspect in 45.2; budget as its own milestone if it needs one |
| **Blob/ROM version drift** | The blob in `~/Source/gith/esp/esp-idf` must match the ROM function-table version | Record the IDF commit and the ROM version register in 45.2; pin it |
| **Debugging on a chip with no QEMU** | | USB-JTAG console, `tools/c6run.py` log capture, the 45.2 trap-by-name stubs |
| **Plan B** | If 45.2 or 45.6 shows the in-process route is unworkable | Stop and revisit with ESP-Hosted's open slave firmware (built with IDF) on the C6, LugalOS on the host side only. This loses the stand-alone C6 node. Do not start plan B without asking. |

## 10. Open items to resolve while executing

* Which memory ranges the blob must live in (IRAM-pinned hot paths per IDF's
  `linker.lf`) — answered by 45.2's link map.
* WS2812 timing with interrupts enabled, RMT versus a tight loop — 45.5.
* U-mode radio tasks with PMP windows, or taint-only — 45.3.
* Which serial channel to use for the first modem (45.13) — likely UART via
  the RP2350's UART1, to be settled once the RP2350-terminal side is read.

## 11. References

* C6 datasheet and technical reference manual: `~/Source/gith/esp/datasheet/`
* IDF Wi-Fi libs and glue: `esp-idf/components/esp_wifi/`, `esp_phy/`,
  `esp_coex/`, `wpa_supplicant/`, ROM scripts in `esp_rom/esp32c6/ld/`
* Waveshare C6-Zero examples (RMT WS2812, Wi-Fi, I2C):
  `~/Source/gith/esp/ESP32-C6-Zero-Basic-Kit/`
* P4-NANO wiring and the C6 co-processor: `plan/phase27_esp32p4_bringup.md` §1, §7
* The P4's Hosted-Wi-Fi lane (the thing we are *not* doing): `~/Source/gith/esp/ESP32-P4-Platform/docs/P4_C6_HOSTED_WIFI.md`
* The CYW43 `netif` as the template: `drivers/cyw43_rp2350.c`
* Sensor persona: `plan/phase26_mqtt_and_environment_sensors.md`
