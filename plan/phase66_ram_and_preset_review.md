# Phase 66 — RAM, Presets, and Folding: Scaling LugalOS Down

**Status: scoped 2026-11-03, not started. Independent of phase 52.**
Milestone scheme: `66.1`, `66.2`, …

* **Scheduled:** 66.1–66.3 (measure, split, right-size), 66.3b (TLS on the C6-Zero sensor node),
  then 66.4, 66.6, 66.7 (P4 layout, service host, folding of kernel-mode endpoints). 66.5 (on-demand
  services) dropped: see there.
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
| Swapping processes out | **Not feasible** without an MMU: no relocation, kernel tasks hold pointers into their stacks. On-demand services were the planned replacement (66.5), dropped: a service reserves at start or allocates per request. |
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

* **DONE 2026-11-03.**
  *As built:* `palloc` records the return address into its caller on the first page of every run
  (`g_fast_owner` in SRAM, 0.5 KB; `g_bulk_owner` in `BULK_BSS`), and `/proc/pages` lists each run with
  its zone, size, caller and, for a task stack, the task. `tools/ramledger.py` captures `/proc/buildid`,
  `meminfo`, `ps` and `pages` from a board (`--c6`, `--p4`, `--rp2350`, or a saved `--capture`),
  refuses an ELF of another build, names the callers with `addr2line` and sums pages by subsystem;
  `--load HOST` drives HTTP, HTTPS and 9P sessions first. `tools/ramsubsys.py` is the one grouping,
  used by it and by `tools/sizereport.py`, which now reports by subsystem, names symbols without line
  info by section (the radio blob's `.radio_bss`), and has `--all --check/--update`. Baselines moved to
  `tools/ram_baselines/<build dir>.json`, all 15 presets; `tests/runner.py` checks them. The silicon
  suites end with a ledger case (every page in use owned, no stack above 90%). `ps` already showed
  isolation and stack high-water; no new view was needed. `doc/dev/kernel/architecture.md` corrected
  (section 1.1's table). Racescan clean on its five builds.
* *Measured* (`tools/ram_baselines/runtime/`; idle = steady state after boot, loaded = after 60 s of
  `--load`):

  | Board | Pages free idle / loaded / total | Stacks: pages held / KB used (loaded) | Next largest owners (pages) |
  | :--- | :--- | :--- | :--- |
  | C6-Zero (radio, BME280, MQTT publishing to nalanda, httpd) | 8 / **5** / 68 | 33 / 28.6 KB | radio 20, TCP 4, httpd 2 |
  | Pico 2 W (unclaimed: setup AP and worker, MQTT; httpd off) | 21 / 19 / 80 | 36 / 26.8 KB | `/ram0` 16, TCP 4 |
  | Tab5 (C6 link, HTTPS, display) | 24 / 24 / 93 | 35 / 27 KB | LLP `take()` 18, TCP 8, httpd 5 |
  | LCD-7B (C6 link, HTTP, display) | 10 / 10 / 71 | 27 / 16 KB | LLP `take()` 18, TCP 8, httpd 5 |

  Every page in use is accounted for on all four. Stacks are the largest owner everywhere, at 12–20%
  use. Candidates for 66.3: the Pico 2 W's `/ram0` (64 KB of SRAM on a board without PSRAM), the
  setup tasks (36 KB of stack held while unclaimed, 3 KB used), LLP's 18 pages on the P4s, and the
  stacks generally. Under 88 HTTPS sessions the Tab5's httpd peaked at 2.4 KB of its 24 KB stack;
  phase 52's 11 KB was on paths the load does not drive (`wss://` sign-in, OTA upload, which need an
  owner key), so 66.3 measures those before it sizes httpd.
* *Found on the way:*
  * The C6-Zero `esp32c6-zero-86c2` had no broker stored, so it published nothing; nalanda
    (192.168.178.32:1883) stored with `mqttcfg` at the owner's request.
  * *Fixed 2026-11-03:* the Pico 2 W had no A/B partition table (`layout=none`). Partitioned by
    1200-baud touch and `ptable.uf2`; the kernel UF2 then failed silently: it is an A2 chip (erratum
    RP2350-E10). Every RP2350 `lugalos.uf2` now starts with the E10 block (`tools/uf2_e10.py`), aimed
    at an unused sector (`LUGALOS_E10_SECTOR_PHYS`) rather than the SDK's end of flash, which on a
    4 MB part is the identity sector. `tools/rp2350_ptable.py` encodes the table itself where
    `picotool` cannot run; `tests/hw/flash.py --ptable` does the first-time install. Verified: the
    identity survived, `layout=partition`, and the next update went to slot A.
  * *Fixed:* `p4flash.py` and `c6flash.py` write a boot record naming slot A active instead of
    erasing the records (`tools/lota.py bootrec_slot_a()`), so `/dev/ota` says `active=a` after a
    cable install and `test_esp32p4.py`'s update-state case passes (Tab5).
  * *Fixed:* the hardware test broker listens on 1883, the one inbound port a test machine opens
    (`doc/dev/testing.md`); the Tab5's Wi-Fi case passes.
  * *Fixed:* `third_party/crypto/p256-m` excludes its 32-bit Arm assembly on AArch64 (noted in
    `ORIGIN.md`); the host harnesses build and pass on an ARM64 host.

### 66.2 Board/profile split, dev/production split (scheduled)

* **DONE 2026-11-03.** *As built:* `cmake/lugalos_layers.cmake` defines the two layers and checks
  them (a key in the wrong layer stops the configure). Board files keep hardware facts, capabilities
  (`NETWORK_CLAIM`: a setup access point) and hardware-specific tuning (`CHESS_HOT_RAM`, the RP2350's
  XIP cache), and name the drivers their hardware needs (`LUGALOS_ENABLE_SPISD`, ...; drivers default
  OFF). Seven profiles (`qemu`, `workstation`, `sensor`, `radio`, `chess`, `clock`, `gateway`) own the
  software flags and the policy keys (Lisp pools, buddy arena, TLS, httpd-when-claimed, RAM disk, chess
  table, NTP server, the clock's behaviour). Presets name a board and a profile, plus SMP. The C6's
  Lisp-light pools became the C6 default in `user/lisp/lisp.c`. `/proc/config` starts with `PROFILE=`
  and `DEV=`. `LUGALOS_DEV` (ON by default): a release image (`-DLUGALOS_DEV=OFF`) leaves out 73
  shell commands (self-tests, isolation checks, scheduler tests, demos, benchmarks) and their helpers;
  diagnostics stay, and so does `/proc/pmp`'s live U-mode probe (ADR 007's fence). The silicon suites
  report dev-only cases as SKIPPED on a release image.
* *Verified:* every preset whose policy did not change is byte-identical (`sizereport --all`, +0);
  QEMU 535/535; the Tab5 53/53 on the new build; the C6-Zero release image 22/22 (9 skipped).
* *Unifications (owner's choice: one policy per profile), measured:* the NANO gets the workstation
  Lisp pools (65536 nodes, +1.4 MB PSRAM) and the 512 KB chess table; the P4s get `ed` (+108 B); the
  Pico 2 W and rp2350-sensor lose `cc`, `ed` and the games (-562 B) and gain httpd-when-claimed.
* *Found on the way:* the NANO's LOWRAM could not take the 8 KB Lisp mark bitmap, so on the P4 the
  bitmaps moved to PSRAM (a 66.4 item done early): -8.5 KB SRAM on the LCD-7B and Tab5; GC on the
  Tab5, 300000 short-lived pairs, 1068 -> 1099 ms (+3 %), worst pause 19.2 -> 20.4 ms.
* *Release against dev, the C6-Zero:* static RAM 177.0 -> 169.8 KB, code -71 KB, heap 68 -> 69 pages;
  under the loaded scenario 6 free pages (dev: 5). Small, as expected: test code runs on demand and
  holds no pages; the gain is its statics.
* *Moved to 66.3:* turning the remaining hard-coded sizes into profile keys (`MAX_TASKS`, klog ring,
  9P links/msize/waiters, endpoint buffers, vterms, the reader view on headless boards), each with
  the value it is measured to need.

* `board-*.cmake`: silicon facts only. New `profile-*.cmake`: personas (terminal, sensor,
  gateway, radio modem, appliance) owning feature selection and every size: `MAX_TASKS`, klog
  ring, 9P links/msize/waiters, Lisp pools, endpoint buffers, vterms, reader view. A preset is a
  board plus a profile.
* One dev flag for self-tests, stress, intruder tests and demo commands. The hardware suites
  either run against a dev image or skip what a production image lacks.

### 66.3 Right-sizing per profile (scheduled)

* **DONE 2026-11-03 (the C6-Zero target met).** *Free pages under the loaded scenario (66.1):*

  | Board | 66.1 | Now | What did it |
  | :--- | :--- | :--- | :--- |
  | C6-Zero (release image) | 5 / 68 | **14 / 74** | release image, sizes below, radio stack |
  | Pico 2 W (dev image) | 19 / 80 | **40 / 84** | sizes below, no `/ram0` (16 pages) |
  | LCD-7B | 10 / 71 | 11 / 71 | `sensor_hub` stack |
  | Tab5 | 24 / 93 | 25 / 93 | `sensor_hub` stack |

  *As built:*
  * Profile keys `CONFIG_KLOG_RING_KB`, `CONFIG_SHELL_HISTORY`, `CONFIG_MAX_VTERMS` (defaults 8 KB,
    10, 8); the sensor and radio profiles take 4 KB, 2, 4.
  * On boards without a panel the reader's one instance has 8 links, 8 contents entries and 2 history
    steps (~2 KB instead of ~7.5 KB).
  * The C6's `.udata_kobj` region is 4 KB (3.1 KB used; 8 KB before), and outgrowing it fails the link.
  * Stacks: `sensor_hub` 3 -> 2 pages (1.0 KB high-water on four boards); the radio's main U-mode
    stack 16 -> 8 KB (1.1 KB used through boot, scans, a rejoin and the loaded hour). The others keep
    their sizes: each was set for a path the loaded scenario does not drive, and the code says which
    (httpd 7-11 KB on a 9P read, the setup worker 11.9 KB in a claim, p9srv filling 8 KB with a remote
    mount nested in a request, orient's 16 KB repaint, the setup button's 4.9 KB reset press).
  * `init.lisp` mounts no `/ram0` on a sensor or radio node (it reads `PROFILE=` from `/proc/config`;
    an older kernel's first line differs, and the old rules apply).
  * Static RAM: sensor and radio presets -15.4 KB, panel-less RP2350s -5.5 KB, others unchanged.
* *Measured, not adopted:* `-ffunction-sections -fdata-sections` everywhere: -1.7 KB static and -24 KB
  code on the C6, -1.1 KB and -15 KB on the Tab5, no heap page on either. It changes every section's
  placement for a flash-only gain; left for a phase that needs the flash.
* *Measured, not changed:* the radio arena (64 KB, one NAPOT region): the blob's heap peaks at ~38 KB
  of its ~54 KB, so 32 KB cannot hold it and 48 KB (two regions) would leave under 1 KB.
* *Not done, no measured need yet:* `MAX_TASKS`, the 9P link count/msize/waiters and the endpoint
  buffers as profile keys; session buffers allocated at session start; LLP's 18 pages on the P4s.
* *Found:* on the P4 the static RAM 66.2 freed (the mark bitmaps, -8.5 KB) is in LOWRAM, whose
  boundary with the heap is fixed: it does not reach the heap until 66.4 makes the boundary move.

* Candidates measured in 66.1/66.2: task stacks (the C6-Zero holds 33 pages of stack and uses 28.7 KB
  under load); the Pico 2 W's `/ram0` (16 pages of SRAM); the setup tasks' stacks while unclaimed;
  LLP's 18 pages on the P4s; the C6's `.udata_kobj` NAPOT region (8 KB, 3.1 KB used: 4 KB would do);
  `-ffunction-sections -fdata-sections` everywhere (today only a few files), so unreferenced functions
  and data in shared files drop out.

* Task stacks from the 66.1 tables plus a fixed margin, per board.
* The sizes 66.2 made configurable, set per profile from 66.1's measurements.
* Rarely used session buffers (scratch, per-connection rx/tx) move from `.bss` to allocation at
  session start where a runtime refusal is acceptable (the `kernel/ota.c` precedent).

### 66.3b TLS on the C6-Zero sensor node (scheduled)

* **DONE 2026-11-03.** Both halves, on the C6-Zero release image, under the loaded scenario:

  | | Free pages (of 74) |
  | :--- | :--- |
  | HTTPS up, MQTT over TLS connected, idle | 10 |
  | lowest, through 49 HTTPS sessions, 9P and MQTT publishing over TLS | 5 (the margin) |

  *HTTPS:* `CONFIG_TLS_SERVER` in the sensor profile (+42 bytes static). httpd now takes a session's
  ~21 KB of records at accept and returns them at close (all TLS boards): an idle HTTPS server holds
  one page. Verified with curl and `openssl s_client` against the CA (TLS 1.3, ChaCha20-Poly1305,
  handshakes 0.36-0.64 s on the C6, 172 ms on the Tab5); `test_esp32p4.py`'s HTTPS case on the Tab5.
  *MQTT over TLS:* a TLS 1.3 client in `net/tls.c` (the server's record layer and key schedule, made
  role-aware; ClientHello, ServerHello, EncryptedExtensions, Certificate, CertificateVerify, Finished;
  session tickets ignored). It asks for 4 KB records (`max_fragment_length`; OpenSSL and mosquitto
  take it), so a connection holds three pages, taken at connect and returned at close, with the
  session state in them (+4 bytes static). `kernel/x509.c` `tls_cert_check()`: the broker's
  certificate signed by the CA and naming the address dialled (subjectAltName, else CN); dates not
  checked. The CA is the owner field `IDSTORE_FIELD_TLS_CA` (its 64-byte key), on `/dev/tlsca`
  (admin writes). `mqttcfg tls://IP` / `mqtt connect tls://IP` (8883 unless given); the TLS flag is a
  trailing byte of the MQTT field, written only when set. `CONFIG_TLS_CLIENT` in the sensor,
  workstation and qemu profiles. `host/ca`: `issue` also installs the root, `trust` alone, `server`
  makes a broker's key and certificate. Admin guide section 4.
  *Verified:* `tls_host` against `openssl s_server` under ASan (by name, by address, a foreign CA and
  a name the certificate lacks); a QEMU test (`test_mqtt_tls`: no CA refused, the CA's broker gets a
  PUBLISH, another CA's and another address's refused); the C6-Zero against mosquitto 2.1.2 on this
  host (TLS 1.3, ChaCha20-Poly1305, publishing every 10 s, keepalive answered through the load).
  *Found on the way:* a record `tls_write()` could not hand to TCP at once stayed queued until the
  next write: the client's CONNECT, two runs in three under QEMU (seen in a packet capture: sent
  only with the close, 8 s later). `tls_read()` now sends what is owed first. The handshake has its
  own allowance (20 s; ~8 s under QEMU), and the CONNACK still gets the caller's timeout after it.
  *Not done:* a fuzz of the client's parsing of a hostile server flight (the server side has one).
  The C6-Zero's key and certificate, and the root on its `/dev/tlsca`, come from a throwaway test CA:
  `lugalos-ca issue --tcp esp32c6-zero-86c2.local --rekey` from the owner's CA replaces them. Its broker
  is nalanda again (plain 1883): nalanda has no TLS listener.

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

* **DONE 2026-11-03 for the heap; the rest measured and left.**
  * *The ROM's download buffers on rev v3 (the LCD-7B):* the heap now runs to `P4_HEAP_TOP` = 0x4ffba000,
    over the buffers (0x4ffa96b8..0x4ffbafc0) that the ROM uses only in download mode and that an IDF
    application has as ordinary RAM (`APP_USABLE_DIRAM_END`). Loadable sections still stop below them
    (the loader runs there); the ROM's start-up stacks and its run-time data above stay untouched.
    LCD-7B heap 71 -> 88 pages; under the loaded scenario **28 of 88 free** (11 of 71 before).
    Verified: `test_esp32p4.py` 53/53; `zonetest`'s new step 7 takes every free fast page up to the
    last reclaimed one (0x4ffb9000), holds a pattern in each and gives them back (0 wrong); flash
    writes, an OTA state check, Wi-Fi through the C6 and the SD card after it. Rev v1.3 (Tab5, NANO)
    has no such region: its download buffers are in LOWRAM, under `.bss` and the stacks.
  * *The LOWRAM/RAM boundary:* not moved. LOWRAM's unused tail is 1-2 pages on each P4 (NANO 4.1 KB,
    LCD-7B 9.4 KB, Tab5 9.1 KB). On v1.3 LOWRAM may hold only NOLOAD sections and loadables must start
    at 0x4ff40000, so the tail could join the heap only as a second SRAM zone; on v3 the two regions
    could merge. Worth it only together with moving statics to PSRAM, and no P4 is short of heap now
    (NANO 93 pages, Tab5 25 of 93 free, LCD-7B 28 of 88 under load).
  * *Statics to PSRAM:* the Lisp mark bitmaps moved in 66.2 (8.5 KB). Others (`g_chess_pos` 8.4 KB,
    9P waiters 8 KB, the log ring 8 KB, shell history 5 KB, the sensor caches 5 KB) would add LOWRAM
    headroom for future statics, not heap; left until a static needs the room. `s_audio_buf` (24 KB)
    was not examined.
  * *Found on the way:* `zonetest`'s BULK_BSS check assumed the RP2350's PSRAM window (0x11000000)
    and failed on every P4; it now uses the board's.

* LOWRAM/RAM boundary computed by the linker instead of a fixed 252 KB.
* Non-DMA, non-hot `.bss` to `.bulk_bss` (PSRAM); `s_audio_buf`'s DMA requirement verified.
* Reclaim the ROM download buffers after boot, once core 1's start-up stack is accounted for.
* Not on the RP2350: its PSRAM shares the QMI with flash and is unreachable during flash writes,
  so no stacks there.

### 66.5 On-demand services -- dropped (2026-11-03, owner's decision)

Stopping an idle service frees pages only until something else takes them; the service's next start can
then fail -- the admin page unreachable when it is wanted, under a load nobody can reproduce. A service
that does not fit should fail at boot, where it is seen and tested. The rule instead, for every service:

* **reserve at start** what it needs to run, or
* **allocate per request** with a clean refusal when there is no room (a refused connection the client
  retries) and a margin measured under the loaded scenario (66.1),
* never stop and restart on demand.

66.3b's per-session TLS records follow the second form (the C6-Zero's lowest point under load is the
5-page margin). A task whose work is over for good (the setup access point once a node is claimed) may
still exit and return its pages: nothing asks for them again. That part moves to 66.6.

### 66.6 Service host

* One task runs timer-driven, non-blocking jobs as callbacks: status LED, watchdog, setup
  button, power, sensor sampling, DHCP renewal, OTA trial. Each now costs 1–3 pages of stack.
* Tasks whose work is over for good exit and return their pages (from 66.5): the setup access point
  and its worker once the node is claimed (36 KB of stack held on an unclaimed Pico 2 W).

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
| End of 66.6 | Pages saved by retired tasks | load scenarios | yes |
| End of 66.7 | Pages and buffers saved per folded endpoint; folded vs. unfolded twin | load scenarios on both twins | yes (QEMU for the twin) |

QEMU gives runtime numbers for the generic parts (scheduler, 9P, channels, folding) without a
board; radio, display and sensor numbers need the boards.
