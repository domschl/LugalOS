# Phase 66 — RAM, Presets, and Folding: Scaling LugalOS Down

**Status: scoped 2026-11-03, not started. Independent of phase 52.**
Milestone scheme: `66.1`, `66.2`, …

* **Scheduled:** 66.1–66.3 (measure, split, right-size), 66.3b (TLS on the C6-Zero sensor node),
  then 66.4–66.7 (P4 layout, on-demand services, service host, folding of kernel-mode endpoints).
* **Optional, decided at the review after 66.7:** 66.8–66.10 (folding of U-mode drivers, a full
  build-time unikernel, an ESP32-C3 budget target).

---

## 0. Scope

Prompted by the Pico 2 W's heap (80 pages), the Tab5's and LCD-7B's low RAM (56 and 328 bytes
free at the 2026-10-29 build), the C6-Zero at 0 free pages with radio, sensors, MQTT, mDNS, httpd
and 9P up (52.13b), and the wish for presets that do not all carry everything.

**Objective:** higher scalability of LugalOS: the same sources running on boards with less RAM, and
more functionality on the boards we have, ideally down to an ESP32-C3-class part (~320 KB usable).

**Concrete target:** TLS on the C6-Zero sensor node (66.3b): HTTPS for its admin page and TLS for
its MQTT connection, under the full sensor load. Phase 52 left it out for want of RAM (§1.3).

**Ground rule:** the microkernel architecture stays the lead. Every component is written, and
first works, as a clean microkernel component. Every exception (a folded service, a waived
isolation boundary) is a property of a preset, derived at build time from that component, and
visible at runtime.

### 0.1 The original idea collection (2026-10-29)

* Unnecessary use of SRAM (avoid it, or use PSRAM where there is some); stack usage and stack
  safety; flash use that could move to SD for a given preset.
* Presets: resource use of each process, service and application configured per platform and
  preset; stricter exclusion of lower-priority functionality per preset.
* Dynamic processes: swapping processes out in favour of a foreground process; process groups
  (shared stack? shared allocations?).
* Extreme measure: a build-time unikernel for appliance presets, strictly derived from a fully
  working microkernel build, perhaps through a glue layer that "folds" several processes into one
  space.

---

## 1. Findings (review of 2026-11-03)

Built for the review: `esp32c6`, `esp32p4-tab5`, `rp2350-wifi` (`tools/sizereport.py`, `nm`,
`size -A`). No hardware attached; stack figures are phase 52's measurements.

### 1.1 Isolation today is partial and board-dependent

| Component | RP2350 | ESP32-P4 / ESP32-C6 |
| :--- | :--- | :--- |
| Drivers: uart, usb_cdc, spisd, i2c, st7735, lcdterm, usbkbd, clock | U-mode, own domain (`driver_umode_enter()`) | kernel tasks (uart, sdmmc via `driver_task_start()`) |
| httpd, mqtt, 9P server, sensor_hub, dhcp, setup, klogd, llp | kernel tasks | kernel tasks |
| Vendor Wi-Fi code | — | U-mode on the C6 (ADR 007) |
| ELF programs | U-mode | U-mode |

`doc/dev/kernel/architecture.md` states that all driver tasks run in U-mode; on the P4 and C6 that
is not true. For most services the microkernel's cost on those boards is **per-task stacks,
per-endpoint buffers and the two copies per call**, not isolation (they have none to lose).
`drivers/loopback_net.c` already says the copies are redundant in this single-address-space build.

### 1.2 Static RAM (2026-11-03 builds)

* **C6-Zero:** `.bss` 133.5 KB + `.radio_bss` 26.7 KB; 124 pages for image, stacks and heap. A
  headless sensor node still carries the reader view (`s_readers` 7.6 KB), vterms (`g_vterms`
  3.4 KB), shell history (`history_stack` 5 KB), the klog ring (`g_ring` 8 KB); Lisp is already
  trimmed (24.8 KB).
* **Tab5:** `.bss` 223.6 KB in a **fixed** 252 KB LOWRAM, next to 1.79 MB already in `.bulk_bss`
  (PSRAM). Candidates for PSRAM: `g_chess_pos` 8.4 KB, `node_mark_bits` 8 KB, `g_waiters` 8 KB,
  `history_stack` 5 KB. `s_audio_buf` (24 KB) may have to stay internal for DMA — to verify.
  `linker/esp32p4_memory_rev3.ld` names the ROM download buffers (0x4ffa96b8..0x4ffbafc0, ~70 KB)
  as reclaimable after a flash boot, with core 1 starting on the ROM's CPU1 stack just above them.
* **Pico 2 W:** `.bss` 164.9 KB; Lisp 48.4 KB, usb_cdc 16.7 KB, uart_net 10.6 KB, reader view 7.6 KB.
* **Everywhere:** 16 `LUGALOS_ENABLE_*` flags. httpd, MQTT, 9P, sensor_hub, reader view,
  self-tests, stress and isolation "intruder" tests are in every image. Buffer sizes (`MAX_TASKS`
  and the 6 arrays sized by it, `KLOG_RING_SIZE`, 9P links/msize/waiters, endpoint buffers) are
  hard-coded in the sources, not per board.

### 1.3 Runtime RAM

* Task stacks come from `palloc` (`task_create_sized()`, default `TASK_STACK_PAGES` = 2).
  Measured over-provisioning: `sensor_hub` 12 KB holds 0.9 KB, `mqttsrv` 8 KB holds 1 KB (52.13b).
  The high-water mark exists (`sched_stack_used()`); nothing collects it per board.
* `palloc` has no owner tags: free pages are known, who holds the rest is not.
* **TLS on the C6-Zero (phase 52 §1.6, measured 2026-10-31):** with radio, sensors, MQTT, mDNS,
  httpd and 9P running, 5 pages (20 KB) are free. One TLS session takes ~21 KB of record buffers
  (`TLS_RECORD_MAX` 16.6 KB receive, `TLS_TX_CAP` 4.1 KB send, `net/include/net/tls.h`), so the
  C6-Zero is not a `CONFIG_TLS_SERVER` board and stays on 52.11b's protected sessions. The
  prerequisites are otherwise in place: hardware entropy while the RF is on (`kernel/random.c`),
  the device key and CSR flow (52.14), and the TLS 1.3 server (52.15, +13 KB code on the RP2350).
  `net/tls.c` is **server-only**: MQTT over TLS needs a client.

### 1.4 Feasibility of the original ideas

| Idea | Verdict |
| :--- | :--- |
| SRAM / stack / PSRAM review | Feasible; returns not exhausted, sizes have never been set per board. |
| Flash → SD | Little RAM effect (`.rodata` is XIP). Matters for fitting two OTA slots on 4 MB parts. Low priority. |
| Tailored presets | Feasible; the largest lever. Board files mix silicon facts with policy today. |
| Swapping processes out | **Not feasible** without an MMU: no relocation, kernel tasks hold pointers into their stacks. Replaced by on-demand services (66.5). |
| Process groups with a shared stack | Not feasible for tasks that block midway. Replaced by a service host running timer-driven jobs as callbacks (66.6). |
| Build-time unikernel | Feasible as **folding** (66.7): the seam exists in `driver_task_t`'s `serve(ctx, req, len, resp, cap)`. |
| TLS on the C6-Zero sensor node | Feasible once 66.2/66.3 free ~6–8 pages under load (§1.3 numbers). Server: RAM only, the code exists. MQTT client: new code (66.3b). |
| Dynamic (runtime) transformation | Not pursued: a boot-time switch is cheap in code but doubles every board's test matrix. Build-time, per preset. |
| ESP32-C3 | A port of its own: no A extension (24 source files use atomics), < 400 KB SRAM with the Wi-Fi blob. First as a budget target (66.10). |

---

## 2. Guardrails (keeping the microkernel in the lead)

1. **Folding is a property of a preset, never of the code.** Components keep the channel API;
   the fold happens in the framework (`driver_task_call()` and its equivalents).
2. **No direct calls to `serve()`** outside the framework: a lint in the style of
   `tools/racescan.py --check`, run by `tests/runner.py`.
3. **Every folded preset has an unfolded twin** that runs the same suite (QEMU at least).
4. **Every waiver is visible at runtime** (`/proc`, 66.1) and every class of waiver has an ADR.
5. Board files hold silicon facts only; policy and sizes live in profiles (66.2).

---

## 3. Milestones

### 66.1 Resource ledger (scheduled)

* **Status 2026-11-03: built; baselines recorded for the C6-Zero, Tab5 and LCD-7B; the Pico 2 W open.**
  *As built:* `palloc` records the return address into its caller on the first page of every run
  (`g_fast_owner` in SRAM, 0.5 KB; `g_bulk_owner` in `BULK_BSS`), and `/proc/pages` lists each run with
  its zone, size, caller and, for a task stack, the task. `tools/ramledger.py` captures `/proc/buildid`,
  `meminfo`, `ps` and `pages` from a board (`--c6`, `--p4`, `--rp2350`, or a saved `--capture`),
  refuses an ELF of another build, names the callers with `addr2line` and sums pages by subsystem.
  `tools/ramsubsys.py` is the one grouping, used by it and by `tools/sizereport.py`, which now reports
  by subsystem, names symbols without line info by section (the radio blob's `.radio_bss`), and has
  `--all --check/--update`. Baselines moved to `tools/ram_baselines/<build dir>.json`, all 15 presets;
  `tests/runner.py` checks them. The silicon suites end with a ledger case (every page in use owned,
  no stack above 90%). `ps` already showed isolation and stack high-water; no new view was needed.
  `doc/dev/kernel/architecture.md` corrected (§1.1's table).
* *Measured* (build 1304, steady state after boot; `tools/ram_baselines/runtime/`):

  | Board | Pages free / total | Stacks: pages held / KB used | Next largest owners |
  | :--- | :--- | :--- | :--- |
  | C6-Zero (radio, sensors, MQTT connected, httpd) | 8 / 68 | 30 / 22 KB | radio 20, TCP 4, httpd 2 |
  | Tab5 (C6 link, httpd, display) | 24 / 93 | 35 / 24 KB | LLP `take()` 18, TCP 8, httpd 5 |
  | LCD-7B (C6 link, httpd, display) | 10 / 71 | 27 / 15 KB | LLP `take()` 18, TCP 8, httpd 5 |

  Every page in use is accounted for on all three. Stacks are the largest owner everywhere, and the
  high-water marks are steady-state figures: the active scenario (an HTTPS session, a 9P session, MQTT
  publishing) still has to run before 66.3 sizes anything from them (httpd's phase 52 figure under TLS
  was 11 KB, against 1 KB here).
* *Open:* the Pico 2 W (`picotool` on this host is an x86 binary; the ARM host needs a native build or
  another flashing route); the active-load scenario; `tools/racescan.py --check` (needs libclang ≥ 19,
  not on this host) for the two new statics, which carry `guarded by:` verdicts.
* *Found on the way:* the C6-Zero `esp32c6-zero-86c2` has no broker stored (`mqttcfg`: none), so it
  publishes nothing; measured with a session-only `mqtt connect` to nalanda.

* `tools/sizereport.py` grouped by subsystem; checked-in baselines for **every** preset, checked
  by `tests/runner.py` (today only the RP2350 has one).
* Owner tags on `palloc` pages (task pid, or a subsystem tag for kernel allocations), shown in
  `/proc/meminfo`.
* Stack high-water export: the hardware suites read every task's high-water mark after a defined
  load scenario and write a per-board stack table.
* **Standard load scenarios** per profile (e.g. sensor node: radio, sensors, MQTT, mDNS, httpd,
  one 9P session; terminal: plus vterms, Lisp, SD), run by the hardware suites, reporting free
  pages, bulk pages and SRAM fallbacks.
* `/proc` view of each task's mode (U / kernel / folded) and any isolation waiver.
* Correct `doc/dev/kernel/architecture.md` to the table in §1.1.

### 66.2 Board/profile split, dev/production split (scheduled)

* `board-*.cmake`: silicon facts only. New `profile-*.cmake`: personas (terminal, sensor,
  gateway, radio modem, appliance) owning feature selection and every size: `MAX_TASKS`, klog
  ring, 9P links/msize/waiters, Lisp pools, endpoint buffers, vterms, reader view. A preset is a
  board plus a profile.
* One dev flag for self-tests, stress, intruder tests and demo commands. The hardware suites
  either run against a dev image or skip what a production image lacks.

### 66.3 Right-sizing per profile (scheduled)

* Task stacks from the 66.1 tables plus a fixed margin, per board.
* The sizes 66.2 made configurable, set per profile from 66.1's measurements.
* Rarely used session buffers (scratch, per-connection rx/tx) move from `.bss` to allocation at
  session start where a runtime refusal is acceptable (the `kernel/ota.c` precedent).

### 66.3b TLS on the C6-Zero sensor node (scheduled)

The acceptance test for 66.2/66.3: the RAM they free on the C6-Zero is spent on TLS.

* **Budget:** in the sensor profile's load scenario, at least 11 free pages before a TLS session
  opens (one session ~6 pages, plus a margin of at least 5; the margin is re-checked against
  66.1's measurements). Today: 5.
* **HTTPS admin page:** `CONFIG_TLS_SERVER` on the C6-Zero sensor profile, one TLS session at
  most. The 16 KB receive record stays: Chrome does not send `record_size_limit`, and an OTA
  upload from the page sends full records. Record buffers allocated per session, not static.
* **MQTT over TLS:** a TLS 1.3 client in `net/tls.c` (sharing the record layer, key schedule
  and ChaCha20-Poly1305 with the server). Decide between certificates from the LugalOS CA
  (52.14; the broker's certificate checked against the owner's root) and TLS-PSK (no public-key
  arithmetic, a key per node in mosquitto, no forward secrecy). A client can ask for smaller
  records with `max_fragment_length` or `record_size_limit`; whether mosquitto/OpenSSL honours
  them is to be verified. If it does, the client session costs ~2 pages instead of ~6.
* Both together only if the budget allows; otherwise the MQTT client first (it runs all the
  time), with HTTPS on demand (66.5).
* *Verification:* the C6-Zero under full sensor load: the admin page over HTTPS in Chromium and
  Firefox, a signed OTA from the page, MQTT publishing to mosquitto over TLS, free pages reported
  by the load scenario at no point below the margin. `test_esp32c6.py` gains the TLS cases. Host
  tests for the client against `openssl s_server`, under ASan.
* The C6-MINI's figures recorded beside the Zero's (same chip, different persona).

### 66.4 ESP32-P4 memory layout

* LOWRAM/RAM boundary computed by the linker instead of a fixed 252 KB.
* Non-DMA, non-hot `.bss` to `.bulk_bss` (PSRAM); `s_audio_buf`'s DMA requirement verified.
* Reclaim the ROM download buffers after boot, once core 1's start-up stack is accounted for.
* Not on the RP2350: its PSRAM shares the QMI with flash and is unreachable during flash writes,
  so no stacks there.

### 66.5 On-demand services

* Services start on first connection or claim and exit when idle, returning their pages
  (`CONFIG_HTTPD_WHEN_CLAIMED` is the first instance): httpd, setup AP, mDNS responder, OTA trial.

### 66.6 Service host

* One task runs timer-driven, non-blocking jobs as callbacks: status LED, watchdog, setup
  button, power, sensor sampling, DHCP renewal, OTA trial. Each now costs 1–3 pages of stack.

### 66.7 Folding of kernel-mode endpoints

* Move the hand-written serve loops (i2c, usb_cdc, spisd, 9P, net services) onto the common
  `driver_task_t` seam first; worthwhile on its own.
* Folded mode, per endpoint, per preset: `driver_task_call()` takes a per-endpoint mutex and
  calls `serve()` in the caller's context. No task, no stack, no copies.
* Constraints: the caller's stack absorbs `serve()`'s depth (66.1 tables); lock order re-checked
  with `lockcheck`; the server's own priority is lost; drivers pinned to a hart stay unfolded.
* Guardrails 2 and 3 in place before the first folded preset.

### Review after 66.7

Measured savings of 66.1–66.7 per board decide whether 66.8–66.10 are scheduled.

### 66.8 Folding of U-mode drivers (optional)

* As 66.7 for the RP2350's U-mode drivers: an explicit isolation waiver per preset, with an ADR.

### 66.9 Build-time unikernel evaluation (optional)

* Whatever overhead remains after 66.7/66.8 (scheduler, channel tables, remaining tasks), and
  whether a further step is worth a second semantics for every service. Expected: folding covers
  most of it.

### 66.10 ESP32-C3 budget target (optional)

* A C6 or QEMU rv32 preset with `palloc` capped to a C3-equivalent budget, running the sensor
  profile's load scenario. A C3 port (atomics without the A extension) only after that fits.

---

## 4. When savings are measured

| Point | What | How | Hardware |
| :--- | :--- | :--- | :--- |
| End of 66.1 | **Baseline**: static RAM per subsystem, free pages under load, stack high-water per task, page owners | sizereport baselines; hardware suites' load scenarios | yes, once per board |
| Every commit from 66.1 on | Static RAM delta per preset | `sizereport --check` in `tests/runner.py` | no |
| End of 66.2 | Static savings of profiles and the dev split (exact per preset) | sizereport against the 66.1 baseline | no |
| End of 66.3 | Stack pages saved (exact from the table change); free pages under load (confirms the margins) | sizereport; load scenarios | yes |
| End of 66.3b | Free pages on the C6-Zero under load, with no session and with each TLS session open; record buffer size the broker accepted | load scenario plus TLS cases in `test_esp32c6.py` | yes (C6-Zero) |
| End of 66.4 | LOWRAM free (static), heap pages gained (runtime) | sizereport; `/proc/meminfo` on Tab5, LCD-7B, NANO | yes |
| End of 66.5 | Free pages idle vs. with each service active | load scenarios, idle and active variants | yes |
| End of 66.6 | Pages saved by retired tasks | load scenarios | yes |
| End of 66.7 | Pages and buffers saved per folded endpoint; folded vs. unfolded twin | load scenarios on both twins | yes (QEMU for the twin) |

QEMU gives runtime numbers for the generic parts (scheduler, 9P, channels, folding) without a
board; radio, display and sensor numbers need the boards.
