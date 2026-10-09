# Phase 47 — The ribbon on the ESP32-P4-WIFI6-Touch-LCD-7B

**Status: 47.0 done (2026-10-09). Written 2026-10-09.** Nothing here has been
run on this board under LugalOS yet. The board facts in §2 come from Waveshare's
own example sources and from one read-only `esptool flash-id` against the
attached unit. Anything marked *(to read)* is what 47.0 turns into a recorded
fact before any driver depends on it.

**Milestone scheme: `47.0`, `47.1`, …** (as in phases 34 and 45).

**Goal.** Bring phase 44's horizontal tiling window system up on the Waveshare
ESP32-P4-WIFI6-Touch-LCD-7B: the menu bar with its minimap, the 1D ribbon of
terminal and canvas windows, multi-canvas isolation, `Cmd`-key navigation, and
screenshots. It should look and behave as it does on the RP2350-LCD-7, except
that the panel is larger (1024 × 600). Nothing in `screen.c`/`ribbon.c`/
`vterm.c` gets forked for the new board. They become display-agnostic, and the
new board supplies the scan-out, the keyboard transport and the memory.

**Scope, in build order.**

1. The board runs today's `esp32p4` kernel unchanged in spirit: console,
   flash, SD, I2C. This needs **silicon revision v3.2** (§3) and a new
   single-port host tool profile (47.0–47.2).
2. 32 MB of in-package PSRAM, usable as phase 38's bulk zone (47.3).
3. The MIPI-DSI panel shows pixels (47.4–47.5).
4. The portable terminal stack runs on it: `screen.c`, the ribbon and vterms,
   with a 1-bpp → panel conversion (47.6–47.8).
5. A USB keyboard on the Type-A OTG port (47.9–47.10). This is priority 1
   among the input work. It depends only on 47.2, not on the display, so it can
   run alongside 47.3–47.8. A keyboard is already plugged in.
6. Polish: backlight, touch as a secondary input, the USB-Serial/JTAG port as a
   9P link, the hardware test suite (47.11–47.13).

---

## 1. Decisions (confirmed by the owner 2026-10-09)

| # | Question | Proposal |
|---|---|---|
| D1 | One preset for both P4 boards, or a new one? | **Confirmed.** A new preset `esp32p4-lcd7b` with its own board file `cmake/board-esp32p4-lcd7b.cmake`. Same `LUGALOS_TARGET`, linker script and driver sources as `esp32p4`. The silicon revision is a board-file fact (`CONFIG_ESP32P4_REV`), chosen at build time, not probed at run time (§3). The NANO (v1.3) must keep building and passing `tests/hw/test_esp32p4.py` at every milestone. |
| D2 | Monochrome or colour? | **Confirmed: monochrome, pixel-identical to the RP2350-LCD-7** for this phase. Colour is evaluated later (e.g. for games) and is out of scope here. The 1-bpp `canvas1_t` model, the zebra title bars and the grey dither all stay. The scan-out format (D3) is picked so that colour can come later without redoing 47.4–47.7. |
| D3 | Scan-out format | **To be measured in 47.5, with GRAY8 as the leading candidate.** The DSI bridge takes `raw_type` 12 = gray (`mipi_dsi_bridge_reg.h`, hw_ver3), so a 1024 × 600 frame is 600 KB and 37 MB/s at 60 Hz. RGB565 doubles both and is the fallback if GRAY8 misbehaves on this panel. Either way the frame lives in PSRAM: 1-bpp has no DPI format, and even GRAY8 does not fit in the ~370 KB heap. |
| D4 | Keyboard | **Confirmed, priority 1: a USB boot keyboard on the Type-A port** (USB 2.0 OTG HS, DWC2 core). A keyboard is already plugged in. It gets a bare-metal host-controller driver built from permissively licensed sources (§5.1). Above the controller it reuses phase 36's portable `usbkbd.c` (enumeration, hub, boot report, keymap). Touch (GT911) is priority 2 or later (47.12). |
| D5 | Console | **Confirmed: UART0 through the on-board CH343P** ("USB TO UART" Type-C), 115200 baud, like the NANO's UART0. The board's other Type-C (P4 USB-Serial/JTAG, "USB1.1 FS direct") is kept free for the 9P link (47.13). |
| D6 | The on-board ESP32-C6 | **Left alone.** It runs its factory ESP-Hosted slave and LugalOS does not touch `C6_CHIP_PU` or the SDIO pins in this phase. The C6 belongs to step 2 (§9). |
| D7 | Ribbon width presets on a 126-column screen | **Confirmed: derived from the screen width, not hard-coded.** Today's 38/48/64/98 are 800-px numbers spread across `screen.c` (`text_cols = 38/48/64`), `ribbon.c` (`800u` defaults) and `console.c` (`48` for a new canvas). 47.6 moves them to a geometry table computed from `w`, so that "half", "third" and "two-thirds" tile exactly, including the 10-px gaps, at both 800 and 1024. The RP2350 must come out with exactly today's numbers. |

## 2. The board

### 2.1 Measured (2026-10-09, `esptool flash-id`, read-only)

```
Chip type:          ESP32-P4 (revision v3.2)
Features:           Dual Core + LP Core, 400MHz
Crystal frequency:  40MHz
Flash:              manufacturer 0xc8 (GigaDevice), device 0x4019, 32 MB
Port:               /dev/serial/by-id/usb-1a86_USB_Single_Serial_5CF7108441-if00
                    (CH343P), classic DTR/RTS auto-reset -- esptool entered
                    download mode and hard-reset via RTS with no button
```

### 2.2 From Waveshare's sources (`~/Source/gith/esp/ESP32-P4-WIFI6-Touch-LCD-7B/`)

| Function | Fact | Source |
|---|---|---|
| Panel | 7", 1024 × 600 IPS, **EK79007**, MIPI-DSI **2 lanes @ 1000 Mbps**, DPI clock 52 MHz, 60 Hz | `examples/arduino/libraries/displays/displays_config.h`, `examples/esp-idf/07_color_panel` |
| Panel timing | HSYNC pw 10, bp 160, fp 160; VSYNC pw 1, bp 23, fp 12 | same |
| Panel init | `B2 10`, `80 8B`, `81 78`, `82 84`, `83 88`, `84 A8`, `85 E3`, `86 88`, `11` (+120 ms), then display-on | same |
| Panel reset | GPIO33 | same |
| DSI PHY supply | on-chip LDO channel 3 at 2500 mV | `07_color_panel` (`TEST_MIPI_DSI_PHY_PWR_LDO_*`) |
| Backlight | GPIO32, **active low**, LEDC 5 kHz 10-bit, inverted output | `displays_config.h` |
| I2C (board bus) | SDA GPIO7, SCL GPIO8, 100 kHz. Shared by GT911, ES8311 (0x18), ES7210 (0x40) and the PH2.0 I2C header | `displays_config.h`, `examples/arduino/README.md` |
| Touch | GT911 at 0x5D or 0x14. Waveshare polls it and assigns no INT/RST | `examples/arduino/README.md`, `libraries/displays/gt911.*` |
| microSD | SDMMC slot 0: CLK 43, CMD 44, D0–D3 39–42, the **same IO_MUX pads as the NANO**. Card power switched by **GPIO45** (owner's schematic reading, 2026-10-09). *(to verify: polarity; on the NANO, low = on)* | `examples/arduino/examples/08_SD_Card`, Arduino README |
| PSRAM | 32 MB in-package (ESP32-P4NRW32) | README |
| UART0 | CH343P bridge *(to read: confirm GPIO37/38)* | schematic "USB to UART" |
| RS485 | UART1 TX GPIO27, RX GPIO26 | Arduino README |
| CAN | TWAI TX GPIO22, RX GPIO21, TJA1051 | Arduino README |
| Audio | I2S MCLK 13, BCLK 12, LRCK 10, DOUT 9, DIN 11; PA enable GPIO53 | Arduino README |
| USB-A | USB 2.0 OTG HS (dedicated PHY pins). VBUS looks **unswitched** (owner's schematic reading, 2026-10-09). *(to verify in 47.0: measure 5 V at the port with the P4 held in reset)* | schematic "POWER"/USB block, nets `USB0_5V`, `USB1_5V`, `VBUS_OUT` |

The schematic is `~/Source/gith/esp/datasheet/ESP32-P4-WIFI6-Touch-LCD-7B.pdf`
(2 pages, Altium). Its text layer gives net names but not pin-to-net mapping,
so 47.0 reads it zoomed, block by block (`pdftoppm -r 400` and crops).
Waveshare say in their README that their repository holds no local schematic
and that the BSP is the authority for pins. Where this PDF and the BSP disagree,
we record both and measure, the way phase 36 §1.5 did.

## 3. The real risk: v3.2 silicon against a v1.3 port

Everything under `arch/riscv/common/*esp32p4*` and `drivers/*_esp32p4.c` was
written against **revision v1.3**, and many files say so:

* register layouts from `soc/esp32p4/register/hw_ver1/` (`clk_esp32p4.c`,
  `uart_esp32p4.c`, `sdmmc_esp32p4.c`, `i2c_bus.c`, `emac_esp32p4.c`);
* ROM entry points from `esp32p4.rom.eco0_4.ld` (`trap.c`, `smp_esp32p4.c`,
  `xip_esp32p4.c`, `flash_esp32p4.c`), and v3.x silicon has a different ROM;
* the CPU clock ladder 40/90/180/360, which `clk_esp32p4.c` enforces. v3.x
  silicon uses **100/200/400** (phase 34 §4 notes the split).

Phase 34 diffed hw_ver1 against hw_ver3 for the clock registers and found them
to differ in places. That work is the template. 47.1 does it for every
register file and ROM symbol the kernel uses, and ends with a table in this
plan's §6: same / moved / different, with a decision for each row. This is the
step where "it boots on the NANO" can make us overconfident, which is the
lesson of the memory note on tracing the reference first. Waveshare's
`02_hello_world` and `07_color_panel` (ESP-IDF v5.5.5 / v6.x, Rev3.x
`postv3` profile) build for this board and are the known-good references for
register diffs.

Also new with this board:

* **32 MB flash, which needs 4-byte addressing above 16 MB.** The kernel's flash
  layout (`cmake/flash_layout_esp32p4.cmake`) stays below 16 MB in this phase,
  and `flash_esp32p4.c` refuses addresses ≥ 16 MB rather than wrapping them.
* **A single host port.** `tools/p4run.py` / `tools/p4flash.py` assume the
  NANO's pair (CH343P for reset, CP2102 for UART0, see `p4run.py:43–50`). On
  the 7B the CH343P *is* UART0, with standard auto-reset wiring. Both tools
  gain a board profile, selected by `--board lcd7b` or by finding no CP2102.
  On this board they must never open the port with DTR/RTS in the
  reset/boot state unless that is the intent.

## 4. The display pipeline

```
 screen.c / ribbon.c / vtterm.c          (portable, unchanged in behaviour)
          |  draw into
          v
 1-bpp framebuffer, 128 B x 600 = 75 KB  (SRAM; canvas1_t, stride 128)
          |  lcdterm flush: dirty rows only, 1 bit -> 1 byte (or 2) via LUT
          v
 GRAY8 (or RGB565) frame in PSRAM, 600 KB (1.2 MB)
          |  L2 write-back of the touched lines
          v
 DW-GDMA -> DSI bridge -> DSI host (2 lanes, 1 Gbps) -> EK79007
```

* **Why keep the 1-bpp buffer.** Everything above it is already tested on
  QEMU (`vtselftest`, `ribbonselftest`, `ramscreen.c`) and on silicon, and
  50 % grey is a dither pattern that only means something at 1 bpp. The
  conversion is the one new thing and can be tested in isolation.
* **Damage tracking.** On the RP2350 the PIO scans the SRAM buffer itself, so a
  write is visible immediately and nothing tracks damage. Here a flush has to
  know which rows changed. 47.7 adds a per-row dirty bitmap to `canvas1_t`
  (600 bits; NULL leaves today's behaviour) and has `lcdterm` convert only
  dirty rows at a fixed cadence (≤ 60 Hz, aligned to the DPI frame-done
  event). The fallback, if the hooks turn out to be scattered, is diff-scan
  against a last-converted copy. That costs 75 KB more, so we only use it if
  measurement says so.
* **Cache coherence is the classic failure here.** The CPU writes the PSRAM
  frame through L2, but the GDMA reads PSRAM directly. Every flush ends with
  a write-back of the converted address range (ROM `Cache_WriteBack_Addr`,
  the v3.x symbol) before the next frame starts. 47.5 tests this
  deliberately: draw, skip the write-back, see stale pixels, restore it.
* **Tearing.** Phase 44 §1.5 paced redraws to whole frames. Here the DPI
  rescans continuously, so a conversion pass that races the scan line can
  tear. We start with one frame and measure. If tearing shows, the DPI
  driver's two-frame-buffer mode is the fix (swap on frame-done, 2 × 600 KB
  of PSRAM).
* **Cost estimate (to verify).** A full repaint is 75 KB read and 600 KB
  written to PSRAM, a few ms at v3.x PSRAM rates. A typed character dirties
  16 rows, i.e. 16 KB. Both are well under one 16.7 ms frame.

## 5. Milestones

### Board and silicon

**47.0 — Board facts and host tooling. [DONE 2026-10-09, §5.2]**
Read the schematic into `cmake/board-esp32p4-lcd7b.cmake`, with every number
labelled by its source as in `board-esp32p4-nano.cmake`: UART0 pads, SD power
polarity (GPIO45 per the owner), USB-A VBUS (believed unswitched; verify), GT911 INT/RST if wired, LED, `BOOT`/`EN`
wiring. Give `p4run.py`/`p4flash.py` a single-port CH343P profile. Add the
`esp32p4-lcd7b` preset. The demo firmware on the board may be erased (owner,
2026-10-09), so we take no backup. If it is ever wanted back, Waveshare's
`firmware/ESP32-P4-WIFI6-Touch-LCD-7B-FactoryOnly.bin` restores it.
*Done when:* `p4run.py --board lcd7b` shows the ROM banner and resets the
board without holding it in reset, and `p4flash.py --board lcd7b` writes and
verifies a minimal image.

**47.1 — v1.3 → v3.2 audit.**
For every hw_ver1 register header and every `eco0_4` ROM symbol the kernel
uses, diff against hw_ver3 / the v3.x ROM `.ld` and record the result in
§6. Turn the differences into `CONFIG_ESP32P4_REV`-selected code. Use
`#if` blocks only where the difference is a constant; where layouts or
sequences differ, use a separate file. The 40 MHz-only path (no PLL) comes up
first, as in phase 27's E1/E2.
*Done when:* `tools/build_minimal_esp32p4.sh` prints on the 7B, and the
NANO's `test_esp32p4.py` still passes.

**47.2 — The kernel boots on the 7B.**
Stage 2, XIP from flash, the shell on UART0, SMP, CLIC/PMP, then the PLL at
100 → 200 → 400 MHz in steps (phase 34's method: each step a config, with
40 MHz always available as the control). SDMMC on the same pads, I2C on
GPIO7/8 (`i2c scan` should find 0x18, 0x40 and 0x5D or 0x14), phase 46
sensors on the header.
*Done when:* `test_esp32p4.py` (or a 7B variant that skips Ethernet; this
board has none) passes on the 7B.

**47.3 — PSRAM.**
The P4's MSPI PSRAM controller, AP hex-mode device (reference:
`esp-idf/components/esp_psram/device/esp_psram_impl_ap_hex.c` and the MSPI
timing tuning it calls), mapped through the cache. Then phase 38's
`CONFIG_PSRAM_BYTES` / `CONFIG_PALLOC_BULK_PAGES` / `BULK_BSS` on this
target, so the Lisp pools, `/ram0`, the chess TT and the canvas backing
stores move there unchanged. Test it the way phase 38 did: size by aliasing,
a march test, and an SRAM-vs-PSRAM speed measurement. The NANO carries the
same package and gets this for free behind its own board-file line.
*Done when:* `palloc_pages_bulk()` hands out PSRAM on the 7B, a full march
test over 32 MB passes, and the bandwidth numbers are recorded here.

### Display

**47.4 — DSI link up, hardware test pattern.**
LDO3 at 2.5 V, DSI PHY and PLL for 1 Gbps × 2 lanes, the DSI host in command
mode for the EK79007 init sequence (§2.2), GPIO33 reset, then video mode with
the DSI bridge's built-in **pattern generator** (IDF's
`MIPI_DSI_PATTERN_BAR_VERTICAL`). That gives pixels on the glass with no
framebuffer and no DMA involved. Backlight on GPIO32 (plain GPIO low first,
LEDC later). New file: `drivers/dsi_esp32p4.c`.
*Done when:* `lcd pattern bars` shows Waveshare's colour bars, and the
references are diffed against `esp_lcd_mipi_dsi_bus.c` /
`esp_lcd_panel_dpi.c` register writes (hw_ver3) where they disagree.

**47.5 — Framebuffer scan-out from PSRAM.**
DW-GDMA linked-list reading a PSRAM frame into the bridge. Try GRAY8 first,
then RGB565 (D3). Show a checkerboard, a 1-px grid and a text page drawn
directly into the frame, and test the cache write-back (§4) both ways. Measure
the frame rate (frame-done interrupt count / s) and the CPU-side fill rate.
*Done when:* the 1-px grid is sharp to the edge with no off-by-one at x 1023
or y 599 (phase 36 found the RP2350 panel's bezel column, and this one may
have its own), the D3 format is chosen and recorded here, and
`screenshot`-style readback matches what was drawn.

### The terminal stack

**47.6 — Separate the portable terminal from the RP2350 panel.**
`drivers/lcd7_rp2350.c` holds two things: the PIO scan-out, and the `lcdterm`
task with its channel, `console_screen_t` hookup, status tick, tee and tests.
Split it into `drivers/lcdterm.c` (portable, U-mode task as today) and a
small backend interface (framebuffer geometry, `flush(rows)`, `backlight(%)`,
`pattern(name)`). The `lcd7_*` entry points in `kernel/console.c` and the
shell become `lcdterm_*`. Do D7's geometry table in the same milestone. The
RP2350 is the regression target: the same screenshots, `vtselftest`,
`ribbonselftest`, `test_rp2350.py`, and `ribbonselftest`'s 800 × 480
expectations unchanged.
*Done when:* `rp2350-terminal` is bit-identical in behaviour (screenshot
diff), and a QEMU `ramscreen` at 1024 × 600 passes `ribbonselftest` with
the derived widths.

**47.7 — `lcdterm` on the 7B.**
The ESP32-P4 backend (`drivers/lcd7b_esp32p4.c`): a 1-bpp buffer in SRAM,
dirty-row conversion into the PSRAM frame (§4), and frame-paced flushing. Boot
into the TEXT layout with the menu bar. Console output is teed to the panel as
on the RP2350 (`lcd7_set_tee` semantics kept). Input is still UART0 only.
*Done when:* the shell runs on the panel at 126 × 34 cells, `ls -l /sd0`
scrolls without tearing, and per-char and per-scroll flush times are recorded.

**47.8 — Ribbon, multi-terminal, multi-canvas.**
Phase 44's whole surface: `ribbon`, `canvas`, vterms, the minimap, and four
PSRAM canvas slots. They are now 1024-wide, i.e. 77 KB each, so the slot size
becomes a function of `w`. Drive it from the UART console (`ribbon focus`,
`ribbon move`, and the hotkey ops through the existing `console_canvas`
commands) so it is testable before a keyboard exists. Run the phase 44 demos:
chess board, `ca.lisp`, `lorenz.lisp`, `cas/plot.lisp`.
*Done when:* phase 44's `verify_multicanvas.py` / `verify_switch.py` scenarios
pass against the 7B over UART0, and screenshots match the RP2350 artefacts
apart from width.

### Keyboard

**47.9 — USB OTG HS host controller (DWC2), bare-metal.** *(Priority 1 after
the GUI; sources in §5.1.)*
Power and clock the OTG HS core and its UTMI PHY, then do host-mode init,
port reset, speed detection (LS/FS keyboards behind the HS root port need
split transactions only through a hub; a directly attached LS/FS keyboard
does not), channel-based control and interrupt-IN transfers. Use **slave
(FIFO) mode, not the core's internal DMA**: it avoids cache maintenance on
every transfer and is what TinyUSB uses by default. Polled first; the
interrupt line (`ETS_USB_OTG_INTR_SOURCE`) comes later. New file:
`drivers/usbhost_esp32p4.c`. Put an interface between
it and phase 36's `usbkbd_rp2350.c` logic (control transfer, interrupt
poll, port events), so that enumeration, the hub driver and the boot-report
path are shared and not copied.
*Done when:* a `usb` shell command enumerates a directly attached keyboard and
prints its device and configuration descriptors (parsed by the existing
`usb_parse_config()`).

**47.10 — Keyboard into the ribbon.**
The `kbd` task on the new transport: boot protocol, keymap, auto-repeat, and
the `Cmd`/Super hotkeys of phase 44 §3.1, delivered to the focused window's
`rx_fifo` as on the RP2350. A hub with a keyboard behind it is a stretch
goal here: it needs split transactions on the HS port.
*Done when:* phase 44's hotkey table works from a USB keyboard on the 7B,
`usbkbdselftest`/`keyselftest` pass, and unplug/replug survives.

### 5.1 Sources for the USB host (surveyed 2026-10-09)

LugalOS is MIT-licensed. All three candidates below are compatible with that.

| Source | License | Location | What it gives us |
|---|---|---|---|
| **TinyUSB 0.18.0**, `src/portable/synopsys/dwc2/` | **MIT** | `~/Source/gith/pico/pico-sdk/lib/tinyusb/` | `hcd_dwc2.c` (1360 lines): a complete DWC2 **host** controller driver, with channel allocation, slave and DMA modes, and split transactions for LS/FS devices behind hubs. `dwc2_type.h` (2287 lines): the whole DWC2 register map as C structs. `dwc2_esp32.h` already names the P4's HS core at **0x50000000** (FS core at 0x50040000). Also `class/hid/hid_host.c` for comparison. |
| **ESP-IDF `esp_hal_usb`** | Apache-2.0 | `~/Source/gith/esp/esp-idf/components/esp_hal_usb/` | The P4-specific part TinyUSB leaves to IDF: `esp32p4/include/hal/usb_utmi_ll.h` (UTMI HS PHY clocks and reset in `HP_SYS_CLKRST` / `LP_AON_CLKRST`), `usb_wrap_ll.h`, `usb_dwc_ll.h`, `usb_dwc_hal.c`. |
| CherryUSB 1.5.2 (IDF example `peripherals/usb/host/cherryusb_host`) | Apache-2.0 | managed component, not downloaded | A second, independent DWC2 host implementation, for cross-checking only. |

**Plan.**

* Take `dwc2_type.h` verbatim, keeping its MIT header, as
  `drivers/include/drivers/dwc2_regs.h`. It is pure register definitions and
  has no dependency on TinyUSB.
* Write `usbhost_esp32p4.c` ourselves, following `hcd_dwc2.c`'s structure:
  core reset, FIFO sizing, root-port handling, channel state machine, NAK
  retry, and the split-transaction path. Where whole functions carry over
  closely, they keep TinyUSB's MIT attribution in the file header. We do not
  import TinyUSB's `usbh.c` core: the phase 36 `usbkbd` code already does
  enumeration, and pulling in a second USB stack would duplicate it.
* PHY and clock bring-up follows the register sequence in IDF's
  `usb_utmi_ll.h` / `usb_wrap_ll.h`. That is a list of register facts, and
  each one is cited in the code like the other `*_esp32p4.c` drivers. Since
  we take facts rather than Apache code, no NOTICE file is needed.
* A known-good reference on this board: IDF's
  `examples/peripherals/usb/host/hid` builds for the P4 with the installed
  toolchain. Flash it once to prove the keyboard and port work, and to diff
  registers if our driver stalls (the trace-the-reference rule).

### Polish and test

**47.11 — Backlight and panel power.**
LEDC PWM on GPIO32 (inverted), `lcd backlight <percent>`, and blanking after
idle if wanted.

**47.12 — Touch as secondary input (GT911, polled). Priority 2 or later; may
move to a later phase.**
Tap a window to focus it, swipe horizontally to scroll the ribbon, and tap the
minimap to jump. No pointer or text-selection model, and nothing that would
make touch a requirement.

**47.13 — USB-Serial/JTAG as the 9P link, and the test suite.**
The P4's USB-Serial/JTAG is the same IP as the C6's (`drivers/usbjtag_esp32c6.c`)
at a different base. Bring it up as the `link_usb_cdc`-equivalent so
`tools/p9sync.py` can update `/sd0` without pulling the card, as on the
RP2350 terminal. Add `tests/hw/test_esp32p4_lcd7b.py`, skipping when the board
is absent: boot, clocks, PSRAM, SD, I2C scan, DSI frame-rate,
screenshot-vs-expected, ribbon scenarios, keyboard enumerated (or skipped if
no keyboard). Update `AGENTS.md` §3/§4/§5 with the board's port topology,
preset and flashing procedure.
*Done when:* the suite passes, `tests/runner.py` is unchanged on QEMU, and
`test_esp32p4.py` on the NANO and `test_rp2350.py` on the terminal still
pass.

## 5.2 What 47.0 found (2026-10-09)

* **Console and reset share one port, and opening it resets the chip.**
  Linux raises DTR and RTS on every open of a tty whose baud is not B0, and on
  this board's U7 (EMH4T2R) that pulled ESP_EN low every time: a RAM-loaded
  image was replaced by a flash boot before it printed a byte. Measured: a
  plain close + reopen reset it every time, and a close at B0 + reopen never
  did. `tools/p4run.py` now parks the tty at B0 whenever it lets go (`park()`),
  drops RTS before DTR, and on a single-cable board lends its open handle to
  esptool for the load and keeps reading afterwards (`Watcher.lent()`). No
  `--board` flag was needed: with no CP2102 attached, the existing port
  detection picks the CH343P for both roles. `--reset-test` passes, and so do
  run and download.
* **v3.x swapped L2MEM's ends.** On v1.3 the ROM's data and download buffers
  are at 0x4ff296b8–0x4ff40000 and the L2 cache is at the top. On v3.x the
  cache starts at **0x4ff00000** and the ROM's data is at
  **0x4ffa96b8–0x4ffc0000** (IDF `bootloader.memory.ld.in`,
  `esp_system/ld/esp32p4/memory.ld.in`). `tools/minimal_esp32p4.ld` linked
  at 0x4ff00000, so it loaded into cache and silently never ran. It now
  links at 0x4ff40000, which is clear on both revisions.
* **The NANO-built kernel boots on v3.2 at 40 MHz**, unmodified apart from
  the board file: XIP, timer, allocators, SMP (two harts), preemption, flash,
  `/sd0` (4-bit, 20 MHz), I2C and the shell. Its RAM half
  (0x4ff40000–0x4ff9e000, heap 372 KB) happens to sit between the two
  regions above. So 47.1 is an audit of what has not been exercised yet,
  not a rescue.
* `i2c scan` on GPIO7/8 finds exactly the board's devices: 0x18 (ES8311),
  0x40 (ES7210), 0x5D (GT911). **The boot-time probes report phantoms**
  ("DS1307-compatible at 0x68", "AT24C32 at 0x57") that the scan does not
  see. That is a bug to fix in 47.2.
* `minimal_esp32p4.c`'s GPIO20 test fails to drive the pad (it reads 0 at
  both levels; pulls work). GPIO20 is not wired on this board. To check in
  47.1: v3.x GPIO output enable, or an unconnected pad.
* After a *flash* boot the minimal image is reset by
  `HP_SYS_HP_WDT_RESET`. The ROM arms a watchdog that the kernel handles and
  the minimal image does not. RAM loads are unaffected.
* `misa` = 0x40903127 on v3.2, which includes the B extension.
* Schematic facts are recorded in `cmake/board-esp32p4-lcd7b.cmake`: SD power
  on GPIO45 (high = off, R29 pull-down); USB-A VBUS always on through a
  DIO7003 current-limited switch; GPIO33 panel reset (held low by R45 until
  driven); GPIO32 backlight (higher = dimmer); GPIO23 touch reset; touch INT
  only on test point TP1; no user LED; the C6 on SDIO GPIO14–19 with
  CHIP_PU on GPIO54.
* The demo firmware is erased, and `p4flash.py --only boot,os --verify` writes
  and verifies this preset unchanged.

## 6. v1.3 → v3.2 audit table (filled by 47.1)

| Area | File(s) | hw_ver1 vs hw_ver3 / ROM | Decision |
|---|---|---|---|
| Clock tree, CPU ladder | `clk_esp32p4.c` | *(47.1)* | |
| UART0 | `uart_esp32p4.c` | | |
| Trap / CLIC / PMP, ROM calls | `trap.c` | | |
| XIP, cache, L2 size | `xip_esp32p4.c`, `linker/esp32p4.ld` | | |
| SMP release | `smp_esp32p4.c` | | |
| Flash (ROM SPI calls) | `flash_esp32p4.c` | | |
| SDMMC | `sdmmc_esp32p4.c` | | |
| I2C | `i2c_bus.c` | | |
| eFuse | `efuse_esp32p4.c` | | |
| Stage-2 loader | `tools/p4flash.py`, boot image header | | |

## 7. Risks

| Risk | Impact | Mitigation |
|---|:---:|---|
| v3.2 differs from v1.3 in more places than the clock tree | High | 47.1 audit before any feature work. Build-time revision selection. NANO regression at every milestone. |
| PSRAM MSPI timing tuning is fiddly and undocumented outside IDF | High | Start at a conservative clock, taking IDF's tuned values as the reference; trace IDF's register writes on this board (memory note) before guessing. The display can fall back to a smaller test frame in SRAM for 47.4/47.5 so the DSI work is not blocked. |
| DSI/DPI bring-up has no visible intermediate state (a black panel says nothing) | Med | Pattern generator first (47.4), with no DMA in the loop. Register-diff against Waveshare's `07_color_panel` built with the installed IDF. |
| GDMA reads stale PSRAM (cache) | Med | An explicit write-back per flush. Test it both ways in 47.5. |
| DWC2 host is a large new driver | High | Polled, single device, boot protocol only. Shared `usbkbd` logic. Hubs are a stretch goal. A UART-driven ribbon (47.8) means the GUI does not wait for it. |
| `Cmd`-key ribbon tests depend on the keyboard | Low | `ribbon`/`console_canvas` commands reach the same code paths from UART0. |
| Shared I2C bus (touch, codecs, header sensors) | Low | One bus owner (`i2c_bus.c`). The codecs are not initialised in this phase. |
| 32 MB flash above 16 MB | Low | Not used in this phase. The driver refuses ≥ 16 MB. |

## 8. Explicitly not in this phase

Colour UI. Camera (MIPI-CSI). Audio (ES8311/ES7210). RS485 and CAN. The LP
core. Battery and charger reporting. Anything on the ESP32-C6, including Wi-Fi,
SDIO, and reflashing the C6 through the "C6-UART" header. Overlapping windows.
USB devices other than keyboards (mass storage, mouse).

## 9. Next: step 2 (not elaborated here)

The second step continues phase 45's ESP32-C6 work as the general Wi-Fi
interface for the P4 boards. Phase 45 §5 already plans this direction for the
NANO (milestones 45.14–45.16: LugalOS on the C6, raw Ethernet frames to the P4
over SDIO, with ESP-Hosted only as plan B). The 7B carries the same C6-MINI-1
on SDIO, plus a separate "C6-UART" header for flashing it. This phase leaves
both untouched so that step 2 starts from a known factory state.
