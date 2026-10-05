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
it runs XIP from flash. §4.4 has the *linked* numbers from 45.2 (the table
above is the raw archives).

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

### 4.4 What the spike measured (45.2, 2026-10-05)

Linked with `-Wl,--gc-sections` from the roots a scan/join/pass-frames station
needs, against the chip's ROM linker scripts, with no OS and no IDF code
(IDF `~/Source/gith/esp/esp-idf`, ESP32-C6 libraries as shipped there):

| What survives | text | rodata | data | bss |
|---|---|---|---|---|
| `libnet80211` | 178 KB | 47 KB | 1.6 KB | 10.9 KB |
| `libpp` | 102 KB | 10 KB | 2.7 KB | 2.9 KB |
| `libphy` | 28.6 KB | 0.6 KB | 0.4 KB | — |
| `libcoexist` | 5.5 KB | 1.3 KB | 0.1 KB | — |
| `libcore` | 0.4 KB | — | — | — |
| **blob total** | **315 KB** | **59 KB** | **4.8 KB** | **13.8 KB** |

* **Flash ≈ 374 KB, static RAM ≈ 19 KB** for the blob itself (mesh, ESP-NOW,
  SmartConfig, WAPI and SoftAP code drop out). It runs from flash through the
  cache; the blob marks 47.6 KB of its code as IRAM-worthy
  (`.wifi0iram` 21.8 KB, `.wifislprxiram` 11.5 KB, `.wifiextrairam` 6.5 KB,
  `.wifirxiram` 4.3 KB, `.wifislpiram` 3.6 KB), which IDF places in IRAM only
  with `CONFIG_ESP_WIFI_{IRAM,RX_IRAM,SLP_IRAM}_OPT` on. **Whether the radio
  interrupt path tolerates flash-cache misses is a 45.6 experiment;** the
  fallback costs ≈ 22–48 KB of SRAM, which the budget absorbs.
* **The link has exactly 46 unresolved symbols, and every one is trivial or
  open source:** libc (`memcpy`, `strcpy`, `sprintf`, `floor`, …), the four
  `*_printf` hooks (`esp_wifi/src/lib_printf.c`), the regulatory tables
  (`esp_wifi/regulatory/`), 24 FTM calibration tables
  (`esp_wifi/src/ftm_load_calibration.c` — unused, stubbed), `WIFI_EVENT`,
  `hexstr2bin`, `rtc_clk_xtal_freq_get`, `g_espnow_user_oui`,
  `mesh_sta_auth_expire_time`. **There is no hidden symbol dependency.** All
  other traffic goes through the function-pointer table and callback
  registrations (`esp_wifi_register_wpa_cb_internal` …) at run time.
* **The blob also needs 304 chip-ROM symbols** (`pp*`, `lmac*`, `rc*`, `phy`
  calibration, `pwr_hal_*`, `coex_core_*`, and soft-float libgcc). The ROM's
  own radio *data* lives at the very top of SRAM (`0x4087fce8–0x4087ffff`,
  e.g. `g_osi_funcs_p = 0x4087ff6c`) — **that range is not ours**, and the
  ROM boot stack at `0x4087e610` is dead only after boot. This pins the ROM
  version: the demo's boot log reports `pp/net80211/coexist rom version
  5b8dcfa`; record it as the version the blob was built against.
* **The blob does soft-float arithmetic** (`__adddf3`, `__muldf3`, `__divsf3`,
  `__floatsidf`, … from ROM libgcc; plus `floor`). That is integer code on a
  core with no FPU, so the AGENTS.md "no float in M-mode" rule is not violated
  here — but it is code that *does* arithmetic on floats, and `floor` must
  come from us. Noted rather than hidden.
* **The OS table (`wifi_osi_funcs_t`) has 127 fields on this chip; the
  reachable code calls at least 100 of them** (static lower bound — a
  straight-line reading of the disassembly through the table pointer):
  interrupts/spinlocks 11 of 14, semaphores/mutexes 10 of 10, tasks 5 of 7,
  timers/time 8 of 8, memory 8 of 11, queues/event groups 5 of 15 (so most of
  the queue and event-group API is IDF's own, not the blob's), NVS 10 of 12,
  coexistence 20 of 23, sleep/PM/regdma 6 of 8, logging 3, random 3, and the
  PHY/clock/MAC/event group 11 of 11. The 27 not seen are queue/event-group
  variants, `_task_create`, `_realloc/_calloc`, the `coex_init/deinit/
  condition_set` trio and the two regdma entries.
* **The table is ABI-checked.** The blob exports
  `esp_wifi_internal_osi_funcs_md5_check` and siblings (crypto functions,
  supplicant header, wifi types): IDF's `wifi_init.c` compares them against
  the headers it was built with. So our shim must be built against **IDF's
  own headers, from the same IDF tree as the libraries**, not a transcription.
* **Interrupts: the C6 has a PLIC (32 CPU interrupt lines), not a CLIC**
  (`SOC_INT_PLIC_SUPPORTED`; `SOC_CPU_CORES_NUM = 1`). The blob asks for four
  sources through the interrupt matrix — `WIFI_MAC`, `WIFI_PWR`, `WIFI_BB`,
  `COEX` — via `_set_intr/_set_isr`. 45.4's interrupt driver has to route
  these.
* **Tasks:** the blob creates one task itself (`pp`, priority 23, stack
  ≈ 6.6 KB in the demo log). The supplicant's `wpa2` task and IDF's event loop
  are IDF's, not the blob's: ours can be inline callbacks.
* **The open code around the blob is ≈ 5 000 lines** that must be ported
  rather than called: `esp_phy/src/phy_init.c` (1 300; calibration, the
  `register_chipv7_phy` call), `phy_common.c` (400), `esp_hw_support`
  `pmu_init.c`/`pmu_param.c` (720; the modem power domain and regulator
  setup), `modem/modem_clock.c` (340; modem clock gating), `rtc_clk*.c`,
  `esp_wifi/src/wifi_init.c` (800), `esp_coex/src/coexist.c` (300). **This —
  not the blob — is the real size of 45.6**, and its one non-obvious part is
  that a chip started from ROM (our `load-ram` path, and later our own
  second stage) has *not* had IDF's `pmu_init()`; the radio's power and
  clock state is ours to establish.
* **The open half of WPA2-PSK is small.** Station-only, internal crypto (no
  mbedtls), no SAE/EAP/WPS: `tools/c6_blob_spike/supplicant.sh` compiles the
  wpa_supplicant files that matter (`wpa.c`, `wpa_ie.c`, `wpa_common.c`,
  `pmksa_cache.c`, SHA-1/256/MD5, AES, CCMP, the ESP glue) to **≈ 46 KB text,
  1.5 KB bss**. The blob reaches the crypto through an 11-entry table
  (`wpa_crypto_funcs_t`: HMAC-SHA256, PBKDF2-SHA1, AES-128 enc/dec, OMAC1,
  CCMP enc/dec, GMAC, SHA-256, AES wrap/unwrap) — all standard algorithms,
  several of which the tree already has (`kernel/sha256.c`). This compiles
  against IDF headers with stubbed FreeRTOS and `sdkconfig.h`; it has *not*
  been linked or run (45.7). The earlier worry that the supplicant would be
  "the largest unknown after the shim" is retired: it is one of the smaller
  ones.
* **RAM at run time is estimated, not measured.** Static: blob ≈ 19 KB + its
  tasks' stacks (≈ 7 KB) + supplicant ≈ 2 KB. Heap: IDF's defaults allocate
  10 static RX buffers of 1 700 B and allow 32 dynamic RX and 32 dynamic TX
  buffers (≈ 1.6 KB each, only while in flight); minimal settings are a
  fraction of that. A rough working figure is **60–120 KB of the 512 KB** for
  the radio, leaving ≈ 300 KB for the kernel, the IP stack and the persona.
  For scale: the Waveshare demo (Wi-Fi + BLE + FreeRTOS + lwIP) boots with
  317 KB of heap free, i.e. ≈ 184 KB static. **Measure in 45.6.**

**Verdict: GO.** Every symbol the blob needs is accounted for, the cost is
within budget with margin, the boundary is one ABI-checked table, and the
supplicant is small. What the spike could *not* show, and 45.3–45.6 must: that
the blob runs under our scheduler's blocking and interrupt semantics, that the
radio's power and clock domains can be brought up without IDF's startup, and
whether the radio ISR path is happy executing from flash.

## 5. Milestones

Each milestone ends with a verification that can be repeated and a commit
subject starting `45.N:`. Hardware milestones have a QEMU-less verification
(there is no C6 in QEMU); pure-function parts are still built for every
target and tested in QEMU, per the phase-26 rule *nothing should be debugged
by flashing*.

### Stand-alone node

* **45.1 Boot and a heartbeat — DONE 2026-10-05** (standalone, as the P4's
  E1 was; the CMake preset and kernel linker script moved to 45.4, where the
  kernel first needs them). `tools/minimal_esp32c6.{c,ld}` +
  `minimal_esp32c6_entry.S`, `tools/build_minimal_esp32c6.sh`,
  `tools/c6run.py` (esptool `load-ram` into HP SRAM, then watches the
  USB-Serial/JTAG console; never writes flash), `cmake/toolchain-esp32c6.cmake`.
  Verified on the C6-Zero (`tools/build_minimal_esp32c6.sh run`): the image
  loads with the installed `riscv64-elf-gcc -march=rv32imac_zicsr_zifencei`,
  prints over USB-Serial/JTAG, heartbeats.

  Findings, from the silicon:

  * `misa = 0x40903105`: A, C, I, M, **U** (and N, X) — M+U, atomics present,
    no S-mode, as the datasheet said. `mhartid = 0`, `mtvec = 0x40001501`
    (the ROM's vector table, mode 1), `mstatus = 1` after clearing MIE (to be
    understood in 45.4).
  * **CPU 160 MHz** out of ROM boot, measured (cycle counter against the
    16 MHz system timer), so no PLL bring-up is needed to run at full speed.
  * **`csrr mcycle` is an illegal instruction on this core.** The cycle
    counter is Espressif's custom CSRs: `0x7e0` PCER (bit 0 = count cycles),
    `0x7e1` PCMR (bit 0 = enable), `0x7e2` PCCR (the count). The kernel's
    timing code must use these, not `rdcycle`.
  * **Console:** USB-Serial/JTAG (`0x6000F000`) works from RAM with no setup;
    EP1 writes need `WR_DONE` to hand bytes to the host, and every wait must be
    bounded (nobody may be listening).
  * **Host-side trap, fixed in `c6run.py`:** opening the console with DTR low
    resets the chip (`rst:0x15 USB_UART_HPSYS`), because Linux raises DTR+RTS on
    open and pyserial then drops DTR first, passing through DTR=0/RTS=1, which
    is the chip's reset line. DTR high / RTS low opens without a reset. Any later
    tool (`c6flash.py`, the tests) must open the port the same way.
  * **The board is not blank and not 4 MB:** 8 MB flash (Winbond-class
    `0x20/0x4017`), chip revision v0.2, MAC `ac:eb:e6:1e:3a:ac`, and a
    Waveshare demo in flash (`ESP32-C6-Zero-DemoTest`, IDF 5.4.3: Wi-Fi scan,
    BLE, WS2812 on RMT, BOOT button on GPIO9). Its Wi-Fi scan ran and found 19
    access points, so the antenna and radio are good — a useful control for
    45.6. *Flash layout (45.4) should assume 8 MB; the demo will be overwritten
    when we flash LugalOS and need not be backed up (same stance as §7.4).*
* **45.2 Spike: what does the blob cost? — DONE 2026-10-05, verdict GO.**
  `tools/c6_blob_spike/{link.sh,census.py,supplicant.sh,spike.ld}`: a *static*
  measurement (nothing is run on the chip); reproduce with
  `tools/c6_blob_spike/link.sh && python3 tools/c6_blob_spike/census.py &&
  tools/c6_blob_spike/supplicant.sh`. Results in §4.4. The plan asked for a
  trap-by-name run of `esp_wifi_init`; that was deliberately not done here,
  because `esp_wifi_init` needs the open PHY/PMU/modem-clock code in front of
  it (§4.4) and would have made this a 45.6 in miniature. The reachability
  question is answered statically instead (a lower bound), and 45.3's host
  tests plus 45.6's first run give the dynamic answer.
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
| **WPA supplicant + crypto** | Open; sized in 45.2 at ≈ 46 KB (§4.4) | Port in 45.7; the 11-entry crypto table is the interface |
| **Radio power/clock bring-up** | The 5 000 open lines around the blob (§4.4); a ROM-booted chip has not had IDF's `pmu_init()` | First thing 45.6 does, with the Waveshare demo (which works on this board) as the control |
| **Blob/ROM version drift** | The blob must match the chip's ROM (304 ROM symbols; ROM rev `5b8dcfa` per the demo log) and our shim must match the blob's ABI md5s | Build only against the IDF tree the libraries came from; pin its commit when 45.3 starts |
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
