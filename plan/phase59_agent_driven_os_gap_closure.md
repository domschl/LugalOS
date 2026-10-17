# Phase 59: Agent-Driven OS Friction Reduction, Self-Extension & Introspection

**Status**: PROPOSED / DRAFT  
**Target Version**: 0.21.0  
**Follows**:
- Phase 42 & 43: Lisp Symbolic Foundations & Computer Algebra System (`cas/`)
- Phase 50: Documentation System (`doc/`, `/sd0/help/*.gmi`)
- Phase 51: Shell Usability & Composability (`kernel/shell.c`)
- Phase 58: On-Device AI Agent (`ea`), LLM Primitives & Math Assistant (`kernel/ea.c`, `net/llm.c`)

**Target Hardware**:
- Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`, 32 MB PSRAM, Wi-Fi 6 via C6 coprocessor)
- Waveshare ESP32-P4-NANO (`esp32p4`, 32 MB PSRAM, RMII Ethernet)
- M5Stack Tab5 (`esp32p4-tab5`, 32 MB PSRAM, 5" 720×1280 HiDPI IPS)
- QEMU VirtIO Targets (`rv32`, `rv64`, `rv64-smp`)

---

## 1. Executive Summary & The 3-Horizon Vision

During the bring-up and empirical testing of the on-device AI agent (`ea`) with `Qwen3.8-Flash-Next` on live hardware (`esp32p4-lcd7b`), inspecting the agent's internal thinking traces revealed an invaluable feedback loop:

> **The AI agent serves as an automated usability auditor and gap detector for LugalOS.**
> Whenever the agent hesitates, loops, hallucinates, or fails an action, its reasoning trace pinpoints exactly where LugalOS documentation, discoverability, introspection, or toolchain semantics diverge from clean, predictable abstractions.

This phase transforms those empirical findings into a structured roadmap designed around **Three Progressive Horizons**:

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│                               THE THREE HORIZONS                                  │
├───────────────────────────────────────────────────────────────────────────────────┤
│                                                                                   │
│  HORIZON 1: Frictionless Autonomous Operation (Phase 59 Primary Objective)        │
│  ─────────────────────────────────────────────────────────────────────────        │
│  Close all discoverability, documentation, and toolchain gaps uncovered by agent  │
│  diagnostics. Provide rich Lisp introspection, modular help topics, command        │
│  chaining, and consistent C compiler semantics so the agent never gets stuck.     │
│                                      │                                            │
│                                      ▼                                            │
│  HORIZON 2: Dynamic System Self-Extension via Lisp (/sd0/ Extensions)            │
│  ────────────────────────────────────────────────────────────────────             │
│  Enable the agent to author, verify, and persist new modular utilities and        │
│  symbolic solvers as Lisp scripts stored in `/sd0/lib/` or `/sd0/bin/`.           │
│  The agent augments the OS runtime dynamically without recompiling firmware.     │
│                                      │                                            │
│                                      ▼                                            │
│  HORIZON 3: Self-Hosting & Recursive Kernel Modification in C                     │
│  ────────────────────────────────────────────────────────────                     │
│  Enable LugalOS's built-in `chibicc` compiler and linker to compile kernel and    │
│  driver modules on the device itself. The on-device agent can inspect kernel C    │
│  source files, generate patches, compile new binaries, and stage OTA reboots.     │
│                                                                                   │
└───────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Empirical Findings from Hardware Diagnostics

Experiments conducted on `esp32p4-lcd7b` at 400 MHz with `Qwen3.8-Flash-Next` running against a local OpenAI-compatible inference server revealed specific failure modes:

| # | Task Tested | Agent Behavior & Thinking Trace | Discovered Gap & Resolution |
|---|---|---|---|
| **1** | *"Differentiate x^3-1 with respect to x and use Lisp to plot the result"* | Agent executed `help lisp`, searched for `plot`, saw help output truncated at 8 KB, tried to cook up a custom Lisp plotter, then attempted C. | **G1**: No dedicated `/sd0/help/plot.gmi`. Plotting was buried in Section 12 of a 31 KB `cas.gmi` file that exceeded the 8 KB capture buffer.<br>**G2**: Zero Lisp introspection (`(apropos "plot")` and `(doc ...)` missing).<br>*(Resolved: Created `/sd0/help/plot.gmi` and added plot alias to stdlib)* |
| **2** | *"Solve the quadratic equation x^2 - 5x + 6 = 0 using CAS in Lisp"* | Agent formulated `(solve '(= (+ (* x x) (* -5 x) 6) 0) 'x)`, observed `((x . 2) (x . 3))`, and finished in 1 turn with 100% precision. | **Insight**: Priming the system prompt with concrete AST syntax guarantees one-shot agent success. |
| **3** | *"Run the cellular automata demo in Lisp and explain what rule it simulates"* | Agent tried `ls -R /`, `find /`, `ls -la /`. Failed with `ls: '-R': no such mount`. Self-corrected to manual tree exploration `ls /` $\rightarrow$ `ls /flash0` $\rightarrow$ `ls /flash0/DEMOS` $\rightarrow$ `CA.LIS`. | **G3**: `lsh` lacks recursive file discovery (`find` or `ls -r`).<br>**G4**: Shell lacks command chaining (`cat /proc/version; ip` treated the semicolon as part of the filename).<br>*(Resolved: Created dedicated `/sd0/help/demos.gmi` indexing all demos across storage)* |
| **4** | *"Check what environmental sensors are connected and read current temperature"* | Agent reasoned to call `sh` with `sensor`. Observed `bme280 at 0x76: 23.19 C`. Provided clean formatted markdown table in 1 turn. | **G5**: Long command strings (>128 bytes) caused UART RX FIFO overrun at 921600 baud without character/chunk pacing. |
| **5** | *"Write a small C program that calculates the 10th Fibonacci number and prints it"* | Agent generated standard C with `int a = 0, b = 1;` and `printf("%d\n", a);`. `chibicc` aborted with `expected ';' at ','`. | **G6**: `chibicc` lacked comma-separated variable declarations (`int a = 0, b = 1;`) and compound assignment (`+=`, `*=`).<br>*(Resolved: Implemented declarator lists and compound assignments in `user/chibicc/`)* |
| **6** | *"Write a C program that computes factorial of 5 and prints it, compile and run"* | Blue screen freeze during U-mode transition; after kernel fix, `Observation:` captured only `=> 0` without stdout `120`; C code corrupted with `'\"}}],n'`. | **G7 (Critical Kernel)**: CLIC privilege mode tracking bug in `entry.S` (`mcause[29:28]`). Zeroed `sp` on trap.<br>**G8 (Critical Parser)**: SSE JSON parser in `net/llm.c` treated `\\"` as an escaped quote rather than an escaped backslash, corrupting `\n`.<br>**G9 (Process Hierarchy)**: `console_capture` only captured `g_cap_pid`, ignoring spawned user child tasks (`uprog`).<br>**G10 (Diagnostic Stream)**: `chibicc` printed errors via async `printk` instead of user-facing `cprintf`.<br>*(Resolved: 100% resolved across kernel, scheduler, console, and net/llm.c! Factorial program compiled, ran, printed 120, and output was captured into Observation)* |

---

## 3. Architecture & Functional Specifications

### 3.1 Introspection & Discoverability (Lisp & Shell)

#### Lisp Introspection Primitives
Currently, discovering functions in LugalOS Lisp requires reading static documentation files. Phase 59 introduces runtime symbol reflection:

1. `(apropos <string-or-symbol>)`:
   - Scans both the **Flash `.rodata` builtins table** (sorted in ASCII order, searched with binary search or linear substring scan) and dynamic user symbols in the current environment.
   - Returns a sorted list of matching symbols.
   - Example: `(apropos "plot")` $\rightarrow$ `(plot-expr plot-fn canvas-plot)`

2. `(doc <symbol>)`:
   - Inspects documentation strings attached to builtins or queries `/sd0/help/` dynamically.
   - Falls back to scanning `.rodata` docstring tables so documentation remains accessible even when SD storage is unmounted or in minimal flash configurations.
   - Example: `(doc 'diff)` $\rightarrow$ `"Usage: (diff expr var [n])\nSymbolic n-th derivative of algebraic expression."`

3. `(dir [path])` / `(glob pattern)`:
   - Returns directory listings as structured Lisp lists of strings rather than requiring shell text scraping.

#### Shell File Discovery: `find` and Recursive Search
Add a lightweight, freestanding `find` builtin to `kernel/shell.c`:
- Syntax: `find [path] [pattern]`
- Traverses VFS mount points (`/sd0`, `/flash0`, `/ram0`) recursively with a bounded stack depth (max 8 levels) to preserve SRAM.
- Matches substrings or extensions (e.g. `find / *.lisp`, `find /flash0 demo`).
- Enables one-shot file discovery for both human users and AI agents.

#### Shell Command Chaining: Semicolon `;` and Conditional `&&`
Enable basic command sequencing in `kernel/shell.c`:
- Tokenize unquoted `;` into sequential commands executed in order.
- Tokenize `&&` to execute subsequent commands only if the preceding command returned exit status 0 (`#t`).
- Prevents agents and human operators from failing when pasting compound shell scripts.

---

### 3.2 Modular Documentation System

To prevent context overflow and tool buffer truncation (such as the 8 KB observation limit in `ea.c`), monolithic manuals must be partitioned into modular, fine-grained Gemini topics:

```
/sd0/help/
├── index.gmi             # Master directory of all help topics
├── shell.gmi             # Core lsh commands, piping, syntax
├── lisp.gmi              # Lisp engine, syntax, data types, REPL
├── calculus.gmi          # (Split from cas.gmi) diff, integrate, taylor, limit
├── algebra.gmi           # (Split from cas.gmi) solve, simplify, factor, expand
├── plot.gmi              # Dedicated plotting: canvas, functions, ASCII/GUI
├── demos.gmi             # Index of all Lisp & C demos in /flash0/DEMOS/ & /sd0/demos/
├── c_programming.gmi     # chibicc compiler syntax, supported subset, lugal.h
├── sensors.gmi           # I2C telemetry, sensors, reading from Lisp & shell
└── networking.gmi        # Wi-Fi, Ethernet, 9P, Spartan, MQTT
```

#### Section-Aware Querying
Support topic subsections in the `help` shell command and `ea` tool:
- `help <topic> <subheading>` (e.g. `help cas solve` or `help c_programming printf`).
- Reads only the relevant heading and paragraph, keeping LLM observations concise and focused.

---

### 3.3 On-Device C Compiler (`chibicc`) & Syscall Ergonomics

The `chibicc` compiler is one of LugalOS's most distinctive capabilities, enabling full on-chip compilation. However, its subset diverges from common programmer expectations in subtle ways.

#### Improvements to `chibicc` Parser (`user/chibicc/parse.c`):
1. **Multiple Variable Declarators**:
   - Extend `stmt_inner()` in `parse.c` to parse comma-separated variable declarations:
     `int a = 0, b = 1;`
   - Loop on `,` after parsing a variable name/initializer until `;` is reached.
2. **Explicit Diagnostics for Variadic `printf`**:
   - In `user/chibicc/codegen.c`, generate code to warn or cleanly handle format strings, or provide a built-in U-mode mini-`snprintf` helper in `/sd0/include/lugal.h` that converts numbers to strings before invoking `SYS_PRINT`.
   - Update `lugal.h` to explicitly declare `putnum(long n)` and `printf(const char *fmt, ...)` ergonomics.
3. **Dedicated RAMDisk Sandbox (`/ram0/`)**:
   - Direct all temporary agent compilation files (`ea_prog.c`, `ea_prog.elf`) to `/ram0/` by default.
   - Completely avoids flash wear on SPI flash / MicroSD cards and provides instant build speeds.

---

### 3.4 Horizon 2: Self-Extension via Persistent Lisp Libraries

Once basic gaps are resolved, the agent can actively extend the operating system:

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│                    AGENT PERSISTENT SELF-EXTENSION WORKFLOW                       │
└────────────────────────────────────────┬──────────────────────────────────────────┘
                                         │
 1. User Request:                        ▼  "Implement Simpson's rule numerical integrator"
                                         │
 2. Agent Research & Reasoning:          ▼  Inspects (doc 'integrate), checks math primitives
                                         │
 3. Agent Authors Script:                ▼  Writes /sd0/lib/simpson.lisp via Action: lisp
                                         │
 4. Automated Verification:              ▼  (load "/sd0/lib/simpson.lisp") -> (test-simpson)
                                         │
 5. Auto-Registration in stdlib:         ▼  Appends (require 'simpson) to /sd0/system/etc/stdlib.lisp
                                         │
 6. Success & Persistence:               ▼  Feature is now permanently available across reboots!
```

- Standardize `/sd0/lib/` as the dynamic module directory.
- Implement `(provide 'module)` and `(require 'module)` dependency tracking.
- Allow the agent to write unit tests, execute them in a sandbox, and confirm working code before offering it to the user.

---

### 3.5 Horizon 3: Self-Hosting & Kernel Modification in C

The long-term objective of LugalOS is self-hosting on RISC-V hardware:
1. Modularize kernel drivers into relocatable or dynamically loadable ELF modules (`.ko` / `.elf`).
2. Expose kernel build scripts or modular driver interfaces so that `chibicc` can compile new drivers directly on the ESP32-P4 or RP2350.
3. The agent can analyze driver issues (e.g. reading datasheets in `/sd0/docs/`), update driver C files, compile them, and load them dynamically into memory.

---

## 4. Implementation Milestones

### Milestone 59.1: Kernel U-Mode Execution & Privilege Mode Integrity
- [x] Fix CLIC privilege tracking in `entry.S` (`mcause[29:28]`).
- [x] Initialize `mcause` (`CSR_CAUSE`) in `umode.S` before `TRAP_RET` (`mret`).
- [x] Hardware cache sync in `xip_esp32p4.c` with dual L1 D-cache and L2 writeback and I-cache invalidates (`CACHE_SYNC_CTRL_REG` at 0x3FF10098).
- [x] Fast internal SRAM allocation for user ELF images in `elf.c`.
- [x] Fix `test_esp32p4.py` test suite for `lcd7b` (46 / 46 PASSED).

### Milestone 59.2: Tool Output Capture & Execution Ergonomics
- [x] Extend `task_t` with `parent_pid` and implement `task_get_parent()` in `kernel/sched.c`.
- [x] Update `console_capture()` in `kernel/console.c` to capture process hierarchy so child task stdout reaches tool observations.
- [x] Add direct `exec <path> [args...]` handler and direct VFS path execution in `kernel/shell.c`.
- [x] Fix SSE JSON string unescaping in `net/llm.c` for backslashes preceding closing quotes (`putchar('\n');`).
- [x] Redirect `chibicc` compiler diagnostics from async `printk` to user-facing `cprintf`.

### Milestone 59.3: `chibicc` C Compiler Expansion
- [x] Support comma-separated variable declarations (`int a = 1, b = 2;`) in `user/chibicc/parse.c`.
- [x] Support compound assignment operators (`+=`, `-=`, `*=`, `/=`, `%=`) in `user/chibicc/parse.c` and `tokenize.c`.
- [ ] Add integer-to-string formatting helper or documented `putnum()` macros in `/sd0/include/lugal.h`.
- [ ] Document supported C11 subset and syscall ABI in `/sd0/help/c_programming.gmi`.

### Milestone 59.4: Modular Documentation Architecture
- [x] Create `/sd0/help/plot.gmi` dedicated to 2D function plotting, canvas commands, and ASCII graphs.
- [x] Create `/sd0/help/demos.gmi` indexing `/flash0/DEMOS` and `/sd0/demos` with descriptions.
- [ ] Split `cas.gmi` into `/sd0/help/calculus.gmi` and `/sd0/help/algebra.gmi`.
- [ ] Add section-level reading support to `help` tool (`help <topic> <subheading>`).

### Milestone 59.5: Lisp Runtime Introspection
- [ ] Implement `(apropos <str>)` scanning Flash builtins and global environment.
- [ ] Implement `(doc <sym>)` retrieving builtin docstrings or `/sd0/help/` summaries.
- [ ] Expose introspection in the `ea` system prompt so the agent queries available functions autonomously.

### Milestone 59.6: Shell Discoverability & Command Sequencing
- [ ] Implement `find [path] [pattern]` builtin in `kernel/shell.c`.
- [ ] Implement semicolon `;` and logical `&&` sequencing in `kernel/shell.c`.
- [ ] Add automated regression test in `tests/runner.py` verifying `find` and command chaining.

### Milestone 59.7: Automated Agent E2E Benchmark Suite
- [ ] Create `tests/hw/test_agent_eval.py` that runs the 5 canonical benchmarks (CAS calculus, quadratic solver, demo discovery, sensor telemetry, and C compilation).
- [ ] Assert that all 5 benchmarks achieve one-shot or two-shot resolution without getting stuck.
- [ ] Track reasoning step counts and token efficiency across firmware revisions.
