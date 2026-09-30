# Phase 36 — A terminal you can sit in front of: the RP2350-LCD-7 persona

**Status: in progress — 36.0 to 36.3 done 2026-09-30, 36.4 next. Written 2026-09-30, revised the same day**, from the board's
schematic (`~/Source/gith/pico/datasheet/RP2350-Touch-LCD-7.pdf`), the ST7262
and RP2350 datasheets in the same directory, and Waveshare's demo tree
(`~/Source/gith/pico/RP2350-Touch-LCD-7-Demo`). Nothing in this document has
run on the board yet. Every pin below was read off the schematic's pin table,
not taken from the demo's headers, and §1.5 lists the places where those two
sources disagree.

**The objective.** `plan/raw_ideas.md` lists, under application scenarios, a
*"Stand-alone 'workstation' with keyboard, display, and P9 connectivity"*.
This phase builds it on the Waveshare RP2350-LCD-7 (the non-touch version):
an RP2350B with a 7" 800×480 RGB panel on the same board. The panel is the
terminal screen, a USB keyboard is the input, and later a UART link carries
9P to the other nodes. The first half, which is the subject of this phase in
detail, is the **screen and the keyboard**. Networking gets a sketch in §7 and
its own milestones once the terminal works.

**It is also a stand-alone LugalOS machine**, not only a terminal onto other
nodes (added 2026-09-30). Three uses are in scope, each with a baseline that
works on the terminal alone and an extension that uses the screen as a
screen:

| Use | Baseline | Extension (this phase) |
|---|---|---|
| **Lisp programming** | the REPL and the `e` editor on the terminal | **graphics primitives from Lisp**: the existing `canvas-*` names, on the 1-bpp screen (§4.6) |
| **Chess** | `chess-console` on the terminal, single core | **a graphical black-and-white board** beside the terminal (§4.7) |
| **Writing** | — | **a writer app**: a full-screen text editor that saves to the SD card (§4.8) |

The SD card (inserted) therefore moves out of "later" and into this phase's
early milestones (36.2).

**Settled with the owner, 2026-09-30:** the keyboard is on the **PIO-USB**
port; ACM0 is on the board's native USB port; **US layout only**; the flash
may be erased and used for LugalOS, and flashing starts **as early as
possible**, in 36.0.

**New preset `rp2350-terminal`, new board file
`cmake/board-rp2350-terminal.cmake`.** Same arch, same linker script, same
Hazard3 cores as every other RP2350 persona. The chip is the **QFN-80
RP2350B**, though, and three things follow from that which no persona has
needed before: GPIOs above 29, a PIO `GPIOBASE` of 16, and 16 MB of flash.

**Milestone scheme: `36.0`, `36.1`, …**, phase 34's scheme.

---

## 0. What is being built, in one picture

```
   USB-C J7 (PIO-USB, GP42/43) ── keyboard
          │  NRZI bits
   PIO0 (TX) + PIO1 (RX/EOP)  ◄── core 1: USB host engine (SRAM-resident, no IRQs)
          │  8-byte boot reports, via an SPSC ring
   "kbd" U-mode task (core 0): enumeration state machine, HID → keymap → bytes/VT sequences
          │
   console input sources: uart · usb_cdc · kbd          (third source → extract, §4.4)
          │
   lsh / Lisp / ed  ──►  console_putc()  ──►  "lcd" console device
                                                  │ batched writes (chan_call)
                                   "lcdterm" U-mode task: VT subset parser, 8×16 glyph blit
                                                  │ writes
                                   48 KB 1-bpp framebuffer in SRAM
                                                  │ DMA, chained, self-rearming, 0 CPU/frame
                                   PIO2: timing + PCLK + 1-bpp → RGB565 expansion
                                                  │ 16 data + DE/HS/VS/PCLK on GP20–39
                                   ST7262 panel, 800×480
```

USB-C **J3** (native USB) keeps doing what it does on every other RP2350
persona: flashing, and the `/dev/ttyACM0` console plus the ACM1 9P link. The
screen and keyboard are **added** to the session. They do not replace it,
which keeps `tests/hw/` working unchanged throughout (§4.5).

---

## 1. The board, as the schematic states it

### 1.1 Pin table (schematic page 1, lower-left table, read 2026-09-30)

| GPIO | Function on this board | Used in this phase |
|---|---|---|
| 0 | **PSRAM /CS** (`QSPI_SS2`, XIP_CS1), 10 K pull-up R21 | **must never be driven**, §1.3 |
| 1 | XL2515 CAN INT | no |
| 2–5 | XL2515 CAN SPI (SCLK, MOSI, MISO, CS), SPI0 | no |
| 6, 7 | I2C1 SDA/SCL: touch controller (not fitted) + header H6 via level shifter | no |
| 8, 9 | **UART1 TX/RX, hard-wired to the SP3485 RS485 transceiver** | later, §7 |
| 10–15 | microSD, SDIO (CLK, CMD, D0–D3) | **yes**, SPI mode on SPI1 (§4.9) |
| 16, 17 | **UART0 TX/RX**, header H7 (3V3, GND, RXD0, TXD0) | debug console |
| 18, 19 | touch INT/RST (not fitted) | no |
| 20 | LCD DE | yes |
| 21 | LCD VSYNC | yes |
| 22 | LCD HSYNC | yes |
| 23 | LCD PCLK | yes |
| 24–28 | LCD B3–B7 | yes |
| 29–34 | LCD G2–G7 | yes |
| 35–39 | LCD R3–R7 | yes |
| 40 | battery ADC | no |
| 41 | LCD RST | yes |
| 42, 43 | **PIO_USB D+ / D−** → USB-C J7 | yes |
| 44 | LCD backlight (PWM10 A, per the RP2350 datasheet function table) | yes |
| 45 | LCD EN | yes |
| 46 | XL2515 RST | no |
| 47 | "AD" (sensor header) | no |

GP24–39 carry **RGB565 in native order**: bits 0–4 blue, 5–10 green, 11–15
red, each channel's low bits dropped by the panel wiring (B3 not B0, and so
on). So a 16-bit `out pins, 16` of an ordinary RGB565 word is correct as it
stands, with no swizzle.

### 1.2 The two USB-C connectors

The Type-C block of the schematic shows two receptacles:

* **J3**: `USB_P`/`USB_N`, the RP2350's own USB PHY. VBUS comes *in* here and
  passes through an ideal-diode arrangement (Q1, AO3401) to power the board.
  This is the port every other persona uses for the CDC console.
* **J7**: `PIO_USB_P`/`PIO_USB_N`, i.e. **GP42/GP43 through 27 Ω**. Its VBUS
  is *supplied* from the board's 5 V through Q4, which makes J7 a **host**
  port. It is a bit-banged USB port, driven by PIO, with no USB controller
  behind it.

**Confirmed by the owner:** the keyboard is on the PIO-USB port (J7), and
ACM0 is on the native one (J3). That matches the board's design intent (J7
sources VBUS; J3 sinks it) and Waveshare's own
`pio-usb/host_hid_to_device_cdc` example (native = device, PIO = host). 36.0
still reads the bus lines, but now to learn the keyboard's **speed** (low or
full), which decides how much timing margin 36.8 has.

### 1.3 GP0 is the PSRAM chip select, and the default board file drives it

Every existing RP2350 board file sets `CONFIG_UART0_TX_GPIO 0`. On this board
GP0 is the /CS of the (fitted, 2 MB) PSRAM, which shares `QSPI_SD0–3` and
`QSPI_SCLK` with the flash that `.text` executes from. A UART TX idles high,
which is harmless. Each start bit drives it low, though, and that **selects
the PSRAM in the middle of an XIP fetch from flash**. Both chips then drive
the shared data lines, and the likely symptom is the board fetching garbage
instructions whenever the console prints. It would look like a random crash
somewhere unrelated.

R21 (10 K to 3V3) holds /CS deasserted against the pad's default ~50 K
pull-down (≈2.75 V, a valid high). The rule is therefore simple: **nothing in
this persona touches GP0 until PSRAM support deliberately configures it as
`XIP_CS1`.** The board file moves UART0 to GP16/17 and says why, in the style
of the existing board files' pin comments.

### 1.4 What is not on the board

* **No user LED on a GPIO.** Led1/Led2 are power/charge indicators.
  `drivers/uart_rp2350.c` drives `CONFIG_LED_EXT_GPIO` unconditionally
  (`cmake/board-rp2350-gateway.cmake:47` says so), so this persona needs "no
  heartbeat LED" to be a legal board fact (36.0).
* **No RTC.** The PCF85063 datasheet in the directory belongs to something
  else; this schematic has no RTC. The clock comes from NTP once networking
  exists, and from the monotonic counter until then, the same as
  `rp2350-sensor`.
* **No touch controller** on the non-touch version. GP6/7 (I2C1) are still
  present on header H6.

### 1.5 Where the demo, the datasheet and the schematic disagree

| Topic | Source A | Source B | Consequence |
|---|---|---|---|
| Panel DCLK | ST7262 datasheet §7.3.4: **23–27 MHz**, HBP/HFP 4–48, VBP/VFP 4–12, Th 808–896, Tv 488–504 | Demo: **16 MHz**, and porches of 32 lines vertically, which is outside the VBP/VFP maximum | The panel tolerates at least the demo's timing. That is one empirical point, and nobody knows where the edge is. 36.3 starts from the demo's timing (known to work) and moves toward the datasheet's typical values, measuring as it goes. |
| Buzzer | Demo `bsp_buzzer.h`: GP41 | Schematic: GP41 = LCD_RST | The demo header is wrong or refers to another board revision. Ignore it. |
| System clock | Demo: `set_cpu_clock(240)` for the LCD demo, `120 MHz` for PIO-USB | LugalOS: 150 MHz, fixed in `boot_header.S` | §3.3 |

Provenance rule for this phase, the same one `drivers/README.md` sets: **the
schematic and the RP2350 datasheet are provenance, and the demo is a
hypothesis.** The demo is still valuable because it proves that *something*
works: a panel lit at 16 MHz, and PIO-USB enumerating a keyboard on GP42/43.

---

## 2. Memory: why the demo's framebuffer is not an option

An RGB565 frame is 800 × 480 × 2 = **768 000 bytes**. The RP2350 has 520 KB
of SRAM, and on this kernel `CONFIG_PALLOC_MAX_PAGES` is 128 × 4 KB, with the
chess persona's idle free-page baseline at about 55 pages.

The demo solves this with PSRAM: two full RGB565 frames in PSRAM, and a DMA
completion ISR that **copies 80-line chunks from PSRAM into SRAM bounce
buffers with the CPU**, at a 240 MHz overclock. Every one of those choices is
wrong for this kernel:

* the CPU copy runs in an ISR, continuously, at a deadline. In LugalOS that
  is either an M-mode ISR stealing a large fraction of a core, or it cannot
  be a U-mode driver at all;
* 240 MHz is an overclock this project has never validated, and every timing
  literal in the tree assumes 150 (§3.3);
* PSRAM bring-up (QMI M1 timing, the /CS pin, the QPI enter sequence) becomes
  a prerequisite for putting a single character on the screen.

**What a terminal actually needs is text, and text is one bit per pixel.**

| Format | Size | Pages | Verdict |
|---|---|---|---|
| RGB565 | 768 000 B | 188 | does not fit; PSRAM only |
| 4 bpp (16 colours) | 192 000 B | 47 | fits on paper, and leaves the kernel starved |
| 2 bpp (4 colours) | 96 000 B | 24 | possible later (§3.1) |
| **1 bpp (fg/bg)** | **48 000 B** | **12** (11.7) | **this phase** |

**Decision: a 1-bpp framebuffer in SRAM. PIO expands each bit to a 16-bit
RGB565 foreground or background colour on the way out.** DMA feeds PIO from
the framebuffer with a self-rearming chained pair, so steady-state refresh
costs **zero CPU**. That leaves core 0 free, a property this project has
defended in every driver-as-task conversion since phase 12.

The colour pair is a runtime setting: two words the PIO program holds in
registers, loaded with `pio_sm_exec`-style forced instructions when changed.
So "amber on black" is a command, not a rebuild.

Glyph rendering into 1 bpp with an 8-pixel-wide font is **byte-aligned**: one
character is 16 byte stores and nothing else. Scrolling one text row is a
48 KB `memmove`. That is roughly 12 000 word copies, on the order of 100 µs
at 144 MHz, done per newline and not per frame. It may tear for one frame;
§6 records the ring-buffer alternative if that is visible.

**PSRAM is not on this phase's critical path at all.** It becomes the route to
colour (4 bpp or RGB565 frames with a CPU-free path still to be designed),
and to a large RAM disk (`raw_ideas.md`: *"PSRAM support — maybe first as RAM
disk only?"*). That is §7.

---

## 3. The three design decisions

### 3.1 Display pipeline: fitting in one PIO block

RP2350 has three PIO blocks with 32 instruction slots and 4 state machines
each. The demo's LCD programs (`libraries/bsp/pio_rgb.pio`: `hsync`, `vsync`,
`rgb`, `rgb_de`) need **about 47 instructions across two blocks**. The
PIO-USB host needs **two blocks** in the reference implementation: TX
(`usb_tx_fs`, 22 instructions) in one, and RX (`usb_nrzi_decoder` 15 +
`usb_edge_detector` 17 = exactly 32) filling another.

So **the LCD gets one PIO block, PIO2, and 32 instructions**, plus whatever
PIO0 has left after USB TX (~10). That constraint drives the design, and it
rules out a port of the demo's programs as they stand.

The shape that fits is the one scan-out engines usually take: **timing as
data, not as code**.

* **Timing SM** (PIO2 SM0, ~6 instructions): a generic "emit these sync
  levels for N PCLKs" loop. A DMA channel feeds it a **static per-frame
  command list** in SRAM: one word per segment (front porch, sync, back
  porch, active) for each line kind, re-armed by a second chained channel. It
  drives HSYNC/VSYNC/DE with `out pins` and raises an IRQ flag at the start of
  each active span.
* **Pixel SM** (PIO2 SM1, ~8–12 instructions): drives PCLK by side-set,
  waits on the timing SM's IRQ flag at the start of each line, then per pixel
  shifts one bit (`out x, 1`, autopull 32) and writes either the foreground
  or the background register to the 16 data pins. It is fed by the
  framebuffer DMA pair: 25 words per line, 12 000 per frame.

That is ~20 instructions, which leaves room for experiments. The exact
programs are 36.3's and 36.4's work, not this document's. What this document
fixes is the **budget** (≤ 32 in PIO2) and the **cycle budget per pixel**:

| clk_sys | PCLK | cycles/pixel | frame (Th 816, Tv 496) | status |
|---|---|---|---|---|
| 144 MHz | 24 MHz | 6 | 59.3 Hz | datasheet-typical; the target |
| 144 MHz | 18 MHz | 8 | 44.5 Hz | out of spec but above the demo's 16 MHz, which is known to work; the fallback if 6 cycles cannot hold both branches |

**Data must be stable on PCLK's rising edge** (DCLKPOL strap assumed
positive; 36.3 confirms by looking at the screen, because a wrong edge shows
as a one-pixel smear on every vertical line).

**GPIOBASE.** GP20–45 lie outside PIO's default 0–31 window. All three PIO
blocks run with `GPIOBASE = 16` (RP2350 datasheet, PIO register `0x168`), so
that PIO pin *n* is GPIO *n+16*: the LCD's GP20–39 and USB's GP42/43 are
both inside 16–47. Nothing else on this persona uses PIO. **cyw43's PIO0 use
is on RP2350W boards only and is not built here.**

**DMA budget:** 2 channels for pixels (data + re-arm), 2 for the timing list,
1 for USB TX, out of 16. No DMA IRQ in steady state.

### 3.2 Keyboard: PIO-USB host on J7, with its engine on core 1

A PIO USB host is **timing-critical in the CPU, not only in the PIO**. The
host must answer a device's DATA packet with ACK within the bus turnaround
window, roughly 16–18 bit times (~1.5 µs at full speed, ~216 cycles at
144 MHz). The reference implementation (sekigon-gonnoc's `pio_usb.c`) meets
that by spinning with interrupts effectively owned, from RAM, at `-O3`. It
also has to emit a SOF (full speed) or keep-alive EOP (low speed) **every
millisecond, forever**, or the device suspends after 3 ms.

The choices:

| | A: timer ISR on core 0 | **B: core 1 as a USB engine** | C: keyboard on native USB (J3) |
|---|---|---|---|
| Cost to core 0 | ~2–10 % in 1 ms bursts, interrupts off during transactions | none | none |
| Latency imposed on every other ISR | up to ~100 µs (a 64-byte descriptor read at FS) | none | none |
| Flash erase (`drivers/flash_rp2350.c` turns XIP off) | ISR must be RAM-resident *and* survive XIP-off | engine RAM-resident and **exempt from the park** (§3.2.1) | no issue |
| Keeps CDC console on J3 | yes | yes | **no**: console moves to UART0/H7 |
| Uses the second core for SMP work | yes | **no** | yes |
| PIO blocks consumed | 2 | 2 | 0 |
| Complexity | PIO-USB from scratch | PIO-USB from scratch | a host-mode arm of the existing USB controller knowledge in `drivers/usb_cdc.c` |

**Recommendation: B.** A terminal node has no use for Lazy-SMP chess. Its
second core is idle, and the one thing PIO-USB needs is a core that is never
interrupted. `kernel/smp.c` already dispatches core 1 on a mode set by core 0
before the handshake (`g_core1_mode`: probe, locktest, join, stage 1–3), so
**`CORE1_MODE_USBHOST`** is one more arm of an existing mechanism, not a new
one.

**Split of responsibilities**, so that the timing-critical part is as small as
possible:

* **Core 1, the engine (M-mode, `.ramfunc`, no interrupts, polling
  TIMER1):** SOF/keep-alive every 1 ms, and executing one *transaction
  descriptor* at a time: {token, address, endpoint, DATA0/1, buffer, length}
  → {ACK/NAK/STALL/timeout, bytes}. It knows nothing about HID, descriptors
  or enumeration.
* **Core 0, the `kbd` U-mode task:** reset/attach detection results,
  enumeration (GET_DESCRIPTOR device → SET_ADDRESS → GET_DESCRIPTOR config →
  SET_CONFIGURATION → HID SET_PROTOCOL(boot) → SET_IDLE(0)), interrupt-IN
  polling at the endpoint's `bInterval`, report diffing, typematic repeat,
  keymap. It talks to the engine through **two lock-free SPSC rings** in a
  shared region (requests down, completions up), the same primitive the
  stream receive buffer already uses (README, "a lock-free SPSC ring").

**Boot protocol only.** An 8-byte report, 6-key rollover, and no HID report
descriptor parser. Every USB keyboard is required to support it. Hubs are out
of scope: the keyboard goes directly into J7.

**From scratch, not ported.** LugalOS carries no TinyUSB or Pico SDK runtime
(README: the CDC stack was *"written from scratch against the hardware"*).
The PIO *programs* are the one place where reading the reference
implementation (MIT-licensed, like this tree) is worth a great deal, because
the NRZI decode and EOP detection are subtle. If any program is derived from
it, the file says so and carries the notice. The C protocol engine is written
here.

**Option C stays only as a fallback** (§6): the keyboard is physically on
J7, so choosing it would mean re-cabling, plus moving the console to UART0 on
H7. The `kbd` task above would stay the same, and only the engine underneath
it would change.

#### 3.2.1 The flash-park contract

`drivers/flash_rp2350.c:143` asks core 1 to park before an erase turns XIP
off, and refuses the write if it does not. A parked engine stops sending SOFs,
a 4 KB erase takes tens of milliseconds, and the keyboard suspends. Instead,
**the engine is written so that it never touches flash**: its code in
`.ramfunc`, its data and stack in SRAM, and its constants copied to SRAM. It
**acknowledges the park request immediately without stopping**.
`smp_flash_park_request()` gains a notion of "this core's current mode is
XIP-free". A test deliberately writes `/flash0` while typing.

### 3.3 The clock: 144 MHz for this persona, and one constant instead of six

PIO-USB wants clk_sys to be a **multiple of 12 MHz**. Full-speed TX runs at an
integer 48 MHz-derived rate, and RX oversamples. At 150 MHz the TX divider is
3.125, which is fractional, and fractional PIO dividers dither the bit edges
by a whole cycle (6.7 ns against an 83 ns full-speed bit). **144 MHz** (VCO
1440 = 12 × 120, post-dividers 5 and 2, the same post-dividers
`boot_header.S` already uses) is 12 × 12, and it also gives the panel an
integer **24 MHz PCLK at 6 cycles**.

The catch: `150000000` is a literal in six drivers (`uart1_link_rp2350.c:163`,
`gps_pps_rp2350.c:376`, `i2c_bus.c:164`, `spisd_rp2350.c`,
`st7735_rp2350.c`, `pico_clock_green_rp2350.c`, plus the enc28j60 and uart
comments). The fix is a seam that should have existed already:
**`CONFIG_CLK_SYS_HZ`**, a board-file fact defaulting to 150 000 000.
`boot_header.S` derives FBDIV from it (as an assemble-time check, not a
runtime computation), and every literal becomes the constant. Other personas'
images must come out **byte-identical** (sizecheck +0), which is the proof
that the refactor changed nothing it was not meant to.

---

## 4. Software shape

### 4.1 Drivers and tasks

| Piece | File | Category (`drivers/README.md`) | Runs as |
|---|---|---|---|
| Panel power, reset, backlight PWM, PIO programs, DMA setup | `drivers/lcd7_rp2350.c` | A (registers) + C | init in kernel at boot; no steady-state code |
| Terminal emulator: VT subset, cursor, scroll, glyph blit | `drivers/vtterm.c` (portable) | D (a console implementation) | `lcdterm` U-mode task, PMP-confined to the framebuffer + its state |
| 8×16 font | `drivers/font8x16.c`, generated | data | `.rodata` |
| PIO-USB engine | `drivers/piousb_rp2350.c` | A | core 1, `CORE1_MODE_USBHOST` |
| HID boot keyboard + US keymap | `drivers/usbkbd.c` (portable above the transaction interface) | C | `kbd` U-mode task |
| 1-bpp canvas: pixel, line, rect, fill, circle, mono bitmap, text-at | `drivers/canvas1.c` (portable) | library over the framebuffer | called by `lcdterm` on behalf of Lisp and chess (§4.6) |
| SD card | `drivers/spisd_rp2350.c`, **unchanged**, CS on GP15 | existing | existing `blk` task |
| Writer | `user/writer/` (or an `e` mode, decided at 36.12) | application | foreground program |

`vtterm.c` and the HID→bytes half of `usbkbd.c` have **no RP2350 content**.
That is deliberate, and it is what makes them testable on QEMU (§4.5).

### 4.2 The font

**Spleen 8×16** (BSD 2-clause, compatible with this tree's MIT licence),
converted from its BDF by a small host script in `tools/` and committed as
a generated C table with the conversion command in its header. ASCII
0x20–0x7E, plus a handful of line-drawing glyphs if the editors want them.
The bit order is fixed **once, in the generator**, to match the pixel SM's
shift direction: shift-right, so byte 0 bit 0 is the leftmost pixel of the
first cell and the font is stored bit-reversed. Nothing at runtime ever swaps
bits.

8×16 cells give a **100 × 30** terminal.

Not this phase: UTF-8, Latin-1 glyphs, a second font size.

### 4.3 The terminal emulator: exactly what this tree emits

Measured, not guessed. A grep of `kernel/`, `user/`, `fs/`, `net/` and
`drivers/` for escape sequences finds this set, and it is the **required**
subset:

| Sequence | Where | Meaning |
|---|---|---|
| `\r` `\n` `\b` `\t` | everywhere | CR, LF (with scroll), BS, tab to 8 |
| `ESC[K` | `line_editor.c` redraw | erase to end of line |
| `ESC[J`, `ESC[2J`, `ESC[H` | `console.c`, `shell.c` | erase below / screen, home |
| `ESC[A/B/C/D` (with optional count) | line editor, editors | cursor up/down/right/left |
| `ESC[?25l` / `ESC[?25h` | line editor | hide/show cursor |
| `ESC[0m`, `ESC[1;32m`, `ESC[1;33m`, `ESC[1;36m` | shell, console | SGR: reset, bold+colour |
| `ESC[48;2;r;g;bm` | `user/chess/src/position.c` | truecolour background (chess board) |

Plus `ESC[row;colH` (CUP), because any full-screen editor will want it and it
costs a dozen lines.

**SGR on a 1-bpp screen:** colours are parsed and **ignored**, bold maps to
nothing, and `7` (reverse) is honoured, because a 1-bpp screen can do
reverse video exactly. An unknown sequence is consumed silently and counted
in `/proc`, never printed. Printing it is how a terminal fills with `[1;36m`.

**Pending-wrap semantics (the VT100 "last column" rule)**: writing column 100
leaves the cursor *on* column 100 with a wrap pending, and the wrap happens
on the next printable character. Without this, the line editor's
`\r` + prompt + buffer + `ESC[K` redraw of a line exactly 100 characters long
scrolls the screen by one each keystroke. This is the most common bug in
small terminal emulators, so it is named here in advance.

**The cursor** is an XOR'd underline or block in the framebuffer, blinked from
the task's own timer. The emulator removes it before any glyph write and
restores it after, so it never corrupts a cell.

**A cell shadow (100 × 30 × 2 bytes: character + attribute, 6 KB)** is kept
beside the framebuffer. The framebuffer alone is enough for a pure terminal,
but graphics (§4.6) draw into the same pixels. With the shadow, the terminal
can redraw itself after a program has drawn over it, and the `full`/`split`
window switch below costs nothing but a repaint.

**The text window is a rectangle, not the whole screen.** By default it is the
full 800 × 480. A program can shrink it (for example to the right-hand 50
columns while the chess board occupies the left, §4.7). Scrolling, erase and
cursor addressing then apply inside the window, and the pixels outside it
belong to the canvas. This is the one mechanism behind the Lisp and chess
extensions, which is why it lives in `vtterm.c` and not in either consumer.

### 4.4 Console integration: a third input source, and the rule says extract

`uart_getc()` (`drivers/uart_rp2350.c:1140`) polls **two** sources today, the
physical UART and USB CDC, open-coded in two places (`uart_has_char()` and
`uart_getc()`). The keyboard is the **third**. `drivers/README.md`: *"Extract
at the third implementation, never at the second."* So 36.9 does not add a
third `if`. It introduces a small registered list of console input sources
({`has_char`, `getc`}), with the existing two converted onto it unchanged,
and the keyboard added as an entry. `console_interrupt_requested()`'s Ctrl-C
latch sees all three for free, because it already funnels through
`uart_has_char()`/`uart_getc()`.

**Output:** a new `DEV_KIND_CONSOLE` device **`lcd`** in `kernel/board.c`,
registered only on this persona. Its `putc` batches into the `lcdterm` task
exactly as the `uart` device batches into the uart task: single characters
batch, and `console_flush()` pushes. This matters, because a `chan_call()` per
character would make `cat` visibly slow. During this phase it **tees to the
`uart` device** (which already mirrors to ACM0), so the host harness sees
everything the screen sees. Whether the tee survives the phase is decided at
36.13, once the terminal has been used for real.

**Binding:** this persona's `init.lisp` does `console-bind "lcd"`. There is
nothing new in that; it is B4's runtime binding, which exists for exactly
this scenario.

**Keyboard → bytes:** printable keys by keymap. Ctrl-letter gives 0x01–0x1A,
which is what `line_editor.c` already acts on (Ctrl-A/E/B/F/D/K...). Enter is
`\r`, Backspace is 0x7F, Tab is `\t`, Esc is 0x1B. Arrows produce
`ESC[A..D`, Home `ESC[1~`, End `ESC[4~`, Delete `ESC[3~`: the forms
`line_editor.c:687–732` already parses. **Typematic** is done locally
(500 ms delay, ~30/s), because SET_IDLE(0) makes the keyboard report only on
change.

**Keymap: US only** (settled 2026-09-30). One table, no layout switching, no
AltGr and no dead keys, so every character a US keyboard produces is ASCII
and in the font. Shift, Ctrl and Caps Lock are the only modifiers that change
a byte. Right Alt and the GUI keys are ignored.

### 4.5 Testing: most of this is not hardware

| Layer | Where it is tested |
|---|---|
| `vtterm.c` parser + renderer | **QEMU**, `tests/runner.py`: feed byte strings, compare a hash of the RAM framebuffer and the cell/cursor state against expected values. The pending-wrap case, every sequence in §4.3's table, scroll, and unknown-sequence swallowing. |
| HID report diff → key events → bytes, keymaps, typematic | **QEMU**: synthetic 8-byte reports through the same function the task calls. |
| Transaction-level USB (CRC5/CRC16, NRZI+bit-stuff encoder) | **QEMU**: known vectors from the USB 2.0 spec. |
| Panel timing, PIO programs, DMA | the board only. `lcdtest` patterns (below) are judged by eye, **and** the framebuffer/DMA state is exposed in `/proc/lcd` so `tests/hw` can assert "DMA is re-arming, frame counter advancing, underrun counter zero" without a camera. |
| USB engine and enumeration | the board, with a real keyboard. The engine keeps counters (SOFs sent, transactions, NAK/timeout/CRC errors, enumerations) in `/proc/usbhost`. A test cannot press keys, but it can assert that a keyboard is enumerated with its VID:PID and that SOFs advance at 1000/s ± 1 %. |
| Canvas primitives (line, circle, bitmap, window switching) | **QEMU**, the same RAM-framebuffer hash method as `vtterm.c`. |
| Writer's wrap/cursor model and atomic save | **QEMU** (wrap/cursor) and the QEMU VirtIO SD image (save, rename, reopen). |
| Console integration | `tests/hw/test_terminal.py`: everything `test_rp2350.py` does over ACM0 still passes with `lcd` bound, since the tee keeps the host's view identical. |

**Keystrokes themselves cannot be automated without a USB HID emulator** on
the other end of J7. That is the one manual check in the phase, and 36.9
defines it as a short script a human follows.

### 4.6 Lisp graphics: the existing `canvas-*` names, on a new screen

Lisp already has canvas primitives: `canvas-fill`, `canvas-pixel`,
`canvas-rect` and `canvas-text` (`user/lisp/lisp.c:3191`), built over the
ST7735 for the chess persona and taking raw RGB565 integers. **The same names
are bound on this persona**, so a program written for one screen runs on the
other.

* **Colour on 1 bpp:** 0 is background and any non-zero value is foreground.
  That keeps `ST7735_BLACK`/`WHITE`-style constants meaningful. A luminance
  threshold was considered and rejected, because a dark-red "foreground"
  would silently disappear.
* **Added primitives** (both screens, where the ST7735 can support them):
  `canvas-line`, `canvas-circle`, `canvas-invert` (XOR a rectangle, which is
  the cheapest possible highlight), `canvas-size` (returns `(800 480)` here
  and `(128 160)` there, so programs need not hard-code a screen), and
  `canvas-bitmap` (a list of row integers, as the chess pieces are stored).
* **Sharing the screen with the terminal:** `(canvas-window 'full)`,
  `(canvas-window 'split n)` (the text window becomes the right-hand *n*
  columns) and `(canvas-window 'text)` (graphics cleared, terminal
  repainted from its shadow). While a program draws on the full screen,
  terminal output still lands in the shadow, and it appears when the window
  comes back.
* **Where the drawing happens:** the framebuffer belongs to the `lcdterm`
  task's PMP domain. Canvas calls therefore go to that task over its channel,
  the way `st7735_*` calls already go to the `st7735` task. A drawing call is
  one message, so a line is one round trip and not 800 pixels. Lisp gains no
  write access to the framebuffer.

**This is the second canvas implementation, so nothing is extracted yet**
(`drivers/README.md`, "at the second implementation, do nothing but note
it"). `lisp.c` and `chess_ui.c` gain a second `#if` arm beside the ST7735
one. The note goes in `plan/hardware_seams.md`, and a third display would
make the extraction.

### 4.7 Chess: a black-and-white board beside the terminal

Single core (`(chess)`), as settled. `CONFIG_ENABLE_SMP` stays off, because
core 1 is the USB engine.

The console chess UI (`chess-console`) already works on any terminal and
needs nothing new. Its ASCII board uses `ESC[48;2;…m` backgrounds for the
squares, which the 1-bpp terminal ignores (§4.3), so the squares lose their
colour but the pieces stay readable as letters.

**The graphical board** follows the TFT mirroring precedent
(`chess_ui.c:1433`, `draw_chess_board()`): after every move, whichever
device made it, the board is redrawn.

* **Layout:** `canvas-window 'split 50`. The board takes the left 400 × 400
  (50 px squares, with the rank/file labels below and beside it), and the
  terminal keeps the right 50 × 30 columns for the move list and commands.
* **Squares:** light squares white, dark squares a 50 % checker dither.
  Plain black dark squares would make black pieces vanish.
* **Pieces:** the existing 16 × 16 bitmaps (`chess_ui.c:1401`) scaled 3× to
  48 × 48, with a one-pixel contrasting outline generated at draw time, so
  that both colours read on both square kinds: white pieces are white-filled
  with a black outline, black pieces black-filled with a white outline. If
  the scaled art looks too blocky, redrawing the six pieces natively at 48 px
  is a data change, not a code change.
* **Input** is typed moves (`e2e4`, SAN) at the console, as now. A
  keyboard-driven square cursor is possible later and is not in this phase.

### 4.8 The writer: a full-screen editor that saves to `/sd0`

The editors today are `ed` (line editor) and `e` (the Emacs-style multi-line
Lisp box, with C-X C-S to save). Neither is a place to write prose. The
writer is a **full-screen text editor** for 100 × 30:

* **Soft word wrap.** A paragraph is one line in the file and wraps on
  screen at word boundaries. The file stays plain text that any machine
  reads.
* **Navigation:** arrows, Home/End, PgUp/PgDn, Ctrl-Home/End. Emacs-style
  Ctrl keys as far as they already exist in `line_editor.c`, so that one set
  of bindings works everywhere.
* **Save** with Ctrl-S to a path on `/sd0`, **atomically**: write
  `name.tmp`, then rename. A pulled card or a power cut then leaves the old
  file or the new one, never half of each. Ctrl-Q quits, and asks if unsaved.
* **Status line:** file name, modified flag, line/column, word count.
* **Size:** the whole document in RAM. The budget is recorded at 36.12 from
  the measured free-page baseline, with a hard limit that refuses to open a
  larger file rather than truncating it.

Whether this is a new program in `user/writer/` or a text mode of `e` is
decided at 36.12, after reading `e`'s implementation. The criterion is which
one gives soft wrap without making `e`'s Lisp-eval path more complicated.

### 4.9 SD card: the existing driver, on pins that happen to fit

The socket is wired for 4-bit SDIO (§1.1), and in SPI mode the same pins are
exactly SPI1: GP10 CLK = **SPI1 SCK**, GP11 CMD = **SPI1 TX** (the card's DI in
SPI mode), GP12 D0 = **SPI1 RX** (DO), and GP15 D3 = **chip select** (DAT3 is
CS in SPI mode) as a plain GPIO, which is how `spisd_rp2350.c` already drives
CS (`CS_MASK`, `SIO_GPIO_OUT_*`). So the board file sets `CONFIG_SPI1_*` with
`CONFIG_SPI1_CS_GPIO 15`, `LUGALOS_ENABLE_SPISD` is ON, and the driver is
unchanged. Not verified yet: that D1/D2 (GP13/14) left as inputs are harmless
in SPI mode, and that the socket has pull-ups on CMD and the data lines. 36.2
checks both.

SDIO 4-bit via PIO would be faster, and it would need a PIO block this
persona does not have spare (§3.1). SPI-mode SD is fast enough for text by
orders of magnitude.

## 4a. Host setup (checked 2026-09-30 on this machine)

| Tool | Found | Notes |
|---|---|---|
| RISC-V GCC | `riscv64-elf-gcc` 15.2.0 (Arch) | the first name `cmake/toolchain-rp2350.cmake` looks for |
| CMake, Ninja | `/usr/bin/cmake`, `/usr/bin/ninja` | `rv32-nommu` and `rp2350-chess` presets build clean |
| QEMU | `qemu-system-riscv32/64` 11.1.1 | for `tests/runner.py` |
| Python / uv | `python3`, `uv` | `tests/hw` has its own `uv` environment (pyserial, fusepy) |
| picotool | **not installed, not needed** | flashing is UF2-copy plus the 1200-baud touch (`tests/hw/flash.py`) |
| udisks2 | active | `flash.py` mounts the BOOTSEL volume itself if nothing else does |
| Serial access | **fixed 2026-09-30** | `/dev/ttyACM*` is `root:uucp 0660`, and the user was not in `uucp`. Now added. Until the next login, commands run under `newgrp uucp` |

The board was running a Pico-SDK firmware on delivery (USB `2e8a:0009`,
"Pico"). The SDK's USB stdio implements the same 1200-baud reset, so the
first flash may not even need the BOOT button.

---

## 5. Milestones

**Every milestone ends on the board.** The persona is flashed from 36.0 on,
and each milestone is done only when its build is running on the LCD-7, not
when it compiles. The flash may be erased freely (settled 2026-09-30).
`flashfs.uf2` is flashed once in 36.0, and after that only when
`tools/sd_root` changes.

### 36.0 — The persona boots on the board, and the board answers three questions

No display, no keyboard. Deliverables:

* **`rp2350-terminal` preset** + `cmake/board-rp2350-terminal.cmake`: UART0
  on **GP16/17** (§1.3, with the PSRAM reason in the comment); SPI1 on
  GP10/11/12 with **CS GP15** for the SD card (§4.9, enabled but not relied
  on until 36.2); no ST7735/TM1638, no DCF77/GPS, no SMP. `cc`, `ed` and
  **chess on**: this is a workstation. New CONFIG keys are added to
  `cmake/gen_config.cmake`'s list.
* **"No heartbeat LED" becomes legal**: `CONFIG_LED_EXT_GPIO` unset means no
  LED task, instead of a build failure or a write to some GPIO.
* **GPIO ≥ 32** in the one place 36.0 needs it: reading GP42/43. Use
  `GPIO_HI_IN` / `GPIO_HI_OUT` / `GPIO_HI_OE` (SIO, RP2350 datasheet §3.1.3),
  never `1u << pin` with pin ≥ 32. That expression is undefined behaviour in
  C and a silent wrong register in practice. Every driver in this phase that
  touches a high pin gets a `_Static_assert` on its pin constants choosing
  the bank.
* **First flash**: `lugalos.uf2` and `flashfs.uf2`, via `tests/hw/flash.py`
  (1200-baud touch), or with the BOOT button if the delivered firmware
  ignores the touch.

A **`boardprobe`** shell command (this persona only) reports:

1. **Chip revision** from SYSINFO `CHIP_ID`: A2 or A3/A4. Erratum RP2350-E9
   (input pads with pull-downs latching high) matters to a USB host, which
   depends on pull-downs on D+/D− to see detach. On A2 the design must not
   rely on the internal pull-downs.
2. **The keyboard's speed.** With GP42/43 as inputs and pull-downs on, the
   keyboard's 1.5 K pull-up wins: D+ high means full speed, D− high means
   low speed.
3. **GP0 is untouched**: IO_BANK0 FUNCSEL for GP0 still at its reset value
   (NULL), read back rather than assumed.

**Done when:** the persona builds with no diagnostics and is **running on the
board**; `lsh` answers on ACM0 (and on H7, if an adapter is attached);
`boardprobe` reports the chip revision and the keyboard's speed, and shows GP0
untouched; `/proc/meminfo`'s idle free-page count is recorded here as this
persona's baseline, since every later milestone spends from it (framebuffer
12 pages, cell shadow ~2, USB engine stack on core 1, writer document); the
other RP2350 sizecheck baselines are unchanged.

#### Done, 2026-09-30 — and the SD card came free

The persona builds with no diagnostics, and so do the six other RP2350
presets. `rp2350-chess`, `rp2350-clock` and `rp2350-gateway` are **+0** on
sizecheck. `tools/sizereport-rp2350-terminal.json` is the new baseline
(157 902 B static, 83 heap pages).

**Flashing.** The board arrived with a Pico-SDK firmware (USB `2e8a:0009`)
that did *not* honour the 1200-baud touch (the DTR ioctl timed out), so the
first flash took one BOOT-button press. Every flash since has been
hands-free through `tests/hw/flash.py`, OS and `flashfs.uf2` alike, because
LugalOS enumerates as its own dual ACM (`2e8a:000a`) and implements the
touch. `flash.py` does not recognise foreign firmware as a console, so the
bootstrap case needs the button, and `tests/hw/README.md` should say so.

`boardprobe` on the board:

```
chip:    CHIP_ID=0x30004927 part=0x0004 stepping A3 (REVISION 0x3), package QFN80 (RP2350B)
         RP2350-E9 (pull-down latch): fixed on this stepping
piousb:  GP42(D+) high 64/64, GP43(D-) high 0/64: full-speed device attached
gp0:     GPIO0_CTRL=0x0000001f FUNCSEL=31: untouched (PSRAM /CS left to its pull-up)
```

* **A3.** E9 is fixed, so the USB host may rely on pad pull-downs for
  detach. The datasheet's stepping table is worth recording because it
  surprises: **A4 reads REVISION 0x8, not 0x4.**
* **The keyboard is full speed.** That is the harder of the two cases for
  PIO-USB (§3.2's turnaround is ~1.5 µs, where low speed would have eight
  times the margin), so §6's first USB risk is the live one.
* **GP0 is at FUNCSEL NULL.** Moving UART0 to GP16/17 did what it had to.

**The SD card was already working at first boot.** The existing
`spisd_rp2350.c` mounted the inserted card at `/sd0` over SPI1 with CS on
GP15, unchanged, as §4.9 predicted. The card is fresh, so every command
currently logs `[FAT32] Device 'spisd0': no directory to create
'system/history.lisp'`. That is the history file looking for
`tools/sd_root`'s layout, not a fault. 36.2's remaining work is the checks
listed there: throughput, a power-cycle round trip, and the D1/D2 and
pull-up questions.

**Baseline for everything after this:** `/proc/meminfo` at idle shows
**70 of 83 pages free** (280 KB of 332 KB heap). The image is 155 KB
(data+bss) with chess, `cc` and `ed` built in. That is the budget the
framebuffer (12), cell shadow (~2), USB engine and writer spend from.

**Changes outside the new files:**
* `drivers/uart_rp2350.c`: no `CONFIG_LED_EXT_GPIO` now means no heartbeat
  task, no LED pad setup and an empty `LED_MASK`.
* `fs/vfs_server.c`: the `LED_EXT_GPIO` line in the board-config report is
  optional, the same as `LED_ONBOARD_GPIO` already was.
* `cmake/gen_config.cmake`: `CONFIG_PIOUSB_DP_GPIO` and `_DM_GPIO`.
* `drivers/boardprobe_rp2350.c` is guard-by-pin-map, like the UART1
  downlink.

### 36.0a — Two things a new card and a new machine found *(added 2026-09-30)*

Neither is specific to this board. Both were invisible on the machine and
card the project grew up on, and both surfaced within an hour of starting
somewhere fresh.

**A new SD card logged a warning after every command.** `[FAT32] Device
'spisd0': no directory to create 'system/history.lisp' in` appeared once at
boot and again after every line typed. There were two writers, and neither
created the directory it wrote into:

* `init.lisp` clears the history at boot with `(write-file
  "/sd0/system/history.lisp" "")`. That was the boot-time line, at 0.315 s
  right after `Loaded stdlib.lisp`, with nothing typed. The first guess, a
  floating UART0 RX on the unconnected H7 header, was wrong, and the log's
  timing is what showed it.
* `add_history()` (`kernel/line_editor.c`) appends every line. That was the
  per-command line.

The fixes are one line each, plus a latch. `init.lisp` does `(mkdir
"/sd0/system")` before the write, which is a silent no-op when the directory
exists or no card is mounted. `add_history()` checks for the directory once,
creates it if missing, and re-checks only after an append fails (a swapped
card). The history belongs to the line editor, so the line editor makes sure
its directory exists, rather than depending on `init.lisp` having run.

**Verified on the board:** the card's `/system` was removed in one Lisp
line (any shell command re-creates it through the history before it runs,
which is how the first attempt failed), then a reboot with nothing typed.
The boot log shows `Subdirectory created: 'system'` and no warning, and the
first command lands in the history.

**B12 failed on every fresh clone.** `tests/runner.py`'s "ELF Loader Rejects
Malformed Program Headers (B12)" execs `/sd0/badelf.bin`, which was meant to
be a checked-in file under `tools/sd_root/`. It never was: `.gitignore`'s
`*.bin` swallowed it, so it existed only in the working tree where it was
made. It is now generated at build time by `tools/gen_badelf.py` (a
twelve-field `struct.pack` of exactly the header the test describes),
staged beside the user programs.

**And the suite was flaky on the new machine, which turned out to be the
harness.** Once RV64 and SMP were built, six full runs produced five
different single failures, each passing on its own. Two causes:
`_strip_echo()` swallowed the line break after the echo whenever a command
was sent with a trailing `"\n"` (so `^address:` could not match text that was
in the log), and seven tests waited for one line and asserted on a later one.
Both are fixed in `tests/runner.py`, and the ~70 sites of the same shape not
yet vetted are an entry in `plan/open_issues.md`. **Four consecutive full
runs since the fix: 368/368** (RV32, RV64, multi-node, two-hart SMP;
~215 s each).

### 36.1 — `CONFIG_CLK_SYS_HZ`, and 144 MHz on this persona

§3.3. The refactor first, at 150 MHz everywhere, then the persona's value.

**Done when:** all RP2350 personas at 150 MHz are **byte-identical** to
before (sizecheck +0, and `cmp` of the images); `rp2350-terminal` boots at
144 MHz with UART0 at the right baud (readable is the proof); `clocks` or
`/proc/cpuinfo` reports clk_sys from PLL registers, not from the constant; a
perft-3 run on this persona is ~4 % slower than on the chess persona, which
confirms the clock actually moved.

#### Done, 2026-09-30 — and the board was not running on its PLL at all

`CONFIG_CLK_SYS_HZ` is a board fact (`arch/riscv/include/arch/rp2350_clocks.h`,
default 150 000 000). `boot_header.S` derives FBDIV from it, with `.if` checks
against the datasheet's limits (a multiple of 1.2 MHz, FBDIV 63..133, at most
150 MHz). The seven literals are gone: UART0 (whose `81/24` now folds from
`(4·clk)/baud`; checked in the 150 MHz chess image's disassembly, still
`li 81` / `li 24`), UART1, GPS, I2C, SPI SD (whose boot line now prints its
real pins and speed instead of "GP10-GP13 … 12.5 MHz"), and the clock
board's PWM. `rp2350-terminal` sets 144 MHz.

**The instrument, `clocks`, found two bugs on its first run, both older than
this phase.**

1. **clk_sys was on the ring oscillator.** `configure_clk_sys` set
   `CLK_SYS_CTRL.SRC = aux` and never set `AUXSRC`, which **resets to ROSC,
   not PLL_SYS**. PLL_SYS was locked and unused. On this board clk_sys was
   ~47 MHz from the bootrom's ROSC. It now moves to clk_ref, sets `AUXSRC =
   PLL_SYS`, and moves back, the datasheet's order for a source that
   "will glitch when switching".
2. **clk_ref was divided by 4.** `configure_clk_ref` selected the crystal
   and inherited the bootrom's `CLK_REF_DIV` (INT = 4 here). TIMER0's "1 µs"
   tick was 4 µs, so every delay, timestamp and uptime on this board ran at a
   quarter speed: the board's `time` advanced 5.3 s while the host counted
   21.4. It is now set to 1 explicitly.

The first measurement read **576 MHz** for a 144 MHz clock, because it
assumed the TIMER ruler was 1 MHz. `clocks` now derives TIMER0's tick from
clk_ref's own registers and scales by it. It also enables `mcycle` for its
window, because Hazard3 resets with the cycle counter inhibited
(`mcountinhibit.CY`) and nothing in this kernel had ever enabled it, so the
very first reading was 0 Hz.

After the fixes, on the board:

```
PLL_SYS:  locked, REFDIV 1, FBDIV 120, POSTDIV 5/2 -> VCO 1440 MHz
clk_sys:  SRC aux, AUXSRC PLL_SYS, DIV 1.0000
clk_ref:  SRC 2 (XOSC), DIV 1; TIMER0 tick 1000000 Hz
clk_sys:  144000000 Hz from the registers, 143993950 Hz measured (mcycle over 20000 TIMER0 ticks)
config:   CONFIG_CLK_SYS_HZ = 144000000 -- agrees with the registers
```

The board's `time` now advances 21.38 s against the host's 21.40 s.

**Perft, same board, same image, only the clock changed:** 16 285 ms at 150
MHz, 16 971 ms at 144. The ratio is 1.0421, against 150/144 = 1.0417. The
clock moves exactly as configured.

**The cold boot, and the other board, 2026-09-30.** After a power cycle,
`clocks` on the LCD-7 reads the same (PLL_SYS, ÷1, 1 MHz tick, 143 997 500 Hz
measured). The Pico 2 chess board's bootrom, read with `peek` on its *old*
firmware, had left `CLK_SYS_CTRL` = 1 (aux on PLL_SYS), `CLK_REF_DIV` ÷1 and
FBDIV 125. So that board has always run at a true 150 MHz, both fixes are
no-ops there as predicted, and only the LCD-7's bootrom leaves the other
state. The new tree on the chess board: 150 000 000 from the registers,
149 997 450 measured.

**The 2.6× perft gap is code layout, not the board.** Same `(perft 3 1)`
suite, all on real silicon:

| image | board | clk_sys | suite |
|---|---|---|---|
| chess persona, old build 575 | Pico 2 | 150 | 6 862 ms |
| chess persona, this tree | Pico 2 | 150 | 7 373 ms |
| chess persona **minus ST7735/TM1638 code** | Pico 2 | 150 | 9 588 ms |
| terminal persona | **Pico 2** | 144 | **16 970 ms** |
| terminal persona | LCD-7 | 144 | 16 971 ms |

The terminal image is as slow on the Pico 2 as on the LCD-7, and the flash
configuration is identical on both (QMI quad continuous read, CLKDIV 3). Just
removing two drivers the search never calls costs the chess preset 30 %.
The engine runs in place from flash through the 16 KB XIP cache, and its
speed depends on where the linker happens to put its hot code and tables. That
is filed in `plan/open_issues.md` with the options. The obvious one, the hot
~21 KB of search code plus ~10 KB of tables in SRAM, costs about 8 heap
pages on every chess persona, so it is a decision rather than a fix. It
matters for 36.11 (chess on this board) and is not a blocker before then.

**Checks:** every RP2350 preset and `esp32p4` build. The only diagnostic is
an objcopy warning on `esp32p4` (*empty loadable segment at 0x40000000*),
which reproduces on a clean checkout of the parent commit, so it predates
this work (probably newer binutils on this machine) and is left for the P4.
QEMU 368/368. Sizecheck +1 byte on every persona, which is 36.0a's
`history_dir_ready` latch, re-baselined here.

### 36.2 — The SD card

§4.9: the existing driver on this board's pins. It comes this early because
it is cheap, and because everything after it (fonts on the card during
bring-up, writer files, chess games in `/sd0/chess/`) can then rely on it.

**Done when:** the inserted card mounts at `/sd0` at boot; `ls`, `cp`, `rm`,
`mkdir` work; a file written survives a power cycle; read throughput is
measured and recorded; GP13/14 and the CMD/data pull-ups are checked
(against the schematic and by reading the pad state) and recorded in the
board file.

#### Done, 2026-09-30

The card had already mounted at 36.0. What 36.2 adds is the measurements and
the pin questions.

**Throughput**, on a 16 GB-class SDHC card, SPI at 12 MHz (clk_sys / 12):

| instrument | sample | rate |
|---|---|---|
| `sdbench` (raw sequential read through `block_dev_t`) | 1024 KB | **349 KB/s** |
| `sdbench w` (FAT32 file write / read back, verified) | 256 KB | write 41, read 181 KB/s |
| `sdbench w` | 1024 KB | write 25, read 71 KB/s |

`sdbench` is the P4's instrument (35.5) with the same output, so the boards
compare side by side: 349 KB/s against the P4's 1547 over 4-bit SD/MMC. This is
the second implementation, so it is noted and not extracted. `sdbench w` is
new and generic (`kernel/shell.c`): it writes an offset-dependent pattern
through the VFS, reads it back, verifies every byte and removes the file. It
is also a QEMU test now (370/370). Its buffers come from `scratch`, not
`.bss`.

**The file rates fall as the file grows, and the cause is in `fs/fat32.c`,
not the card.** Every `pread`/`pwrite` walks the cluster chain from the
file's first cluster (`fat32.c:483`), and each step re-reads a FAT sector from
the device with no cache (`fat_get_entry()`), so a whole-file pass is
quadratic in its length. Filed in `plan/open_issues.md`. It is harmless at
the sizes the writer app will save (36.12 records the document budget), and a
one-sector FAT cache is the likely fix. That touches every FAT volume and the
P4's two-core build, so it gets its own change.

**Pins, from the schematic's "SD Card" block:** all six SDIO lines have 10 K
pull-ups to 3V3 (R61–R66), including D1/D2, which is what SPI mode wants on
unused lines. The socket's card-detect switch (TF-07F pin 9) goes to no GPIO,
so **there is no card detect**, and a card inserted after boot is found only
by a retry. On the board, GP13/14 are at FUNCSEL NULL with pads `0x116`
(ISO = 1, IE = 0), the RP2350's reset state for an unclaimed pad. They read
0 in `GPIO_IN` whatever the line does, which is expected and not a fault:
nothing drives them and the card sees them pulled high.

**Power cycle:** a marker written with `write-file` was read back intact after
the USB cable was unplugged and replugged (uptime 21.7 s at the check, against
485 s before). The marker was removed afterwards. The command history also
survived, which it has done since 36.0a.

### 36.3 — The panel lights: power, reset, backlight, sync, a solid colour

`LCD_EN` (GP45) high, reset pulse on GP41 (20 ms low, 200 ms settle, per the
demo; the ST7262 datasheet has the authoritative figures, so check them),
and backlight PWM on **PWM10 A / GP44** at ~5 kHz. The demo drives it
*inverted* (`level = WRAP·(100−percent)/100`), so check that against the
backlight circuit before trusting it. Then the timing SM alone, plus a pixel
SM that emits **one constant colour** with no DMA. That proves the sync
polarity, the porches and the pin order, and nothing else.

Start from the demo's proven timing (16 MHz-equivalent porches). Then move to
the datasheet's typical values: 24 MHz, Th 816, Tv 496. Record where it
stops working, if it does.

**Done when:** red, green, blue and white fill the screen **as those
colours**, which proves the bit order of all 16 pins; the backlight follows a
`(backlight n)` setting 0–100; the timing in use is written down in the
driver with its source.

#### Done, 2026-09-30 — lit first time, and at the datasheet's clock

`drivers/lcd7_rp2350.c`. The design turned out simpler than §3.1 sketched:

* **Timing is one PIO2 state machine running a four-instruction program**
  (`out pins,3` / `out x,13` / `nop` / `jmp x--`, PCLK by side-set on every
  instruction). It executes 16-bit segment commands `{DE/VS/HS levels,
  length}`. The ~6-instruction estimate in §3.1 was 4, which leaves 28 of
  PIO2's 32 slots for 36.4's pixel machine.
* **The whole frame is one 4096-byte command table** (512 lines × 4
  segments × 2 bytes), fed by **one DMA channel in RP2350's ENDLESS mode**
  through a 4 KB read ring. There is no re-arm, no chaining and no IRQ, and
  the CPU is never involved once it starts. The cost is one heap page, whose
  4 KB alignment is exactly what the ring needs. The line count is 512 so
  that the table is a power of two: 4 + 12 + 480 + 16.
* **The data pins are plain SIO outputs holding one colour** (`lcd colour
  <hex>`). This tested sync, polarity and pin order in isolation from any
  pixel path.
* Instructions are hand-encoded (the tree has no pioasm), with the delay
  derived from `LCD_CYCLES_PER_PCLK`. At a delay of 3 the macro reproduces
  the four literal words that first lit the panel, which is its check.

**On the board, judged by eye (the owner's):** solid blue at first boot.
Then red, green and white, and each channel's top bit alone (`8000` dark
red, `0400` dark green, `0010` dark blue), which puts all 16 data pins on
the right colour groups; the order of the lower bits within each group waits
for 36.4's gradient bars. `lcd backlight 20` visibly dims white, so R41/R42
*are* fitted on this board and the inverted PWM is right.

**Timing:** first lit at 18 MHz (8 cycles per PCLK), then moved to **24 MHz,
the ST7262's typical DCLK: 836 × 512 total, 56.0 Hz, "solid blue, no
artefacts"**. H: pulse 4, back 16, front 16. V: pulse 4, back 12, front 16
(the datasheet's maximum is 12, the demo's is 32; the panel does not
mind). `lcd` reports the SM's pc, the TX-stall (underrun) flag, which has
been "none" throughout, and the DMA read pointer walking the ring.

**Costs:** one heap page (the table), 11 bytes of static state
(`tools/sizereport-rp2350-terminal.json` re-baselined), the other personas
+0. The panel starts at boot in `kernel_main()`, before the scheduler, and
never needs it again.

### 36.4 — The 1-bpp framebuffer, through DMA, at zero CPU

48 000 B framebuffer, placed so that the `lcdterm` domain can be granted it
as NAPOT pieces (32 K + 16 K; phase 12 already grants non-power-of-two images
this way). Chained data + re-arm DMA pair, and the same for the timing list.
The pixel SM expands to fg/bg.

A `lcdtest` command with patterns that are **diagnostic, not decorative**:

* **one-pixel border** at x = 0, 799 and y = 0, 479. An off-by-one in either
  axis shows as a missing edge;
* **1-pixel vertical stripes** (0x55 bytes). A wrong PCLK edge or too few
  cycles per pixel shows as grey mush instead of stripes;
* **8 × 16 checkerboard**, which previews the text grid alignment;
* **fg/bg swap** without touching the framebuffer, which proves the colours
  live in PIO registers.

`/proc/lcd`: frames scanned, DMA re-arms, PIO TX-FIFO stall count
(`FDEBUG.TXSTALL` for the pixel SM, which is **an underrun**: the panel got a
late pixel).

**Done when:** all four patterns are stable, with no shimmer, for 10 minutes;
the underrun counter is **0** at the final PCLK; an idle-loop benchmark shows
no measurable CPU cost with the panel on versus off.

### 36.5 — Text on the screen

The generated font (§4.2), glyph blit, 100 × 30 grid, scroll by `memmove`, and
the XOR cursor. No escape sequences yet. `lcdtest text` fills the screen with
the character set and scrolls it.

**Done when:** every printable ASCII glyph is legible at arm's length;
scrolling 1000 lines leaves no residue; the time for one scroll is measured
and recorded (§2's "~100 µs" was a guess until then).

### 36.6 — The terminal emulator and the `lcd` console

`vtterm.c` per §4.3, including the cell shadow and the text window. The
`lcdterm` U-mode task with batched writes, and the `lcd` console device with
its tee. The kernel log goes to the screen from the moment the task starts.
Before that, the boot path draws directly with the same code, the fallback
shape `st7735_rp2350.c` uses.

**QEMU tests land here**, with the parser. They are independent of the
panel, so they gate the milestone as much as the screen does.

**Done when:** QEMU suite green with the new `vtterm` cases (including pending
wrap and a window smaller than the screen); on the board, with `lcd` bound, a
full `lsh` session driven from the **host keyboard over ACM0** renders
correctly on the panel: the line editor (insert mid-line, history, a line
exactly 100 characters long), `ls`, `cat` of a long file, `ed`, `e`, and the
Lisp REPL. `test_rp2350.py` still passes with `lcd` bound.

### 36.7 — PIO-USB: the engine on core 1 talks to the keyboard

`CORE1_MODE_USBHOST`; TX and RX PIO programs in PIO0/PIO1 at GPIOBASE 16;
the NRZI + bit-stuff encoder and CRC5/16 in C (QEMU-tested against spec
vectors first); bus reset (SE0 ≥ 10 ms); SOF / keep-alive every 1 ms from
TIMER1, polled rather than interrupt-driven; one control transfer:
GET_DESCRIPTOR(device) to address 0.

The engine's rings (§3.2) exist from the start, even though the first caller
is a shell command. Otherwise the second caller is a rewrite.

**Done when:** `usbprobe` prints the keyboard's 18-byte device descriptor,
VID:PID and bMaxPacketSize0; `/proc/usbhost` shows SOFs advancing at
1000/s ± 1 % over a minute; a `/flash0` write during that minute neither stops
the SOF count nor is refused (§3.2.1); `(perft 3 1)` on core 0 takes the same
time with the engine running as without it.

### 36.8 — Enumeration and the boot keyboard

The `kbd` task: the full enumeration sequence (§3.2), SET_PROTOCOL(boot),
SET_IDLE(0), interrupt-IN polling at `bInterval`, DATA0/1 toggling, and NAK
treated as "no news" rather than as an error. Detach is detected (SE0 held
longer than 2.5 µs with no traffic), and re-attach re-enumerates. Report
diffing produces make/break events. The Caps Lock LED via SET_REPORT is
**nice to have**, and is done only if the control-transfer path is already
solid.

**Done when:** `kbdlog` prints make/break events with modifier state for every
key pressed on the test keyboard; unplugging and re-plugging the keyboard 20
times re-enumerates 20 times with no reboot; the keyboard model and speed are
recorded here.

### 36.9 — Typing into the shell

The input-source extraction (§4.4), with the existing two sources converted
**first and on their own commit**, so a regression bisects to the refactor
and not to the keyboard. Keyboard bytes via the US keymap, VT sequences for
the navigation keys (arrows, Home/End, Delete, PgUp/PgDn), and typematic.

**Done when:** with no host computer attached at all (J3 on a USB charger, the
keyboard on the PIO-USB port), a human can: log in if auth is configured,
edit a line with every line-editor binding, recall history with the arrow
keys, interrupt a long `(perft 4 1)` with Ctrl-C, write and run a three-line
C program with `ed` and `cc`, and edit and evaluate a Lisp file with `e`. The
steps are written as a checklist in `tests/hw/README.md`, next to the other
failure signatures.

### 36.10 — Lisp graphics

§4.6: `canvas1.c`, the canvas calls on the `lcdterm` channel, the existing
four `canvas-*` primitives bound on this persona, the five new ones, and
`canvas-window`. QEMU tests for the canvas by framebuffer hash.

**Done when:** a Lisp program typed on the board draws a labelled function
plot (axes, a sine curve with `canvas-line`, text with `canvas-text`) in
`split` mode while the REPL stays usable on the right; `(canvas-window
'text)` restores the terminal intact from its shadow; the same program, minus
anything the ST7735 cannot do, still runs on the `rp2350-chess` persona.

### 36.11 — The chess board

§4.7: the split layout, dithered dark squares, scaled and outlined pieces, and
redraw on every move from either side.

**Done when:** a full game against `(chess)` at a level that answers in a few
seconds is played **from the keyboard, on the board, with no host
attached**; the graphical board matches the ASCII board after every move
(checked on at least one castling, one en-passant and one promotion, which
are the moves that change more than two squares); the PGN lands in
`/sd0/chess/`.

### 36.12 — The writer

§4.8. The first step is reading `e`'s implementation and recording the
decision (new program or `e` mode) in this document.

**Done when:** a text of several pages is written on the board from the
keyboard, saved to `/sd0`, the board is power-cycled, and the file reopens
intact; the same file opened on a PC shows paragraphs as single lines with no
inserted line breaks; saving while the card is removed reports an error and
keeps the document in memory; the QEMU tests for wrap/cursor and atomic save
pass.

### 36.13 — Documents

README: the persona in "Working today" and in the preset table, with the
three uses. `plan/hardware_seams.md`: `vtterm.c` under Console
implementations, `CONFIG_CLK_SYS_HZ` in §1 as the clock row's RP2350
refinement, and the second canvas implementation noted (§4.6). A sizecheck
baseline for the new persona. The decision on the ACM0 tee (§4.4). This
document's §1.5 updated with whatever the board contradicted.

---

## 6. Risks, and what would change the plan

* **The pixel SM does not fit 6 cycles per pixel with both colour branches
  balanced.** Fall back to 8 cycles and 18 MHz (§3.1). That is still above
  the demo's 16 MHz, which is known to work.
* **PIO-USB full-speed turnaround is not met from core 1.** Measure it. The
  engine's timeout counter says so directly. Low-speed keyboards have eight
  times the margin. If full-speed cannot be met, the fallback is option C
  (native USB) with the console on H7, not an overclock.
* **Scroll tearing is visible.** Replace `memmove` with a ring framebuffer:
  the re-arm DMA channel starts each frame at the ring's current top and a
  second channel covers the wrap. There is no copy at all, at the cost of
  one more DMA channel and a slightly less trivial re-arm list.
* **The panel's real tolerance is narrower than the demo suggests** (for
  example, porches the demo got away with at 16 MHz fail at 24). §1.5 gains a
  row, and the driver uses what works, with the measurement in its comment.
* **RAM runs out before the applications do.** Chess (with its
  transposition table), `cc`, the framebuffer, the cell shadow and a writer
  document all draw from the same 128 pages. 36.0 records the baseline, and
  each later milestone records what it took. If the writer or chess cannot
  fit alongside the terminal, the first lever is the one phase 15 already
  uses: working memory taken for the duration of a command and returned
  afterwards. PSRAM is the second lever, and not the first.
* **RP2350-E9 on this chip's stepping** (36.0 finds out). This affects
  detach detection only. Attach detection is safe, because the device's
  1.5 K pull-up dominates.

---

## 7. After the terminal: what this board offers next

Not scheduled by this phase. Each item will be a milestone of its own, or a
follow-up phase, once 36.13 is done.

* **UART networking, the stated goal.** Two wires are available, and they
  are different kinds of link:
  * **UART0 on H7 (GP16/17), TTL, point-to-point.** This is the existing SLIP
    9P link (`drivers/uart_net.c`, gateway's `uart1_link_rp2350.c`) on
    different pins. It is cheap, and it is the natural first step. The cost
    is that UART0 is also this persona's debug console, so once the screen
    is the terminal the question is whether debug moves to ACM0 alone.
  * **UART1 through the SP3485 RS485 transceiver (GP8/9, J9/J10).** This one
    is half-duplex. Direction is switched automatically by the TX line
    (U11/Q7 on the schematic), so the driver needs no DE pin, but the medium
    is a **multi-drop bus**. SLIP point-to-point works on two nodes, while
    more than two needs addressing and arbitration that this tree does not
    have. That is a design question, not a driver.
* **The other 12 MB of flash.** `cmake/flash_layout.cmake` describes the
  Pico 2's 4 MB, and that layout is valid on this 16 MB part as it stands.
  A larger `/flash0`, or fonts and bitmaps kept in flash, only needs a
  per-board layout, and the flash is free to use.
* **PSRAM (2 MB on QMI CS1, GP0).** A RAM disk first, as `raw_ideas.md` says.
  **Couple it with the XIP-layout performance question** (36.1,
  `plan/open_issues.md`, owner's suggestion 2026-09-30): what moves *out* of
  SRAM into PSRAM (the RAM disk, possibly the Lisp heap) pays for what moves
  *in* from flash (the chess engine's hot ~31 KB of code and tables). Measure
  the Lisp heap's behaviour on PSRAM before committing to that half.
  Then colour, which needs a CPU-free path from PSRAM to PIO that this
  document has not designed.
* **2-bpp, four-colour text** from the same PIO trick (`out pc, 2` into four
  `mov pins, <reg>` slots: background, fg, bold, and one more), at 96 KB.
  This makes SGR colours real without PSRAM.
* **CAN (XL2515 on SPI0, GP2–5)** and the **battery ADC (GP40)**.
