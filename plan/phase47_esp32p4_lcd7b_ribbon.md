# Phase 47 — The ribbon on the ESP32-P4-WIFI6-Touch-LCD-7B

**Status: 47.0–47.3 done (2026-10-09). Written 2026-10-09.** The kernel
runs on the board at 40 MHz with its own v3 memory layout. The HIL suite passes
25/25 there; the BME280 and EMAC tests skip, because the board has neither.
The NANO passes 25/25 on the same tree.
§2 was written from Waveshare's sources before anything ran; what 47.0 and 47.1
measured is in §5.2 and §6, and the schematic readings are in
`cmake/board-esp32p4-lcd7b.cmake`.

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
| D3 | Scan-out format | **Decided in 47.5: GRAY8** (§6.6). The DSI bridge takes `raw_type` 12 = gray and expands it to R = G = B. A 1024 × 600 frame is 600 KB of PSRAM and 37 MB/s of scan-out at 60 Hz, about a third of the measured PSRAM read bandwidth. RGB565 runs too (1.2 MB, 74 MB/s, also tested clean); it's a runtime switch for colour later. |
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

**47.1 — v1.3 → v3.2 audit. [DONE 2026-10-09, §6]**
For every hw_ver1 register header and every `eco0_4` ROM symbol the kernel
uses, diff against hw_ver3 / the v3.x ROM `.ld` and record the result in
§6. Turn the differences into `CONFIG_ESP32P4_REV`-selected code. Use
`#if` blocks only where the difference is a constant; where layouts or
sequences differ, use a separate file. The 40 MHz-only path (no PLL) comes up
first, as in phase 27's E1/E2.
*Done when:* `tools/build_minimal_esp32p4.sh` prints on the 7B, and the
NANO's `test_esp32p4.py` still passes.

**47.2 — The kernel boots on the 7B. [DONE 2026-10-09, §6.1]**
Stage 2, XIP from flash, the shell on UART0, SMP, CLIC/PMP, then the PLL at
100 → 200 → 400 MHz in steps (phase 34's method: each step a config, with
40 MHz always available as the control). SDMMC on the same pads, I2C on
GPIO7/8 (`i2c scan` should find 0x18, 0x40 and 0x5D or 0x14), phase 46
sensors on the header.
*Done when:* `test_esp32p4.py` (or a 7B variant that skips Ethernet; this
board has none) passes on the 7B.

**47.3 — PSRAM. [DONE 2026-10-09, both boards at 200 MHz, §6.2–6.3]**
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

**47.4 — DSI link up, hardware test pattern. [DONE 2026-10-10, §6.4]**
LDO3 at 2.5 V, DSI PHY and PLL for 1 Gbps × 2 lanes, the DSI host in command
mode for the EK79007 init sequence (§2.2), GPIO33 reset, then video mode with
the DSI bridge's built-in **pattern generator** (IDF's
`MIPI_DSI_PATTERN_BAR_VERTICAL`). That gives pixels on the glass with no
framebuffer and no DMA involved. Backlight on GPIO32 (plain GPIO low first,
LEDC later). New file: `drivers/dsi_esp32p4.c`.
*Done when:* `lcd pattern bars` shows Waveshare's colour bars, and the
references are diffed against `esp_lcd_mipi_dsi_bus.c` /
`esp_lcd_panel_dpi.c` register writes (hw_ver3) where they disagree.

**47.5 — Framebuffer scan-out from PSRAM. [DONE 2026-10-10, §6.6; D3: GRAY8]**
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

## 6. v1.3 → v3.2 audit (47.1, done 2026-10-09)

**Method.** ROM: every `0x4fc0xxxx` address the kernel uses, looked up in both
`esp32p4.rom.eco0_4.ld` (v1.3) and `esp32p4.rom.ld` (v3.x). Registers: a
semantic diff of `soc/esp32p4/register/hw_ver1` against `hw_ver3`, comparing
`#define` values with comments stripped (most of the textual churn is
documentation), restricted to the blocks the kernel touches. Then
measurement on the board for everything the diff could not settle.

| Area | File(s) | v1.3 vs v3.2 | Decision |
|---|---|---|---|
| ROM entry points (flash, cache, SMP, CPU freq, UART) | `flash_esp32p4.c`, `trap.c`, `xip_esp32p4.c`, `smp_esp32p4.c`, `clk_esp32p4.c` | **All 23 called addresses identical.** The two that moved (`ets_clk_get_cpu_freq`, `Cache_Get_IROM_MMU_End`: 0x554/0x560) are only named in a comment explaining why they are avoided. | No change. |
| UART | `uart_esp32p4.c` | `uart_reg.h` byte-identical | No change. |
| GPIO, IO_MUX, SDMMC, systimer, timer groups, LP WDT, cache | various | No register moved and no field changed; only additions, and removals the kernel does not use | No change. |
| I2C | `i2c_bus.c` | No definition changed | No change. |
| Clock tree | `clk_esp32p4.c` | `hp_sys_clkrst`: no address or field changed (TWAI resets gone, 70 new); `lp_clkrst` identical | No change at 40 MHz; the 100/200/400 ladder is 47.2. |
| Interrupt matrix | `trap.c` | 537 names gone / 1640 new, but **by address** every source we route or will route is unchanged: UART0 0x7c, USB OTG 0x174/0x178, DSI 0x158/0x160, GDMA 0x60, PPA 0x180, I2C0 0xb0, SDMMC 0x5c, LEDC 0xd0 | No change. |
| eFuse | `efuse_esp32p4.c`, `clk_esp32p4.c` | Register addresses identical; MAC (BLK1 40..87), ACTIVE_HP_DBIAS (BLK1 144..147) and OPTIONAL_UNIQUE_ID (BLK2) at the same bits in both IDF tables. MAC read on the board matches esptool's. | No change; chip revision now read and checked (below). |
| **CLIC** | `trap.c`, `shell.c` | **Different.** v1.3 has a pre-standard CLIC: `mintstatus` at CSR 0x346, threshold memory-mapped. v3.x follows the ratified spec: `mintstatus` 0xFB1, threshold in CSR `mintthresh` 0x347 (IDF `soc/interrupt_reg.h`, `riscv/csr_clic.h`). Reading 0x346 on v3.2 traps (illegal instruction) — `clicdump` found it. | `P4_MINTSTATUS_CSR` / `P4_MINTTHRESH_CSR` in `arch/esp32p4_intr.h` by revision; `p4_clic_init` also clears `mintthresh` on v3. Kernel code never read `mintstatus` (only the diagnostic did); the MIL drop via `mcause.MPIL` + `mret` works unchanged: `spin1` +200 ticks. |
| **L2MEM map** | `linker/esp32p4.ld` | **Different, and the one that corrupted memory.** v3.x puts the L2 cache at the *bottom* and the ROM's data at the *top*. The ROM leaves the cache at **128 KB** on v3.2 (v1.3: 256 KB), measured from `CACHESIZE_CONF` (0x3ff10278, now in the `[L2]` boot line). | `MEMORY` moved to `linker/esp32p4_memory_rev{1,3}.ld`, chosen by CMake from `CONFIG_ESP32P4_REV`; asserts use per-revision bounds (`P4_LOAD_FLOOR`, `P4_RAM_FLOOR`, `P4_RAM_CEIL`, `P4_LOWRAM_CEIL`, `P4_HEAP_FLOOR`). v3: LOWRAM 0x4ff20000 (+252K), RAM 0x4ff5f000..0x4ffa9000, **heap 284 KB** (NANO 372 KB). NANO layout byte-identical to before. |
| Stage-2 / boot image | `tools/p4flash.py` | ROM loads the RAM half from 0x2000 unchanged | No change. |

**The corruption, for the record.** Under the v1.3 layout the 7B booted, ran the
shell and the SD card, and passed a marker test. A build that shifted `.bss` by
a few hundred bytes then put `g_fast_bitmap` (0x4ff03904) on a cache line in use,
and the page allocator handed out the `uart` task's live stack; palloc's own
overlap check halted it. A scan of L2MEM for copies of XIP code lines, meant to
locate the cache directly, *hung* the chip — reading the cache's live storage
is not survivable — and was removed again.

**The revision is now checked, not just stated.** `esp32p4_chip_rev()` reads
the wafer version from eFuse (BLK1 bits 64..69 and 87). Boot prints
`[Chip] ESP32-P4 v3.2`, and if the board file and the silicon disagree across
v3.0 the kernel halts with the reason. That was exercised by flashing the NANO
build onto the 7B on purpose.

**Also fixed on the way.**
* `/proc/meminfo`'s image size has been wrong on *both* boards since phase 32
  (it reported 261722 KB, because the flash window was counted as part of the
  image). It now adds the three parts.
* The `CONFIG_ESP32P4_REV` encoding: the NANO is **103** (major·100+minor),
  not the 132 that 47.0 wrote.
* Host tooling: `--board nano|lcd7b` with `~/.config/lugalos/p4-ports.env`.
  With two CH343Ps attached, detection now refuses to guess. The HIL suite
  opens and closes ports without touching DTR/RTS, closes its session before
  a reboot, flashes `--build`, and skips the EMAC tests on a board without
  Ethernet.

**Still open after 47.1, moved to 47.2:** phantom boot-time I2C detections
(RTC at 0x68, EEPROM at 0x57 on a bus where `i2c scan` sees neither);
`minimal_esp32p4.c`'s GPIO20 drive test; the CPU clock ladder; the ROM
watchdog after a flash boot, which the kernel handles but the minimal image
does not. Reclaiming the ROM's download buffers (0x4ffa96b8..0x4ffbafc0, about
72 KB) for heap is possible but unmeasured, because core 1 starts on the ROM's
CPU1 stack just above them.

## 6.1 What 47.2 found (2026-10-09)

**Core power: the DC-DC handover was never running on this board.** IDF moves
the core from the internal LDO onto the external DC-DC on every P4 boot, and
phase 34 found 360 MHz impossible without it. Here the handover lived at the
end of `esp32p4_regulator_apply_efuse_dbias()`, which returns early when the
eFuse trim would *lower* the dbias. The NANO's trim asks for 25 against a reset
value of 24, so it always ran there. The 7B's asks for 23, so on this board the
handover never happened, and nothing said so. It is now its own function,
`esp32p4_core_onto_dcdc()`, called unconditionally, with the logged result
`[PMU] core on the external DC-DC, DCM_VSET 27`. Chips **above v3.01** get
IDF's extra steps: `PMU_DCDC_FB_RES_FORCE_PD` held across the switch, then
`LP_FIB_SEL` = 0xEF (digital feedback register) and the force released. All
three were read back on the board. The LDO's XPD bit drops, so the DC-DC
carries the core.

**Clock: 100 / 200 / 400 MHz.** On v3 the CPLL runs at 400 MHz (IDF's div 10
above ECO1; the ROM leaves it at 320, as on v1.3). The divider shapes are
v1.3's scaled up: 400 = CPU÷1, MEM÷2, APB÷2 (MEM 200, APB 100), keeping IDF's
"MEM ≤ 200, APB ≤ 100" limits. `CPLL_NOMINAL_HZ` and the divider table are
selected by `CONFIG_ESP32P4_REV`. Measured on one image with `cpufreq`:
99.999 / 199.995 / 400.004 MHz. The board now **boots at 400 MHz**:
`(perft 3 1)` gives 75 depths, 0 errors, **2420 ms** (NANO at 360: 2563 ms
in phase 34, 2666 ms today). Core 1's probe (`smpstart`) counts 8.0 M per
100 ms against the NANO's 7.2 M, matching the clock ratio. Core 1 in the
kernel is not started at boot on either board; that is unchanged from phase 34.

**I2C: phantom devices, and a touch controller that went missing.**
* *Phantoms.* After a NACKed register read, the next read of an empty address
  reported success with 0xff (`TRANS_COMPLETE` without `NACK`). This is how
  boot announced a DS1307 at 0x68 and an EEPROM at 0x57 on a bus that has
  neither. Cause: the NACK interrupt fires *before* the transfer ends, because
  the controller still clocks out its STOP. The driver reset the FSM straight
  away, inside that window. Every failure captured SR with BUS_BUSY set; the
  phantom success had it clear. Fix: wait for BUS_BUSY to clear before acting
  on any result, which is what IDF's master does.
* *The GT911 vanishing.* With that fixed, `i2c scan` stopped finding the GT911
  (0x5D), and a register read after a scan failed too, although the part is
  there: `i2c rd 5d 81 40 4` → "911". Cause: the driver ran the 9-pulse bus
  clear after *every* NACK, i.e. at every empty address of a scan, and the
  GT911 does not tolerate that. IDF clears the bus only after a timeout or
  when it finds the bus busy, never for a NACK. Same policy now: a clean NACK
  gets nothing beyond the next transaction's FSM/FIFO reset (which the NANO's
  stale-controller fix needs).
* Result: boot reports "No RTC at 0x68" and "No EEPROM at 0x57", and the scan
  shows 0x18 (ES8311, chip id 0x83), 0x40 (ES7210) and 0x5D (GT911, "911")
  every time. New bring-up command: `i2c rd ADDR [REG..] N`.

**GPIO20 in the minimal image.** Not a v3 register difference (the whole
output path is identical in both headers). The test read the pad straight
after writing it; on v3.2 that is too soon. From the kernel, with time between
poke and peek, GPIO2 and GPIO20 both follow. A short settle fixed the test,
which now passes on both boards.

**Not done, deliberately:** the ROM watchdog after a *flash* boot of the
minimal image (a RAM-load tool; the kernel handles the watchdog); reclaiming
the ROM's download buffers for heap (unmeasured, see §6).

**Header sensors (owner connected a BME280, 2026-10-09).** Found at 0x76
(chip ID 0x60), sampled by `sensor_hub`, and present in phase 46's
`/proc/sensor/{fused,inferred,bme280}`: 24.7 °C, 957.5 hPa, 41 % RH, the same
pressure as the NANO's BME280 on the same desk. The HIL suite's sensor tests
now run rather than skip, and all pass, including "stable on the first
transaction" (probes 1,1,1) and readings over 9P. **LCD-7B: 25/25, and only
the Ethernet tests skip.**

## 6.2 What 47.3 found (2026-10-09)

**`drivers/psram_esp32p4.c`, one driver for both boards.** Nothing powers or
initialises the PSRAM before us: the ROM boots our image from flash without
IDF's second-stage bootloader. So the driver follows IDF's
`esp_psram_impl_enable()`:

1. **Power.** On both schematics VDDO_PSRAM (pin 72, VFB/VO2) is the only
   feed to VDD_PSRAM_0/1 (pins 59, 67), and LDO2 was off on both boards
   (register 0x40200000). It is switched on at 1.8 V from the chip's eFuse
   calibration. The two chips have different calibrations: NANO dref 10 /
   mul 3, LCD-7B dref 3 / mul 7.
2. **Clock.** MPLL at 400 MHz, with IDF's per-revision analog programming
   (`clk_ll_mpll_set_config_v1` / `_v3`, plus pmu_init's v3-only bias writes)
   behind `CONFIG_ESP32P4_REV`. The v1 path runs on the NANO, the v3 path on
   the 7B.
3. **Controller, device, cache port, MMU.** These follow IDF line by line. The
   mode registers go through the ROM's user-command routines (identical in both
   ROMs). The PSRAM MMU maps 32 MB at 0x48000000.

All addresses and fields come from a **generated** header,
`drivers/include/drivers/esp32p4_psram_regs.h`
(`tools/gen/p4_regs.py < tools/gen/p4_psram_regs.txt`). The generator
evaluates IDF's own macros for hw_ver1 *and* hw_ver3 and refuses to emit
anything if they disagree, so "the same registers on both revisions" is
checked rather than assumed.

**Results, identical on both boards:** vendor 0x0d (AP Memory), MR2 density
256 Mbit, known-good-die pass; 32 MB, bring-up 2.7 ms. The bulk zone is
8175 pages (32.7 MB) above BULK_BSS. `psram test` passes over all 30.6 MB that
were free (write, write-back, invalidate, read back, twice), with `/ram0`
intact. Bandwidth at 20 MHz: reads 32 MB/s, writes with write-back 12 MB/s.

**The phase 38 interface is now board-neutral.** `drivers/psram.h` picks the
board's driver; `kernel/main.c`, the shell, Lisp's `(psram)` and
`/proc/meminfo` test `CONFIG_PSRAM_BYTES` alone. `linker/esp32p4.ld` gained a
PSRAM region and the generated `lugalos_bulk.ld`. The P4 zeroes the bulk zone
through the cache (`uncached_delta` 0); `esp32p4_extmem_writeback/invalidate`
maintain L1D *and* L2 for DMA buffers.

**The Lisp heap in PSRAM costs nothing measurable.** A/B on one tree, with the
7B board file with and without PSRAM:

| | pools in PSRAM (20 MHz) | pools in SRAM |
|---|---|---|
| `(fib 18)` | 97–100 ms | 95–98 ms |
| allocation + GC loop | 2448 ms | 2445 ms |
| `(perft 3 1)` | 2438 ms | 2435 ms |

So the 7B now has the RP2350 terminal's pool sizes (64 K nodes, 3072
strings), 1.5 MB of BULK_BSS. New shell command: `time CMD`.

**One fault, contained but not explained.** With the 1.5 MB BULK_BSS, every
boot faulted with a *load access fault on a flash address* (the Lisp builtins
table, deterministic). This happened after `psram_init` zeroed BULK_BSS through
the cache, leaving L2 full of dirty PSRAM lines. Writing the range back
explicitly at the end of `psram_init` removes it. *(Explained in 47.4b, §6.5: not the dirty lines but the flash interface, which `spi_flash_attach()` had dropped to 10 MHz single-line, so the burst of misses that followed the zeroing hit the CPU's DBUS timeout. The write-back stays as ordinary coherence hygiene.)* Implicit evictions in
general are fine: 56 s of allocation and GC over the 1 MB node pool, with
continuous L2 eviction to PSRAM, ran clean. IDF's P4 rev-3 PSRAM workaround
(`esp_psram_p4_rev3_workaround`, dummy reads with error responses disabled)
applies to revision 3.0.0 only and is not ported. Watch for this in 47.5,
where DMA and the CPU will share PSRAM.

**Flash writes are not a hazard here.** Unlike the RP2350, where PSRAM and
flash share the QMI, the P4's PSRAM has its own controller, and
`flash_esp32p4.c` never disables the cache. PSRAM stays usable across an
erase.

## 6.3 What 47.3b found: 200 MHz through timing tuning (2026-10-09)

IDF's P4 PSRAM tuning (the DQS scheme, `tuning_scheme_impl/mspi_timing_by_dqs.c`)
is now ported in `psram_esp32p4.c`:
1. Write IDF's 128-byte reference pattern at 0x80 at 20 MHz.
2. Switch both ports to the target speed and read the pattern back under each
   of the 4 DQS phases. The best phase is the first of the longest passing run.
3. At that phase, sweep the 31 delay-line pairs, 100 reads each, and choose
   the middle of the longest run that passes 100/100.

The delay goes into every PSRAM pad's DLC field and both DQS pads'
DELAY_90/270. These fields are now in the generated register header, again
checked equal on both revisions. Mode registers and dummy cycles are set for
the target speed from the start: latency counts clocks, so the same settings
serve the 20 MHz bring-up. IDF disables the cache around its speed switches
because they also move the flash clock; this one moves only the PSRAM's,
before anything has used it. If no phase passes, or the delay-line run is a
single point, the driver falls back to 20 MHz and logs it.
`CONFIG_PSRAM_SPEED_MHZ` (20 or 200) is a board-file key; both P4 boards ask
for 200.

| | NANO (v1.3) | LCD-7B (v3.2) |
|---|---|---|
| DQS phases passing | all 4 → phase 0 | all 4 → phase 0 |
| delay lines passing 100/100 | #5–28 (24) → #16 {data 1, dqs 0} | #4–26 (23) → #15 {0, 0} |
| full-chip pattern test | PASS, 878 ms/pass (was 3.4 s) | PASS, 735 ms/pass |

The windows are wide (23–24 of 31 configs) and sit one step apart on the two
boards, which is what per-boot tuning is for.

| bandwidth (1 MB, LCD-7B) | 20 MHz | 200 MHz |
|---|---|---|
| read | 32.6 MB/s | 107.9 MB/s |
| write + write-back | 12.1 MB/s | 80.6 MB/s |

GC churn over the 1 MB Lisp node pool takes 16.6 s at 200 MHz, against 55.9 s
at 20 MHz. Boot zeroing of BULK_BSS drops from 120 to 65 ms. The display's
~37 MB/s of scan-out now uses about a third of the read bandwidth.

## 6.4 What 47.4 found: colour bars on the glass (2026-10-10)

`drivers/dsi_esp32p4.c` follows IDF's DSI driver call by call, from
`esp_ldo_acquire_channel` through `esp_lcd_new_dsi_bus` +
`mipi_dsi_hal.c`, `esp_lcd_new_panel_io_dbi` and `esp_lcd_new_panel_dpi`
(without DMA and interrupts) to `esp_lcd_dpi_panel_set_pattern`, taking the
LL layers at their v3 branches. In between, it sends Waveshare's EK79007
sequence. Register constants come from a second generated header,
`esp32p4_dsi_regs.h` (`tools/gen/p4_regs.py < tools/gen/p4_dsi_regs.txt`). The
generator now accepts `v3!` lines. Those are registers taken from hw_ver3
alone: the DSI bridge, which gained a reset bit and GRAY8 input, and the DSI
clock control, which gained the PLL reference select. The DSI host itself is
identical in hw_ver1 and hw_ver3. The panel is brought up on the first
`lcd pattern`, not at boot, so a failing link cannot stop a boot. 47.5 moves
it to boot.

| | value | where it comes from |
|---|---|---|
| LDO channel 3 | 2500 mV → dref 13, mul 3 | IDF's search over the eFuse-calibrated K 1014, Vos 3, C 999 (BLK1) |
| PHY PLL | XTAL 40 MHz × 50 / 2 = 1000 Mbps, hsfreqrange 0x2a; locks, lanes stop | v3 uses XTAL as the reference (`_DEFAULT`, not `_LEGACY` F20M) |
| DPI clock | PLL_F240M / 5 = 48 MHz (52 asked for) | host hsa 24, hbp 385, hline 3255 byte clocks; bridge hfp 56 → 26.04 µs per line on both sides, 60.4 Hz |
| panel init | 9 DCS writes in LP mode, 0 host errors | B2 10 (2 lanes), 80–86 vendor registers, 11 + 120 ms |
| bring-up time | 253 ms | mostly the reset (10 + 120 ms) and sleep-out (120 ms) waits |

Seen on the glass by the owner: vertical colour bars (`lcd pattern bars`)
and horizontal ones (`lcd pattern hbars`), both correct. Integer arithmetic
gives the same results as IDF's `roundf()` for every value above.

**Open:**
* DCS reads (`lcd id`, `lcd rd`) return 0x00. That includes a power-mode read
  (0x0A) made in command mode right after sleep-out, which should give 0x9C.
  Writes are acknowledged. Reads are not needed for the display, and nothing
  here relies on them. Waveshare's code never reads the panel either.
* The latent flash-load fault the first 47.4 image hit is found and fixed:
  §6.5.
* The owner saw the panel dim slowly to black while the board was being
  reflashed (P4 in reset, link stopped, GPIO32 floating). That is expected,
  but it means the backlight pin needs a defined level whenever the kernel
  is not driving it. That belongs to 47.11 (LEDC dimming).

## 6.5 What 47.4b found: our own `spi_flash_attach()` slowed flash 8× (2026-10-10)

**Symptom.** The first 47.4 image faulted deterministically. The first shell
command whose `strcmp` chain reached the literal "sensor" (0x40060fb8)
took an imprecise load access fault (cause 5; `epc` two instructions after
the `lbu`). A different layout hid it.

**How it was found.** By the reference method (the memory note on tracing
the reference first):
1. An IDF image for the 7B (`ref7b`: XIP from flash, PSRAM 200 MHz, L2 128
   KB) dumped the cache controller, both MSPI controllers, HP_SYSTEM and the
   MMU entries. Our kernel's `peek` dumped the same blocks, and the two were
   diffed.
2. The failing layout was rebuilt with `.text`/`.rodata` byte-for-byte in
   place, the trap reporter moved to SRAM, and a pad restoring `.rodata`'s
   address. That gave a deterministic reproducer with a diagnosis line:
   ```
   [Trap Cache] L1 fail raw 0x0 ... MSPI flash int 0x18, psram int 0x18 ...
                core timeout raw 0x10
   ```
   Bit 4 is `HP_CORE0_DBUS_TIMEOUT_INT`. The load waited longer than the HP
   CPU's DBUS timeout (`HP_SYSTEM_CORE_DBUS_TIMEOUT_REG`: enabled, 0xffff
   cycles, ~164 µs at 400 MHz), and the timeout protection answered it with
   an error response.

**Why a flash load took that long.**

| | `SPI_MEM_C_CTRL` | `_CLOCK` | read command | per 64 B line |
|---|---|---|---|---|
| ROM boot (our header: QIO) | 0x012c200c | XTAL/2 = 20 MHz | 0xEB, QIO | |
| after `flash_p4_init()`'s `spi_flash_attach()` | 0x00200000 | /4 = 10 MHz | 0x03, 1 line | ~55 µs (measured) |
| IDF app | 0x012c200c | 80 MHz (SPLL/6, undivided) | 0xEB, QIO | |
| 47.4b | 0x012c200c | 80 MHz | 0xEB, QIO | ~2.3 µs |

`flash_p4_init()` called the ROM's `spi_flash_attach()`, a leftover from
phase 27's `esptool load-ram` boot. It ran while the kernel was executing
from flash, and it reset the live interface to plain single-line READ at
10 MHz. A burst of cache misses (the shell's literal chain, or 47.3's
builtins table right after BULK_BSS zeroing) then outlasted the timeout.
The control run settles it: the failing layout without the `attach()` call
doesn't fault either.

**Fix**, as IDF does it:
* `flash_esp32p4.c` no longer calls `attach()`.
* The stage-2 stub (`xip_esp32p4.c`, `flash_clock_80m()`) moves the MSPI
  core clock to SPLL/6 = 80 MHz (`bootloader_init_mspi_clock()`) and sets
  divider 1 for SPI0 and SPI1 with the ROM's `config_clk`. It keeps the
  ROM's read mode, QIO.

`SPI_MEM_C` now matches IDF register for register, except CS setup/hold
(ours 1/2 cycles, IDF 0). `psram evict` (a new stress test: dirty 256 KB of
PSRAM, then stream the whole flash image, every fill evicting a dirty line)
dropped from 481 ms to 20 ms per pass. The same fix runs on the NANO (same
ROM jump table).

**Also learned.**
* The `--only os` boot that faulted in `sensor_hub_init` is a separate,
  explained effect: the stage-2 image carries `.data`'s initial values, so a
  new OS image under an old boot image reads stale `.data`. Always flash
  `boot,os` together.
* `clk_esp32p4.c` said IDF never programs `FLASH_CLK_SRC_SEL`. Wrong:
  `bootloader_init_mspi_clock()` does. Corrected.
* The trap dump now carries a `[Trap Cache]` line on access faults: L1/L2
  fail status, both MSPI controllers' interrupt status, and the core bus
  timeout status. The first version compared the raw `mcause` (CLIC bits
  included) and never printed. It now uses the cause code.
* Differences from IDF that remain and are deliberate: no preload strategy
  (`undef_op`), and no cache/PSRAM fail interrupts enabled.

## 6.6 What 47.5 found: a PSRAM frame on the glass at 60 Hz (2026-10-10)

`drivers/dsi_esp32p4.c` gained IDF's DPI scan-out with one frame buffer:
* **The frame:** 600 KB GRAY8 (or 1.2 MB RGB565) from the PSRAM bulk zone.
* **The transfer:** a single DW-GDMA link-list item carries the whole frame
  from memory (master port 1, increment, 64-bit, burst 512) to the bridge
  window `MIPI_DSI_BRG_MEM_BASE` (master port 0, fixed, burst 256), with the
  bridge's hardware handshake and the DMA as flow controller.
* **Per frame:** the item is marked last, and the transfer-done interrupt
  (matrix source 24 → CLIC 18, `ESP32P4_CLIC_IRQ_GDMA`) re-arms it each
  frame, as IDF's `mipi_dsi_dma_trans_done_cb()` does.
* **The item:** it lives in L2MEM and is written only through the non-cached
  alias (+0x40000000). Its `.bss` line is written back and invalidated once
  first, as IDF's `esp_cache_msync(C2M | INVALIDATE)` does.
* **Registers:** GDMA constants come from the generated header (identical in
  hw_ver1/3). IDF's `dw_gdma_reg.h` uses `DR_REG_DMAC_BASE`, which no IDF
  header defines; the generator aliases it to `DR_REG_GDMA_BASE` (IDF itself
  uses the linker symbol `DW_GDMA = 0x50081000`).

| measured on the 7B | GRAY8 | RGB565 |
|---|---|---|
| frame rate (frame-done interrupts / s) | 60.0–61.0 Hz | 61.0 Hz |
| DMA errors, bridge underruns | 0, none | 0, none |
| `lcd verify` (write back, invalidate, read the chip, compare 614,400 px) | PASS | PASS |
| full-frame memset | 7.2 ms (85 MB/s) | — (per-pixel loop) |
| write-back of the whole frame | 0.44 ms | 0.50 ms |

Seen on the glass by the owner:
* **Grid:** the 1-px grid is sharp, and its border lines at x 0/1023 and
  y 0/599 sit at the very edge. This panel has no bezel column (the RP2350
  panel had one).
* **Text page:** crisp, with white and light-gray rows and a mid-gray rule.
* **Gray ramp:** black to white, left to right.
* **Coherence test** (`lcd wbtest`): a black box drawn without write-back
  showed only scattered lines, the ones the caches happened to evict, and
  came out complete after the write-back. §4's rule holds both ways.

**Found on the way:**
* **Font bit order.** `font8x16` glyph rows are **LSB-first**: bit 0 is
  the leftmost pixel, the order the RP2350's PIO scans `canvas1_t` bytes in.
  The first text page read them MSB-first and mirrored every letter. 47.7's
  1-bpp → GRAY8 conversion must use the same order.
* **Restart must happen at a frame boundary.** Switching format live
  (GRAY8 → RGB565) by stopping the DMA mid-frame left the rest of the old
  stream in the bridge FIFO, and every later frame came out shifted. The
  owner saw the rightmost 2–3 pixels wrapped to the left edge. The bridge
  knows no frame start other than "the next byte". RGB565 straight after
  boot was clean, which ruled out the format itself. Now `fb_start()` lets
  the ISR finish the running frame without re-arming, waits for the bridge
  FIFO (`RAW_BUF_DEPTH`) to drain, and only then stops the DPI stream. Five
  live switches in a row stayed clean.
* **The panel stays dark between bring-up and `lcd fb`.** The kernel brings
  the panel up on first use; 47.7 moves the frame start to boot.

API for 47.7: `dsi_lcd_fb()` (the frame, stride 1024 px) and
`dsi_lcd_fb_flush(y0, y1)` (write back rows).

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
