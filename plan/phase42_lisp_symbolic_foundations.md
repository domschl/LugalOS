# Phase 42 — Lisp Symbolic Foundations & Engine Optimizations

**Status: DRAFT / PLANNED (2026-10-02).**
Branch: `lisp_symbolic`
Follows Phase 13 (Lisp Engine Extensions), Phase 37.3a (Screen/Lisp Built-ins), and Phase 38.5 (PSRAM Bulk BSS).

---

## Executive Summary & Hardware Context

LugalOS features a self-contained Lisp interpreter (`user/lisp/lisp.c`) that doubles as the system shell (`kernel/shell.c`). The target platforms are freestanding RISC-V hardware:
- **RP2350:** Dual Hazard3 3-stage in-order RISC-V cores (`RV32IMAC`, 144–150 MHz, **no ARM support**). The Hazard3 cores access external QSPI flash via a 16 KB unified instruction/data XIP cache.
- **ESP32-P4:** Dual-core high-performance RISC-V (`RV32IMAFC`, up to 400 MHz) with 32 KB L1D / 32 KB L1I caches and 512 KB/768 KB internal L2 SRAM.
- **Memory Diversity:** Some boards feature 8–32 MB external PSRAM (e.g., Waveshare RP2350-LCD-7 with 65,536 nodes mapped to `.bss.bulk`), while others operate purely on ~512 KB internal SRAM (Pico 2, 1,024 nodes).
- **Toolchain & Build:** CMake presets support varied persona and board targets under `-ffreestanding -nostdlib`.

This phase establishes the **language foundations for symbolic mathematics** (AST manipulation, exact arithmetic, quasiquoting, pattern matching) and delivers **critical interpreter optimizations** tailored for Hazard3 in-order execution. It purposefully excludes user-level CAS algorithms (e.g., `cas.lisp`), focusing exclusively on core interpreter mechanics.

---

## Architectural Principles

1. **Zero SRAM Impact on Constrained Boards:**
   Flash memory (4–16 MB) is abundant across all targets. All new primitive tables, keyword singletons, and core dispatch structures must reside strictly in `.rodata` / `.text`. SRAM usage on non-PSRAM boards must not increase.
2. **Hazard3 & Pipeline Sympathy:**
   The Hazard3 core lacks dynamic branch prediction and out-of-order execution. Hot paths must minimize function call overhead, eliminate multi-iteration string comparisons, and favor single-cycle register operations (`beq`).
3. **Fail-Safe Resource Bounds:**
   Symbolic manipulation can generate significant AST churn. Low-RAM boards retain existing node limits and GC safepoints; complex symbolic operations that exceed working memory will fail safely with `"Node pool exhausted"` without corrupting the kernel heap.
4. **Preserve Complete Backward Compatibility:**
   All existing POSIX-shell commands, scripts, REPL commands, and board drivers must remain fully operational and pass all regression suites.

---

## Detailed Milestones

```
42.1 (Hot-Path Micro-Opts) ──► 42.2 (Flash-Resident Builtin bsearch)
                                     │
42.3 (Symbol Interning & Singletons) ◄
     │
     ├──► 42.4 (Dotted Pairs & Reader) ──► 42.5 (Quasiquote & Splicing)
     │                                           │
     ├──► 42.6 (Tree Equality & Accessors) ◄─────┘
     │           │
     │           ├──► 42.7 (Exact Rational Arithmetic)
     │           │
     │           └──► 42.8 (AST Rewrite & Pattern Matching Primitives)
     │
     └───────────────────────────────────► 42.9 (Depth Tuning & Verification)
```

---

### 42.1: Hot-Path Interpreter Micro-Optimizations

* **Problem:** In [`user/lisp/lisp.c`](../user/lisp/lisp.c), several frequently executed paths incur avoidable execution overhead on Hazard3:
  1. [`streq()`](../user/lisp/lisp.c#L196) unconditionally enters a character-comparison loop even when the first characters mismatch.
  2. [`lisp_eval_step()`](../user/lisp/lisp.c#L4454-L4735) executes up to 10 sequential `streq()` calls testing for special forms (`quote`, `if`, `begin`, `let`, `let*`, `while`, `cond`, `define`, `set!`, `lambda`) before evaluating standard function applications.
  3. [`prim_chain_compare()`](../user/lisp/lisp.c#L1113) and [`prim_eq()`](../user/lisp/lisp.c#L1096) use [`lisp_list_ref(args, i)`](../user/lisp/lisp.c#L1002) in a loop, traversing the argument list from the head repeatedly and turning argument verification into an $O(N^2)$ operation.
* **Design & Changes:**
  1. Add fast-path checks in `streq()`:
     ```c
     static inline int streq(const char *s1, const char *s2) {
         if (s1 == s2) return 1;
         if (*s1 != *s2) return 0;
         while (*s1 && (*s1 == *s2)) { s1++; s2++; }
         return *s1 == *s2;
     }
     ```
  2. Implement an initial character filter / `switch` in `lisp_eval_step()` for special forms so that normal procedure calls (e.g. `+`, `car`, user functions) bypass all 10 checks in 1–2 cycles.
  3. Replace `lisp_list_ref` loops in comparison primitives with a direct linear pointer traversal `for (lisp_val_t *c = ...; c; c = c->u.pair.cdr)`.
* **Verification:**
  - Verify all standard arithmetic and comparison operations (`=`, `<`, `>`, `<=`, `>=`) retain exact semantics.
  - Profile cycle counts for tight loops (e.g., `(while (< i 1000) (set! i (+ i 1)))`) in QEMU and real RP2350 hardware.

---

### 42.2: Compile-Time Sorted `.rodata` Builtins (`bsearch`)

* **Problem:** The current builtins table (`builtins[BUILTIN_MAX]`) is stored in `.bss` (SRAM) with a hard cap of 184 entries ([`lisp.c:962`](../user/lisp/lisp.c#L962)). Today, 176 entries are already used. Adding primitives will either overflow the table or waste scarce SRAM. Furthermore, `builtin_get()` performs an $O(N)$ linear scan on every primitive lookup.
* **Design & Changes:**
  1. Convert `builtins` into a `static const builtin_t builtins[]` array placed in `.rodata` (Flash).
  2. Pre-sort the array alphabetically by identifier name at compile time.
  3. Replace the linear scan in `builtin_get()` with `bsearch()` ($O(\log_2 N)$). For ~200 primitives, maximum search depth drops from ~180 comparisons to $\le 8$.
  4. Eliminate the 176 runtime `BUILTIN(...)` registrations in [`lisp_init()`](../user/lisp/lisp.c#L3783), making Lisp initialization instant.
* **Benefits:**
  - Frees ~1.5 KB of SRAM across all boards.
  - Completely lifts the 184-primitive ceiling.
  - Yields a 5× to 10× speedup in primitive symbol resolution.
* **Verification:**
  - Verify symbol lookup succeeds for all existing 176 builtins.
  - Verify an unknown symbol returns `NULL` after at most 8 comparisons.
  - Confirm `.bss` size shrinks by ~1.5 KB via `size` / `nm`.

---

### 42.3: True Symbol Interning & Static Keyword Singletons

* **Problem:** Currently, [`intern_string()`](../user/lisp/lisp.c#L437) does not intern strings; it simply allocates consecutive slots in `string_small`/`string_large`. Every symbol read consumes a string pool slot. Environment resolution in [`env_binding()`](../user/lisp/lisp.c#L926) must repeatedly call `streq()`.
* **Design & Changes:**
  1. Introduce an interning hash table (e.g. 64 or 128 buckets) for symbols in `make_sym()`. If the symbol already exists, reuse the existing pointer.
  2. With unique symbol pointers, simplify `env_binding()`:
     ```c
     if (k && k->type == LISP_SYMBOL && k->u.sym == sym) // Single pointer comparison!
     ```
  3. Define `static const lisp_val_t` singletons in Flash for common syntax keywords (`sym_quote`, `sym_lambda`, `sym_define`, `sym_if`, `sym_else`, etc.), matching the pattern already used for `small_ints` (`-16..255`).
* **Benefits:**
  - Eliminates duplicate strings in `string_pool`, drastically extending headroom on non-PSRAM boards.
  - Reduces variable lookup in lexical scopes from string comparison to a single-instruction RISC-V register comparison (`beq`).
* **Verification:**
  - `(eq? 'foo 'foo)` evaluates to `#t`.
  - Reading 100 occurrences of `'alpha` uses exactly 1 string pool slot.
  - GC correctly sweeps unreferenced dynamically interned symbols while preserving permanent singletons.

---

### 42.4: Dotted Pairs `(a . b)` in Reader and Printer

* **Problem:** [`lisp_read()`](../user/lisp/lisp.c#L4158) only parses proper lists ending in `nil`. In symbolic algebra, association lists (alists) like `((x . 1) (y . 2))` and improper pairs are standard for variable substitution environments.
* **Design & Changes:**
  1. Update `lisp_read()` to recognize `.` as a list delimiter: when encountered, read the next form as the direct `cdr` of the pair and expect an immediate closing `)`.
  2. Update `lisp_print()` to display dotted notation when `cdr` is not a pair and not `nil`.
* **Verification:**
  - `'(a . b)` reads and prints as `(a . b)`.
  - `(car '(a . b))` $\rightarrow$ `a`.
  - `(cdr '(a . b))` $\rightarrow$ `b`.
  - Nested pairs `'(a . (b . c))` print as `(a b . c)`.

---

### 42.5: Quasiquoting & Splicing (`` ` ``, `,`, `,@`)

* **Problem:** Symbolic transformation rules (such as differentiation and algebraic simplification) construct ASTs from patterns. Without quasiquoting, tree construction requires verbose and error-prone `cons`/`list`/`append` chains.
* **Design & Changes:**
  1. Reader support for reader macros:
     - `` `expr `` $\rightarrow$ `(quasiquote expr)`
     - `,expr` $\rightarrow$ `(unquote expr)`
     - `,@expr` $\rightarrow$ `(unquote-splicing expr)`
  2. Evaluator support: Implement `quasiquote` as a special form (or reader desugaring pass) expanding into `cons`, `list`, and `append`.
* **Verification:**
  - `` `(x ,(+ 1 2) z) `` $\rightarrow$ `(x 3 z)`.
  - `` `(a ,@(list 'b 'c) d) `` $\rightarrow$ `(a b c d)`.
  - Nested quasiquotes evaluate correctly.

---

### 42.6: Structural Tree Equality & List Destructuring Primitives

* **Problem:** The current `=` operator only compares numbers. [`lisp_values_equal()`](../user/lisp/lisp.c#L1076) returns `false` for any `LISP_PAIR`. A symbolic system cannot verify if two expression trees are identical.
* **Design & Changes:**
  1. Add `equal?`: Recursive structural equality testing pairs, strings, symbols, and integers.
  2. Add `eq?`: Pointer / identity equality.
  3. Add AST destructuring accessors (in C for zero allocation):
     - `caar`, `cadr`, `cdar`, `cddr`, `caadr`, `caddr`, `cadddr`.
  4. Add dictionary / list search primitives:
     - `assoc`, `assq` (lookup key in alist).
     - `member`, `memq`.
* **Verification:**
  - `(equal? '(+ x 1) '(+ x 1))` $\rightarrow$ `#t`.
  - `(equal? '(+ x 1) '(+ x 2))` $\rightarrow$ `#f`.
  - `(cadr '(+ x y))` $\rightarrow$ `x`, `(caddr '(+ x y))` $\rightarrow$ `y`.
  - `(assoc 'y '((x . 1) (y . 2)))` $\rightarrow$ `(y . 2)`.

---

### 42.7: Arbitrary-Precision Integers (`LISP_BIGNUM`) & Rationals (`LISP_RATIO`) From Scratch

* **Requirement:** No external libraries (no GMP, no third-party bignum packages). Everything must be implemented 100% from scratch in freestanding C (`-ffreestanding -nostdlib`).
* **Problem:** Symbolic calculus and algebra generate arbitrarily large coefficients (e.g., expanding $(x+1)^{20}$ or differentiating high-degree polynomials) and require exact fractional coefficients without floating-point rounding errors.
* **Design & Implementation From Scratch:**
  1. **Limb Representation & RISC-V M Extension:**
     - Limbs are unsigned 32-bit words (`uint32_t`, base $2^{32}$).
     - Multiplication of two limbs produces a 64-bit product (`uint64_t`). On RISC-V (Hazard3 on RP2350 and HP-core on ESP32-P4), the hardware `mul` and `mulhu` instructions handle this with zero software emulation.
  2. **Memory Management & GC Integration:**
     - Fixed-size limb chunks allocated from a GC-traced pool (reusing the existing chunk/string pool mechanism, or a dedicated limb slab).
     - Bignum nodes in `node_pool`:
       ```c
       typedef struct {
           int sign;      /* +1 or -1 */
           int len;       /* number of active limbs */
           uint32_t *limbs;
       } lisp_bignum_t;
       ```
     - Rational nodes (`LISP_RATIO`) hold two `lisp_val_t*` pointers:
       ```c
       struct {
           struct lisp_val *num; /* LISP_INT or LISP_BIGNUM */
           struct lisp_val *den; /* LISP_INT or LISP_BIGNUM (strictly > 1) */
       } ratio;
       ```
       Since `num` and `den` are standard `lisp_val_t*` pointers, the GC traces them identically to `pair.car` and `pair.cdr` with zero GC changes.
  3. **Core Arithmetic Algorithms (Self-Contained):**
     - **Addition & Subtraction:** Standard schoolbook multi-limb add with carry and subtract with borrow ($O(N)$).
     - **Multiplication:** Classical schoolbook $O(N \cdot M)$ multi-limb multiplication with 64-bit accumulator. Compact, deterministic stack usage, and zero external dependencies.
     - **Division:** Binary shift-and-subtract long division or classical Knuth Algorithm D ($O(N \cdot M)$).
     - **Greatest Common Divisor (GCD):** Stein's Binary GCD algorithm (operates purely via bit shifts, subtractions, and parity tests; requires no multi-limb division, making it exceptionally fast and compact on RISC-V).
  4. **Seamless Numerical Tower (Auto-Promotion & Demotion):**
     - **Fast Path:** Operands that fit in `long` (`LISP_INT`) use single-cycle hardware arithmetic. Overflow is detected via compiler builtins (`__builtin_add_overflow`, `__builtin_mul_overflow`).
     - **Promotion:** When integer addition or multiplication overflows `LONG_MAX` / `LONG_MIN`, operands automatically promote to `LISP_BIGNUM`.
     - **Exact Rationals:** Division `/` of non-divisible integers produces a canonical `LISP_RATIO`, with $\gcd(|num|, den)$ reduced to 1 and denominator $> 1$.
     - **Cross-Cancellation:** Rational multiplication $\frac{a}{b} \cdot \frac{c}{d}$ reduces $\gcd(a, d)$ and $\gcd(c, b)$ *before* multiplying, preventing premature bignum limb growth.
     - **Demotion:** Whenever a rational's denominator reduces to 1, it automatically demotes back to `LISP_INT` (or `LISP_BIGNUM`).
  5. **Reader & Printer:**
     - Reader parses arbitrarily long integer literals and fraction literals (e.g. `12345678901234567890`, `355/113`, `-1/1000000000000`).
     - Printer outputs exact decimal representations for bignums and fractions without buffer overruns.
* **Verification:**
  - `(+ 10000000000000000000 1)` $\rightarrow$ `10000000000000000001`.
  - `(* 10000000000 10000000000)` $\rightarrow$ `100000000000000000000`.
  - `(/ 1 2)` $\rightarrow$ `1/2`.
  - `(+ 1/2 1/3)` $\rightarrow$ `5/6`.
  - `(- 100000000000000000000/3 1/3)` $\rightarrow$ simplifies to bignum `33333333333333333333`.
  - Division by zero yields a clean Lisp error without crashing.


---

### 42.8: Tree Transformation Primitives (AST Rewriting Foundations)

* **Problem:** Term rewriting in pure Lisp via user functions generates large volumes of intermediate cons cells and deep call stacks.
* **Design & Changes:**
  1. Implement `subst` in C:
     - `(subst new old tree)`: Recursively replaces all occurrences of `old` with `new` throughout `tree`.
     - Allocates only nodes along rewritten paths; untouched subtrees share structure.
  2. Implement `match` primitive (or rule matcher):
     - `(match pattern expr)`: Compares `pattern` against `expr`. Symbols prefixed with `?` (e.g., `?x`, `?y`) act as wildcards, binding to matched subtrees in an alist.
* **Verification:**
  - `(subst 'y 'x '(+ (* x 2) (sin x)))` $\rightarrow$ `(+ (* y 2) (sin y))`.
  - `(match '(+ ?x ?x) '(+ 5 5))` $\rightarrow$ `((?x . 5))`.
  - `(match '(+ ?x ?x) '(+ 5 6))` $\rightarrow$ `#f`.

---

### 42.9: Recursion Depth Tuning & System Verification

* **Problem:** Non-tail recursive tree traversals currently hit `LISP_MAX_EVAL_DEPTH = 100` ([`lisp.c:175`](../user/lisp/lisp.c#L175)), which can prematurely truncate complex symbolic tree evaluations.
* **Design & Changes:**
  1. Re-evaluate `LISP_MAX_EVAL_DEPTH`: Scale to `256` or `512` based on board stack bounds (`_stack_top - _stack_bottom` is 16 KB on RP2350).
  2. Implement an automated test suite in `tests/test_lisp_symbolic.py` verifying:
     - Hot-path speedup (benchmark comparisons).
     - Correctness of exact rational math, quasiquoting, dotted pairs, and `equal?`.
     - Symbol interning and GC stability under memory pressure.
     - Confirmation that `.bss` RAM usage on low-resource boards did not increase.
* **Verification:**
  - Full test suite passes on both QEMU RISC-V targets (`rv32-nommu`, `rv64-mmu`).
  - Hardware test suite passes on RP2350 (standard and LCD-7 terminal personas) and ESP32-P4.

---

## Milestone Dependency & Execution Order

| Step | Milestone | Primary Artifacts | Expected SRAM Impact |
| :--- | :--- | :--- | :--- |
| **1** | **42.1** Hot-Path Micro-Opts | `user/lisp/lisp.c` | 0 bytes |
| **2** | **42.2** `.rodata` Builtin `bsearch` | `user/lisp/lisp.c` | **-1.5 KB (frees SRAM)** |
| **3** | **42.3** Symbol Interning & Singletons | `user/lisp/lisp.c`, `lisp.h` | Prevents string pool exhaustion |
| **4** | **42.4** Dotted Pairs Reader/Printer | `user/lisp/lisp.c` | 0 bytes |
| **5** | **42.5** Quasiquote & Splicing | `user/lisp/lisp.c` | 0 bytes |
| **6** | **42.6** Tree Equality & Accessors | `user/lisp/lisp.c` | 0 bytes (in Flash `.rodata`) |
| **7** | **42.7** Bignums & Exact Rationals | `user/lisp/lisp.c`, `lisp.h` | 0 bytes |
| **8** | **42.8** AST Rewrite & Matcher | `user/lisp/lisp.c` | 0 bytes |
| **9** | **42.9** Depth Tuning & Test Suite | `user/lisp/lisp.c`, `tests/` | 0 bytes |

