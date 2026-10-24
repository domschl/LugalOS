# LugalOS Interactive Graphics & Algorithm Demos (`demos`)

[Documentation](index.md) > [User Manual](user.md) > **Graphics & Algorithm Demos**

---

LugalOS provides ready-to-run interactive demonstrations in Lisp and C, showcasing canvas graphics, cellular automata, chaotic attractors, and hardware acceleration across display targets.

## 1. Demos Directory Locations

Demos are located in two primary storage locations:
* `/flash0/DEMOS/` (Embedded Flash ROM disk, available on all targets even without an SD card):
  * `CA.LIS`: Cellular automata simulator
  * `LORENZ.LIS`: Lorenz strange attractor
* `/sd0/demos/` (MicroSD storage):
  * `ca.lisp`: Cellular automata (Rule 30, 90, 110)
  * `lorenz.lisp`: Lorenz strange attractor in fixed-point math

## 2. Cellular Automata Demo (`ca.lisp` / `CA.LIS`)

Simulates one-dimensional elementary cellular automata on the graphical canvas:
* Rule 30: Chaotic pseudo-random pattern (Wolfram's generator)
* Rule 90: Fractal Sierpinski triangle
* Rule 110: Proven Turing-complete universal computation

### Running the Demo
```lisp
;; Load the demo script
(load "/sd0/demos/ca.lisp")
;; Or from flash:
;; (load "/flash0/DEMOS/CA.LIS")

;; Run Rule 30 with 3-pixel cell size (Ctrl-C to stop)
(ca 30 3)

;; Run Rule 90 (Sierpinski triangle)
(ca 90 2)

;; Run Rule 110 (Turing complete)
(ca 110 3)
```

## 3. Lorenz Strange Attractor Demo (`lorenz.lisp` / `LORENZ.LIS`)

Renders the classic 3D butterfly strange attractor projected onto the 2D canvas:
* Computed using pure integer fixed-point arithmetic (scale 1000) with zero floating-point operations.
* Integrates continuous differential equations:
  dx/dt = \sigma (y - x)
  dy/dt = x (\rho - z) - y
  dz/dt = x y - \beta z

### Running the Demo
```lisp
;; Load the Lorenz demo
(load "/sd0/demos/lorenz.lisp")

;; Run 3000 integration steps in a split canvas window
(lorenz 3000)
```

## 4. C Application Demos

In addition to Lisp demos, user-mode C applications can be compiled and executed directly from `/sd0/`:
* `/sd0/hello.c`: Minimal U-mode syscall demonstration
* `/sd0/fib.c`: Recursive and iterative Fibonacci benchmarking
* `/sd0/prime.c`: Sieve of Eratosthenes prime generation

### Compiling and Executing C Demos
```text
lsh> cc /sd0/hello.c /ram0/hello.elf
lsh> exec /ram0/hello.elf
```

---

**Navigation:** [<- User Manual](user.md) | [Documentation Index](index.md)
