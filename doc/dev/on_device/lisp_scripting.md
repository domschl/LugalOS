# LugalOS Lisp Dialect, Primitives & Scripting

[Documentation](../../README.md) > [Developer Manual](../index.md) > **Lisp Dialect, Primitives & Scripting**

---

LugalOS features a self-contained Scheme-like Lisp machine (`user/lisp/lisp.c`) that serves as the interactive system console, an appliance scripting language, and the mathematical runtime for the Computer Algebra System (CAS).

---

## 1. Dialect Overview & Special Forms

The dialect is lexically scoped, dynamically typed, and evaluates expressions in applicative order.

### Special Forms
* `quote` / `'`: Inhibits evaluation.
* `quasiquote` / `` ` ``, `unquote` / `,`, `unquote-splicing` / `,@`: Template construction.
* `lambda`: Function definition with lexical closures `(lambda (x y) (+ x y))`.
* `define`: Global binding `(define name val)` and shorthand `(define (fn args...) body...)`.
* `set!`: Mutates existing binding.
* `if`: Conditional branching `(if test then else)`.
* `cond`: Multi-branch conditional `(cond ((= x 1) 'one) (else 'other))`.
* `let` / `let*`: Local bindings.
* **Named `let`**: Tail-recursive loop construct:
  ```lisp
  (let loop ((i 0) (sum 0))
    (if (> i 10)
        sum
        (loop (+ i 1) (+ sum i))))
  ```
  *(Note: `letrec` is not built-in; use named `let` for local recursion).*
* `while`: Imperative loop construct `(while test expr...)`.
* `and` / `or`: Short-circuiting logical operations.
* `define-macro`: Syntactic macro expansion.

---

## 2. Zero-SRAM Builtins Architecture

To support hundreds of primitives without exhausting the 520 KB SRAM limit on RP2350 or ESP32-C6:
* **Storage in Flash `.rodata`**: All core C primitives are compiled into static tables in [`user/lisp/builtins_table.h`](file:///home/dsc/Source/gith/domschl/lugalos/user/lisp/builtins_table.h).
* **Binary Search**: When resolving an identifier, the evaluator first checks `global_env`. If absent, it queries `builtins_table` using $O(\log N)$ `bsearch()`.
* **Zero Overhead**: Primitives consume **0 bytes of `.bss` SRAM** and **0 cons pairs in `global_env`**.

---

## 3. Standard Library & Modular Packages

LugalOS uses a two-tier library model:
1. **Boot Library (`/sd0/system/etc/stdlib.lisp`)**: A minimal set of essential combinators (`not`, `print`, `when`, `unless`, `identity`) loaded at boot.
2. **Modular Packages (`/sd0/system/lib/`)**: Loaded on demand via `(require 'package)`:
   * `(require 'seq)`: Sequence utilities (`map`, `filter`, `foldl`, `foldr`, `zip`).
   * `(require 'str)`: String manipulation (`string-split`, `string-join`, `trim`).
   * `(require 'dict)`: Key-value association dictionaries and hash tables.
   * `(require 'matrix)`: Vector and matrix linear algebra routines.
   * `(require 'chart)`: Real-time graphical charting on the ribbon canvas.
   * `(require 'ota)`: The node's update state as data, read from `/dev/ota`. `(ota-info)` returns an association list of `("key" . "value")` strings (`board`, `layout`, `version`, `running`, `trial`, `slot_a`, `slot_b`, `at_a`, `at_b`, ... ; see [updates](../../admin/updates.md)); `(ota-get "running")` returns one value or `#f`. Read-only.
3. **Board identity**: `(board)` returns the board file's id (`esp32p4-nano`); `(board-info)` returns the one-line description the console banner and the boot log use: `PRESET vVERSION (HARDWARE, CHIP) "NAME" uid UID`. The same facts, one per line, are `/proc/board`.

---

## 4. Hardware & System Primitives

Lisp scripts can interact directly with system services and microcontroller hardware:

### Hardware I/O & Timers
* `(sleep-ms <ms>)`: Cooperative sleep without spinning the CPU.
* `(time-ms)`: System uptime in milliseconds.
* `(i2c-read <addr> <reg> <n>)`: Read $N$ bytes from I2C bus address and register.
* `(i2c-write <addr> <reg> <byte-list>)`: Write bytes to I2C peripheral.
* `(gpio-set <pin> <val>)`: Set GPIO logic level (0 or 1).
* `(gpio-get <pin>)`: Read GPIO logic level.

### Sensors
Readings come from the sensor hub's cache, so no call waits on the I2C bus. Values are integers in each channel's fixed-point unit ([sensors](../../user/sensors.md)).
* `(sensor-list)`: The detected devices, as symbols (`(bme280 ina226)`).
* `(sensor-read)`: Everything, as `((hw (dev (chan . val) ...) ...) (inferred ...) (fused ...))`.
* `(sensor-read 'dev)`: One device's channels; `(sensor-read 'ina226 'voltage)` one value; add `'age` for its age in seconds.
* `(sensor-read 'chan)`: The best source of a channel (`'temp`, `'co2`, ...).
* Channel names: `temp`, `pressure`, `humidity`, `lux`, `eco2`, `tvoc`, `gas-res`, `co`, `no2`, `nh3`, `co2`, `pressure-msl`, `abs-humidity`, `dew-point`, `iaq`, `voltage` (mV), `current` (mA), `power` (mW), `battery` (0.1 %), `accel-x/y/z` (mg), `gyro-x/y/z` (0.1 deg/s).
* `(sensor-cal ['save | 'restore | 'clear])`: Gas-sensor calibration baselines.

### Filesystem & Process Control
* `(load <path>)`: Load and evaluate Lisp source file.
* `(read-file <path>)`: Read file content directly into a Lisp string.
* `(write-file <path> <string>)`: Write string content to disk.
* `(spawn <elf-path>)`: Launch an isolated U-mode program in the background.

### Privileged Primitives
`poke`, `net-config`, `net-identity` (with a name: renaming), `identity-name`, `identity-provision`, `identity-key`, `peers-add`, `peers-remove` and `wlan-set` change keys, identity, network configuration or memory. They work from the console and from scripts. Evaluated for an `ea` tool call they refuse and return `#f`, and file writes under any `system/etc/` directory fail there too (`kernel/include/kernel/privilege.h`).

---

## 5. Computer Algebra System (CAS) Scripting

To utilize symbolic mathematics in Lisp:

```lisp
lsh> lisp
(load "/sd0/cas/simplify.lisp")

;; Simplify expressions
(simplify '(+ (* 2 x) (* 3 x)))
;; => (* 5 x)

;; Symbolic differentiation
(diff '(* x (sin x)) 'x)
;; => (+ (sin x) (* x (cos x)))

;; Exact polynomial roots
(poly-factor '(+ (^ x 2) (* 5 x) 6) 'x)
;; => (* (+ x 2) (+ x 3))
```
For complete CAS documentation, see the [CAS Manual](../../user/cas.md).

---

**Navigation:** [<- C Programming](c_programming.md) | [Developer Manual](../index.md) | [Documentation Index](../../README.md)
