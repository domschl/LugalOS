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
* **Containment: the radio runs in U-mode under a PMP domain** (decided in
  45.3a, §4.5): the blob, the chip-ROM code it calls and the supplicant execute
  as one U-mode task confined to a handful of PMP regions, reaching the kernel
  only through `ecall`. What that buys is **fault containment**: a wild store
  or jump in 400 KB of closed code faults the radio task instead of corrupting
  the kernel, and the node can restart the radio. What it does *not* buy is
  protection against a hostile blob: the radio's MAC is a bus master doing its
  own DMA, and PMP governs the CPU only (§4.5). The taint label stays the
  honest statement about trust; the domain is a robustness measure.
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

### 4.3 The shim, as built (45.3b)

The blob reaches its OS through **two** ABI-checked tables, not one: Espressif's
`wifi_osi_funcs_t` (127 entries on this chip) and, separately, the coexistence
library's `coex_adapter_funcs_t` (20; the ROM keeps its pointer at
`g_coa_funcs_p`). The 45.2 census found only the first. Both are filled by
`drivers/radio/esp32c6_osi_table.c`, the only file that includes IDF's headers.

```
        blob + ROM (U-mode, radio domain)
          │  function pointers (the two tables)
          ▼
   drivers/radio/osi_impl.c ──── in the domain, no ecall ────► kernel/uheap.c   heap
          │                                                     uprintf.c       log formatting
          │ ecall (one number per operation)                    nvs_ram.c        key/value store
          ▼
   kernel/kobj_sys.c  ── ownership + pointer validation ──► kernel/kobj_sched.c ──► kernel/kobj.c
                                                             (blocking, timeouts)    (state machines)
```

| Layer | What | Where it runs |
|---|---|---|
| `kobj.c` | semaphores, mutexes (recursive), queues, event groups, timers as non-blocking state machines with FIFO direct hand-off; allocates nothing | kernel; host-tested against reference models |
| `kobj_sched.c` | the blocking, timed waits and wake-ups (`task_block_until_ms`), one leaf spinlock | kernel |
| `kobj_sys.c` | the syscalls (`kobj_abi.h`): handles honoured only for the creating domain, every user pointer copied through `uaccess`, threads in the caller's domain, random, MAC, one log line | kernel |
| `uheap.c` | the radio's own heap in its own RAM (boundary tags, 32 size classes, corruption check) — malloc is not a syscall | domain |
| `uprintf.c` | a printf with no libc and no 64-bit arithmetic, for the blob's log calls | domain |
| `nvs_ram.c` | IDF's NVS protocol in RAM (the blob keys off `NOT_FOUND`) | domain |
| `osi_impl.c` | the 127 + 20 entries, in plain C types | domain |
| `esp32c6_osi_table.c` | wires them to IDF's structs; compiles against IDF's headers; `tools/c6_blob_spike/osi_table_check.sh` checks types and completeness | domain, C6 build only |
| `plat.h` | the seam to the chip: PHY enable/disable/country, modem clock and MAC reset | 45.6 |

Entries by where they are served: **in the kernel** (an `ecall`): semaphores,
mutexes, queues, event groups, timers, threads, time, random, the MAC, one log
line, interrupts, events. **In the domain**: malloc/free and friends, the
key/value store, formatting, `is_from_isr` (the stack pointer says which
thread this is), ticks (1 tick = 1 ms; `0xffffffff` blocks forever), and every
constant or no-op. **Ported from IDF**: the PHY and modem clock/reset.

Design points that were decisions, not just code:

* **Kernel objects stay out of the radio's memory.** A semaphore whose count
  lives in the blob's RAM is one a stray store can turn into a deadlock; a
  handle into a kernel table can only be misused as a handle, and is refused
  (`KO_FAIL`) unless the caller's domain made it.
* **A critical section is mutual exclusion among the domain's threads.** The
  hardware interrupt itself runs only a kernel stub; the handler logic runs in a
  thread of the domain (§4.5), so excluding that thread is excluding the
  interrupt. No priority inheritance (the scheduler has none), so a low-priority
  holder can delay the interrupt thread; the sections are short.
* **Threads are real kernel tasks in the caller's own domain.** The blob creates
  its `pp` task through the table; the shim adds a timer thread (it calls the
  blob's timer callbacks) and, in 45.6, an interrupt thread. The entry and the
  stack are validated against the creator's regions before the task exists.
* **The log path is a formatter in the domain and one ecall**, not the kernel's
  printk engine, which a U-mode caller cannot reach.

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

### 4.5 Can the radio run in U-mode? (45.3a, 2026-10-05) — yes

A standalone probe (`tools/build_umode_probe_esp32c6.sh run`, loaded to RAM, no
flash written) sets up PMP on the C6, drops to U-mode, and provokes both the
things that must work and the things that must fault. **16 of 16 pass.**

**On the silicon:**

* **PMP: 16 entries, 4-byte granularity, none locked, and the ROM leaves it
  empty.** (After a reset nothing restricts anything; M-mode is unrestricted.)
  NAPOT regions of any power of two ≥ 8 bytes work. `MEM_DOMAIN_MAX_REGIONS`
  is 5 because RP2350 has 8 entries and 3 are spoken for; **it must become a
  per-target figure** for the C6 (the radio domain below needs 8–9).
* **A confined U-mode task behaves as the kernel's domains promise:** runs with
  text + stack granted; instruction access fault (cause 1) with no text grant,
  on a jump into M-mode text, and on a ROM call without a ROM grant; load
  fault (5) and store fault (7) against kernel data; MMIO faults without a
  grant and works with one (a system-timer write + read from U-mode).
* **U-mode can call chip-ROM code and use the ROM's own data.** `ets_delay_us`
  (a ROM trampoline into real ROM code) runs from U-mode once ROM code
  (`0x40000000`, 512 KB NAPOT) and **the ROM's working data (`0x4087c000`,
  16 KB)** are granted. The first attempt granted only the top 1 KB of SRAM,
  where the blob's exported ROM data lives, and faulted at `0x4087faa0`: the ROM
  keeps more state than it exports. The ROM data region is the whole
  `0x4087c000–0x4087ffff`, not a window around `g_osi_funcs_p`.
* **CSRs:** the cycle counter's **user alias `0x802` works from U-mode** (it is
  what the ROM reads); the machine one (`0x7e2`) and `mstatus` trap as illegal
  instruction (cause 2). The counters must be enabled from M-mode first
  (`0x7e0`/`0x7e1`, as in 45.1).
* **`mtvec` is forced to vectored mode, and its base must be 256-byte
  aligned:** writing `0x40800200` reads back `0x40800201`; a handler at an
  unaligned address is silently rounded down and the first trap executes
  whatever is there (this restarted the probe from `_start` until aligned).
  Synchronous exceptions go to BASE; interrupt `n` goes to `BASE + 4n`, so 45.4
  needs a vector table, as IDF's does.
* **The cost of the boundary is small.** A fast-path `ecall` round trip
  (U → M → U, saving two registers and stepping `mepc`) is **34 cycles
  (0.21 µs at 160 MHz)**, against 8 for a plain call/return. A real syscall
  with dispatch and pointer validation will cost several times that; at an
  assumed 300 cycles and 50 000 shim calls a second (a busy link) the radio's
  boundary overhead is under 10 % of one 160 MHz core. Measured floor, not a
  measured system.
* **`ets_delay_us` runs fast on this chip until told the CPU frequency:**
  1000 µs measured as 53 000 cycles (≈ 330 µs at 160 MHz). The ROM keeps its
  own CPU-frequency variable; IDF calls `ets_update_cpu_frequency()` at start.
  45.6 must too — the PHY uses `ets_delay_us`.

**In the code the blob runs:**

* **The blob contains no CSR, `wfi`, `mret` or `fence` instruction at all.**
* **Following the control flow of the ROM code it calls** (`rom_scan.py`, over
  a dump of this chip's ROM: 304 entry points, 13 495 instructions reached,
  580 indirect jumps not followable) finds **no privileged instruction except
  two reads of `0x802`** — the user-accessible cycle counter — in
  `write_chan_freq`. A privileged instruction the scan could not see would
  trap as cause 2 with the offending `mepc`: loud, not silent.
* **What the radio domain must be granted** (peripheral pages addressed by the
  blob and by the reachable ROM code): `0x600A0000–0x600AFFFF` (MAC, baseband,
  PHY and modem registers; one 64 KB region), `0x600B0000–0x600B1FFF` (PMU and
  LP_AON; 8 KB), `0x60096000` (**PCR**, the clock/reset control of every
  peripheral; 4 KB, touched by the PHY's temperature-sensor setup),
  `0x6000E000` (SAR-ADC, same code). Plus ROM code (1 region), ROM data (1),
  the blob's text/rodata window in flash (1; linked at a 512 KB-aligned virtual
  address so one NAPOT region covers it), and its RAM (1). **≈ 9 regions of 16.**
  Granting PCR and PMU to a task that can then gate clocks and power is a real
  exposure; it is the price of not rewriting the PHY, and it is smaller than the
  exposure of running those 400 KB in M-mode.

**What U-mode does not give — stated plainly:**

* **PMP restricts the CPU, not bus masters.** The Wi-Fi MAC does DMA on its own;
  the TRM's permission controller (APM/TEE, ch. 16) can confine masters by
  address range, but the masters it lists are the CPUs, SDIO slave, the memory
  monitor, trace and the GDMA channels — **not the Wi-Fi MAC**. So a blob that
  *wants* to corrupt the kernel can program its DMA to do it. U-mode contains
  *bugs*; the `tainted` label covers *trust*. (Whether the MAC's DMA is
  confinable some other way is not established; it is not needed for the
  fault-containment goal.)

**What it means for the design (45.3b):**

* **The radio is a U-mode driver task**, in the shape phases 12/30 already
  gave every RP2350 driver: one domain, one message channel to the kernel. The
  IP stack's `netif` driver (kernel side, M-mode) sends frames and control
  messages down the channel; the radio task calls `esp_wifi_internal_tx` and the
  scan/connect API, and sends received frames and events up. Frames are copied,
  as every channel in this kernel does.
* **Every `wifi_osi_funcs_t` entry becomes an `ecall`.** The table is ABI-fixed
  and md5-checked against IDF's headers, so the U-mode side is *generated* from
  it (stub per entry, one syscall number each, arguments in registers, pointers
  validated against the domain by `mem_domain_permits`). Blocking entries
  (`semphr_take`, `queue_recv`, `task_delay`) block the calling U-mode task in
  the kernel, as `SYS_CHAN_CALL` already does.
* **Callbacks need threads in the domain.** The blob hands us function pointers
  to run: timers (`_timer_setfn/_arm`) and interrupt handlers (`_set_isr`). The
  kernel cannot call into U-mode from an interrupt, so each gets a small U-mode
  thread in the radio domain that waits on a kernel queue and calls the
  pointer: a *timer thread*, and an *ISR thread* that the kernel interrupt stub
  wakes after masking the source (the MAC and PWR lines are level-triggered; the
  thread unmasks through a syscall). **ISR latency is the new risk** — a
  context switch where IDF has a direct call — and is a 45.6 measurement against
  the radio's deadlines, with M-mode ISRs (taint-only for just that path) as the
  fallback.
* **`_wifi_int_disable/_restore`** (the blob's critical sections, 4 call sites)
  become "no preemption by the ISR thread and the timer thread" syscalls. They
  guard blob state shared between its task and its ISR, so they must be correct
  before anything else is.
* **Kernel work this implies, small and known:** a per-target region budget
  (above), `mtvec` vector table (45.4), and the generated syscall table. None of
  it is blob-specific.

**Not yet tested, deliberately:** interrupt delivery into M-mode while U-mode
runs (45.4 builds the PLIC/matrix driver), execution of the blob's text from
the flash window under a U-mode grant (a PMP region is a physical-address
check; nothing about it is flash-specific), and any real radio register
access from U-mode (the radio clocks are not up; 45.6).

### 4.6 What building it found (45.3b)

* **The lock checker caught a real bug on first contact.** `kq_create` allocated
  pages while holding the object spinlock; `[Lock BUG] took g_palloc_lock while
  holding g_kobj_lock`. A spinlock is a leaf. The core now allocates nothing:
  queue storage is the caller's, allocated before the lock and returned by
  delete for freeing after it.
* **On RV32 every 64-bit shift is a libgcc call, and U-mode text cannot call
  libgcc.** The formatter's first version faulted on its first `v >> bit`
  (`__lshrdi3`). It now divides in 16-bit limbs. Nothing warns about this — the
  linker resolves it happily — so `tools/check_radio_text.py` reads the objects
  and fails on any reference outside the shim (mutation-tested: an injected
  64-bit divide is caught as `__udivdi3`); it runs in the suite. **On the C6 the
  same applies to the shim, not to the blob**, whose own soft-float helpers
  resolve into ROM (§4.4).
* **`EXCLUDE_FILE` in GNU ld applies to the pattern that follows it, not to the
  list.** `*(EXCLUDE_FILE(x) .rodata .rodata.*)` excluded `.rodata` and let
  `.rodata.str1.1` through, so a log format string stayed in kernel read-only
  data and the first `%s` faulted in U-mode. Each pattern needs its own.
* **GCC refuses to put code and constants in one section** (`section type
  conflict`), so a U-mode test's strings live in a second input section the
  linker folds into the same read-only+execute region.
* **A differential test against libc found four format edge cases** in the
  formatter that hand-written cases had not (`%.0d` of 0, `%#.4o`, `%#.0o` of
  0, a precision/zero-flag interaction): 60 000 random formats × four seeds now
  agree with `snprintf`.
* **`os_get_time` is the supplicant's `struct os_time {long; long}`**, not a
  `timeval` — 8 bytes here, not 16. Read from IDF's source before it was wrong in
  the radio.
* **IDF's sleep-retention wrappers return 1, not 0**, and
  `_wifi_disable_ac_ax` returns false: a table of constants has to copy the
  constants.
* **Verification in the suite now:** `kobjselftest` (kernel tasks, real timeouts,
  a timeout racing a grant 100 times — it must go both ways, and does: 60/40),
  `kobjutest` (29 checks: every operation, the ownership and pointer boundary,
  a second thread in the domain), `radioosi` (22 checks through the shim's
  entries, in a domain whose text and data are the linker's), the host harnesses
  `kobj_host`, `uheap_host`, `uprintf_host`, `nvs_host` under ASan/UBSan (each
  mutation-tested), and `check_radio_text.py`.

### 4.7 What bringing the kernel up found (45.4, 2026-10-05)

**On the silicon (`tests/hw/test_esp32c6.py`, 14 of 14):** boots to a shell with a
256 KB heap and the ROM's top 16 KB of SRAM fenced off; the tick holds 100 Hz
(+20 per 200 ms) through matrix and PLIC; `preempttest` PREEMPTED, `lockselftest`
16/16, `priostress` FAIR; `pmpinfo` 16 entries, 8-byte minimum region, nothing
locked; `usertest` (ecall cause 8), `isolationtest` (store fault contained, canary
untouched), `deputytest` (refused); `kobjselftest` (the timeout-versus-grant race
goes both ways, 61/39), `kobjutest` 29 checks, `radioosi` 22 checks; and the
kernel clock keeps counting past a minute — which is what finds a watchdog that
resets a kernel that booted perfectly.

* **A RAM-resident kernel does not fit, so execute-in-place came first.** The
  image is ~540 KB against 496 KB usable: the kernel has grown since the P4's first
  boot (net stack, MQTT, NTP, 9P, the kernel objects) and nothing was left for a
  heap. The P4's phase-32 shape — `.text`/`.rodata` in flash, boot code and state in
  SRAM — was therefore a prerequisite, not a follow-up. SRAM now holds 155 KB of
  `.bss` and a 256 KB heap.
* **A cached read of mapped flash returns `0x0addbad0` until the ROM's
  `spi_flash_boot_attach()` has run.** The MMU sequence is IDF's second-stage
  bootloader's (cache off, 64 KB pages, unmap, map, un-shut the buses, cache on) and
  is not the problem; the SPI0 read path (command, dummy cycles, pins) is
  unconfigured when the chip was started by `esptool load-ram`, because the flash
  writes esptool does go through SPI1 and need none of it. A board that boots *from*
  flash has had it run for it. With it, 65 536 words read back exactly. Found with a
  standalone probe before any kernel code depended on it.
* **`mtvec` is hardwired to vectored mode and wants a 256-byte-aligned base**
  (45.3a measured the read-back). So the vector is a table: 32 four-byte jumps (slot
  0 takes synchronous exceptions, slot *n* interrupt *n*), every slot jumping to the
  one handler that decodes `mcause` as on every other target.
* **An M-mode interrupt line also needs its bit in `mie`** — the TRM says so in one
  bullet ("further needs to be unmasked at core level") and nothing else reports the
  omission: the line was routed, enabled, asserted (`int_raw`=1) and never taken.
  `intrdump` exists because that was found by reading every link of the chain, not by
  guessing.
* **Line 8 does not deliver.** The same source on line 10 ticks at exactly 100 Hz and
  a different source on line 9 was delivered, so it is not the wiring; mie bit 8 is
  where the standard puts the user-external enable. Refused in
  `arch/esp32c6_intr.h` with the evidence.
* **Interrupt-matrix sources are the *register offset / 4*,** not a count of the
  header's enum (off by one here): system-timer target 0 is source 57, FROM_CPU_INTR0
  is 22, USB-Serial/JTAG is 48. The table is in `arch/esp32c6_intr.h`.
* **The TRM's INTPRI registers (`0x600C5000`) and IDF's `PLIC_MX` (`0x20001000`) are
  not mirrors:** writes to the second do not show in the first, and the second is what
  takes interrupts (IDF: "ESP32C6 should use the PLIC controller instead of INTC").
  The kernel uses PLIC_MX, as IDF does in M-mode.
* **Interrupts can be delegated to U-mode on this core** (`mideleg`, `uie`, the N
  extension; `misa` bit 13 was set in 45.1). That is a real alternative to the
  interrupt-*thread* of §4.5 for the radio's MAC/PWR lines: the handler would run in
  U-mode directly, with no kernel stub and no thread wake-up, which is the latency
  risk §9 names. Not built; the thread design stands until 45.6 measures it.
* **Flash boot works and survives.** `tools/c6flash.py` writes stage 2 (the RAM half
  as the ROM's image format) at `0x0` and the OS image at `0x20000`; the board came up
  running from flash with nothing attached and was still counting at 103 s. The
  flash-boot watchdogs (RWDT, MWDT0, the super watchdog) are disarmed in `.boot.text`
  exactly as IDF's bootloader does; `load-ram` arms none, so this is a no-op on the
  development loop and essential on the final one.

**Still polled, deliberately:** the console. The USB-Serial/JTAG peripheral has an
interrupt (source 48) and line 11 is allocated for it, but a task waiting for a key
yields in a loop as every console here did before M4. It moves to the interrupt when
something needs the CPU back (45.6's measurements will say).

### 4.8 What bringing the radio up found (45.6)

The blob links and runs unchanged; everything that went wrong was *the chip around it*, and
every one of these was found by looking, not by reading:

* **The blob's libc calls bind to the kernel's.** `memcpy`, `strlen`... are the kernel's
  names in kernel text, which U-mode cannot execute; and Espressif's ROM linker scripts
  *silently replace* the kernel's same-named symbols (a script assignment beats an
  object's). `tools/c6_radio_libs.py` rewrites the blob's imports in copies of the archives
  to `radio_*` names (ROM address or `radio_libc.c`); `radio_redirect.h` does the same for
  the compiler's own struct-copy calls; `check_radio_text.py` fails the build if a radio
  object references a kernel symbol.
* **SRAM survives a reset.** The shim's state lives in a NOLOAD region; a stale
  thread-semaphore table from the previous run made the blob wait on a dead handle. Now
  cleared at init.
* **U-mode reads of peripherals returned zeros, silently — the HP APM.** Its reset state
  (region 0 = everything, attribute 0) denies every REE mode, which is U-mode, by returning
  0 to reads and dropping writes. PCR and the modem blocks looked "unpowered" for hours
  (M-mode `peek` saw `0x7e600000` where U-mode saw 0). The kernel opens the APM for the
  radio; the PMP stays the fence that is ours. `peek`/`poke` shell commands exist for this.
* **A chip started by the ROM has none of IDF's boot-time modem setup:** the ICG
  (clock-gating) maps for every modem domain (a gated block stalls the bus), the analog
  I2C master's clock source and enable (without it the ROM's `regi2c` read polls its busy
  bit forever — found with a tick-time sampler that prints `epc`), the Wi-Fi low-power
  clock (RC_SLOW) and the power clock — **without which the MAC raises no interrupt at all**,
  with everything else apparently working. Ported behind `plat.h` from IDF's
  `modem_clock*.c`/`rtc_clk_init.c`/`esp_perip_clk_init` using IDF's own `*_ll.h` headers.
* **`register_chipv7_phy` returns 1 = "calibration data not stored", which is normal;** a
  full calibration takes ~90 ms. There is no NVS: every boot calibrates.
* **Kernel-side interrupts:** the blob's interrupt numbers are names, not hardware lines;
  each gets a free line (12..19). The hardware interrupt lands in a stub that masks the
  (level-triggered) line and wakes a U-mode interrupt thread, which runs the blob's handler
  and unmasks (`KOBJ_OP_ISR_WAIT/DONE`). 42 interrupts per scan, serviced with the thread
  design; ISR latency is not yet measured against a loaded link (45.7).
* **Small blob contracts:** `_wifi_create_queue` returns a `wifi_static_queue_t *`, not a
  handle; the crypto table must have the right size/version even for a scan; the
  supplicant callback table must be registered (all stubs) or the first AP found is a null
  call; the timer thread must be started.

**Left for 45.7:** the supplicant and its crypto (auth modes, WPA2-PSK), the event sink, the
`netif`, PLL temperature tracking (`phy_common.c`), and putting the blob's data back to its
initial state so `radio` can start twice in one boot.

### 4.9 What association and IP found (45.7, 2026-10-06)

* **A byte-wise `memcpy` into MMIO broke every encrypted frame.** The blob copies the CCMP keys
  into the Wi-Fi MAC's key RAM (`0x600A5800`..) with `memcpy`, and in the radio's domain that is
  ours (`radio_memcpy`, drivers/radio/radio_libc.c). The key RAM ignores byte enables: a byte store
  rewrites the whole word with only its own lane set, so after a byte loop each key word held its
  last byte alone (`59000000 bf000000 ...`). The supplicant's 4-way handshake runs in software and
  succeeded; every protected frame after it, both directions, used a mangled key -- associated,
  "connected", and not one data frame through, on three different access points. An open network
  worked at once (DHCP in 70 ms), which is what isolated it. Fixed by storing whole aligned words,
  as the ROM's (newlib's) memcpy does. **Rule: a libc function substituted for a ROM one must keep
  its access widths -- the blob uses it on hardware.**
* **The LP APM silently dropped the PHY's PMU writes.** Like the HP APM (§4.8), the LP APM
  (`0x600B3800`, in front of PMU/LP clock/LP_AON) denies REE by reading zeros and dropping writes.
  The PHY library switches the RF blocks' I2C power bits in `PMU_RF_PWC` itself while it
  calibrates; from U-mode those writes vanished and a cold chip's PLL calibration timed out
  (`pll_cal exceeds 2ms`, then a scan finding nothing). Opened at radio start like the HP APM; the
  PMP still limits the radio to its 8 KB PMU/LP_AON window. PMU registers survive a chip reset, so
  only a power cycle tests this -- a warm reset hid it twice.
* **Cold boot needs IDF's startup, not the ROM's.** A flash boot leaves the CPU at 40 MHz on the
  crystal with the PLL off; the RF synthesizer derives from that PLL. The radio start (M-mode,
  `radio_plat_cpu_to_pll`, `rtc_osc_tuning`, IDF's unmodified `pmu_init()`) now does what IDF's
  bootloader and `esp_clk_init` do. `pmu_init` and friends carry `IRAM_ATTR` (`.iram1`), which the
  linker script must place or the image silently lacks them (an illegal instruction in
  `PMU_instance`).
* **PHY bring-up as IDF does it:** `phy_init_param_set(1)` before calibration (combo PHY), the
  calibration-only clocks (BT baseband, BT APB, modem-security APB) released afterwards
  (`phy_module_disable()`), and `phy_param_track_tot()` on enable and once a second.
* **Method.** What found the key bug was a known-good reference: IDF's stock `station` example
  built for the same board (toolchain in `~/.espressif`), its modem registers dumped and diffed
  against ours. Crypto configuration was identical, which pointed at the key bytes themselves.
* **Not yet:** an authenticated 9P attach over TCP. The server answers on 564 and negotiates
  `version`, then (correctly) refuses an unauthenticated `attach`; authenticating needs the
  node's key, and the C6 has no identity store yet (45.8). The shell has no `ping`, so "ping
  both ways" is host-to-board only for now. The kernel froze three times during this work
  (once each during a join, a scan and early after boot) -- under investigation.

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
* **45.3a Can the radio run in U-mode? — DONE 2026-10-05, yes.** Measured
  on the silicon and in the blob/ROM code; §4.5 has the results and the
  consequences for the design. Tools: `tools/umode_probe_esp32c6.{c,ld}`,
  `umode_probe_esp32c6_entry.S`, `tools/build_umode_probe_esp32c6.sh`,
  `tools/c6_blob_spike/rom_scan.py`, and the shared `tools/c6_standalone.h`.
* **45.3b The OS shim — DONE 2026-10-05, on QEMU.** Everything the blob's two
  OS tables ask of an operating system is implemented and tested in a confined
  U-mode domain on rv32 (PMP), rv64 (Sv39) and the two-hart rv64-smp build, with
  no C6 involved; §4.3 describes what was built and §4.6 what it found. What
  stays open is exactly the part that needs the chip: interrupts, the PHY and
  clock port, the event sink (`radio_plat_*`, `KOBJ_OP_INTR_*`,
  `KOBJ_OP_EVENT_POST`) — 45.4 and 45.6.
* **45.4 C6 kernel bring-up — DONE 2026-10-05, on the silicon.** LugalOS boots
  to `lsh` on the ESP32-C6-Zero, preempts, runs the whole U-mode stack under a
  16-entry PMP, and boots from flash by itself. Four steps, in the order they
  turned out to be necessary (§4.7 has what each found):
  * **45.4.1 First boot, cooperative.** The `esp32c6` preset, board file,
    USB-Serial/JTAG console driver (polled), system-timer clock, a stated-empty
    device table. It linked and did *not fit* in SRAM — see 45.4.2.
  * **45.4.2 Execute in place.** `.text`/`.rodata` run from flash at
    `0x42000000` through the MMU; `.boot.text` in SRAM maps it. A standalone probe
    (`tools/xip_probe_esp32c6.c`) established the sequence on this chip first.
  * **45.4.3 Interrupts and the tick.** Interrupt matrix → PLIC_MX → a 32-slot
    256-aligned vectored `mtvec`; the tick is system-timer comparator 0 on CPU
    line 10.
  * **45.4.4 U-mode and the shim on the silicon.** The kernel-object suites and
    the radio OS-table shim run unchanged on the C6, plus the kernel's own
    isolation proofs. And `tools/c6flash.py`: both halves in flash, no host.

  `tests/hw/test_esp32c6.py` is the repeatable form — **14 of 14 pass**.
* **45.5 LED — DONE 2026-10-05, confirmed by eye.** WS2812 on GPIO8 through RMT
  channel 0 (XTAL / 4 = 10 MHz pulse clock, 0.3/0.9 us bit widths). The wire
  encoder is portable (`drivers/ws2812.c`) and checked on the host against the
  datasheet windows, with a mutation (a swapped pulse width) caught
  (`tests/host/ws2812_host.c`); `drivers/ws2812_esp32c6.c` is the peripheral.
  Shell `led R G B` / `led off`, Lisp `(led r g b)`. Findings: the Zero's LED takes
  **R,G,B** order (as its vendor demo says; a bare WS2812 is G,R,B) —
  `CONFIG_WS2812_RGB_ORDER`; the RMT RAM is written by plain APB access
  (`SYS_CONF.APB_FIFO_MASK`). Not done: a `/dev/led` node — the shell and Lisp
  entry points cover the use, and a file node earns its place only when 45.6
  wants to show Wi-Fi state on the LED.
* **45.6 Wi-Fi bring-up — DONE 2026-10-05: `radio` scans 14 channels and lists the access
  points.** PHY + `esp_wifi_init` + start + scan, the blob in its confined U-mode domain,
  interrupts through the kernel's interrupt thread (§4.8). Hardware test:
  `test_radio_scan` (16/16 on the suite; needs one AP in range). Not yet: association
  (supplicant, 45.7) — and the scan's `authmode` is wrong (all 1) because IE parsing is the
  supplicant's.
* **45.7 Associate (WPA2-PSK) and DHCP — DONE 2026-10-06: the board joins WPA2-PSK
  from a cold boot, gets its address by DHCP and answers ping.** Supplicant (IDF's, in the
  radio domain), `wlan0` over frame rings onto the phase-19 stack, DHCP client, factory MAC
  from eFuse (§4.9). Hardware tests: `test_radio_join`, `test_radio_ip` (lease + host ping;
  17/17 on the suite). Not yet: authenticated 9P over TCP (needs the identity store, 45.8),
  a shell `ping`.
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
| **Shim bugs are timing bugs** | The blob assumes FreeRTOS scheduling and ISR semantics | Done for the primitives (45.3b: models on the host, real tasks and races in QEMU); what remains is the blob's own expectations, found only by running it (45.6) |
| **ISR latency in U-mode** | The radio's MAC/PWR interrupts become a kernel stub + a U-mode thread wake-up (§4.5) | Measure in 45.6; fallback is M-mode handlers for that path only |
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
* ~~U-mode radio tasks with PMP windows, or taint-only — 45.3.~~ U-mode (§4.5). Open: ISR-thread latency (45.6).
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
