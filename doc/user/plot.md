# Mathematical Plotting & Canvas Visualization (`plot`)

[Documentation](../README.md) > [User Manual](index.md) > [CAS](cas.md) > **Plotting**

---

LugalOS provides a hardware-accelerated 2D function and curve plotting subsystem (`/sd0/cas/plot.lisp`) that renders mathematical graphs directly into graphical terminal canvas windows across all display-equipped hardware targets (ESP32-P4-7B, Tab5, RP2350-Terminal).

## 1. Quick Start

Plotting automatically splits the screen to allocate a graphics canvas window next to your text console (`canvas-window 'split'`).

```lisp
;; Load plotting subsystem (automatically pre-loaded by ea)
(load "/sd0/cas/plot.lisp")

;; Plot a quadratic parabola y = x^2 from x = -3 to 3
(plot '(* x x) '(x -3 3))

;; Plot a cubic polynomial and its symbolic derivative on shared axes!
(plot-diff '(- (^ x 3) 1) '(x -2 2) '((title . "f(x) = x^3 - 1 and f'(x)")))
```

## 2. Core Plotting Commands

```
| Function | Signature | Description |
| :--- | :--- | :--- |
| plot | (plot expr '(var min max) [opts]) | Graphs y = f(x) over [min, max] with auto-scaled axes. |
| plot-diff | (plot-diff expr '(var min max) [opts]) | Differentiates f(x) via CAS and graphs f(x) & f'(x) together. |
| plot-multi | (plot-multi '(f1 f2 ...) '(var min max) [opts]) | Overlays multiple functions on shared coordinate axes. |
| plot-parametric | (plot-parametric fx fy '(t min max) [opts]) | Graphs parametric curve (x(t), y(t)) for t in [min, max]. |
```

## 3. Plotting Options (opts)

Options are passed as an association list of key-value pairs:
* `'title`: Title banner string displayed at the top of the canvas.
  Example: `'((title . "Harmonic Oscillator"))`
* `'samples`: Number of sampled evaluation points across the domain (default: 40–50).
  Example: `'((samples . 60))`
* `'layout`: Window arrangement:
  * `'split` (default): Shares display with the text terminal / shell.
  * `'canvas`: Full-screen canvas graph.
* `'ymin`, `'ymax`: Explicit overrides for vertical axis scaling bounds.

## 4. Examples

### Graphing a Function and Derivative
```lisp
;; Differentiate and graph x^3 - 1
(plot-diff '(- (^ x 3) 1) '(x -2 2) '((title . "x^3 - 1 and derivative")))
```

### Comparing Multiple Functions
```lisp
;; Compare sin(x) and cos(x)
(plot-multi '((sin x) (cos x)) '(x 0 7) '((title . "Trigonometric Waves")))
```

### Parametric Curves: Unit Circle
```lisp
;; Parametric unit circle x = cos(t), y = sin(t)
(plot-parametric '(cos t) '(sin t) '(t 0 7)
                 '((title . "Unit Circle") (samples . 60)))
```

## 5. Architectural Features
* **Zero Floating-Point in Kernel / M-Mode**: All coordinate transformations, aspect ratio adjustments, and axis tick calculations use exact rational arithmetic and fixed-point math.
* **Auto-Scaling**: Automatically calculates domain and range extents with padding and plots coordinate grid lines and numeric labels.

---

**Navigation:** [← Calculus Manual](calculus.md) | [CAS Manual](cas.md) | [User Manual](index.md) | [Documentation Index](../README.md)
