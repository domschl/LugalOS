# Phase 50 — Unified Documentation System, On-Device Help, and Codebase Comment Rationalization

**Status: COMPLETE.** Milestones M50.1–M50.6 completed, verified, and merged to `main` (shipped as 0.17.0). Full regression test suite passing (490/490 tests, 100%).  
Branch: `main` (version 0.17.0).  
Follows: Phase 47 (ESP32-P4 LCD7B Ribbon), Phase 48 (Lisp Expansion), Phase 49 (C6 Network Nodes).

---

## 1. Executive Summary & Strategy

Across phases 1 through 48, LugalOS has evolved from a minimal RISC-V proof-of-concept into a dual-core, capability-isolated microkernel operating system running across multiple silicon targets (RP2350, ESP32-P4, ESP32-C6, QEMU RV32/RV64), featuring a Plan 9 IPC model, 9P networked filesystems, an exact Lisp Computer Algebra System (CAS), on-device C compilation, hardware sensor hubs, and a horizontal tiling window ribbon.

However, the documentation has grown ad-hoc:
1. **[`README.md`](file:///home/dsc/Source/gith/domschl/lugalos/README.md) is 2,070 lines (~131 KB)**: It combines an elevator pitch with early RP2350 breadboard wiring tables, IP stack treatises, Lisp primitive catalogues, and historical logs, while burying prominent presets like the 7" LCD terminals and ESP32-C6.
2. **Inline Comments Contain 750+ Historical "Archaeology" References**: Code comments routinely reference obsolete milestone numbers (`M3, plan/phase12...`), superseded intermediate shims, and bug histories, obscuring actual invariants and inflating context for both human developers and AI pair programmers.
3. **Missing Structured Manuals**: Only [`doc/cas_user_manual.md`](file:///home/dsc/Source/gith/domschl/lugalos/doc/cas_user_manual.md) exists as a standalone manual; operational knowledge for users, administrators, and developers is scattered across planning specs in [`plan/`](file:///home/dsc/Source/gith/domschl/lugalos/plan/).
4. **On-Device Shell Help is Monolithic & Unpaged**: `cmd_help` in [`kernel/shell.c`](file:///home/dsc/Source/gith/domschl/lugalos/kernel/shell.c) dumps over 200 lines of hardcoded `cprintf()` strings, conflating user commands with dozens of internal kernel test commands, with no topic help or terminal paging.

### Dual Audience & Dual Format Architecture

Phase 50 establishes a unified, structured documentation system serving two distinct audiences across two distinct presentation media:

```
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│                                LUGALOS DOCUMENTATION SYSTEM                             │
├─────────────────────────────────────────────┬───────────────────────────────────────────┤
│ HOST ENVIRONMENT (Markdown / Web / IDE)     │ ON-DEVICE RUNTIME (LugalOS Terminal / TTY)│
│ Read by: AI Agents & Human Developers       │ Used by: Interactive Users & Operators    │
├─────────────────────────────────────────────┼───────────────────────────────────────────┤
│ • Richly formatted GitHub-Flavored Markdown │ • Plain 80/126-column text files in /sd0/help/│
│ • Visuals: photos, screenshots, diagrams    │ • Interactive `man` & `help` topic browser│
│ • Comprehensive manuals under `doc/`:       │ • Numbered link refs `[1]`, `[2]` with    │
│   - doc/user/ (Shell, Ribbon, Chess, CAS...)│   one-key jump and history stack `[b]`    │
│   - doc/admin/ (Presets, Flashing, 9P, Net) │ • Integrated terminal-height pager (more) │
│   - doc/dev/ (Kernel, Drivers, C, Lisp, Test│ • Clean categorized shell `help` command  │
│   - doc/history/adr/ (Design Decisions)     │ • Zero RAM overhead (streamed from disk)  │
│ • High-impact, concise `README.md` (~350 L) │ • Automated host-to-device generator     │
│ • Streamlined `AGENTS.md` (Invariants/Index)│   (`tools/gendocs.py`) guarantees parity  │
└─────────────────────────────────────────────┴───────────────────────────────────────────┘
```

---

## 2. Documentation Architecture & Directory Layout

All official documentation will reside in [`doc/`](file:///home/dsc/Source/gith/domschl/lugalos/doc), organized into dedicated manuals:

```
doc/
├── README.md               # Master Documentation Index & Topic Navigation
├── assets/                 # Central repository for diagrams, photos, screenshots
│   ├── images/             # Hardware photos (chess, lcd7b, rp2350, c6)
│   └── screenshots/        # Terminal & canvas captures (from tests/hw/lcdterm_shots.py)
├── user/                   # USER MANUAL: Day-to-day operation & applications
│   ├── index.md            # User manual introduction & quickstart
│   ├── shell.md            # lsh shell: commands, syntax, jobs, pipes, devfs & procfs
│   ├── editors.md          # Full-screen Emacs editor (e), teletype line editor (ed)
│   ├── ribbon.md           # Horizontal window ribbon, touch gestures, hotkeys, canvases
│   ├── chess.md            # LugalOS Chess Computer: board controls, TM1638, Lazy SMP
│   ├── cas.md              # Computer Algebra System (migrated from cas_user_manual.md)
│   └── sensors.md          # Environmental telemetry, MQTT node configuration & data
├── admin/                  # ADMINISTRATOR MANUAL: Provisioning, deployment & operations
│   ├── index.md            # Admin overview
│   ├── presets.md          # Hardware presets & capabilities comparison matrix
│   ├── flashing.md         # Firmware flashing guides (picotool, p4flash.py, c6flash.py)
│   ├── consoles.md         # TTY mappings, dual-CDC-ACM, CH343P/CP2102, baud rates
│   ├── networking.md       # 9P networking, p9sync.py, fuse-p9 over TCP/SLIP, Ethernet, Wi-Fi
│   ├── identity.md         # Provisioning, idstore, TLS/WLAN credentials (wifi.env)
│   └── storage.md          # FAT32 SD cards, flashfs ROM disk, RAM disks, formatting
├── dev/                    # SYSTEM PROGRAMMER MANUAL: Writing software for/in LugalOS
│   ├── index.md            # Developer guide overview & architecture summary
│   ├── on_device/          # Part A: User-Space Application Development
│   │   ├── c_programming.md   # chibicc, lugal.h, libc subset, syscall ABI, ELF execution
│   │   └── lisp_scripting.md  # Scheme/Lisp dialect, primitives, macros, CAS integration
│   ├── kernel/             # Part B: LugalOS Kernel & Driver Architecture
│   │   ├── architecture.md    # Microkernel philosophy, memory domains (PMP/Sv39), IPC
│   │   ├── scheduler.md       # Priority tiers, preemption, cross-hart lock handoff, SMP
│   │   ├── logging.md         # Tokenized logging macro system, klogd, rodata dictionary
│   │   ├── drivers.md         # Driver isolation framework, IRQs, register grant windows
│   │   └── porting.md         # Bringing up a new SoC/board: linker scripts, clocks, XIP
│   └── testing.md          # Part C: Testing Framework & QA
│       ├── host_tests.md      # ASan/UBSan host harnesses (FAT32, chibicc, 9P, IP)
│       ├── qemu_runner.md     # Automated regression runner (tests/runner.py)
│       └── hw_silicon.md      # Hardware-in-the-loop suites (tests/hw/)
└── history/                # ARCHITECTURE DECISION RECORDS (ADRs)
    ├── index.md            # Index of durable design decisions
    └── adr/                # Formal records (e.g., FPU disable, lock hand-off, tokenized klog)
```

---

## 3. On-Device Help & Interactive Hypertext System

On-device documentation must run efficiently on microcontrollers with 520 KB SRAM without causing memory pressure or polluting `.bss`.

### 3.1 Two-Tier Shell Help Command
In [`kernel/shell.c`](file:///home/dsc/Source/gith/domschl/lugalos/kernel/shell.c), `cmd_help` will be restructured:
* `help`: Prints a concise, categorized overview of primary user commands (~30 lines, fits on one screen):
  * **Filesystem**: `ls`, `cat`, `cp`, `rm`, `mkdir`, `rmdir`, `df`
  * **System**: `uname`, `ps`, `top`, `meminfo`, `date`, `rtc`, `klog`
  * **Applications**: `lisp`, `e`, `ed`, `chess`, `canvas`, `screenshot`
  * **Network & 9P**: `net`, `wifi`, `mqtt`, `ntp`, `p9share`
  * **Guidance**: *"Type 'help <topic>' or 'man <topic>' for details. Type 'help test' for hardware tests."*
* `help test`: Lists the internal hardware self-tests, PMP isolation checks, and benchmark commands (`blkisotest`, `st7735isotest`, `trapselftest`, `sdbench`, etc.), keeping the default console clean.

### 3.2 Interactive Hypertext Topic Browser (`man` & `help <topic>`)
When the user executes `man <topic>` or `help <topic>` (aliased):
1. **Source**: The command opens `/sd0/help/<topic>.txt` (or falls back to `/flash0/help/<topic>.txt`).
2. **Terminal-Height Paging**:
   - Reads the terminal row count (126×34 on 7" panels, default 80×24 on serial).
   - Pauses when the screen is filled with a footer prompt:
     ```
     [Space/Enter: Next | b: Back | q: Quit | 1..9: Follow Link]
     ```
3. **Numbered Hypertext Navigation**:
   - Links in topic files are formatted as `[1] topic_name` (e.g. `See also: [1] scheduler, [2] ribbon`).
   - Pressing a single key `1`..`9` immediately pushes the current topic onto an internal history stack (depth 8) and opens `/sd0/help/<target>.txt`.
   - Pressing `b` (or Left Arrow) pops the history stack and returns to the previous topic.
   - Pressing `q` or `Esc` exits cleanly to the shell.
4. **Standalone Pager (`more <file>`)**:
   - The same paging engine is exposed as a general shell utility `more <path>`.

### 3.3 Host-to-Device Parity Generator (`tools/gendocs.py`)
To prevent documentation drift, a host Python tool [`tools/gendocs.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/gendocs.py) will:
1. Parse the master Markdown documents in `doc/user/`, `doc/admin/`, and `doc/dev/`.
2. Strip complex Markdown styling while preserving indentation and code blocks.
3. Automatically transform Markdown links `[Title](../path/target.md)` into numbered on-device cross-references `[N] target`.
4. Output cleanly wrapped plain-text files into [`tools/sd_root/help/`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root).
5. CMake automatically stages these files into `lugalos_sd.img` and `flashfs.bin`.

### 3.4 Lisp Interactive Help (`(help 'symbol)` & `(doc 'symbol)`)
In [`tools/sd_root/system/etc/stdlib.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/etc/stdlib.lisp), define `doc` and extend `help`:
- `(doc 'name)` prints the function signature, argument types, and a concise summary.
- The documentation metadata is stored in an associative list or on-disk documentation index, incurring zero SRAM overhead when idle.

---

## 4. Code Commentary Standard & Global Scrubbing Strategy

### 4.1 The LugalOS Code Commentary Standard

Comments in the LugalOS source tree must strictly follow these rules:

#### What Belongs in Source Code Comments:
1. **Invariants & Contracts**: Function preconditions, postconditions, nullability rules, and pointer ownership (e.g., *"Caller owns returned buffer"*).
2. **Hardware Quirks & Errata**: Silicon errata workarounds, precise register timing constraints, hardware revisions (e.g., ESP32-P4 v1.3 vs v3.2 memory map differences).
3. **Concurrency & Memory Isolation**: Which lock guards which structure, why a lock must be held across a context switch, PMP alignment/granularity rules.
4. **Mathematical/Algorithmic Rationale**: Explanations of fixed-point scaling, polynomial reduction logic, or zero-float algorithms.

#### What MUST Be Removed from Source Code:
1. **Milestone & Phase Bookkeeping**: Eliminate all tags like `/* M3, plan/phase12... */`, `/* 47.6, plan/phase47... */`, and `/* Y5f */`.
2. **Historical Narrative / Archaeology**: Remove discussions of past shims or superseded designs (e.g., *"Replaces the pre-B2 shim which was bookkeeping only..."*, *"Opposite of what §3 originally said"*).
3. **Changelog Comments**: Remove comments explaining bugs fixed in earlier development phases.
4. **Dead / Commented-Out Code**: Delete permanently.

#### Where Architectural Decisions Go:
Durable design justifications (e.g., *"Why M-Mode FPU is disabled"*, *"Why the scheduler lock is handed across stacks"*, *"Why tokenized dictionary logging was chosen"*) are extracted and documented in `doc/history/adr/` (Architecture Decision Records) or `doc/dev/kernel/`.

### 4.2 One-Go Global Scrubbing Strategy
As agreed, rather than multiple small increments that could cause confusion, the code comment cleanup will be executed in **one coordinated, atomic sweep**:
1. **Pre-requisite**: All ongoing feature development across all active branches is halted and committed.
2. **Sweep Execution**:
   - Scrub `kernel/` and `kernel/include/`
   - Scrub `drivers/`
   - Scrub `fs/`, `net/`, and `libc/`
   - Scrub `user/` and `arch/`
3. **Validation**: Run the complete automated test suite (`tests/runner.py`) and verify that compilation across all presets remains 100% clean with zero warnings.

---

## 5. Concise Rules for Agents (`AGENTS.md`)

[`AGENTS.md`](file:///home/dsc/Source/gith/domschl/lugalos/AGENTS.md) will be refined to serve as the concise rulebook and fast index for AI agents:
1. **Golden Architectural Invariants**:
   - Freestanding C (`-nostdlib -ffreestanding`).
   - Zero Floating Point in M-Mode (`mstatus.FS = 0`; pure fixed-point or exact integer math).
   - Zero SRAM Overhead for tables (Flash `.rodata` sorted for $O(\log N)$ binary search).
   - Copy-always message channels and capability-isolated memory domains (PMP / Sv39).
2. **Hardware Port Mappings & Reset Invariants**:
   - Strict detection via `by-id` symlinks; zero hardcoded port numbers.
   - DTR/RTS modem control rules for ESP32-P4 and ESP32-C6.
3. **Rules for Agents Maintaining Code and Documentation**:
   - **Comment Discipline**: When generating C code, agents must adhere strictly to the Commentary Standard (Section 4.1). Never introduce `phaseXX` references, milestone tags, or narrative historical comparisons.
   - **Documentation as Code**: Any PR or commit that introduces a new shell command, Lisp primitive, driver, or build preset must update the corresponding manual in `doc/` and regenerate on-device help via `tools/gendocs.py`.
   - **Pointer Index**: `AGENTS.md` provides links to `doc/dev/` for deep technical topics rather than embedding encyclopedic prose.

---

## 6. High-Impact `README.md` Overhaul

The root [`README.md`](file:///home/dsc/Source/gith/domschl/lugalos/README.md) will be reduced from 2,070 lines to ~350 lines, focused on project showcase and navigation:
1. **Hero Section**:
   - Project name, badge bar (build, tests, targets).
   - Punchy description: *"LugalOS is a bare-metal, capability-isolated RISC-V microkernel operating system featuring Plan 9 IPC, exact Lisp algebra, on-device compilation, and horizontal tiling window ribbons."*
   - Visual showcase: Hardware photos and screenshots (Chess computer photo, RP2350 7" LCD ribbon screenshot, ESP32-P4-LCD7B screenshot, photo placeholders for boards).
2. **Hardware Presets Matrix**:
   - Clean markdown table listing all supported targets:
     | Preset | Architecture | SoC / Board | Display & UI | Storage & Net | Role |
     | :--- | :--- | :--- | :--- | :--- | :--- |
     | `rp2350-terminal` | RV32 Hazard3 | Waveshare RP2350-LCD-7 | 7" 800×480 Ribbon, Touch | SD, Dual CDC 9P | Workstation Terminal |
     | `esp32p4-lcd7b` | RV32 Dual-Core | ESP32-P4-WIFI6-LCD-7B | 7" 1024×600 DSI, Touch | SD, 9P, PSRAM | High-Perf Ribbon Station |
     | `rp2350-chess` | RV32 Hazard3 | Pico 2 + ST7735 + TM1638| 160×128 TFT, Keypad/LEDs | SD card | Dedicated Chess Computer |
     | `esp32c6` | RV32IMAC | ESP32-C6-Zero | Serial Console | Flashfs, Wi-Fi | Low-Resource Wireless Node |
     | `esp32p4` | RV32 Dual-Core | ESP32-P4-NANO | Serial Console, 400 MHz | SD, Ethernet, PSRAM | Microkernel Compute Node |
     | `rp2350-clock` | RV32 Hazard3 | Waveshare Clock-Green | SM16106 LED Matrix | DCF77, RTC | Precision Radio Clock |
     | `rv32` / `rv64` | RV32/RV64 | QEMU virt | VirtIO Console | VirtIO Blk, Net | Automated CI / VM Target |
3. **5-Minute Quickstart**:
   - 2-line command to build and run in QEMU.
   - 2-line command to flash connected hardware.
4. **Microkernel Architecture Diagram**:
   - Clean ASCII / Mermaid diagram showing hardware isolation (User tasks, drivers in PMP domains, copy-always channels).
5. **Documentation Portal**:
   - Direct links to User Manual, Administrator Manual, System Programmer Manual, and AGENTS guide.

---

## 7. Milestone Breakdown & Merge Strategy

Phase 50 will be executed on a dedicated branch `feature/phase50-docs`. Merges back to `main` will occur at clearly defined milestone checkpoints.

```
                    M50.1               M50.2               M50.3               M50.4             M50.5 (One-Go)          M50.6
                  Foundation         User/Admin/README     Dev & ADRs          On-Device Help     Comment Scrubbing      Verification
feature/phase50 ──────●───────────────────●───────────────────●───────────────────●─────────────────────●─────────────────────●───► (Merged)
                      │                   │                   │                   │                     │                     │
main ─────────────────▼───────────────────▼───────────────────▼───────────────────▼─────────────────────▼─────────────────────▼───► main
                 [Merge M50.1]       [Merge M50.2]       [Merge M50.3]       [Merge M50.4]         [Merge M50.5]         [Merge M50.6]
```

### Milestone M50.1: Architecture, Tooling Foundation & `gendocs.py`
* **Tasks**:
  1. Create directory structure in `doc/` (`user/`, `admin/`, `dev/`, `history/adr/`, `assets/`).
  2. Implement [`tools/gendocs.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/gendocs.py) to convert Markdown docs to 80/126-column plain-text pages with `[1..9]` numbered reference links.
  3. Update [`AGENTS.md`](file:///home/dsc/Source/gith/domschl/lugalos/AGENTS.md) with the new Commentary Standard and Agent Documentation Rules.
* **Deliverables**: Working doc build tool, directory layout, updated `AGENTS.md`.
* **Merge Checkpoint**: Merge M50.1 to `main`.

### Milestone M50.2: User & Administrator Manuals + README Overhaul
* **Tasks**:
  1. Author User Manual chapters in `doc/user/` (`shell.md`, `editors.md`, `ribbon.md`, `chess.md`, `cas.md`, `sensors.md`).
  2. Author Administrator Manual chapters in `doc/admin/` (`presets.md`, `flashing.md`, `consoles.md`, `networking.md`, `identity.md`, `storage.md`).
  3. Restructure [`README.md`](file:///home/dsc/Source/gith/domschl/lugalos/README.md) into the ~350-line visual project showcase with hardware presets matrix and links to manuals.
  4. Organize image assets in `doc/assets/images/` and screenshots in `doc/assets/screenshots/`.
* **Deliverables**: Complete User and Administrator manuals, overhauled `README.md`.
* **Merge Checkpoint**: Merge M50.2 to `main`.

### Milestone M50.3: System Programmer Manual & Architecture Decision Records (ADRs)
* **Tasks**:
  1. Author Developer Guide in `doc/dev/on_device/` (`c_programming.md`, `lisp_scripting.md`).
  2. Author Kernel Guide in `doc/dev/kernel/` (`architecture.md`, `scheduler.md`, `logging.md` covering the tokenized dictionary macro system, `drivers.md`, `porting.md`).
  3. Author Testing Guide in `doc/dev/testing.md` (Host harnesses, `tests/runner.py`, `tests/hw/`).
  4. Extract historical architectural decisions from planning docs into `doc/history/adr/`.
* **Deliverables**: Complete System Programmer Manual and ADR collection.
* **Merge Checkpoint**: Merge M50.3 to `main`.

### Milestone M50.4: On-Device Terminal Help System & Interactive Pager
* **Tasks**:
  1. Clean up `cmd_help` in [`kernel/shell.c`](file:///home/dsc/Source/gith/domschl/lugalos/kernel/shell.c) into categorized overview + `help test`.
  2. Implement interactive topic help (`man` and `help <topic>`) reading `/sd0/help/<topic>.txt` with numbered link jumping `[1..9]`, back navigation (`b`), and terminal-height paging.
  3. Expose standalone pager `more <path>`.
  4. Add Lisp `(doc 'symbol)` and `(help 'symbol)` in [`tools/sd_root/system/etc/stdlib.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/etc/stdlib.lisp).
  5. Run `tools/gendocs.py` to stage generated help pages into `tools/sd_root/help/` and integrate into CMake build rules.
* **Deliverables**: Interactive on-device help system running on real hardware and QEMU.
* **Merge Checkpoint**: Merge M50.4 to `main`.

### Milestone M50.5: Global Codebase Comment Rationalization (One-Go Sweep)
* **Pre-condition**: Confirm that all ongoing development work has ceased and branches are committed.
* **Tasks**:
  1. Execute a comprehensive sweep across all source directories (`kernel/`, `drivers/`, `fs/`, `net/`, `libc/`, `user/`, `arch/`).
  2. Remove all 750+ `plan/phase...` comments and obsolete historical changelogs.
  3. Enforce the Commentary Standard (clean function contracts, locks, hardware errata).
  4. Run full regression test suite (`tests/runner.py`) to confirm zero functional changes.
* **Deliverables**: Clean, professional source tree with high signal-to-noise ratio.
* **Merge Checkpoint**: Merge M50.5 to `main`.

### Milestone M50.6: Automated Doc Verification & Final Polish
* **Tasks**:
  1. Add automated documentation verification checks to [`tests/runner.py`](file:///home/dsc/Source/gith/domschl/lugalos/tests/runner.py) (verify no broken internal markdown links, verify on-device help parity with `gendocs.py`).
  2. Verify builds across all hardware presets (`rp2350-terminal`, `esp32p4-lcd7b`, `esp32c6`, `rv32`, `rv64`, `rv64-smp`).
  3. Final audit and phase sign-off.
* **Deliverables**: 100% verified documentation suite, automated regression gating.
* **Merge Checkpoint**: Merge M50.6 to `main` and conclude Phase 50.

---

## 8. Definition of Done

Phase 50 is complete when:
1. `README.md` is concise, visually compelling (~350 lines), and links to the comprehensive manuals.
2. Complete User, Administrator, and Developer manuals exist in `doc/` with zero broken links.
3. `AGENTS.md` is streamlined and provides authoritative invariants, port mappings, and documentation rules.
4. On-device `help` and `man` provide interactive, numbered-link topic navigation and paging on LugalOS terminals and serial consoles.
5. All 750+ obsolete `plan/phase...` comments are removed from the source tree without breaking tests or functionality.
6. `tests/runner.py` passes 100% and includes automated documentation integrity tests.
