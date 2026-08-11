# Phase 7 — Kernel CONFIG (platform defaults + pin mapping)

**Status:** K0-K3 complete and hardware-verified, 2026-08-11 (15/15 `tests/hw/`
on real RP2350 silicon, including the CP2102-dongle UART0 test and an SD-card
compile exercising SPI1). Successor to `phase6_memory_and_processes.md` (C0-C8,
complete at v0.8.0). Only remaining gap: a human visual check of the two LED
GPIOs, which no automated test can perform.

**Theme.** `plan/raw_ideas.md` collects a "Kernel CONFIG" idea with four sub-areas:
platform defaults, pin-to-port mapping, port-to-driver binding, and build-time feature
enable/disable. This phase does the first two only. Port-to-driver *protocol* binding
already has a real, working runtime registry from C8 (`kernel/device.c`/`board.c`,
driven from `init.lisp`, `/proc/ports`) and stays exactly as it is. A real interactive
"menuconfig" UI and build-time feature exclusion are real ideas from `raw_ideas.md`,
deliberately deferred to a later phase, not part of this one.

The bug this phase removes: `0x40070000` (RP2350's UART0 MMIO base) was hand-typed
**twice**, independently — `kernel/board.c`'s `board_uart_base()` and
`drivers/uart_rp2350.c`'s own `UART0_BASE` `#define` — with nothing keeping them in
sync. Individual GPIO pin numbers existed only as raw numeric literals inline in
register-poke expressions, documented only in header comments. `PALLOC_MAX_PAGES`
was a second, independent `#ifdef CONFIG_BOARD_RP2350` ladder. All of this is now
centralized in one generated, per-board source of truth.

Full design rationale (config-format choice, `#define`-vs-struct-table decision, the
`dev_wire_t` namespace boundary) is in the plan this phase was built from:
`/Users/dsc/.claude/plans/sequential-sparking-fern.md`.

---

## K0 — Generator + CMake wiring, zero behavior change *(done, 2026-08-11)*

New: `cmake/gen_config.cmake` (modeled on `cmake/gen_build_id.cmake`'s
write-if-changed shape — schema/required-keys validation lives in the generator,
not the board files), `cmake/board-{rp2350,rv32-nommu,rv64-mmu}.cmake` (plain
`set()` calls, full real data from day one). `CMakeLists.txt` sources the right
board file per `LUGALOS_TARGET` and wires `gen_config.cmake` the same way
`lugalos_build_id` was already wired. K210 gets no board file — no real drivers
reference it today; the generator's `FATAL_ERROR` on a missing required key is
what tells a future implementer what's needed if that ever changes.

**Verified:** all three targets configure and build with no source changes yet
(nothing `#include`s the generated header) — 181/181 QEMU tests unchanged, proving
zero behavior change. Inspected `build/<target>/lugalos_config.h` directly for all
three targets; content matched the board files exactly. Confirmed the generator's
`FATAL_ERROR` path fires correctly on a board file missing a required key.

## K1 — `PALLOC_MAX_PAGES` becomes the first platform-default consumer *(done, 2026-08-11)*

`kernel/include/kernel/palloc.h`: the `#if defined(CONFIG_BOARD_RP2350)` ladder is
gone, replaced by `#include "lugalos_config.h"` and
`#define PALLOC_MAX_PAGES CONFIG_PALLOC_MAX_PAGES`. Public symbol name unchanged,
so nothing downstream needed touching.

**Verified:** all three targets build clean; 181/181 QEMU tests pass (the existing
`/proc/meminfo` regex assertions from C6/C7 are the regression net for this value).

## K2 — RP2350 pins: UART0 + SPI1 + LEDs *(done, 2026-08-11 — hardware-verified)*

`drivers/uart_rp2350.c` and `drivers/spisd_rp2350.c`: every raw GPIO literal in a
register-poke expression (`IO_BANK0_CTRL(0)`, `PADS_BANK0_PAD(25)`,
`(1u << 16)`, etc.) now reads a `CONFIG_*` name; the locally-`#define`d
`UART0_BASE`/`SPI1_BASE`/`CS_PIN` alias the generated constants.
`kernel/board.c`: `board_uart_base()`'s ladder collapsed to
`return CONFIG_UART0_BASE;` unconditionally, removing the second hand-typed copy
of the same address. Landed as one change, not split, per the plan's own risk note
(a half-migrated state would have recreated the exact duplication bug being fixed).

**Verified:**
- All three targets build clean; RP2350's own driver files compile with no new
  warnings, and the linked image size is byte-identical to the pre-migration build
  (1307648 bytes).
- 181/181 QEMU tests pass (QEMU cannot exercise this code at all — neither
  `uart_rp2350.c` nor `spisd_rp2350.c` are compiled into the QEMU targets — so
  this is a build-integrity check, not a correctness one).
- **Disassembly-verified the strongest way available without physical hardware**:
  every constant baked into `uart_init()` and the SPI1 init path (`0x40070000`,
  GPIO 0/1/25/16 in every `IO_BANK0_CTRL`/`PADS_BANK0_PAD`/`SIO_GPIO_*` call,
  `0x40088000`, GPIO 10/11/12/13) was checked instruction-by-instruction against
  `riscv64-elf-objdump -d` output and matches the pre-migration literals exactly.
- **Hardware run completed, 2026-08-11**: flashed via `tests/hw/flash.py --verify`
  (board reported build `186.3813d656+`, matching the local build), then
  `tests/hw/test_rp2350.py` with a CP2102 dongle and SD card both attached —
  **15/15 passed**. `p9share` (the only test that touches physical UART0 rather
  than USB CDC) passed, confirming the migrated UART0 TX/RX pin mux. `C6/C7`
  (`cc /sd0/prime.c`) passed, confirming the migrated SPI1 pins against a real
  SD card end-to-end. `K3: /proc/config` matched all 11 fields.
- **Still not machine-checked**: the LED GPIOs (25/16) have no automated
  coverage before or after this phase — nothing in `tests/hw/` watches a GPIO's
  electrical state, only what the compiled binary contains (already
  disassembly-verified above). A visual check (3 blinks at boot, then the
  0.5 Hz heartbeat on GP16) is the only way to close that gap, and needs a
  human looking at the board.

## K3 — `/proc/config` introspection primitive *(done, 2026-08-11)*

`fs/vfs_server.c`: added `"config"` to the existing flat `/proc` dispatch pattern
(alongside `ports`/`devices`/`buildid`). Always reports `PALLOC_MAX_PAGES` and
`UART0_BASE`; under `CONFIG_BOARD_RP2350`, also reports the UART0 TX/RX GPIOs,
SPI1 base + 4 pins, and both LED GPIOs.

**Verified:** all three targets build clean; 181/181 QEMU tests pass; `cat
/proc/config` checked live on a running QEMU rv32 instance and reports
`PALLOC_MAX_PAGES=4096` as expected. Added `test_board_config` to
`tests/hw/test_rp2350.py` (registered before the node-pool-exhaustion test, which
must stay last), asserting all 11 RP2350 fields by regex against the exact
literals now baked into the drivers — the first automated check that would catch
a wrong *generated* pin value independent of whether that pin is otherwise
exercised. Run against real hardware 2026-08-11 as part of the same `test_rp2350.py`
pass described under K2 — **all 11 fields matched**.

---

## Outstanding before this phase is fully closed

QEMU-green (181/181, every milestone), hardware-green (15/15, `tests/hw/`,
2026-08-11) and statically verified (disassembly-checked constants for K2). One
item remains, and it needs a human, not another test run: a visual check of the
LED GPIOs (3 blinks at boot, then the 0.5 Hz heartbeat on GP16) — nothing in
`tests/hw/` watches a GPIO's electrical state, so this is the one migrated pin
pair with no automated proof, only the disassembly evidence that the correct
constants (25, 16) reached the binary.
