# Phase 43 — Symbolic Computer Algebra System (CAS) & Mathematical Visualization

**Status: IN PROGRESS (Milestones 43.1 & 43.2 COMPLETE, Verified on Hardware; 2026-10-03).**  
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

### Milestone 43.3: Polynomial Algebra & Expansion (`cas/poly.lisp`)

* **Goal:** High-level algebraic manipulation of univariate and multivariate polynomials.
* **Deliverables:**
  1. **Polynomial Expansion (`expand`):**
     - Distributive multiplication over addition: $(a + b)(c + d) = ac + ad + bc + bd$.
     - Binomial expansion using exact coefficients: `(expand '(^ (+ x 1) 4))` $\rightarrow$ `(+ (^ x 4) (* 4 (^ x 3)) (* 6 (^ x 2)) (* 4 x) 1)`.
  2. **Polynomial Inspection:**
     - `(poly-degree expr var)`: Returns highest exponent of `var`.
     - `(poly-coeffs expr var)`: Returns list of coefficients ordered by descending degree.
     - `(poly-lead-coeff expr var)`: Leading coefficient.
  3. **Polynomial Evaluation & Horner's Rule:**
     - `(poly-eval expr '((x . 5)))`: Fast evaluation using Horner's method.

---

### Milestone 43.4: Symbolic Differentiation & Integration (`cas/calculus.lisp`)

* **Goal:** Perform exact symbolic calculus transformations.
* **Deliverables:**
  1. **Symbolic Differentiation (`diff`):**
     - `(diff expr var [n])`: Computes the $n$-th derivative $\frac{d^n}{d\text{var}^n}(\text{expr})$.
     - Linearity: $(u + v)' = u' + v'$, $(c \cdot u)' = c \cdot u'$.
     - Product rule: $(u \cdot v)' = u' v + u v'$.
     - Quotient rule: $(u / v)' = \frac{u' v - u v'}{v^2}$.
     - Power & Chain rule: $(u^n)' = n u^{n-1} u'$, and $(u^v)' = u^v (v' \ln u + v \frac{u'}{u})$.
     - Trigonometric & Transcendental rules:
       $\frac{d}{dx}\sin(u) = \cos(u) u'$, $\frac{d}{dx}\cos(u) = -\sin(u) u'$,
       $\frac{d}{dx}\tan(u) = (1 + \tan^2(u)) u'$,
       $\frac{d}{dx}e^u = e^u u'$, $\frac{d}{dx}\ln(u) = \frac{u'}{u}$.
  2. **Basic Symbolic Integration (`integrate`):**
     - `(integrate expr var)`: Indefinite integral $\int \text{expr} \, d\text{var}$.
     - Power rule: $\int x^n dx = \frac{x^{n+1}}{n+1}$ ($n \ne -1$), $\int \frac{1}{x} dx = \ln(x)$.
     - Linearity: $\int (a f + b g) = a \int f + b \int g$.
     - Elementary trigonometric & exponential integrals: $\int \cos(x) dx = \sin(x)$, $\int \sin(x) dx = -\cos(x)$, $\int e^x dx = e^x$.

---

### Milestone 43.5: Exact Equation & System Solving (`cas/solve.lisp`)

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

### Milestone 43.6: Infix Formatting & Mathematical Pretty-Printing (`cas/format.lisp`)

* **Goal:** Present mathematical expressions cleanly to human users.
* **Deliverables:**
  1. **Infix String Formatter (`to-infix`):**
     - Converts internal prefix ASTs to standard algebraic notation with operator precedence:
       `(+ (* 3 (^ x 2)) (* 2 x) 1)` $\rightarrow$ `"3*x^2 + 2*x + 1"`.
     - Handles parentheses minimally based on precedence rules ($*$, $/$ bind tighter than $+$, $-$).
  2. **Interactive CAS REPL Mode:**
     - `(cas-repl)`: Dedicated prompt (`cas> `) where typing `(diff '(* x (sin x)) 'x)` automatically simplifies and displays results in infix format:
       ```lsh
       cas> (diff '(* x (sin x)) 'x)
       => sin(x) + x*cos(x)
       ```

---

### Milestone 43.7: Mathematical Plotting on LugalOS Canvas (`cas/plot.lisp`)

* **Goal:** Provide visual graphing of mathematical functions and curves on the LCD-7, ST7735, and virtual panel.
* **Deliverables:**
  1. **Screen Layout Integration:**
     - Automatically requests split window via `(canvas-window 'split)` or full canvas.
     - Detects canvas dimensions via `(canvas-size)`.
  2. **2D Function Plotter (`plot`):**
     - `(plot expr (var min max) [options])`:
       Example: `(plot '(- (^ x 3) (* 3 x)) (x -3 3))`
     - Evaluates function across $N$ sample points along the canvas width.
     - Auto-scales or sets custom $[y_{min}, y_{max}]$ range.
     - Renders background, coordinate grid, origin axes $(x=0, y=0)$, and tick labels using `canvas-line` and `canvas-text`.
     - Draws smooth connected curve segments using `(canvas-line x0 y0 x1 y1 color)`.
  3. **Calculus Visualizer (`plot-diff`):**
     - Graphs $f(x)$ and its symbolic derivative $f'(x) = \frac{d}{dx}f(x)$ on the same axes in distinct colors (e.g. blue for $f(x)$, amber/red for $f'(x)$), demonstrating symbolic calculus visually.
  4. **Parametric & Multi-Function Plotting:**
     - `(plot-multi '(sin cos) (x -3.14 3.14))`
     - `(plot-parametric x-expr y-expr (t tmin tmax))` (e.g. circles, ellipses, Lissajous curves).

---

### Milestone 43.8: Hardware Verification & Benchmarks on Real Silicon

* **Goal:** Ensure high performance, stability, and zero regressions on target hardware.
* **Deliverables:**
  1. **Automated Host & QEMU Suite:**
     - Create [`tests/test_lisp_cas.py`](file:///home/dsc/gith/domschl/lugalos/tests/test_lisp_cas.py) covering simplification, expansion, differentiation, integration, equation solving, and formatting.
  2. **Silicon Verification on RP2350 (Hazard3):**
     - Verify on physical Pico 2 and RP2350-LCD-7 workstation.
     - Verify memory headroom under deep symbolic tree evaluations (confirming stack guard and GC behave safely).
  3. **Verification on ESP32-P4:**
     - Verify cross-target execution on dual RV32 @ 360 MHz.
  4. **Memory Impact Audit:**
     - Run `size` across all board presets (`rp2350`, `rp2350-terminal`, `rp2350-clock`, `rp2350-gateway`, `esp32p4`).
     - Confirm that non-CAS presets incur **0 bytes of SRAM change** and minimal flash change ($\le 1\text{ KB}$).

---

## 3. Risk Assessment & Mitigations

| Risk | Impact | Mitigation |
| :--- | :---: | :--- |
| **AST Tree Churn / Heap Exhaustion** | Med | Low-memory boards (Pico 2 with 1024 nodes) will hit `"Node pool exhausted"` on huge expressions. The Lisp GC safepoint and safe unwind ensure it fails gracefully without corrupting the kernel heap. |
| **Deep Recursion in Simplifier** | Low | Phase 42.9 stack headroom check prevents C stack overflow; algebraic flattening minimizes tree depth. |
| **Canvas Redraw Overhead** | Low | Function curves are rendered into the canvas tile once and cached; layout switching uses Phase 38.8 PSRAM canvas preservation. |
| **Transcendental Calculation Precision** | Low | Pure symbolic manipulations remain 100% exact rational; numerical conversion is deferred solely to pixel rasterization in `plot.lisp`. |
