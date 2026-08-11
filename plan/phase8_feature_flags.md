# Phase 8 — Kernel CONFIG: build-time feature flags

**Status:** F0-F3 complete, 2026-08-11. Successor to `phase7_kernel_config.md`
(K0-K3, complete). Closes the fourth and last sub-area of the "Kernel CONFIG" idea
in `plan/raw_ideas.md`: *"software options, possibility to enable or disable
functionality (e.g. sensor node doesn't need cc, chess computer doesn't need
networking, etc.)"*.

**Theme.** A deliberately small step, not a menuconfig UI. Reuse the K0 generator
(`cmake/gen_config.cmake` -> `lugalos_config.h`, introspectable via `/proc/config`)
rather than inventing a second config mechanism. Two real, self-contained
subsystems are gated as proof: the in-kernel C compiler (`cc`, chibicc) and the
line editor (`ed`). Both were chosen because each has exactly one outside
touchpoint (a registration site in `lisp.c` or a dispatch branch in `shell.c`) and
its own leaf source files — no tangled dependency graph to unwind, unlike the
networking drivers (`loopback_net`/`uart_net`/`usb_cdc`), which board.c, main.c,
time.c, printk.c, and console.h all reach into and are explicitly out of scope for
this phase.

Default is **ON** for every flag on every target — this phase changes what *can*
be built, not what *is* built by default. `tests/runner.py` exercises both `cc`
(lines 392-393, 619) and `ed` (line 603) today, so the default configuration must
keep passing all 181 QEMU tests unchanged; the OFF path is verified manually per
milestone, the same way `ENABLE_SANITIZERS`'s per-target default is not covered by
a dedicated CI matrix.

Mechanism: `option(LUGALOS_ENABLE_CC ...)` / `option(LUGALOS_ENABLE_ED ...)` in
`CMakeLists.txt` (a build persona choice, orthogonal to which board — unlike K0's
pin maps, these aren't hardware facts, so they don't belong in `cmake/board-*.cmake`).
Passed into `gen_config.cmake` as `-DENABLE_CC=/-DENABLE_ED=`, which bakes them into
`lugalos_config.h` as `CONFIG_ENABLE_CC`/`CONFIG_ENABLE_ED` (0 or 1) unconditionally
(unlike K2's board-optional pin keys, every target has these) so C code gates with
one uniform `#if CONFIG_ENABLE_CC` regardless of target.

---

## F0 — Generator + CMake option wiring, zero behavior change *(done, 2026-08-11)*

`CMakeLists.txt`: add `option(LUGALOS_ENABLE_CC "..." ON)` and
`option(LUGALOS_ENABLE_ED "..." ON)`. Pass through to `gen_config.cmake` via
`-DENABLE_CC=$<BOOL>` / `-DENABLE_ED=$<BOOL>` on both the configure-time
`execute_process` and the always-run `lugalos_config` custom target, matching how
`BOARD`/`OUT` are already passed. `gen_config.cmake`: two new **mandatory** keys
(every target gets them, not board-optional like K2's pins) —
`#define CONFIG_ENABLE_CC ${ENABLE_CC}` / `CONFIG_ENABLE_ED`. `ENABLE_CC`/`ENABLE_ED`
are now required generator inputs (`FATAL_ERROR` if either is undefined), passed
from `CMakeLists.txt`'s new `option(LUGALOS_ENABLE_CC ...)` / `LUGALOS_ENABLE_ED`.
K210 needed no special-casing: it already gets no `lugalos_config.h` at all (no
board file), and `kernel/palloc.h` has included that header unconditionally since
K1 — so K210 was already unbuildable before this phase, for an unrelated,
pre-existing reason. Nothing new to resolve here.

Nothing consumes the new defines yet.

**Verify:** all three buildable targets (rv32/rv64/rp2350) configure and build with
no source changes; 181/181 QEMU tests unchanged with both flags at their default ON.
Confirmed live: `build/rv32/lugalos_config.h` shows `CONFIG_ENABLE_CC 1` /
`CONFIG_ENABLE_ED 1`.

## F1 — `cc` (chibicc) becomes the first consumer *(done, 2026-08-11)*

- `CMakeLists.txt`: wrapped the `user/chibicc/*.c` lines in the `SOURCES` list in
  `if(LUGALOS_ENABLE_CC)`.
- `user/lisp/lisp.c`: gated the `#include "user/chibicc/include/chibicc.h"`,
  `prim_cc()`, and its `env_set(&global_env, "cc", make_prim(prim_cc))`
  registration behind `#if CONFIG_ENABLE_CC`.
- `kernel/shell.c`: gated the `chibicc.h` include and the `cc <src> <dst>` help
  line behind the same `#if`.

**Verified:** default (ON) build — all three targets build clean, 181/181 QEMU
tests pass unchanged, binary size unchanged (within a few bytes of build-id
noise). Manual OFF build (`-DLUGALOS_ENABLE_CC=OFF -DLUGALOS_ENABLE_ED=OFF`,
built together with F2 below) on rv32: boots clean, `cc /sd0/hello.c
/ram0/x.elf` comes back `Unbound symbol: cc` rather than crashing, and `help`
no longer lists it.

## F2 — `ed` becomes the second consumer *(done, 2026-08-11)*

- `CMakeLists.txt`: wrapped `user/ed/ed.c` in `if(LUGALOS_ENABLE_ED)`.
- `kernel/shell.c`: gated the `ed.h` include, both the `ed`/`ed <file>` dispatch
  branches, and the `ed [file]` help line behind `#if CONFIG_ENABLE_ED`.

**Verified:** same shape as F1 — default build unchanged (181/181), manual OFF
build boots and `ed /sd0/hello.c` comes back `Unbound symbol: ed`, `help` omits
it. Confirmed `e` (the unrelated heap-based Emacs-style editor from the
2026-08-11 bugfix) is untouched — it does not depend on `user/ed/`. The combined
`-DLUGALOS_ENABLE_CC=OFF -DLUGALOS_ENABLE_ED=OFF` rv32 image is ~15 KB smaller
than the default build (2148088 vs. 2163232 bytes).

## F3 — `/proc/config` reports enabled features *(done, 2026-08-11)*

`fs/vfs_server.c`: extended the existing `"config"` branch (K3) with
`ENABLE_CC=%d\nENABLE_ED=%d\n`, unconditionally (not RP2350-gated like the pin
fields) since every target has these flags now.

**Verified:** 181/181 QEMU tests pass; `cat /proc/config` on a running rv32
instance shows `ENABLE_CC=1`/`ENABLE_ED=1` on the default build and
`ENABLE_CC=0`/`ENABLE_ED=0` on the manual OFF build.

---

## Outstanding before this phase is fully closed

Nothing — F0-F3 are done, QEMU-green (181/181, default ON build, every
milestone) and manually verified on both a default and an all-OFF rv32 build.
No hardware-only code paths were touched, so a `tests/hw/` run wasn't strictly
required for this phase (unlike phase7's K2 pin migration) — but one was run
anyway as a freshness check: flashed via `tests/hw/flash.py --verify` (board
reported build `189.31c1b2fb`, matching this phase's commit), then
`tests/hw/test_rp2350.py` — **15/15 passed**, including `K3: /proc/config`
(still matching all 11 pin/platform fields; `ENABLE_CC`/`ENABLE_ED` aren't
board-optional so aren't part of that specific assertion, but the board booted
and ran the full command surface with both flags at their default ON).

Networking (`loopback_net`/`uart_net`/`usb_cdc`) and any further software options
stay out of scope, per the theme note above — real candidates for a later phase
once/if the dependency graph gets untangled, not an oversight here.
