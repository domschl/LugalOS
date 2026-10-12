# ADR 005: Zero SRAM Builtins in Flash `.rodata` Tables

## Status
Accepted (2026-09-28)

## Context
As LugalOS Lisp grew to support hundreds of primitives (math, string, vector, hardware I2C, GPIO, and CAS algorithms), registering these functions as standard global environment bindings (`global_env`) created severe memory pressure on low-resource microcontrollers like the RP2350 (520 KB SRAM) and ESP32-C6 (512 KB SRAM). In a 1,024-node pool, binding dozens of builtins consumed over 50% of the available nodes before the user typed a single expression.

## Decision
All core Lisp primitives and C builtins are stored in static tables compiled into Flash `.rodata`:
1. The table `builtins_table` in `user/lisp/builtins_table.h` is sorted alphabetically at compile time.
2. The runtime evaluator first checks `global_env` for user-defined symbols. If not found, it resolves primitives via $O(\log N)$ binary search (`builtin_get()`) against `.rodata`.
3. Standard libraries are tiered: a lean `stdlib.lisp` loads at boot, while heavier libraries (`matrix.lisp`, `dict.lisp`, `chart.lisp`) are stored on disk and loaded on-demand via `(require 'pkg)`.

## Consequences
* **Positive**: Primitives consume **zero bytes of `.bss` SRAM** and zero cons pairs in `global_env`.
* **Positive**: Minimal-profile microcontrollers run the full battery of built-in primitives without memory starvation.
* **Negative**: New C builtins must be added to the sorted `.rodata` table.
