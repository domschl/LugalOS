# LugalOS Testing Framework & QA Guide

LugalOS maintains a layered testing framework spanning host unit tests, AddressSanitizer (ASan) memory safety harnesses, automated QEMU regression suites, and physical hardware-in-the-loop (HIL) silicon verification.

---

## 0. What To Run When: Test Tiers (the development and commit rule)

The layers below say *what kinds* of tests exist. The tiers say *how much* of them a change needs. During development and bug fixing only the smallest reasonable delta is retested; the full all-platform run is kept for major changes and for closing a milestone (owner, 2026-11-11, after phase 77.1 spent most of its time on full re-runs after small changes).

| Tier | When | What | Time |
| :--- | :--- | :--- | :--- |
| **0** | every edit | build the affected presets (`ninja -C build/<preset>`); the matching self-test, generator `--check`, or `tests/quick.py --run "<cmd>"` | seconds to 1 min |
| **1** | a change is done | `tests/quick.py <group>...` on rv32 (or `--target rv64` / `rv32-fold` where the change is architecture-specific): only the groups the [area map](#area-map) names | 1-3 min |
| **2** | the change touches a board's code | only that board, only the matching tests: `tests/hw/test_esp32p4.py --only NAME`, `test_rp2350.py --only NAME`, `test_visionfive2.py --only NAME` | minutes per board |
| **3** | closing a milestone; before a release; after a major or cross-cutting change | everything: `tools/precommit.py`, `tests/runner.py` (all targets), the full board suites | about an hour, plus the boards |

**Every commit**, whatever its tier, passes `python3 tools/precommit.py` (about 3 minutes): every preset in `build/` builds with no warning, no static RAM growth past the baselines (`tools/sizereport.py`), no documentation or generated-table drift, every shared static has its ownership verdict (`tools/racescan.py`). Plus the tier 0-2 runs for what the commit changes, named in its message.

**Tier 3 is required** before a milestone is marked done, before a release, and after changes to the shared foundations -- the scheduler, locks and memory domains, the console and its input path, `ugui`, the screen and the compositor, the VFS, the build system, the test harness itself. A milestone closes on a tier 3 pass.

**When tier 3 finds what a smaller tier should have caught**, the area map or a group was wrong: fix the map (or move the check into a group) in the same commit as the fix. The tiers are meant to be tuned this way.

### Area map

What a change in an area needs at tiers 1 and 2. `tests/quick.py --list` shows the groups; they are `tests/qemu_groups.py`'s, the same checks `tests/runner.py` runs.

| Area (files) | Tier 1: `quick.py` groups | Tier 2: boards, `--only` |
| :--- | :--- | :--- |
| Screen, ribbon, vterms (`drivers/screen.c`, `vtterm.c`, `ribbon.c`, `lcdterm.c`, `kernel/vterm.c`) | `screen` | each panel board: `titlebar_menu`, `stacks_fullscreen` |
| `ugui`, views (`drivers/ugui.c`, `view.c`, `launcher_view.c`, `lispui_view.c`) | `screen`, `settings` | P4: `view_and_mouse`, `touch_launcher`; RP2350: `hello_ugui` |
| Settings, paper warmth, the Settings app (`kernel/settings.c`, `drivers/paper.c`, `settings_view.c`) | `settings` | `settings` on the P4s, `settings_app` on the RP2350 and the VF2 |
| Reader, text sizes, Markdown (`drivers/reader_view.c`, `font_serif_faces.c`, `fs/markdown.c`) | `reader` | P4: `reader` |
| Keys, USB input (`kernel/keyseq.c`, `drivers/usb*.c`, `kernel/console.c` input) | `input`, `screen` | the board whose input changed |
| Editor, printk (`kernel/editor*.c`, `kernel/printk.c`) | `editor` | -- |
| A panel driver (`drivers/dsi_esp32p4.c`, `lcd7_rp2350.c`, `dc8200_jh7110.c`) | `screen` | that board: `settings` / `settings_app` (colours), its panel tests |
| Anything else | the closest group, and `--run` its self-test | the boards it runs on |

An area with no group yet runs its own self-test through `quick.py --run`, and gets a group when it is next worked on: its checks move from `tests/runner.py` into `tests/qemu_groups.py`, where both tiers use them.

---

## 1. Test Layers

```
+------------------------------------------------------------------------+
|                        LAYER 4: HARDWARE SILICON                        |
|   tests/hw/test_rp2350.py   *   tests/hw/test_esp32p4.py               |
|   tests/hw/test_esp32c6.py  *   tests/hw/lcdterm_shots.py (PBM diffs)  |
\-----------------------------------^------------------------------------+
                                    | Silicon Verification
+-----------------------------------+------------------------------------+
|                    LAYER 3: AUTOMATED QEMU RUNNER                       |
|   tests/runner.py (750 tests: RV32, RV64, RV64 SMP, RV32 folded twin) |
|   Preemption * Memory Domains * Copy-Always Channels * FAT32 * Net * VM|
\-----------------------------------^------------------------------------+
                                    | Emulation & Architecture
+-----------------------------------+------------------------------------+
|                    LAYER 2: HOST HARNESSES (ASan/UBSan)                 |
|   tests/host/ (FAT32 engine, chibicc, 9P protocol, embedded IP stack)   |
\-----------------------------------^------------------------------------+
                                    | Memory Safety & Host Sanitizers
+-----------------------------------+------------------------------------+
|                     LAYER 1: PURE ALGORITHMIC TESTS                     |
|   tests/test_lisp_cas.py (Exact arithmetic, limits, Gaussian rationals)|
|   tests/test_cas_simplify.py (AST canonical forms, polynomials)        |
|   tools/gendocs.py --check (Documentation parity verification)         |
\------------------------------------------------------------------------+
```

---

## 2. Layer 1: Pure Algorithmic Unit Tests

Fast host-based Python test suites verifying core mathematical, algebraic, and documentation algorithms:

```bash
# Verify exact arithmetic, complex numbers, integrals, limits, and matrices
python3 tests/test_lisp_cas.py

# Verify canonical CAS simplification and polynomial factorization
python3 tests/test_cas_simplify.py

# Verify zero documentation drift between Markdown and on-device help
python3 tools/gendocs.py --check
```

---

## 3. Layer 2: Host Harnesses with AddressSanitizer & UBSan

To ensure memory safety and catch buffer overflows, memory leaks, or undefined behavior before flashing hardware, LugalOS compiles core standalone subsystems natively for Linux using `clang`/`gcc` with `-fsanitize=address,undefined`:

* **FAT32 Engine Harness**: Tests cluster allocation, directory traversal, and corrupt partition recovery under aggressive fuzzing.
* **`chibicc` Compiler Harness**: Compiles C test suites natively, verifying parser correctness and abstract syntax trees.
* **9P2000 Protocol Harness**: Tests wire serialization, packet framing, and auth handshakes.
* **IP Stack Harness**: Fuzzes TCP/UDP packet processing and reassembly queues.

---

## 4. Layer 3: Automated QEMU Regression Runner (`tests/runner.py`)

The primary continuous integration and regression suite is [`tests/runner.py`](file:///home/dsc/Source/gith/domschl/lugalos/tests/runner.py):

```bash
# Run the complete automated test suite
python3 tests/runner.py
```

### Coverage
The runner executes about **750 test cases** (750 at the end of phase 66) across four virtual targets:
1. **RV32 NOMMU Target**: Validates PMP memory domain isolation, cooperative/preemptive task switches, and device driver confinements.
2. **RV64 MMU Target**: Validates Sv39 3-level page tables, virtual address space creation and destruction, and 64-bit bignum arithmetic.
3. **RV64 SMP Multicore Target**: Validates dual-hart scheduler lock hand-offs, atomic `amoswap` contention, and cross-hart thread synchronization.
4. **RV32 Folded Twin** (`rv32-fold`): the RV32 core suite again, with the `uart`, `blk` and `i2c` endpoints folded (no driver tasks), and a check that they are folded and still answer. A folded build must behave like its unfolded twin ([ADR 008](../history/adr/008_folded_endpoints.md)). Built with `cmake --preset rv32-fold`, skipped when absent.

The static checks run first: `tools/racescan.py --check` (ownership of shared statics), `tools/sizereport.py --all --check` (static RAM against its baseline) and `tools/servecheck.py` (no direct calls to a driver's `serve()`: only the framework may, so an endpoint can be folded).

---

## 5. Layer 4: Hardware-in-the-Loop Silicon Suites (`tests/hw/`)

Hardware tests communicate with physical development boards over serial and USB CDC interfaces. They are safe to run speculatively: if a target board is not connected, the test skips cleanly rather than failing.

### The Test Machine
* **Serial ports:** the user running the suites is in the group that owns `/dev/ttyACM*` (`uucp` on Arch, `dialout` on Debian and Ubuntu).
* **One inbound port: TCP 1883.** Boards on the LAN connect to the test broker the suites start on this host (`tests/mqttbroker.py`), always on 1883. Everything else is this host connecting out to the boards. A local mosquitto must not be running during the suites, since it would hold the port.

### Running Silicon Suites
```bash
# Test RP2350 microcontrollers (RP2350-LCD-7, Pico 2, Pico 2 W)
python3 tests/hw/test_rp2350.py

# Test ESP32-P4 boards (Tab5, LCD-7B, or NANO)
python3 tests/hw/test_esp32p4.py --board tab5
python3 tests/hw/test_esp32p4.py --board lcd7b

# Test ESP32-C6-Zero: what is running, or this build loaded first (overwrites slot A:
# restore with tools/c6flash.py build/esp32c6 --only slot)
python3 tests/hw/test_esp32c6.py
python3 tests/hw/test_esp32c6.py --clobber-slot-a

# Test CYW43439 Wi-Fi link and network traffic, and setup mode on a claimed node
uv run --project host/p9lib python3 tests/hw/test_wifi.py 192.168.178.171 --key HEX

# The phone setup, end to end, with a spare Wi-Fi interface on this machine as the phone.
# WIPES THE NODE'S OWNER PART (grants, owner key, Wi-Fi); the new owner key goes to
# ~/.config/lugalos/keys/NAME.p9key
uv run --project host/p9lib --with pyserial python3 tests/hw/test_wifi.py 192.168.178.171 --key HEX \
    --claim-cycle --console /dev/serial/by-id/usb-LugalOS_...-if00 --wifi-iface wlp6s0
```

`--key` is any key the node accepts: a grant, or a `p9key` set at its console for the run (clear it
afterwards with `p9key clear`: while set it overrides the stored grants). The claim cycle clears the
console key itself before it resets the owner, so the final check really uses the new owner key.

### RAM Ledger

Where each preset's RAM goes, in two halves that group by the same subsystems (`tools/ramsubsys.py`):

* **Static** (`.bss`, `.data`, code in SRAM, and `BULK_BSS` in PSRAM): `tools/sizereport.py`, read off the ELF.
  Each preset has a baseline in `tools/ram_baselines/<build dir>.json`, and `tests/runner.py` fails if any
  built preset grew past its own. Growth that is intended is re-baselined, so the numbers land in the diff:

  ```bash
  python3 tools/sizereport.py build/esp32c6/lugalos.elf --subsystems   # one preset, by subsystem
  python3 tools/sizereport.py --all --check                            # every preset built here
  python3 tools/sizereport.py --all --update                           # re-baseline after intended growth
  ```

* **Runtime** (pages from the page allocator): `tools/ramledger.py` reads `/proc/meminfo`, `/proc/ps` and
  `/proc/pages` from a running board and names every page run's caller from the ELF. It refuses an ELF
  whose build id is not the one the board runs. Task stacks are listed with their high-water marks.

  ```bash
  python3 tools/ramledger.py --c6 zero --elf build/esp32c6/lugalos.elf --wait-tasks mqttsrv,httpd
  python3 tools/ramledger.py --p4 tab5 --elf build/esp32p4-tab5/lugalos.elf --json tab5.json
  python3 tools/ramledger.py --rp2350 /dev/serial/by-id/...-if00 --elf build/rp2350-wifi/lugalos.elf
  ```

  `--wait-tasks` waits until the named services run, so the figures describe the board under its normal
  load rather than just after boot. `--load HOST` then drives the node from this host for `--load-secs`
  (60) before the capture: two threads fetching the admin page over HTTP and HTTPS, one opening 9P
  sessions (`Tversion`, no grant needed). High-water marks and `Pages Peak` keep what the load drove
  them to. It does not cover paths that need an owner key (`wss://` sign-in, OTA upload). `--check` also fails when a page in use has no run in `/proc/pages`
  or a stack is above `--max-stack-pct` (90) of its size. Recorded runs are kept in
  `tools/ram_baselines/runtime/`: `<preset>.json` under load, `<preset>-idle.json` without.

The silicon suites (`test_esp32c6.py`, `test_esp32p4.py`, `test_rp2350.py`) end with the same check
(`RAM ledger: every page owned, no stack above 90%`), run after the rest of the suite has driven the
board's tasks.

**A smaller chip's budget:** `LUGALOS_HEAP_CAP_PAGES=N` caps the page allocator at N pages, so a board can be run on the heap a smaller chip would leave. It's a measurement setting, not a configuration; `/proc/config` shows `HEAP_CAP_PAGES=` when it is set. For example, an ESP32-C3's budget on a C6-Zero:

```bash
cmake --preset esp32c6-release -B build/esp32c6-c3budget -DLUGALOS_HEAP_CAP_PAGES=44
ninja -C build/esp32c6-c3budget
```

### Visual Regression Testing (`lcdterm_shots.py`)
For display presets (RP2350-LCD-7 and ESP32-P4-LCD-7B), [`tests/hw/lcdterm_shots.py`](file:///home/dsc/Source/gith/domschl/lugalos/tests/hw/lcdterm_shots.py) plays automated shell scenarios, fetches PBM screenshots over 9P, and compares them **pixel-by-pixel** against golden baseline images (with the RTC clock area masked):

```bash
# Record baseline screenshots on RP2350 terminal:
python3 tests/hw/lcdterm_shots.py --out golden_dir

# Compare current build against baseline:
python3 tests/hw/lcdterm_shots.py --compare golden_dir

# Run ribbon scenario (multi-window, Rule 30/90, chess, CAS plot) on LCD-7B:
python3 tests/hw/lcdterm_shots.py --scenario ribbon --board lcd7b

# The views: the launcher, its control gallery, a view of its own type and the mouse cursor (dev image):
python3 tests/hw/lcdterm_shots.py --scenario views --board tab5
```

On the Tab5 and the LCD-7B the shots come back over the console's own wire at 921,600 baud; a full RGB565 frame takes a while, so a scenario with several shots runs for some minutes.

#### Notes on Visual Scenarios & Escapes:
* **Screenshots on the card:** each shot is written as `/sd0/lcdterm-shot-<n>.pbm` and fetched from there.
* **Prerequisites for LCD-7B:** The ribbon scenario requires `/sd0/demos` and `/sd0/cas` (sync them with `python3 tools/p9sync.py --p4 lcd7b --src build/esp32p4-lcd7b/sd_root`).
* **Console Hotkey Emulation:** From a serial connection, the GUI Cmd hotkeys map to:
  * `ESC [ 1 ; 9 C` / `ESC [ 1 ; 9 D`: Cmd+Right / Cmd+Left (`;13` adds Ctrl).
  * `ESC [ <codepoint> ; 9 u`: Cmd+character (e.g. Cmd+1 is `ESC [ 49 ; 9 u`, from `kernel/keyseq.h`).
* **Serial Disambiguation:** When an ESP32-P4-NANO is connected simultaneously, run `test_rp2350.py --no-uart` to prevent its CP2102 adapter from being mistaken for an RP2350 UART dongle.
