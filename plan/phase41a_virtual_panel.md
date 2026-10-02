# Phase 41a — A virtual panel: the LCD-7's screen on QEMU, in a window

**Status: planned 2026-10-02, not scheduled.** Agreed with the owner the same
day: to be done before phase 41 (the chess pane), whose dashboard is the
first GUI that would be built and iterated against it. Small: a few hundred
lines of kernel code and a host script.

## 0. Why

Two 7" panels will soon run this OS's GUI: the RP2350-LCD-7 now, the
ESP32-P4's soon (phase 39). Iterating on a GUI on them costs a flash
cycle per change, and nobody can see what the panel shows from the desk.
QEMU already runs the same screen code -- `drivers/screen.c` is portable and
`drivers/ramscreen.c` puts an 800 x 480 screen in RAM so the suite can test
Lisp's canvas -- but nothing shows it, and it carries only the canvas: text
goes to the serial line, not into the screen.

## 1. Decided (owner, 2026-10-02)

* **The framebuffer goes over 9P** to a host viewer, not through a QEMU
  display device. On QEMU the link is the virtio-console socket the suite
  already uses (`test_9p_virtio_link`, `host/p9lib`'s `connect_unix()`), so
  speed is not a concern: a whole 48 KB frame is a pipe write.
* **Display only.** Keys come from the terminal QEMU runs in, through the
  same key parser as the panel's keyboard (`kernel/keyseq.c`; Super keys
  arrive as CSI-u). Input from the viewer's window is a possible later step.
* **The same viewer can watch a real board** over its own 9P link (the
  LCD-7 over USB ACM1, the P4 over Ethernet). That is a bonus, not the goal;
  over USB it needs §5's measures and is not part of this phase's
  done-condition.

## 2. What it does not show

Timing (PSRAM, the panel's DMA scan-out, redraw speed), the PIO-USB
keyboard, and anything specific to Hazard3 or the panel -- phases 23, 36 and
38 each met silicon behaviour QEMU does not have. The virtual panel is for
layout, logic and interaction; performance and hardware stay on the boards.

## 3. Design

### 3.1 The virtual panel on QEMU

`ramscreen.c` grows from "a canvas" to "the panel": with the panel on, it
is also a console output device -- what the shell prints goes into
`screen_write()`, as on the LCD-7 -- and `console_size()` reports its
98 x 27 text window, the title and the menu bar's indicators work, and the
layouts behave exactly as on the panel. Off is today's behaviour, unchanged,
and stays the default: the suite's tests rely on a serial console whose
size is "unknown".

Switched at run time (`screen panel on|off`, and a boot-script line for a
QEMU session that wants it from the start) rather than by a preset, so one
QEMU build serves both.

**To settle in 41a.1:** with the panel on, programs that size themselves
(`e`, the chess pane) draw for 98 x 27, and the serial terminal shows those
escape sequences at its own size -- garbled. The terminal is then the
keyboard and the log; the viewer is the screen. Acceptable, or should panel
mode stop echoing screen output to the serial line?

### 3.2 `/dev/screen`

A read-only file in `/dev`, beside `clipboard`, on every target with a
screen (QEMU's virtual panel, the LCD-7; the P4 when it has one):

| Offset | Content |
|---|---|
| 0 | a header: magic, version, width, height, format (1 = 1 bpp, leftmost pixel in bit 0; room for the P4's colour), stride, a frame sequence |
| header | one 32-bit hash per pixel row (480 rows: 1 920 bytes) |
| after the hashes | the pixels, `stride` bytes per row |

Stateless: a viewer reads the header and the row hashes, compares them with
its own, and reads only the rows that changed, each by its offset. Nothing
in the drawing code changes -- the hashes are computed when the header is
read, so no draw primitive needs to mark rows dirty. On QEMU that is cheap;
on the LCD-7 it is a 48 KB scan per poll, which §5 measures.

### 3.3 The viewer

`host/panelview/`, Python, beside `host/p9lib` and using it: connects by
unix socket (QEMU), TCP (the P4) or serial (the boards' ACM1 link); polls a
few times a second; draws the frame in a window at 1x or 2x; a key saves a
PNG. No dependency beyond what `host/` already pulls in, plus a GUI toolkit
(tkinter is in the standard library).

### 3.4 Running it

A script that starts QEMU with the panel's virtconsole socket and the
viewer beside it (`tools/qemu_panel.sh <preset>`), so "change, build, look"
is one command.

## 4. Milestones

| | What | Done when |
|---|---|---|
| 41a.1 | The virtual panel on QEMU (§3.1) | `screen panel on`: the shell, `e` and Lisp's canvas run in the RAM screen exactly as on the LCD-7; `vtselftest` unchanged |
| 41a.2 | `/dev/screen` (§3.2) | a runner test reads it over `host/p9lib`, checks the header, a pixel `canvas-get` also sees, and that one drawn line changes exactly the rows it crosses |
| 41a.3 | The viewer and the script (§3.3, §3.4) | the owner runs the Lorenz demo and the editor on QEMU in the window |
| 41a.4 | The LCD-7 over USB, measured | the 9P link's throughput on ACM1 and the viewer's frame rate on it, recorded -- input to §5, not a requirement |
| 41a.5 | Documents | README, `tests/hw/README.md`, this file's status |

## 5. Later, for watching real boards

If 41a.4 shows the USB link too slow to watch the LCD-7 comfortably:

* **Run-length encoding** of the changed rows (a `/dev/screen` variant) --
  these screens are mostly white, the grey desktop pattern and frames.
* **Streaming the drawing instead of the pixels**: `screen.c` and `vtterm.c`
  are portable, so the viewer could run them natively and be fed the
  console bytes and canvas requests. Tiny on the wire, but it needs a full
  frame to start from and every request in order, or the picture drifts from
  the panel's without anyone noticing -- so only if pixels are not enough.

## 6. Not in this phase

Colour (its own later phase; the header's format field leaves room), input
from the viewer's window, and any QEMU display device (`ramfb`,
`virtio-gpu`).
