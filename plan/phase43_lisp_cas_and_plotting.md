# Phase 43 — Symbolic Computer Algebra System (CAS) & Mathematical Visualization

**Status: IN PROGRESS (Milestones 43.1, 43.2 & 43.3 COMPLETE, Verified on Hardware; 2026-10-03).**  
Branch: `lisp_symbolic`  
Follows Phase 42 (Lisp Symbolic Foundations & Engine Optimizations) and Phase 37/38 (Screen Layouts, Canvas API & PSRAM).

---

## 1. Executive Summary & Strategy

LugalOS operates across diverse hardware environments:
- **Resource-Constrained Microcontrollers (No PSRAM):** E.g. Raspberry Pi Pico 2 with only 520 KB SRAM, where every byte of `.bss` directly reduces free heap space.
- **Workstations & High-End Silicon:** E.g. Waveshare RP2350-LCD-7 (8 MB PSRAM, 800×480 RGB canvas) and ESP32-P4 (Dual 400 MHz RISC-V with internal L2 SRAM).

### The "Zero-Bloat" Hybrid Architecture

To provide an industrial-strength Computer Algebra System without penalizing presets that do not need symbolic mathematics (such as `rp2350-sensor`, `rp2350-gateway`, or `rp2350-clock`), Phase 43 adopts a **strictly layered architecture**:

```
┌────────────────────────────────────────────────────────────────────────┐
│  USER SPACE / FILESYSTEM LIBRARIES (/sd0/system/lib/cas/*.lisp)       │
│  - cas/simplify.lisp : Algebraic simplification & like-term collection │
│  - cas/poly.lisp     : Polynomial expansion & factoring                │
│  - cas/calculus.lisp : Symbolic differentiation & integration          │
│  - cas/solve.lisp    : Linear, quadratic, & rational equation solving   │
│  - cas/format.lisp   : Infix mathematical pretty-printing              │
│  - cas/plot.lisp     : 2D function & derivative canvas plotting        │
│  => ZERO kernel footprint, loaded on-demand via (require 'cas)         │
└────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│  CORE LISP ENGINE ENHANCEMENTS (Flash .rodata / .text, 0 bytes SRAM)   │
│  - Fast binary exponentiation: (expt base exp) for ints & rationals    │
│  - Exact integer square root: (isqrt n) for bignums                    │
│  - Canonical ordering: (symbol<? s1 s2), (string<? s1 s2)              │
│  - Fast math evaluators for plotting: (math-sin x), (math-cos x), etc.  │
│  - Leverages Phase 42 C foundations: match, subst, bignums, rationals  │
└────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│  HARDWARE DRIVERS & SUBSYSTEMS                                         │
│  - Canvas API (Phase 37/38): (canvas-window 'split), (canvas-line ...)  │
│  - Hardware Bignum Engine (Phase 42.7): 64 limbs, 2048-bit arithmetic  │
└────────────────────────────────────────────────────────────────────────┘
```

1. **Zero Permanent SRAM Impact:**  
   The CAS algorithms, term rewriting rules, calculus transformations, and plotting logic reside in modular Lisp files on the filesystem (`/sd0/system/lib/cas/` or flash filesystem). Presets that do not use CAS load nothing and pay **0 bytes of RAM and 0 bytes of flash**.
2. **C Performance Exceptions (Flash-only, 0 bytes SRAM):**  
   Only core arithmetic and ordering primitives where interpreted Lisp would incur combinatorial GC churn or stack depth (`expt`, `isqrt`, `symbol<?`, transcendental evaluation for screen plotting) are implemented in C. These reside entirely in `.rodata`/`.text` (Flash), incurring **zero permanent `.bss` overhead**.
3. **Interactive Graphical Visualization:**  
   The CAS connects directly to LugalOS's built-in canvas subsystem ([`kernel/screen.c`](file:///home/dsc/gith/domschl/lugalos/drivers/screen.c)). In workstation/terminal mode, running `(plot '(+ (* 2 (^ x 2)) (* -3 x) 1) (x -4 4))` automatically splits the screen, renders axes and tick marks, and graphs both expressions and their symbolic derivatives.

---

## 2. Milestone Breakdown

```
43.1 (Engine Math Foundations) ──► 43.2 (Canonicalization & Simplifier)
                                          │
43.3 (Polynomial Algebra) ◄───────────────┘
     │
     ├──► 43.4 (Symbolic Calculus: diff & integrate)
     │           │
     │           ├──► 43.5 (Equation & Linear System Solving)
     │           │
     │           └──► 43.6 (Infix Math Pretty-Printing)
     │
     └───────────────────────────────────► 43.7 (Canvas Plotting & Visualization)
                                                 │
                                                 ▼
                                           43.8 (Hardware Verification & Benchmarks)
```

---

### Milestone 43.1: Engine Mathematical Foundations (Flash C Builtins) — [COMPLETE]

* **Status:** Complete (2026-10-03).
* **Goal:** Equip the C interpreter with critical operations that are inefficient or impractical to write in interpreted Lisp.
* **Deliverables:**
  1. **Integer & Rational Exponentiation (`expt`, `^`):**
     - `(expt base power)`: Implemented in C using binary square-and-multiply ($O(\log N)$).
     - Integer base, integer power $\ge 0$: computes exact `LISP_INT` or `LISP_BIGNUM`.
     - Rational base $(a/b)^n = a^n / b^n$.
     - Negative powers: $(a/b)^{-n} = (b/a)^n$, turning integers into exact rationals (e.g. `(expt 2 -3)` $\rightarrow$ `1/8`).
  2. **Exact Integer Square Root (`isqrt`):**
     - `(isqrt n)`: Integer square root $\lfloor\sqrt{n}\rfloor$ for arbitrary-precision integers and bignums using integer Newton-Raphson iteration.
     - Crucial for simplifying radical expressions ($\sqrt{18} = 3\sqrt{2}$) and distance calculations without precision loss.
  3. **Symbol & String Lexicographical Ordering (`symbol<?`, `string<?`):**
     - Enables $O(N \log N)$ sorting of variables and terms for canonical polynomial representations.
     - In C, pointer comparison or single `strcmp()` pass, returning `#t` or `#f`.
  4. **Fast Numerical Math Primitives for Plotting:**
     - `(math-sin x)`, `(math-cos x)`, `(math-tan x)`, `(math-sqrt x)`, `(math-log x)`, `(math-exp x)`.
     - Accepts ints and rationals, evaluates via C `libm` / freestanding polynomial approximation, and returns rational or scaled integer coordinates.
* **SRAM & Storage Footprint:**
  - Placed in `static const builtin_t builtins[]` in `.rodata`.
  - SRAM impact: **0 bytes**. Flash impact: $\approx 800$ bytes.
* **Verification:**
  - `tests/test_lisp_cas.py`: 46/46 unit tests passing on QEMU.
  - Verified on live RP2350 silicon via interactive console.

---

### Milestone 43.2: Symbolic Expression Canonicalization & Pattern Simplifier (`cas/simplify.lisp`) — [COMPLETE]

* **Status:** Complete (2026-10-03).
* **Goal:** Transform arbitrary mathematical expressions into canonical, minimal algebraic forms.
* **Deliverables:**
  1. **Canonical Expression Representation:**
     - Operator prefix form: `(+ term ...)`, `(* factor ...)`, `(^ base exp)`.
     - Associative flattening: `(+ a (+ b c))` $\rightarrow$ `(+ a b c)`.
     - Order of terms: numbers collected into a single coefficient; symbols sorted by `symbol<?` (e.g. `(+ y x 3)` $\rightarrow$ `(+ 3 x y)`).
  2. **Algebraic Identity Rules:**
     - Additive identities: $x + 0 = x$, $0 + x = x$.
     - Multiplicative identities: $x \cdot 1 = x$, $1 \cdot x = x$, $x \cdot 0 = 0$, $0 \cdot x = 0$.
     - Exponent identities: $x^0 = 1$, $x^1 = x$, $1^x = 1$, $0^x = 0$.
     - Inverses: $x - x = 0$, $x / x = 1$, $-(-x) = x$.
  3. **Like-Term Collection:**
     - Linear combinations: $3x + 2x = 5x$, $x + x = 2x$, $4x - x = 3x$.
     - Multivariate like terms: $2xy^2 + 5xy^2 = 7xy^2$.
  4. **Constant Folding:**
     - Automatic exact simplification of all constant subexpressions via Phase 42.7 bignum/rational arithmetic:
       `(+ 1/2 (* 3 1/4))` $\rightarrow$ `5/4`.
  5. **Radical & Transcendental Simplification:**
     - Exact square roots and radical factorization ($\sqrt{18} = 3\sqrt{2}$, $\sqrt{72} = 6\sqrt{2}$, $\sqrt{1/4} = 1/2$, $\sqrt{x^2} = x$).
     - Elementary transcendental identities ($\ln(e^x) = x$, $\sin(0) = 0$, $\cos(0) = 1$).
  6. **Tooling & Storage Synchronization:**
     - Created `tools/p9sync.py` to synchronize staged SD cards over 9P.
     - Synchronized `cas/simplify.lisp` and system binaries to `/sd0/cas` on physical RP2350-terminal.
* **Verification:**
  - `tests/test_cas_simplify.py`: 65/65 unit tests passing on QEMU.
  - Integrated into `tests/runner.py` regression suite.
  - Verified live on physical RP2350-terminal silicon via interactive console (`/dev/ttyACM1`).

---

### Milestone 43.3: Polynomial Algebra & Expansion (`cas/poly.lisp`) — [COMPLETE]

* **Status:** Complete (2026-10-03).
* **Goal:** High-level algebraic manipulation of univariate and multivariate polynomials.
* **Deliverables:**
  1. **Polynomial Expansion (`expand`):**
     - Distributive multiplication over addition: $(a + b)(c + d) = ac + ad + bc + bd$.
     - Binomial & multinomial expansion using exact coefficients via binary exponentiation:
       `(expand '(^ (+ x 1) 4))` $\rightarrow$ `(+ 1 (* 4 x) (* 6 (^ x 2)) (* 4 (^ x 3)) (^ x 4))`.
     - Multi-factor products: `(expand '(* (+ x 1) (+ x 2) (+ x 3)))` $\rightarrow$ `(+ 6 (* 11 x) (* 6 (^ x 2)) (^ x 3))`.
     - Scalar and constant distributions.
  2. **Polynomial Inspection:**
     - `(poly-degree expr var)`: Returns highest non-negative integer exponent of `var`.
     - `(poly-coeffs expr var)`: Returns list of coefficients ordered by descending degree $(c_n, c_{n-1}, \dots, c_0)$, filling missing powers with 0.
     - `(poly-lead-coeff expr var)`: Leading coefficient ($c_n$).
  3. **Polynomial Evaluation & Horner's Rule:**
     - `(poly-eval-horner coeffs x)`: Exact polynomial evaluation using Horner's scheme.
     - `(poly-eval expr '((x . 5)))`: Evaluation with variable substitution and simplification.
  4. **Polynomial Long Division (`poly-div`):**
     - `(poly-div num den var)`: Returns `(list quotient remainder)` with exact rational coefficients.
  5. **High-Level Aliases:**
     - `(poly-add p1 p2)` and `(poly-mul p1 p2)`.
  6. **Storage Synchronization:**
     - Synchronized `cas/poly.lisp` to `/sd0/cas/poly.lisp` on physical RP2350-terminal using `tools/p9sync.py`.
* **Verification:**
  - `tests/test_cas_poly.py`: 47/47 unit tests passing on QEMU.
  - Integrated into `tests/runner.py` regression suite.
  - Verified live on physical RP2350-terminal silicon via interactive console (`/dev/ttyACM1`).

---

### Milestone 43.4: Symbolic Differentiation & Integration (`cas/calculus.lisp`) — [COMPLETE]

* **Status:** Complete (2026-10-03).
* **Goal:** Perform exact symbolic calculus transformations.
* **Deliverables:**
  1. **Symbolic Differentiation (`diff`):**
     - `(diff expr var [n])`: Computes the $n$-th derivative $\frac{d^n}{d\text{var}^n}(\text{expr})$.
     - Linearity: $(u + v)' = u' + v'$, $(c \cdot u)' = c \cdot u'$.
     - Product rule: $(u \cdot v)' = u' v + u v'$ generalized over multiple factors.
     - Quotient rule: $(u / v)' = \frac{u' v - u v'}{v^2}$.
     - Power & Chain rule: $(u^n)' = n u^{n-1} u'$, and $(u^v)' = u^v (v' \ln u + v \frac{u'}{u})$.
     - Trigonometric & Transcendental rules:
       $\frac{d}{dx}\sin(u) = \cos(u) u'$, $\frac{d}{dx}\cos(u) = -\sin(u) u'$,
       $\frac{d}{dx}\tan(u) = (1 + \tan^2(u)) u'$,
       $\frac{d}{dx}e^u = e^u u'$, $\frac{d}{dx}\ln(u) = \frac{u'}{u}$,
       $\frac{d}{dx}\arcsin(u) = \frac{u'}{\sqrt{1 - u^2}}$, $\frac{d}{dx}\arctan(u) = \frac{u'}{1 + u^2}$.
  2. **Symbolic Integration (`integrate` & `integrate-def`):**
     - `(integrate expr var)`: Indefinite integral $\int \text{expr} \, d\text{var}$.
     - Power rule: $\int x^n dx = \frac{x^{n+1}}{n+1}$ ($n \ne -1$), $\int \frac{1}{x} dx = \ln(x)$.
     - Linearity: $\int (a f + b g) = a \int f + b \int g$.
     - Polynomial integration: term-by-term integration of any expanded or factored polynomial.
     - Elementary trigonometric & exponential integrals with scaled arguments:
       $\int \cos(a x + b) dx = \frac{\sin(a x + b)}{a}$, $\int \sin(a x + b) dx = -\frac{\cos(a x + b)}{a}$, $\int e^{a x + b} dx = \frac{e^{a x + b}}{a}$.
     - Integration by parts: $\int x e^x dx = e^x (x - 1)$, $\int x \cos(x) dx = x \sin(x) + \cos(x)$, $\int x \sin(x) dx = \sin(x) - x \cos(x)$, $\int \ln(x) dx = x \ln(x) - x$.
     - Inverse trigonometric forms: $\int \frac{1}{x^2+1} dx = \arctan(x)$, $\int \frac{1}{\sqrt{1-x^2}} dx = \arcsin(x)$.
     - Definite integration: `(integrate-def expr var a b)` computing exact $F(b) - F(a)$.
     - Fundamental Theorem of Calculus verified: $\frac{d}{dx} \int f(x) dx = f(x)$.
  3. **Lisp Engine Enhancements:**
     - Supported standard Scheme rest-parameter binding `(define (f . args) ...)` and `(define (f x . rest) ...)` in `user/lisp/lisp.c`.
     - Hardened `streq` against NULL pointer dereferences on memory exhaustion.
     - Sized QEMU `CONFIG_LISP_NODE_POOL` to 32768 in `board-rv32-nommu.cmake` and `board-rv64-mmu.cmake`.
  4. **Hardware Verification:**
     - Synchronized `cas/calculus.lisp` to `/sd0/cas/calculus.lisp` on physical RP2350-terminal via 9P (`tools/p9sync.py`).
     - Verified interactive differentiation, integration, higher-order derivatives, and definite integration live on Hazard3 RISC-V silicon.
* **Verification:**
  - `tests/test_cas_calculus.py`: 61/61 unit tests passing on both RV32 and RV64 QEMU targets.
  - Integrated into `tests/runner.py` regression suite.
  - Verified live on physical RP2350-terminal silicon via interactive console (`/dev/ttyACM1`).

---

### Milestone 43.5: Exact Equation & System Solving (`cas/solve.lisp`) — [COMPLETE]

* **Status:** Complete (2026-10-03). `tests/test_cas_solve.py`: 17/17 on RV32 and RV64; integrated into `tests/runner.py`; verified on RP2350-terminal silicon (linear, quadratic, 2x2 system; the 3x3 reply was cut off by the probe's read timing, but passes on QEMU).
* **Notes:** Also provides `solve-linear`, `solve-quadratic` (double roots, negative discriminants via `i`, symbolic discriminants), pure `a*x^n + c` roots, and Gauss-Jordan with row pivoting (`singular` is returned when there is no unique solution; `none`/`all` for contradictory/identity linear equations). `map` is single-list only in this engine, so row operations use explicit loops.
* **Goal:** Solve algebraic equations analytically.
* **Deliverables:**
  1. **Linear Equations:**
     - `(solve '(= (+ (* 3 x) 4) 10) 'x)` $\rightarrow$ `(x . 2)`.
     - Supports fractional solutions with exact rationals: `(solve '(= (* 3 x) 2) 'x)` $\rightarrow$ `(x . 2/3)`.
  2. **Quadratic Equations:**
     - `(solve '(= (+ (^ x 2) (* -5 x) 6) 0) 'x)` $\rightarrow$ `((x . 2) (x . 3))`.
     - Automatic discriminant computation $\Delta = b^2 - 4ac$ with exact radical / rational demotion.
  3. **Linear Systems (Exact Rational Gaussian Elimination):**
     - `(solve-system '((= (+ (* 2 x) y) 5) (= (- x (* 3 y)) -8)) '(x y))` $\rightarrow$ `((x . 1) (y . 3))`.
     - Solves without any floating-point rounding error using exact bignum/rational arithmetic.

---

### Milestone 43.6: Infix Formatting & Mathematical Pretty-Printing (`cas/format.lisp`) — [COMPLETE]

* **Status:** Complete (2026-10-03).
* **Goal:** Present mathematical expressions cleanly to human users and provide bidirectional infix conversion so users can use natural mathematical notation.
* **Deliverables:**
  1. **Infix String Formatter (`to-infix`):**
     - Converts internal prefix ASTs to standard algebraic notation with operator precedence:
       `(+ (* 3 (^ x 2)) (* 2 x) 1)` $\rightarrow$ `"3*x^2 + 2*x + 1"`.
     - Handles parentheses minimally based on operator precedence ($*$, $/$ bind tighter than $+$, $-$).
     - Formats quotients with negative exponents naturally: `(* 3 (^ (+ x 1) -1))` $\rightarrow$ `"3/(1 + x)"`.
     - Formats solution pairs and solution lists: `((x . 2) (x . 3))` $\rightarrow$ `"[x = 2, x = 3]"`.
  2. **Bidirectional Infix Parser (`from-infix`):**
     - Full recursive-descent parser with precedence climbing ($=$, $+/-$, $*//$, unary $-$, power `^`/`**` right-associative).
     - Implicit multiplication: `2x`, `3sin(x)`, `(x+1)(x-1)`.
     - Multi-letter juxtaposition splitting: `xy` $\rightarrow$ `(* x y)`.
     - Exact decimal parsing: `0.25` $\rightarrow$ `1/4`, `.5` $\rightarrow$ `1/2`.
     - List parsing: `[a, b, c]` $\rightarrow$ `(cas-list a b c)`.
  3. **High-Level Front-Ends (`calc`, `math`):**
     - `(calc str)` parses infix expression, evaluates CAS commands (`diff`, `integrate`, `expand`, `simplify`, `solve`), and returns the simplified prefix AST or solution.
     - `(math str)` parses, computes, and prints the result back in clean infix math notation.
  4. **CAS Entrypoint (`cas/cas.lisp`):**
     - Single loader that loads `simplify.lisp`, `poly.lisp`, `calculus.lisp`, `solve.lisp`, and `format.lisp`.
  5. **New Lisp Primitives:**
     - Added `string->symbol` and `symbol->string` to `user/lisp/lisp.c` and registered in `builtins_table.h`.
* **Verification:**
  - `tests/test_cas_format.py`: 43/43 unit tests passing on RV32 and RV64 QEMU.
  - Verified live on physical RP2350-terminal silicon via interactive console (`/dev/ttyACM1`):
    - `(math "diff(x*sin(x), x)")` $\rightarrow$ `sin(x) + x*cos(x)`
    - `(math "solve(x^2 - 5x + 6 = 0, x)")` $\rightarrow$ `[x = 2, x = 3]`
    - `(math "integrate(3x^2 + 4x + 5, x)")` $\rightarrow$ `x^3 + 2*x^2 + 5*x`
  - Synchronized `cas/cas.lisp` and `cas/format.lisp` to physical SD card via `tools/p9sync.py`.

---

### Milestone 43.7: Mathematical Plotting on LugalOS Canvas (`cas/plot.lisp`) — [COMPLETE]

* **Status:** Complete (2026-10-03).
* **Goal:** Provide visual graphing of mathematical functions and curves on the LCD-7, ST7735, and virtual panel.
* **Deliverables:**
  1. **Screen Layout Integration:**
     - Automatically requests split window via `(canvas-window 'split)` and hooks redraw callback with `canvas-on-redraw`.
     - Detects canvas dimensions dynamically via `(canvas-size)`.
  2. **2D Function Plotter (`plot`):**
     - `(plot expr (var min max) [options])`:
       Example: `(plot "x^2" '(x -3 3))` or `(plot '(- (^ x 3) (* 3 x)) '(x -3 3))`
     - Evaluates function across $N$ sample points along domain using 100% exact rational & fixed-point arithmetic (`plot-floor`).
     - Auto-scales or sets custom $[y_{min}, y_{max}]$ range.
     - Renders background, coordinate grid, origin axes $(x=0, y=0)$, and tick labels using `canvas-line`, `canvas-frame`, and `canvas-text`.
     - Draws smooth connected curve segments using `(canvas-line x0 y0 x1 y1 color)`.
  3. **Calculus Visualizer (`plot-diff`):**
     - Graphs $f(x)$ and its symbolic derivative $f'(x) = \frac{d}{dx}f(x)$ on the same axes with automatic dual auto-scaling and combined formula titles.
     - Example: `(plot-diff "x*sin(x)" '(x -6 6))`.
  4. **Parametric & Multi-Function Plotting:**
     - `(plot-multi '(expr1 expr2 ...) (var min max) [options])`
     - `(plot-parametric x-expr y-expr (t tmin tmax) [options])`:
       Example: `(plot-parametric '(cos t) '(sin t) '(t 0 7))` (unit circle).
* **Verification:**
  - `tests/test_cas_plot.py`: 20/20 unit tests passing on RV32 and RV64 QEMU.
  - Verified live on physical RP2350-LCD-7 silicon:
    - `(plot "x^2" '(x -3 3))` $\rightarrow$ verified with screenshot `shot-002.png`
    - `(plot-diff "x*sin(x)" '(x -6 6))` $\rightarrow$ verified with screenshot `shot-003.png`
    - `(plot-parametric '(cos t) '(sin t) '(t 0 7))` $\rightarrow$ verified with screenshot `shot-004.png`
  - Transferred screenshots over 9P (`tools/p9sync.py` / `lugal9p get`).

---

### Milestone 43.8: Hardware Verification & Benchmarks on Real Silicon — [COMPLETE]

* **Status:** Complete (2026-10-03).
* **Goal:** Ensure high performance, stability, and zero regressions on target hardware.
* **Deliverables:**
  1. **Automated Host & QEMU Suite:**
     - 7 automated CAS test suites created, running across RV32 and RV64 targets:
       - `tests/test_lisp_cas.py` (46 tests: math foundations, bignum/rational expt, isqrt, trig)
       - `tests/test_cas_simplify.py` (65 tests: canonical AST, identities, constant folding)
       - `tests/test_cas_poly.py` (47 tests: expansion, polynomial division, Horner evaluation)
       - `tests/test_cas_calculus.py` (61 tests: differentiation, integration, FTC)
       - `tests/test_cas_solve.py` (17 tests: linear, quadratic, Gauss-Jordan systems)
       - `tests/test_cas_format.py` (43 tests: bidirectional infix parsing, precedence, formatting)
       - `tests/test_cas_plot.py` (20 tests: function evaluation, bounds, coordinate transformation)
     - Total: **299 / 299 automated CAS tests passing 100%**.
     - Full master regression suite (`tests/runner.py`): **463 / 463 tests passing** (0 failures).
  2. **Silicon Verification on RP2350 (Hazard3 RV32 @ 150 MHz):**
     - Verified interactive CAS and math front-end on physical Waveshare RP2350-LCD-7 workstation.
     - Live hardware execution via dual CDC-ACM console (`/dev/ttyACM1`):
       - `(math "diff(x*sin(x), x)")` $\rightarrow$ `sin(x) + x*cos(x)` (~25 ms)
       - `(math "solve(x^2 - 5x + 6 = 0, x)")` $\rightarrow$ `[x = 2, x = 3]` (~40 ms)
       - `(math "integrate(3x^2 + 4x + 5, x)")` $\rightarrow$ `x^3 + 2*x^2 + 5*x` (~35 ms)
     - Live LCD canvas plotting verified and captured via hardware screenshot utility `(screenshot)` over 9P (`tools/p9sync.py`):
       - Parabola $y = x^2$ (`shot-002.png`)
       - Calculus visualizer $x \sin x$ with derivative $x \cos x + \sin x$ (`shot-003.png`)
       - Parametric unit circle $(\cos t, \sin t)$ (`shot-004.png`)
     - Plot rendering latency: ~1.8 seconds for 40 rational bignum sample evaluations at 150 MHz.
  3. **Verification on ESP32-P4 (Dual RV32 @ 360 MHz):**
     - Packaged CAS into FAT32 flash disk image (`flashfs.bin`) and flashed to SPI flash region `0x110000` via `tools/p4flash.py`.
     - Verified `poly.lisp` loading and evaluation on silicon.
     - S4 node pool safety verified: on boards without PSRAM (where `NODE_POOL_SIZE = 2048` to preserve LOWRAM), loading full 14,000+ node multi-module CAS triggers safe exhaustion report without hanging or kernel corruption.
  4. **Memory Impact Audit (Zero SRAM Overhead):**
     - Audited static memory consumption using `size` across all board presets:
       - `rp2350`: +0 bytes SRAM (+88 bytes flash)
       - `rp2350-terminal`: +0 bytes SRAM (+88 bytes flash)
       - `rp2350-clock`: +0 bytes SRAM (+88 bytes flash)
       - `rp2350-gateway`: +0 bytes SRAM (+88 bytes flash)
       - `esp32p4`: +0 bytes SRAM (+88 bytes flash)
       - `rv32`: +0 bytes SRAM (+88 bytes flash)
       - `rv64`: +0 bytes SRAM (+88 bytes flash)
     - All CAS algorithms reside purely on filesystem storage (`/sd0/cas` and `/flash0/cas`), with zero runtime SRAM impact on non-CAS workloads.

---

## 3. Risk Assessment & Mitigations

| Risk | Impact | Mitigation |
| :--- | :---: | :--- |
| **AST Tree Churn / Heap Exhaustion** | Med | Low-memory boards (Pico 2 with 1024 nodes) will hit `"Node pool exhausted"` on huge expressions. The Lisp GC safepoint and safe unwind ensure it fails gracefully without corrupting the kernel heap. |
| **Deep Recursion in Simplifier** | Low | Phase 42.9 stack headroom check prevents C stack overflow; algebraic flattening minimizes tree depth. |
| **Canvas Redraw Overhead** | Low | Function curves are rendered into the canvas tile once and cached; layout switching uses Phase 38.8 PSRAM canvas preservation. |
| **Transcendental Calculation Precision** | Low | Pure symbolic manipulations remain 100% exact rational; numerical conversion is deferred solely to pixel rasterization in `plot.lisp`. |
