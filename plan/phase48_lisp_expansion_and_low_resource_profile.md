# Phase 48 — Lisp Language Expansion, Hardware Telemetry & Dual Resource Profiles

**Status: IN PROGRESS (Milestones 48.1, 48.2a, 48.4a, 48.4c–e Completed & Verified on Hardware).**  
Branch: `lisp_expansion`  
Follows: Phase 42 (Lisp Symbolic Foundations), Phase 43 (Lisp CAS & Plotting), Phase 46 (Sensor Framework), Phase 47 (ESP32-P4 LCD7B & Ribbon Window System).

---

## 1. Executive Summary & Hardware Strategy

LugalOS features a self-contained Lisp engine ([`user/lisp/lisp.c`](file:///home/dsc/Source/gith/domschl/lugalos/user/lisp/lisp.c)) that acts simultaneously as the kernel shell (`lsh`), the interactive system console, an appliance runtime, and an algebraic computing environment. 

As LugalOS scales, it runs across two radically divergent silicon tiers:
1. **High-Resource Workstations & Displays (with PSRAM / Large SRAM):**
   * *Targets:* Waveshare RP2350-LCD-7 (8 MB PSRAM, 800×480 RGB canvas), Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (32 MB PSRAM, 1024×600 MIPI-DSI), QEMU virt (128 MB RAM).
   * *Characteristics:* 16,384 to 65,536 Lisp nodes mapped into `BULK_BSS` ([`plan/phase38_psram.md`](file:///home/dsc/Source/gith/domschl/lugalos/plan/phase38_psram.md)), multi-window ribbon terminal, hardware sensor hub, and graphical CAS plotting.
2. **Resource-Constrained Microcontrollers (No PSRAM):**
   * *Targets:* Raspberry Pi Pico 2 (standalone RP2350, 520 KB SRAM), Waveshare ESP32-C6-Zero (RV32IMAC @ 160 MHz, 496 KB usable SRAM), Waveshare Pico-Clock-Green (LED matrix appliance).
   * *Characteristics:* Every byte of `.bss` directly subtracts from `palloc`'s physical heap. The Lisp node pool is hard-capped at 1,024 nodes in `.bss`. In a 1,024-node pool, loading massive standard libraries or CAS scripts at boot exhaust `global_env` and causes `"Node pool exhausted"` faults during basic shell commands.

```
┌────────────────────────────────────────────────────────────────────────┐
│                   LUGALOS DUAL-PROFILE ARCHITECTURE                     │
├────────────────────────────────────┬───────────────────────────────────┤
│    MINIMAL PROFILE (No PSRAM)      │      FULL PROFILE (With PSRAM)    │
│    Pico 2, ESP32-C6, Clock-Green   │      RP2350-LCD-7, ESP32-P4-LCD7B │
├────────────────────────────────────┼───────────────────────────────────┤
│ • Node Pool: 1,024 nodes in .bss   │ • Node Pool: 16k–64k in BULK_BSS  │
│ • Boot stdlib: ~10 pairs (lean)    │ • Boot stdlib: Full battery set   │
│ • Modules: Loaded strictly on-demand│ • Modules: Preloaded / cached     │
│ • C Builtins: Flash .rodata (0 RAM)│ • C Builtins: Flash .rodata (0 RAM│
│ • CAS: Not loaded by default       │ • CAS: Full algebraic suite       │
│ • Telemetry: Text / raw stream     │ • Telemetry: Live canvas charts   │
└────────────────────────────────────┴───────────────────────────────────┘
```

### The Architectural Solution: Zero-SRAM Builtins & Tiered On-Demand Libraries

To expand Lisp's capabilities (macros, vectors, hardware control, CAS power series, live sensor plotting) **without regressing memory on low-resource MCUs**, Phase 48 establishes three structural rules:

1. **All Core Primitives in Flash `.rodata` (Zero SRAM Overhead):**
   All new primitives (`sleep-ms`, `error`, `i2c-read`, `i2c-write`, `gpio-*`, `define-macro`, `vector-*`) are implemented in C and registered in [`user/lisp/builtins_table.h`](file:///home/dsc/Source/gith/domschl/lugalos/user/lisp/builtins_table.h). Because they are resolved via compile-time binary search (`builtin_get()`), they consume **0 bytes of `.bss` SRAM** and **0 cons pairs in `global_env`**. Constrained boards gain the full power of new primitives without paying a single byte of memory.
2. **Tiered Standard Library (`stdlib.lisp` vs Modular Packages):**
   * [`/sd0/system/etc/stdlib.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/etc/stdlib.lisp) remains ultra-lean at boot ($\le 12$ definitions, e.g. `not`, `print`, `when`, `unless`).
   * Heavy libraries (sequences, strings, matrices, dictionaries) are split into standalone modular files in `/sd0/system/lib/` (or `/flash0/system/lib/`) and loaded on demand using a new `(require 'name)` mechanism.
3. **Flat Vector Representation (`LISP_VECTOR`):**
   Linked lists cost 1 pair node (24–32 bytes) per element. In a 1,024-node pool, a 100-sample sensor window or a 10×10 matrix consumes 10–20% of the entire system's node budget. A flat contiguous vector type drastically reduces node allocation pressure.

---

## 2. Milestone Overview

```
48.1 (Foundations & Dual-Profile Core) ──► 48.2 (Hardware & Sensor Integration)
                 │                                         │
                 ▼                                         ▼
48.4 (Advanced Core: Macros & Vectors) ◄── 48.3 (Symbolic Math Expansion - CAS)
                 │                                         │
                 └────────────────────┬────────────────────┘
                                      ▼
                 48.5 (Dual-Profile Verification & Benchmarks)
```

---

## 3. Detailed Milestones

### Milestone 48.1: Language Foundations, Immediate Fixes & Dual-Profile Core (Phase A) — **COMPLETED**

* **Goal:** Fix latent runtime faults in the CAS and shell, introduce essential execution primitives (`error`, `sleep-ms`), and establish the dual-profile library loading architecture.

#### 48.1a: Builtin `error` Primitive — **COMPLETED**
* **Problem:** In [`cas/solve.lisp:51`](file:///home/dsc/Source/gith/domschl/lugalos/cas/solve.lisp#L51), [`cas/solve.lisp:178`](file:///home/dsc/Source/gith/domschl/lugalos/cas/solve.lisp#L178), and [`cas/calculus.lisp:177`](file:///home/dsc/Source/gith/domschl/lugalos/cas/calculus.lisp#L177), error branches call `(error "...")`. However, `error` is not defined anywhere in LugalOS. Invoking an error branch triggers an unhandled `Unbound symbol: error` fault.
* **Design & Status:**
  - Implemented `prim_error(args, env)` in [`user/lisp/lisp.c`](file:///home/dsc/Source/gith/domschl/lugalos/user/lisp/lisp.c).
  - Signature: `(error message [datum])`.
  - Behavior: Formats the error to the active console via `printk("[Lisp Error] ...\n")`, aborts current evaluation cleanly, and unwinds without leaking nodes.
  - Registered in `builtins_table.h` in Flash `.rodata`. Verified in QEMU `rv32` and on RP2350 silicon.

#### 48.1b: Scheduler Sleep Primitive (`sleep`, `sleep-ms`) — **COMPLETED**
* **Problem:** The kernel provides [`task_sleep_ms(uint32_t ms)`](file:///home/dsc/Source/gith/domschl/lugalos/kernel/include/kernel/sched.h#L271), but Lisp has no sleep primitive. Delay loops currently spin on the CPU, wasting power and starving lower-priority background tasks (such as `klogd` or network packet pumps).
* **Design & Status:**
  - Implemented `prim_sleep_ms(args, env)` in `user/lisp/lisp.c`.
  - Signature: `(sleep ms)` and `(sleep-ms ms)`.
  - Calls `task_sleep_ms((uint32_t)val)` which places the current task in `TASK_BLOCKED` with an exact deadline in the monotonic scheduler timer wheel.
  - Registered in `builtins_table.h` in Flash `.rodata`. Returns `#t`. Verified on QEMU and RP2350 silicon.

#### 48.1c: Dual-Profile Configuration & Modular `require` — **COMPLETED**
* **Problem:** Loading large Lisp scripts at boot creates persistent nodes in `global_env` that cannot be garbage collected. On non-PSRAM MCUs (1,024 nodes), this exhausts memory before the user runs their first program.
* **Design & Status:**
  - Implemented `(require 'feature)` and `*modules*` tracking list in [`stdlib.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/etc/stdlib.lisp).
  - Modular directory layout created in `tools/sd_root/system/lib/`:
    ```
    /sd0/system/lib/ (and /flash0/system/lib/)
      ├── macros.lisp  ; dotimes, dolist, inc!, dec!, push!, pop!
      ├── seq.lisp     ; fold-left, fold-right, iota, range, take, drop, zip, partition, every, any, flatten
      ├── str.lisp     ; string-split, string-join, string-trim, string-prefix?, string-suffix?, string-contains?
      ├── dict.lisp    ; alist-ref, alist-set, alist-delete, plist-get, plist-put
      ├── matrix.lisp  ; linear algebra (Milestone 48.3c)
      └── chart.lisp   ; canvas telemetry widgets (Milestone 48.2e)
    ```
  - Standard boot behavior:
    - **Low-Resource Profile (`CONFIG_LISP_PROFILE_MINIMAL`, default on Pico 2 & ESP32-C6):** [`init.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/etc/init.lisp) loads only `stdlib.lisp` (core essentials: `not`, `print`, `when`, `unless`). `cas.lisp` is NOT loaded at boot. Headroom remains $> 850$ free nodes in SRAM.
    - **Full Profile (`CONFIG_LISP_PROFILE_FULL`, on RP2350-LCD-7 and ESP32-P4):** Full libraries and CAS loaded on demand or at boot.

#### 48.1d: Core Syntactic Macros in Minimal Standard Library — **COMPLETED**
* **Design & Status:** Implemented zero-cost syntactic control constructs as true syntactic macros in [`stdlib.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/etc/stdlib.lisp) using the Milestone 48.4a macro system:
  ```scheme
  (define not (lambda (x) (if x #f #t)))
  (define print (lambda (msg) (begin (display msg) (newline))))
  (define-macro (when test . body)
    (list 'if test (cons 'begin body) #f))
  (define-macro (unless test . body)
    (list 'if test #f (cons 'begin body)))
  ```
  Verified on QEMU `rv32` and live on RP2350-terminal hardware.

---

### Milestone 48.2: Hardware Integration & Sensor Telemetry (Phase B) — **IN PROGRESS**

* **Goal:** Connect LugalOS Lisp directly to physical buses (I2C, GPIO, ADC, RTC) and bridge the Sensor Framework ([`plan/phase46_sensor_framework.md`](file:///home/dsc/Source/gith/domschl/lugalos/plan/phase46_sensor_framework.md)) to the 1-bpp/RGB Canvas subsystem.

#### 48.2a: Raw I2C Master Primitives — **COMPLETED**
* **Problem:** Lisp only exposes `i2c-scan`. Developers cannot read registers or send commands to custom I2C devices (OLEDs, DACs, motor controllers, or the DS1307/DS3231 RTC) from the REPL without writing a C driver.
* **Design & Status:**
  - Implemented `prim_i2c_read(args, env)` and `prim_i2c_write(args, env)` in [`user/lisp/lisp.c`](file:///home/dsc/Source/gith/domschl/lugalos/user/lisp/lisp.c).
  - `(i2c-read addr reg len)`: Reads `len` bytes from device `addr` starting at sub-register `reg`. Returns a list of byte integers `(b0 b1 ... bn)`, or `#f` on NAK / bus failure.
  - `(i2c-write addr reg '(b0 b1 ...))`: Writes a byte payload. Returns `#t` on ACK, `#f` on NAK.
  - Built with Flash-resident C wrappers calling `drivers/i2c.h` (`i2c_xfer()`). Consumes 0 bytes of SRAM. Verified on QEMU and RP2350 silicon.

#### 48.2b: GPIO Pin Primitives
* **Design:**
  - `(gpio-mode pin 'in | 'out | 'pullup | 'pulldown)`
  - `(gpio-set pin 0 | 1)`
  - `(gpio-get pin)` $\to$ `0` or `1`
  - Allows hardware testing, LED blinking, and signal probing interactively in `lsh`.

#### 48.2c: ADC & Hardware Timers
* **Design:**
  - `(adc-read channel)`: Samples RP2350 ADC (GPIO 26–29) or ESP32 SAR ADC. Returns scaled millivolts or 12-bit integer.
  - `(time-us)`: Returns 64-bit monotonic microsecond counter as `LISP_BIGNUM` or `LISP_INT` for high-precision benchmarking.

#### 48.2d: RTC Inspection & Battery-Backed NVRAM
* **Design:**
  - `(rtc-status)`: Returns alist with RTC state: `((chip . "DS1307") (valid . #t) (time . "2026-10-11 17:15:00"))`.
  - `(rtc-nvram-read offset len)`: Reads battery-backed non-volatile SRAM (56 bytes on DS1307).
  - `(rtc-nvram-write offset byte-list)`: Persists parameters across power cuts without flash wear.

#### 48.2e: Real-Time Live Sensor Strip-Chart on Canvas
* **Design:**
  - Connect `prim_sensor_read` to the canvas in `/sd0/system/lib/chart.lisp`:
  - `(sensor-plot channel [sample-interval-ms] [window-seconds])`
  - *Example:* `(sensor-plot "temp" 200 60)`
  - *Mechanism:*
    1. Opens or claims a canvas pane (ST7735, RP2350 800×480, or ESP32-P4 1024×600).
    2. Runs a loop with `(sleep-ms sample-interval-ms)`.
    3. Samples `(sensor-read channel)`.
    4. Auto-scales min/max integer bounds with 10% margin.
    5. Clears and draws rolling time-series lines via `canvas-line`, displaying current, min, and max values using `canvas-text`.
    6. Exits cleanly on keypress/Ctrl-C without leaving dirty screen state.
  - `(sensor-log file-path '(temp humidity pressure) interval-ms count)`: Logs comma-separated sensor telemetry to SD card.

---

### Milestone 48.3: Symbolic Math Expansion (`cas/`) (Phase C)

* **Goal:** Expand LugalOS CAS with power series expansions, trigonometric/exponential identity rewriting, and exact linear algebra—adhering strictly to zero-floating-point M-mode rules.

#### 48.3a: Symbolic Taylor & Maclaurin Power Series (`taylor`)
* **Design:**
  - Add to [`cas/calculus.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/cas/calculus.lisp):
    `(taylor expr var order [center])`
  - Formula:
    $$P_n(x) = \sum_{k=0}^{n} \frac{f^{(k)}(x_0)}{k!} (x - x_0)^k$$
  - Since [`diff`](file:///home/dsc/Source/gith/domschl/lugalos/cas/calculus.lisp#L175) and arbitrary-precision integer factorials already exist, this generates exact rational polynomial coefficients.
  - *Example:*
    `(taylor '(math-exp x) 'x 4 0)` $\to$ `(+ 1 x (/ (^ x 2) 2) (/ (^ x 3) 6) (/ (^ x 4) 24))`
  - *Low-Resource Safety:* Order clamped to $\le 10$ to prevent AST combinatorial explosion on 1,024-node systems.

#### 48.3b: Advanced Trigonometric & Exponential Simplification Rules
* **Design:** Add transformation rules in [`cas/simplify.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/cas/simplify.lisp):
  - $\sin^2(u) + \cos^2(u) \to 1$
  - $1 + \tan^2(u) \to \sec^2(u)$
  - $\cosh^2(u) - \sinh^2(u) \to 1$
  - $\sin(2u) \to 2 \sin(u) \cos(u)$
  - $e^a \cdot e^b \to e^{a+b}$ and $(e^a)^b \to e^{a \cdot b}$
  - Invertible transformations: $\ln(e^x) \to x$ and $e^{\ln(x)} \to x$ (for $x > 0$).

#### 48.3c: Exact Symbolic Linear Algebra (`cas/matrix.lisp`)
* **Design:** Create a dedicated exact rational matrix module:
  - Representation: List of row lists or flat `LISP_VECTOR` (see Milestone 48.4).
  - Operations:
    - `(matrix-add A B)`, `(matrix-sub A B)`, `(matrix-mul A B)`
    - `(matrix-transpose A)`
    - `(matrix-det A)`: Exact determinant via Bareiss fraction-free algorithm or Gaussian elimination with exact rationals.
    - `(matrix-inv A)`: Exact matrix inverse via Gauss-Jordan elimination.
    - `(matrix-solve A B)`: Generalized linear system solver $A X = B$.
    - `(vector-dot u v)` and `(vector-cross u v)`.

#### 48.3d: Partial Fractions & Calculus Enhancements
* **Design:**
  - `(apart P Q var)`: Heuristic rational decomposition for $\frac{P(x)}{Q(x)}$ with linear and quadratic irreducible factors.
  - `(integrate expr var)` extension: Enables exact symbolic integration of rational functions resulting in $\ln |x - a|$ or $\arctan(x)$.

---

### Milestone 48.4: Advanced Core: Macros, Vectors & Standard Libraries (Phase D)

* **Goal:** Deliver macro metaprogramming and flat vector memory structures to eliminate node churn and complete the standard library ecosystem.

#### 48.4a: Syntactic Macro System (`define-macro`, `defmacro`, `macro?`) — **COMPLETED**
* **Problem:** Lisp functions evaluate their arguments eagerly. Without macros, control structures like `when`, `unless`, `dotimes`, `dolist`, and `case` cannot be defined in pure Lisp; they either require wrapping bodies in `(lambda () ...)` or writing hardcoded C special forms.
* **Design & Status:**
  1. Added `LISP_MACRO` type to `lisp_type_t` in [`user/lisp/include/lisp.h`](file:///home/dsc/Source/gith/domschl/lugalos/user/lisp/include/lisp.h).
  2. Implemented `define-macro` and `defmacro` special forms in `lisp_eval_step()` in [`user/lisp/lisp.c`](file:///home/dsc/Source/gith/domschl/lugalos/user/lisp/lisp.c).
  3. When an operator in `lisp_eval_step()` evaluates to `LISP_MACRO`:
     - Arguments are passed **unevaluated** to the macro transformer.
     - The returned AST (expansion) replaces the original form and is evaluated in the caller's environment (`tail_call` trampoline).
  4. Implemented `(macro? val)` primitive in Flash `.rodata` `builtins_table.h`.
  5. Created standard control macros in [`/sd0/system/lib/macros.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/lib/macros.lisp):
     - `dotimes`, `dolist`, `inc!`, `dec!`, `push!`, `pop!`.
  6. Verified in QEMU `rv32` and live on RP2350-terminal hardware.

#### 48.4b: Contiguous Fixed-Size Vector Type (`LISP_VECTOR`)
* **Problem:** In a linked list, each element requires one `LISP_PAIR` cons node (24 bytes on RV32). A 1,000-element list consumes 24,000 bytes and 1,000 node pool slots! On non-PSRAM systems with 1,024 total nodes, lists of even modest size exhaust the pool. Furthermore, indexing is $O(N)$.
* **Design:**
  1. Add `LISP_VECTOR` to `lisp_type_t`:
     ```c
     struct {
         uint16_t len;
         lisp_val_t **elems; /* contiguous array of pointers */
     } vector;
     ```
  2. Primitives:
     - `(make-vector len [default-val])`
     - `(vector elem1 elem2 ...)`
     - `(vector-ref vec index)` $\to O(1)$ access
     - `(vector-set! vec index val)` $\to O(1)$ mutation
     - `(vector-length vec)`
     - `(vector->list vec)` / `(list->vector lst)`
  3. Memory Allocation:
     - The vector wrapper itself is a single `lisp_val_t` node in the node pool.
     - The `elems` array is allocated from the string/byte pool or `palloc` page zone, consuming **zero pair nodes**.
     - Provides huge relief to the GC during matrix operations, telemetry buffering, and image/canvas manipulation.

#### 48.4c: Standard Sequence Library (`/sd0/system/lib/seq.lisp`) — **COMPLETED**
* Added standard functional utilities (SRFI-1 subset) in [`tools/sd_root/system/lib/seq.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/lib/seq.lisp):
  - `(fold-left kons knil lst)` and `(fold-right kons knil lst)` (tail-recursive)
  - `(zip lst1 lst2 ...)` and `(unzip lst)`
  - `(iota count [start] [step])` / `(range start end [step])`
  - `(take lst n)` / `(drop lst n)`
  - `(partition pred lst)`
  - `(every pred lst)` / `(any pred lst)`
  - `(flatten lst)`
  - Verified on QEMU and RP2350 silicon.

#### 48.4d: Standard String Library (`/sd0/system/lib/str.lisp`) — **COMPLETED**
* Added string manipulation utilities (SRFI-13 subset) in [`tools/sd_root/system/lib/str.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/lib/str.lisp):
  - `(string-split str delim)` $\to$ list of substrings.
  - `(string-join str-list delim)` $\to$ concatenated string.
  - `(string-trim str)` $\to$ strips whitespace.
  - `(string-prefix? prefix str)` / `(string-suffix? suffix str)`
  - `(string-contains? needle haystack)`
  - Verified on QEMU and RP2350 silicon.

#### 48.4e: Association & Property List Library (`/sd0/system/lib/dict.lisp`) — **COMPLETED**
* Added dictionary abstractions in [`tools/sd_root/system/lib/dict.lisp`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root/system/lib/dict.lisp):
  - `(alist-ref alist key [default])`
  - `(alist-set alist key val)`
  - `(alist-delete alist key)`
  - `(plist-get plist key [default])` / `(plist-put plist key val)`
  - Verified on QEMU and RP2350 silicon.

---

### Milestone 48.5: Dual-Profile Verification, Benchmarks & Memory Audit

* **Goal:** Verify zero-regression on non-PSRAM hardware, validate memory safety, and benchmark speed and node pool headroom across all supported targets.

#### Verification Protocol:

1. **Non-PSRAM Low-Resource Baseline Audit:**
   * **Target:** Raspberry Pi Pico 2 (`build/rp2350`, `CONFIG_BOARD_RP2350=1`, no PSRAM) and ESP32-C6-Zero (`build/esp32c6`).
   * **Checks:**
     - Boot into `lsh`. Inspect `(gc-stats)`:
       $$\text{Free Nodes} \ge 900 \quad (\text{out of } 1,024 \text{ total})$$
     - Execute full regression suite: `python3 tests/hw/test_rp2350.py` and `python3 tests/hw/test_esp32c6.py`.
     - Confirm `.bss` size of `user/lisp/lisp.c` did NOT increase by more than 128 bytes.
     - Confirm all new C builtins execute from `.rodata` Flash without allocating heap memory.
     - Confirm `cas.lisp` is NOT loaded at boot, preserving SRAM.

2. **High-Resource Workstation Validation:**
   * **Target:** Waveshare RP2350-LCD-7 (`build/rp2350-terminal`) and ESP32-P4-LCD-7B (`build/esp32p4-lcd7b`).
   * **Checks:**
     - Node pool sized at 16k–64k in PSRAM (`BULK_BSS`).
     - Load full CAS: `(load "/sd0/cas/cas.lisp")`.
     - Execute `(test-cas)` suite verifying `taylor`, `matrix-*`, and trigonometric identities.
     - Run live real-time canvas chart: `(sensor-plot "temp" 200 30)` on 800×480 and 1024×600 panels.

3. **Macro & Vector Stress Testing:**
   * Test macro hygiene, recursion, and expansion in `tests/test_lisp_cas.py`.
   * Create a 1,000-element vector on Pico 2; verify node pool usage is $\le 2$ nodes and GC does not reclaim elements prematurely.

---

## 4. Implementation Phasing & Task Dependency Matrix

| Step | Milestone | Files Modified | Memory Impact | Dependencies | Status |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1** | **48.1a** (`error`) | `user/lisp/lisp.c`, `user/lisp/builtins_table.h` | 0 B SRAM (Flash) | None | **Done** (QEMU & Silicon verified) |
| **2** | **48.1b** (`sleep-ms`) | `user/lisp/lisp.c`, `user/lisp/builtins_table.h` | 0 B SRAM (Flash) | `kernel/sched.h` | **Done** (QEMU & Silicon verified) |
| **3** | **48.1c** (Modular Loader) | `tools/sd_root/system/etc/init.lisp`, `stdlib.lisp` | Saves ~150 B SRAM on boot | None | **Done** (QEMU & Silicon verified) |
| **4** | **48.2a** (I2C Master) | `user/lisp/lisp.c`, `builtins_table.h` | 0 B SRAM (Flash) | `drivers/i2c.h` | **Done** (QEMU & Silicon verified) |
| **5** | **48.2b–d** (GPIO/ADC/RTC) | `user/lisp/lisp.c`, `builtins_table.h` | 0 B SRAM (Flash) | `drivers/gpio.h`, `adc.h` | Planned |
| **6** | **48.2e** (Sensor Plotting) | `/sd0/system/lib/chart.lisp` | On-demand filesystem | 48.1b, 48.2a | Planned |
| **7** | **48.3a–b** (Taylor & Trig) | `cas/calculus.lisp`, `cas/simplify.lisp` | On-demand filesystem | 48.1a | Planned |
| **8** | **48.3c** (Matrix Algebra) | `cas/matrix.lisp` | On-demand filesystem | None | Planned |
| **9** | **48.4a** (`define-macro`) | `user/lisp/lisp.c`, `user/lisp/include/lisp.h` | 0 B SRAM (Flash) | None | **Done** (QEMU & Silicon verified) |
| **10**| **48.4b** (`LISP_VECTOR`) | `user/lisp/lisp.c`, `user/lisp/include/lisp.h` | Drastically reduces SRAM | 48.4a | Planned |
| **11**| **48.4c–e** (Stdlib Modules) | `/sd0/system/lib/{macros,seq,str,dict}.lisp` | Modular filesystem | 48.4a | **Done** (QEMU & Silicon verified) |
| **12**| **48.5** (Verification) | `tests/test_lisp_expansion.py`, `tests/hw/*.py` | N/A | All steps | **In Progress** |

---

## 5. Non-Regression Invariants

* **Invariant 1 (Zero-Float in Kernel):** No soft-float or hardware FPU instructions permitted in any new C primitive or Lisp function. All math must use exact rationals or integer/fixed-point scaling.
* **Invariant 2 (Flash `.rodata` Table Sorting):** Every new primitive added to `user/lisp/builtins_table.h` must maintain **strict ASCII sorting** to preserve $O(\log N)$ binary search lookup.
* **Invariant 3 (Low-Resource Boot Headroom):** On Pico 2 and ESP32-C6 (`CONFIG_LISP_PROFILE_MINIMAL`), a clean boot into `lsh` must never leave fewer than **850 free nodes** in the 1,024-node pool.
