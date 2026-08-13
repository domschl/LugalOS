# Phase 11 — Pico-Clock-Green (SM16106/SM5166P LED matrix + DS3231 temp)

**Status:** L0-L4 all done and hardware-verified, 2026-08-13 — same day,
one continuous session from feasibility research through a working clock
on real hardware. Two real hardware-only bugs found and fixed along the
way (a `clk_adc` clock-enable boot hang, L2; an I2C-read-cadence display
flicker, L4), neither of which QEMU or code review alone would have
caught — see those sections for the full accounts. Phase closed except
for the deliberately-out-of-scope items below. From `plan/raw_ideas.md`'s
"Application scenarios": "clock with matrix led display (waveshare
pico-clock-green)".

**Target board.** Waveshare Pico-Clock-Green baseboard
(https://www.waveshare.com/wiki/Pico-Clock-Green), populated with an RP2350W
("Pico 2 W") rather than the plain RP2350 the rest of this tree targets today.
Vendor sample firmware (Pico SDK / ARM) is vendored locally at
`~/gith/Pico-Clock-Green` and was read in full (`Pico-Clock-Green.c`,
`define.h`, `Ds3231.c/h`, `ziku.h`) as the hardware reference — there is no
public register-level datasheet for the SM16106/SM5166P beyond what that
firmware demonstrates, so it is the authority for the protocol below.

**Scope, explicitly narrowed (user-confirmed 2026-08-13).** The vendor
firmware is a full-featured clock: 12h/24h toggle, alarms, count-up/down
timer, scrolling text, weekday indicator LEDs, °C/°F toggle, on-device
button-driven settings menus, and a battery-voltage ADC diagnostic. This phase
ports none of that. In scope: read DS3231 time (24h only) and temperature
(°C only), render `HH:MM` + temperature on the matrix, and LDR-driven
auto-brightness. Buttons/buzzer and on-device time-set are deliberately
deferred (time is already settable today via the shell's `date` command over
console/9P — see `kernel/shell.c:620`).

---

## L0 — Feasibility: protocol, pin conflicts, RP2350W impact (research done, 2026-08-13)

**Display protocol (SM16106 ×2 + SM5166P ×1), reverse-engineered from the
vendor firmware, not a datasheet.** SM16106 is a 16-channel constant-current
LED sink shift register, two of them daisy-chained for the matrix's columns.
SM5166P is a fixed 8-line row driver, row selected by binary address (not a
shift register). The vendor firmware drives both together as a classic
8-row multiplexed scan (`Pico-Clock-Green.c:373-393`, inside its 1ms timer
callback):

1. Shift 4 bytes of column data out MSB-per-bit-loop but LSB-first overall
   (`send_data()`, `Pico-Clock-Green.c:557-573`: for each of 8 bits, drive
   `SDI` from `data & 1`, `data >>= 1`, pulse `CLK`).
2. Latch with a single `LE` pulse (high then low).
3. Set the row address on `A0`/`A1`/`A2` (3-bit binary, one of 8 rows).
4. Un-blank via `OE` (active-low: `OE_OPEN` = `gpio_put(OE, 0)`).
5. Advance to the next row; a full 8-row frame is ~8ms (1ms/row from the
   vendor's timer), i.e. ~125Hz — flicker-free.

Brightness/auto-dimming is done by toggling `OE` at high frequency instead of
holding it open (`repeating_timer_callback_us()`, `Pico-Clock-Green.c:170-189`):
above an LDR ADC threshold, `OE` opens 1 cycle in 3 (~33% duty); below it,
`OE` stays open. Character data is a 5×7 (digits) / 4×7 (`:`) bitmap table
(`ziku.h`) with a hand-rolled glyph-to-column-bit packing
(`display_char()`, `Pico-Clock-Green.c:575-621`) — small, portable, worth
porting as literal data rather than re-deriving.

DS3231 time read/write is **already implemented** in
`drivers/i2c_rtc.c`/`i2c_rtc.h` and already defaults to 24-hour mode
(`tm->hour = bcd2dec(buf[2] & 0x3F)`, no 12/24 branch) — no change needed
there. Temperature is **not** currently read; DS3231 exposes it at registers
`0x11` (signed integer °C) / `0x12` (upper 2 bits = 0.25°C fraction), matching
`Pico-Clock-Green.c`'s `get_add_high = 0x11, get_add_low = 0x12`. This is a
small, additive extension to `i2c_rtc.c` (new `i2c_rtc_read_temperature()`
alongside the existing read/write-time functions, same `i2c_read_bytes()`
plumbing already in that file) — not a new driver.

**Pin conflicts: real, and specific.** Pico-Clock-Green is a fixed baseboard
with soldered GPIO wiring (`define.h`), not something LugalOS can route around
by picking different pins. Checked against `cmake/board-rp2350.cmake`:

| Signal (Pico-Clock-Green) | GPIO | Claimed today by | Conflict |
|---|---|---|---|
| `CLK`/`SDI`/`LE`/`OE` (display) | 10/11/12/13 | `SPI1` = SD card (`spisd_rp2350.c`, unconditionally compiled for any RP2350 target) | **Full, all 4 pins** |
| `A0` (row addr bit 0) | 16 | `LED_EXT` heartbeat LED, driven *unconditionally* in `drivers/uart_rp2350.c` (not behind a feature flag) | **Yes** |
| `SDA`/`SCL` (I2C1) | 6/7 | TM1638 STB/CLK (chess persona) | Only if `ENABLE_TM1638` stays on |
| `UP`/`A1` | 17/18 | ST7735 CS/SCK (chess persona) | Only if `ENABLE_ST7735` stays on |
| `A2`, `SET_FUNCTION`, `DOWN`, `SQW`, `BUZZ`, `ADC_Light`(26) | 22, 2, 15, 3, 14, 26 | free | No |
| `ADC_VCC` | 29 | free on plain RP2350 | Dropped anyway (out of scope), see RP2350W note below |

Two of these (SD card, `LED_EXT`) are not feature-flag-gated today — they are
always-on for *any* RP2350 build, so this board persona needs the
board-config/feature-flag mechanism extended one more step (K0/F0 pattern),
not new invention: (a) one new `LUGALOS_ENABLE_SPISD`-shaped flag so
`spisd_rp2350.c` can be excluded from this persona's build entirely (it
currently has no flag at all — it's unconditional per
`CMakeLists.txt`'s `if(LUGALOS_TARGET STREQUAL "RP2350")` block), and
(b) remap `CONFIG_LED_EXT_GPIO` off 16 to any genuinely free pin (e.g. GP9) in
the new board file. TM1638/ST7735/chess simply get built out
(`LUGALOS_ENABLE_TM1638=OFF`, `LUGALOS_ENABLE_ST7735=OFF`,
`LUGALOS_ENABLE_CHESS=OFF`) for this persona — no code change needed there,
just build flags.

**Storage: not actually a problem.** Losing SPI1 means no SD card
(`spisd_get_device()` simply won't be probed on this persona), but
`/flash0/` (embedded flash FAT32, `drivers/flashdisk.c`) mounts independently
and unconditionally (`fs/vfs_server.c:122-133,251`) and is sufficient to hold
a program image. DS3231 keeps time across power loss on its own coin cell, so
no writable storage is needed for the clock's actual function either.

**CMake plumbing gap.** `CMakeLists.txt:222-228` picks exactly one
`cmake/board-*.cmake` file per `LUGALOS_TARGET` string, hardcoded — there is
currently no way to select a second RP2350 persona without either overloading
`LUGALOS_TARGET` (heavy: duplicates arch/ABI/linker-script selection that is
identical to plain RP2350) or adding an override cache var. The latter is the
right fix: wrap the existing `elseif(LUGALOS_TARGET STREQUAL "RP2350")` branch
in `if(NOT DEFINED LUGALOS_BOARD_FILE)`, so a caller can pass
`-DLUGALOS_BOARD_FILE=cmake/board-rp2350-clock.cmake` to select this persona
while everything else about the RP2350 target (arch flags, linker script,
`CONFIG_BOARD_RP2350`) stays shared.

**RP2350W ("Pico 2 W") impact — answers the user's direct question.**
`GPIO23/24/25/29` are reserved for the CYW43439 wireless chip's SPI-like link
(on/CS/data/clock). Of these, `GPIO23`/`24`/`25` aren't header-exposed on a
*plain* RP2350 Pico either (internal-only on both variants), so nothing
changes there. `GPIO29` is the one real difference: it's freely available as
`ADC3` on plain RP2350 and is *not* usable as an ADC on the W variant (it's
the wireless module's SPI clock instead). The only thing in the vendor
firmware that touches GPIO29 is the optional battery-voltage diagnostic
(`show_adc()`, `ADC_VCC` = GPIO29) — already out of scope per the narrowed
feature set above, so this is a non-issue for what we're building. Nothing
required for the clock (display, DS3231, LDR on GPIO26/ADC0) touches
23/24/25/29. Future NTP-over-Wi-Fi work (flagged as a future idea in
`raw_ideas.md` already) will need a from-scratch CYW43 SPI driver — a
separate, substantial future phase, not blocked or made harder by anything
here.

---

## L1 — Board profile + CMake plumbing *(done, 2026-08-13 — build-verified, not yet hardware-verified)*

Landed with one addition beyond what this section originally planned: the
user asked, on seeing the `LUGALOS_BOARD_FILE` override designed below,
how a *growing* set of board personas should stay manageable in general —
not just this one. Answer built: `CMakePresets.json` (CMake-native, schema
v2, matches this repo's `cmake_minimum_required(3.20)`), one named preset
per persona (`rv32-nommu`, `rv64-mmu`, `rp2350-chess`, `rp2350-clock`),
bundling the toolchain file + `LUGALOS_BOARD_FILE` + every `LUGALOS_ENABLE_*`
choice so a build is `cmake --preset <name>` instead of a remembered `-D`
flag list. `cmake --list-presets` is now the answer to "what personas exist."
Deliberately kept `rp2350-chess`'s own `binaryDir` at the historical
`build/rp2350` (not `build/rp2350-chess`) rather than renaming it — found by
checking first, not assumed: `tests/hw/flash.py`'s `DEFAULT_UF2` and several
messages in `tests/hw/test_rp2350.py` hardcode that exact path, and this is
real hardware-test tooling per [[falsify_on_hardware_not_qemu]], not
something to casually rename out from under. README's build instructions
now document the presets as the primary path.

What actually shipped, some of it different from the original bullet plan
below (kept struck-through-in-spirit, corrected inline) once building it for
real surfaced gaps a read-through hadn't:

- `CMakeLists.txt`: `LUGALOS_BOARD_FILE` is now overridable
  (`if(NOT DEFINED LUGALOS_BOARD_FILE) ... endif()`), default unchanged for
  every existing target.
- New `cmake/board-rp2350-clock.cmake`: same `CONFIG_UART0_*` (GP0/1,
  untouched), no `CONFIG_SPI1_*`/`CONFIG_ST7735_*`/`CONFIG_TM1638_*`,
  `CONFIG_LED_EXT_GPIO` remapped off 16 to GP9. The `CONFIG_CLOCK_*` display
  pin facts (`OE`/`SDI`/`CLK`/`LE`/`A0`-`A2`/`SDA`/`SCL`/etc.) are **not**
  added yet, on reflection — no consumer exists until L2's driver does, and
  writing facts into a board file that `gen_config.cmake`'s `_optional_keys`
  allowlist doesn't yet forward to `lugalos_config.h` would be silently
  dead data, not real plumbing. Deferred to L2, alongside the driver that
  gives them meaning.
- New `LUGALOS_ENABLE_SPISD` CMake option (default `ON`, so every existing
  persona is a no-op change). **Corrected from the original plan**: rather
  than gating `spisd_rp2350.c` out of `CMakeLists.txt`'s source list (which
  would have needed `fs/vfs_server.c`'s `spisd_get_device()` call sites
  individually re-gated to keep linking), the file now stubs *itself*
  internally via `#if CONFIG_ENABLE_SPISD` / `#else` — same shape as
  `drivers/i2c_rtc.c`'s pre-existing non-RP2350 stub. `spisd_get_device()`
  always exists and returns `NULL` when disabled, so `vfs_server.c` needed
  no source change for its two block-device call sites — except one third
  call site this approach didn't anticipate: `/proc/config`'s introspection
  output (`fs/vfs_server.c:626-652`) also referenced `CONFIG_SPI1_*`
  unconditionally, inside the same `#if defined(CONFIG_BOARD_RP2350)` block
  that already conditionally reports `ST7735_*`/`TM1638_*` pins behind
  `#if CONFIG_ENABLE_ST7735`/`#if CONFIG_ENABLE_TM1638` — found only by
  actually building `rp2350-clock` and reading the compile error, not by the
  earlier read-through of the file. Fixed the same way: wrapped in
  `#if CONFIG_ENABLE_SPISD`, and `ENABLE_SPISD` added alongside the other
  flags `/proc/config` already reports.
- `LUGALOS_ENABLE_PICO_CLOCK_GREEN` is **not** added yet — deferred to L2 with
  the driver it would gate, matching the `CONFIG_CLOCK_*` pin-fact deferral
  above and the project's own precedent of not defining a flag before it has
  a real consumer to attach to (`LUGALOS_ENABLE_CHESS` was the one prior
  exception, and even that got its consumer the same phase, not left
  dangling across phases).

**Build-verified, all four presets, 2026-08-13:** `rp2350-chess` and
`rp2350-clock` both configure and build clean (`ninja` zero errors; only
pre-existing, unrelated warnings). `rv32-nommu`/`rv64-mmu` unaffected —
191/191 QEMU tests still pass after these changes (the flag/generator
machinery is shared across every target, so this was worth re-confirming,
not assumed safe because "only RP2350 files changed").

**Real-hardware side effect worth recording, not just the technical
finding:** verifying `rp2350-chess` builds via `cmake --build` silently
reflashed the user's physical RP2350 board — `tools/uf2conv.py` (vendored,
pre-existing, not written by this session) auto-detects and writes to *any*
mounted RP2350 BOOTSEL drive as an unconditional side effect of its normal
UF2-conversion path, independent of and in addition to the `-o` output file
already requested. Not caught before it happened; disclosed to the user
immediately on discovery (mid-verification, via the build log's own
`Flashing /Volumes/RP2350 ...` line) rather than after the fact. The board
was flashed with an otherwise-stock chess-persona image (the `ENABLE_SPISD`
restructuring is a no-op with the flag left at its default `ON`) and had
already rebooted out of BOOTSEL by the time this was noticed — no data loss,
user confirmed no concern, board now unplugged pending L2-L4 giving it
something real to flash. **Worth remembering for any future RP2350 build
session on this machine:** if a board is present in BOOTSEL mode, *any*
`cmake --build`/`ninja` for an RP2350 target will flash it, silently, with
whatever was just built — not just an explicit flash command.

## L2 — `drivers/pico_clock_green_rp2350.c`: matrix driver + LDR auto-brightness *(done, 2026-08-13 — hardware-verified)*

Landed as `drivers/pico_clock_green_rp2350.c` (`_rp2350` suffix, matching
`uart_rp2350.c`/`spisd_rp2350.c`/`st7735_rp2350.c`/`tm1638_rp2350.c` — every
other file in this tree that's genuinely single-target-exclusive rather than
stubbed-portable like `i2c_rtc.c`) + `drivers/include/drivers/
pico_clock_green.h`. `CONFIG_CLOCK_*` pin facts (deferred from L1 for lack
of a consumer) now live in `cmake/board-rp2350-clock.cmake` and
`cmake/gen_config.cmake`'s `_optional_keys`; a new
`LUGALOS_ENABLE_PICO_CLOCK_GREEN` option (default **OFF** — opposite of `ENABLE_ST7735`/
`TM1638`/`CHESS`/`SPISD`, because unlike those four, `CONFIG_CLOCK_*` only
exists in the clock board file, so defaulting ON would break the default
persona) gates the driver into `CMakeLists.txt`'s RP2350 source list and
into `CMakePresets.json`'s `rp2350-clock` preset. Eager `pico_clock_green_
init()` call added to `kernel/main.c` alongside the existing ST7735/TM1638
block, same "dedicated, always-wired hardware for this board persona"
rationale already documented there.

- Row-scan refresh (L0's protocol) and a small hand-picked subset of
  `ziku.h`'s glyph table ported: digits 0-9, `:`, the Celsius-degree symbol,
  `-`. Transcribed only the *first, clean* occurrence of each — found while
  transcribing that `ziku.h`'s own later entries (past index ~26) have an
  inconsistent byte stride the vendor's fixed 7-byte indexing can't actually
  reach anyway, so nothing was lost by not porting them. Time layout (`HH:MM`
  column positions) kept identical to the vendor's own already-hardware-
  validated `Show_Time()` math rather than re-derived; temperature layout
  (sign + 2 digits + °C, clamped to ±99) is this driver's own, since the
  vendor firmware never had a non-scrolling temperature readout to copy.
- ADC bring-up (`GPIO26`/channel 0, single-shot, polled) is new — LugalOS
  had no ADC driver at all before this. Register facts (base address
  `0x400a0000`, `CS`/`RESULT` layout, `RESETS_RESET_ADC` bit) verified
  against `~/gith/pico/pico-sdk/src/rp2350/hardware_regs/include/hardware/
  regs/{adc,resets}.h` rather than assumed from RP2040 memory — RP2350
  moved several peripheral base addresses, the same caution phase9 H0 used
  for SPI0/SPI1.
- **Revised from L0's sketch**, found only while actually implementing:
  vendor's dimming (`repeating_timer_callback_us()`) is a *second*,
  independent microsecond timer toggling `OE` across many row-scans: LugalOS
  has no generic one-shot IRQ callback a driver could reuse for that without
  a larger, out-of-scope kernel change. Replaced with a software-PWM pulse
  done entirely inside `pico_clock_green_scan_step()` itself, using the
  `time_delay_us()` primitive `tm1638_rp2350.c` already established: bright
  ambient leaves `OE` open continuously (matches vendor's non-dimming
  branch exactly, zero added latency, the common case); dim ambient does
  `OE_OPEN(); time_delay_us(330); OE_CLOSE()` once per row instead, still
  giving proportional-feeling brightness control with no new kernel
  primitive. Threshold (`ADC > 2800` out of 12-bit full scale) and duty
  (~33%) kept numerically identical to the vendor's own already-tuned
  values for the same physical LDR/divider hardware, even though the
  mechanism producing that duty differs.
- Bit-banged GPIO shift-out follows the same pattern as
  `drivers/tm1638_rp2350.c`'s `tm1638_write_byte()` (SIO `GPIO_OUT_SET/CLR`
  direct register writes, no hardware SPI peripheral involved — the vendor
  firmware doesn't use one either).

**Build-verified 2026-08-13** on all presets, 191/191 QEMU tests unaffected.

**Hardware-verified 2026-08-13, and it found a real bug QEMU could never
have caught** ([[falsify_on_hardware_not_qemu]] again). First flash of the
full clock persona onto real RP2350W hardware hung completely — no shell,
no boot banner past `uart_init()`'s own startup line, over both USB CDC and
a physical CP2102 UART (the UART test is what proved it wasn't a USB/DTR
quirk: a real UART with a mature host driver, same total silence). Root-
caused by binary bisection on real hardware rather than guessing: a build
with `LUGALOS_ENABLE_PICO_CLOCK_GREEN=OFF` (same board file otherwise)
booted fine; re-enabling it with just `adc_hw_init()`'s call skipped also
booted fine; that isolated the fault to `adc_hw_init()` itself, immune to
every timeout-bounded poll already in that function because the actual
hang was a **bus transaction stalling on an unclocked peripheral**, not a
software loop — `clk_adc` has no glitchless mux and is disabled at reset
(`CLOCKS_CLK_ADC_CTRL_ENABLE_RESET=0`, `clocks.h`), so any register access
before explicitly enabling it (`CLOCKS_BASE+0x6C` bit 11) hangs the bus
waiting for a clock edge that never arrives. `uart_rp2350.c` already
handles the identical requirement for `clk_peri` (`CLOCKS_BASE+0x48` bit
11) — L0's own SDK cross-referencing checked register *addresses* and
*bit positions* thoroughly but never checked whether a peripheral's clock
needed enabling at all, since none of this tree's other drivers (I2C,
SPI, UART) needed it explicitly (their clocks are already running for
other reasons by the time those drivers touch them). Fixed with one
register write mirroring `uart_init()`'s own pattern exactly. Rebuilt,
reflashed: full clock persona now boots cleanly to `lsh>` over both UART
and USB CDC, and `i2c` reports device `0x68` on `GP6/GP7` — L3's I2C1
fix confirmed reaching the real DS3231, not just compiling.

**LDR auto-brightness confirmed working, 2026-08-13 (L4 session), after
one round of doubt.** First live test ("I tried to cover it?!") found no
visible dimming — before assuming a bug, added a small diagnostic,
`(clock-light)` (new public `pico_clock_green_read_light()` +
`user/lisp/lisp.c` primitive, same gating as `(clock)`), returning the raw
12-bit ADC reading on demand. Real numbers settled it fast: bright light
directly on the LDR reads `16`; a finger reads `1520` (still well below
`LDR_DARK_THRESHOLD`'s `2800`); a genuinely opaque dark object reads
`3868` (comfortably above). Polarity and threshold were both already
correct — the first test's "cover" (a finger) simply wasn't opaque enough
to cross the threshold, not a bug. Re-tested with the same opaque object
this time: "display gets considerably darker" — confirmed. `(clock-light)`
kept as a permanent diagnostic primitive rather than thrown away, matching
`(i2c)`'s own precedent of a small hardware-introspection command.

**Two findings surfaced while implementing L2, out of L2's own scope,
recorded here for L3/whoever picks it up next:**
1. **The vendor's DS3231 is wired to I2C1 on GP6/GP7 (`~/gith/Pico-
   Clock-Green/define.h`: `#define SDA 6`, `#define SCL 7`, `#define
   I2C_PORT i2c1`), not the I2C0/GP4-GP5 pair `drivers/i2c_rtc.c` hardcodes
   today.** RP2350's GPIO-to-I2C-controller mapping alternates every 4 pins
   (GP4/5 → I2C0, GP6/7 → I2C1, GP8/9 → I2C0, ...), so this isn't a
   function-select choice that can be reconfigured in software — GP6/7
   physically only reach the I2C1 hardware block (base address differs from
   I2C0's `0x40090000`). L0's "no change needed" verdict for the RTC's
   read/write *logic* still holds, but L3 as originally scoped ("small
   addition to `i2c_rtc.c`") undersold this: `i2c_rtc.c`'s I2C0 register
   bring-up would need to become I2C-instance-generic (or a second,
   parallel bring-up added) before it can reach this board's actual DS3231
   at all. Missed at L0 because that research checked GP6/7 for conflicts
   with *other already-claimed pins* (TM1638), not for whether the
   existing driver's hardcoded peripheral instance could physically reach
   them.
2. **Pre-existing, likely-dormant bug found by cross-referencing the SDK
   header for L2's own ADC reset sequence, unrelated to this phase:**
   `drivers/i2c_rtc.c`'s `RESETS_RESET_DONE` is defined as `RESETS_BASE +
   0x000C`; `resets.h`'s `RESETS_RESET_DONE_OFFSET` is `0x0008`
   (`drivers/spisd_rp2350.c` has this one right, for comparison). Likely
   benign in practice — the poll loop's 10000-iteration timeout still burns
   enough real time for the actual (near-instant) reset to complete
   underneath it regardless of which address it's reading, which is
   probably why this has stayed unnoticed across however long `i2c_rtc.c`
   has been hardware-verified. Not fixed here (outside L2's scope, and
   touching a file this phase doesn't otherwise need without being asked
   crosses the same line [[feedback_prefer_root_cause]] warns about in the
   other direction — fixing things nobody asked about, mid-task); flagged
   for a deliberate decision instead.

## L3 — DS3231 temperature read + the I2C0/I2C1 gap + the dormant reset-offset bug *(done, 2026-08-13 — hardware-verified: `i2c` shows 0x68 on GP6/GP7)*

All three landed together, since the I2C0-vs-I2C1 gap (found while doing L2)
and the temperature read both touch the same file and the reset-offset fix
sits directly in the code path both needed anyway.

- **I2C0-vs-I2C1 gap closed.** `drivers/i2c_rtc.c`'s previously-hardcoded
  `I2C_SDA_PIN`/`I2C_SCL_PIN` (4/5) and `I2C0_BASE` (`0x40090000`) are now
  board facts: `CONFIG_I2C_RTC_SDA_GPIO`/`_SCL_GPIO`/`_BASE`, following the
  same pattern every other pin group in this tree already uses. Default
  persona (`cmake/board-rp2350.cmake`) keeps the original GP4/GP5/I2C0
  values — a no-op change there. The clock persona
  (`cmake/board-rp2350-clock.cmake`) gets GP6/GP7/I2C1 (`0x40098000`),
  matching the vendor's actual wiring. The one thing *not* made an
  independent board fact: which `RESETS` bit to assert (bit 4 for I2C0, bit
  5 for I2C1, confirmed against `resets.h`) — derived in `i2c_rtc.c` from
  `CONFIG_I2C_RTC_BASE` via a preprocessor `#if` instead, so a board file
  can't get that one wrong independently of the base address it already has
  to get right. `at24c32.c` (a separate, unrelated EEPROM breakout used on
  the *default* persona's dev setup, not part of Pico-Clock-Green at all)
  was deliberately left untouched — it shares whichever bus `i2c_rtc.c`
  brings up rather than doing its own reset/pin bring-up, so on the clock
  persona it harmlessly times out looking for an EEPROM on an
  now-unconfigured I2C0 (GP4/GP5, genuinely free on this baseboard) and
  reports "not detected," the same graceful-absence behavior every other
  optional-hardware probe in this tree already has. Confirmed by reading
  its own probe logic, not assumed.
- **Dormant bug fixed.** `RESETS_RESET_DONE` was `RESETS_BASE + 0x000C`;
  the real offset (`resets.h`'s `RESETS_RESET_DONE_OFFSET`, and
  `spisd_rp2350.c`'s own copy) is `0x0008`. Fixed outright now that it's
  been found, per the user's explicit instruction — no longer just flagged.
- **Temperature read added**: `i2c_rtc_read_temperature_c(int *temp_c)`,
  registers `0x11` (signed integer part) / `0x12` (bits 7:6 = quarter-degree
  fraction, per the DS3231 datasheet's own representation — always a
  non-negative offset from the integer part, so no separate sign handling
  needed), rounded to the nearest whole degree (half-away-from-zero,
  correct for negative values too — checked by hand-tracing both the
  `+0.25`/`+0.75` and negative-integer-part cases, not just the common
  positive case). Gated on `g_rtc_detected` like `i2c_rtc_write_time()`
  already is, not on `i2c_rtc_read_time()`'s unconditional-probe shape,
  since L4 will only ever call it after init already knows detection
  status. DS1307 (no temperature sensor) isn't distinguished from DS3231 at
  the driver level — noted in the header comment as a caveat rather than
  built out, matching how this file already treats both chips
  interchangeably everywhere else.
- Boot/`i2c` command messages (`i2c_rtc_init()`'s "detected" line,
  `i2c_scan_bus()`'s banner) now report the real GPIO numbers from
  `CONFIG_I2C_RTC_SDA_GPIO`/`_SCL_GPIO` instead of a hardcoded "GP4/GP5" —
  found while making this change that the old hardcoded text would have
  been silently wrong on the clock persona otherwise. `/proc/config` also
  gained `I2C_RTC_BASE`/`_SDA_GPIO`/`_SCL_GPIO`, matching every other pin
  group already reported there.

**Build-verified 2026-08-13**: both RP2350 presets compile and link clean;
`rp2350-chess`'s generated `lugalos_config.h` confirmed unchanged in effect
(`CONFIG_I2C_RTC_BASE=0x40090000`, GP4/GP5); `rp2350-clock`'s confirmed
correct (`0x40098000`, GP6/GP7). 191/191 QEMU tests still pass.

**Hardware-verified 2026-08-13** (after L2's `clk_adc` fix unblocked boot
entirely — see L2's own section): the `i2c` shell command on real
RP2350W hardware reports `I2C Bus Scan (GP6 SDA / GP7 SCL)` with device
`0x68` found — the DS3231 is genuinely reachable over I2C1 on this board,
not just correct at the register-fact level. The temperature-read function
itself (`i2c_rtc_read_temperature_c()`) has no shell/Lisp command wired to
it yet, so its actual reading is still unverified pending L4.

## L4 — `(clock)` Lisp primitive + boot wiring *(done, 2026-08-13 — hardware-verified: real matrix shows time and temperature)*

Mirrors `(chess)` (`user/lisp/lisp.c`, `chess_ui.c`'s pattern): a blocking
loop, `pico_clock_green_run()` (`drivers/pico_clock_green_rp2350.c`), that
refreshes the display continuously and exits cleanly via
[[standardized_interrupt_polling]] (`console_interrupt_requested()`/
`console_interrupt_clear()`, the same Ctrl-C convention `chess_ui.c` already
uses — not a new mechanism). Allocates nothing on the heap, so
[[heap_stateless_user_programs]] is satisfied trivially rather than by
effort. `pico_clock_green_init()`'s eager boot call already landed in L2, so
L4 only needed the primitive itself.

**Boot wiring ended up bigger than planned, from a design conversation with
the user, not from a technical finding.** The original plan bullet ("boot
wiring") undersold it: the user drew a real distinction between chess (a
general-purpose program also run interactively on non-dedicated boards, so
it must stay *opt-in*) and the clock (single-purpose appliance hardware,
where auto-start is simply correct). Landed as a new `(boot-program)` Lisp
primitive (`user/lisp/lisp.c`, same shape as the existing `(board)`/`(arch)`
— a build-time fact, not a runtime setting), returning `"clock"` only when
`CONFIG_ENABLE_PICO_CLOCK_GREEN` is set, `""` otherwise — deliberately
*not* wired to `CONFIG_ENABLE_CHESS`, so chess keeps using
`/sd0/system/etc/usr_init.lisp` exactly as before, unchanged. `init.lisp`
gained one dispatch tier between the existing SD-card override and the
plain shell: `(boot-program)` = `"clock"` runs `(begin (clock) (lsh))`
rather than `(clock)` alone — an appliance with no SD card has no
`usr_init.lisp` escape hatch, so Ctrl-C needs to land somewhere useful
(a shell, to run `date`), not nowhere. Harmless for every other persona:
`(boot-program)` is `""` there, so it's just `(lsh)`, unchanged.

**Hardware-verified 2026-08-13, first real flash of the finished feature:**
matrix showed `HH:MM` and, cycling in every ~8s, temperature — both legible
and correct in kind (temperature reading ~28°C was accurate; the time was
wrong only because the DS3231 had never been set, not a bug — fixed the
same session by Ctrl-C into the new `(lsh)` fallback and running `date`).

**Real bug found on first flash, fixed same session — a second one QEMU
could not have caught:** the display flickered, visibly worse for time
than temperature. Root cause: `pico_clock_green_run()`'s original loop
called `i2c_rtc_read_time()`/`i2c_rtc_read_temperature_c()` on *every*
iteration (~once/row, ~1kHz) — only the redraw was value-gated, not the
I2C read itself. An I2C transaction at 100kHz blocks for real wall-clock
time (~800µs-1ms for time's 7-byte read, ~300-400µs for temperature's
2-byte read) — comparable to or longer than the entire ~1ms row period —
so it visibly disrupted `scan_step()`'s cadence on whichever row it landed
on, worse for time exactly in proportion to its longer transaction length
(matches the user's own observation precisely, which is what pointed at
this rather than a more generic "the LDR-dimming logic is wrong" guess).
Fixed by decoupling the two cadences entirely: poll I2C once/second
(`CLOCK_READ_INTERVAL_MS`), independent of `scan_step()`'s own ~1ms
per-row loop — reduces the disruption from every row to roughly 1 row in
1000. Confirmed fixed live: "Flicker is gone!"

Both RP2350 presets + both QEMU presets rebuilt clean after each change;
191/191 QEMU tests unaffected throughout (none of this touches anything a
QEMU target's own tests exercise).

---

## Deliberately out of scope for this phase

- 12h/AM-PM mode, alarms, count-up/down timer, scrolling text, weekday
  indicator LEDs, °F, on-device button settings menus, battery-voltage ADC
  diagnostic (`ADC_VCC`/GPIO29 — also unusable as ADC on RP2350W, see L0).
- SD card (`/sd0/`) on this board persona — no free pins for `SPI1` once the
  display claims GP10-13.
- Buttons (`SET_FUNCTION`/`UP`/`DOWN`) and buzzer wiring — time is already
  settable via the shell's `date` command; on-device time-set is a candidate
  follow-up, not blocked by anything in L1-L4 (the pins are reserved and free
  in the new board file even though L2-L4 don't read them yet).
- NTP / 9P-over-Wi-Fi via the RP2350W's CYW43439 — needs a from-scratch
  wireless SPI driver, a separate future phase (already tracked in
  `raw_ideas.md`), and confirmed in L0 not to be blocked by anything here.

## Straightforward future additions (not scheduled)

- Weekday indicator LEDs: the data path already renders into the same matrix
  buffer the vendor firmware uses for this (`disp_buf` bit macros in
  `define.h`), so this is a small addition once `dayofweek` is read from
  DS3231 — not a new subsystem.
- Buttons for on-device time-set, buzzer for alarms — noted above.
