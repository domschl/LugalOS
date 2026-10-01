# Phase 37 — Sharing the screen: layouts, text, and the first graphical applications

**Status: planned. Written 2026-10-01, from the owner's proposal of the same
day and the review that followed.** Decisions marked *(owner, 2026-10-01)*
are settled. Decisions marked **[sign-off]** are recommendations that wait
for the owner's yes before 37.1 starts.

**Where this comes from.** Phase 36 (`plan/phase36_rp2350_lcd7_terminal.md`)
built the RP2350-LCD-7 into a stand-alone terminal: a 1-bpp 800 × 480 screen,
a VT terminal in its own U-mode task (`lcdterm`), and a USB keyboard. Its
last three application milestones (Lisp graphics, a chess board, a writer)
each needed to put graphics and text on the same screen. Phase 36 gave them
one *mechanism*, a text-window rectangle (§4.3 there). It did not give them a
*policy*: who decides the layout, what happens when a program ends, how a
program learns its space. Without that, three applications invent three
layouts. This phase settles the policy, builds it, and then builds the three
applications on it.

**Milestone scheme: `37.0`, `37.1`, …**, as phase 36.

---

## 0. The decision: constrained tiling, not a window system

There were two extremes.

* **Change nothing.** The whole screen is both a terminal and a canvas, and
  each application arranges itself. The cost is incoherence: every program
  decides its own geometry, and nobody restores the screen after a program
  exits or crashes.
* **A window system**, even one introduced step by step. Its real cost on
  this board is backing store. A full-screen 1-bpp buffer is 48 KB, against
  roughly 55 free 4 KB pages at idle. Overlapping windows need either stored
  copies or redraw requests, plus clipping, z-order and focus.

**Chosen: a small set of fixed, non-overlapping, character-aligned layouts,
owned by the `lcdterm` task** *(owner, 2026-10-01)*. An application
*requests* a layout and never computes geometry itself. Tiles never overlap,
so nothing ever needs to be uncovered, and there is no backing store.

**Tiles belong to the foreground program.** They are not windows of
different programs. LugalOS runs one foreground program at a time, and the
shell is simply what the text tile shows. That keeps the kernel side small,
and §6 keeps the door open to real windows later.

---

## 1. Layouts

Glyph drawing on the 1-bpp screen is byte-aligned (8-pixel cells), so every
tile boundary is a **column boundary** *(owner, 2026-10-01: char-based)*.
Free ratios such as 0.618 are not offered; they would snap anyway.

### 1.1 The status bar **[sign-off]**

**Recommended: one status bar, always present, in the top text row (16 px),
drawn inverted (white on black), owned by `lcdterm`.** This is the first
window decoration on the way to the Mac look (phase 36 §7.1), and it costs
one text row: the screen becomes **100 × 29** cells below the bar.

* **Left:** the title of each tile, written over the tile it names (in a
  split, the bar is divided at the tile boundary). Titles are set by the
  application (`"Lisp"`, `"Chess — white to move"`) and default to the
  program's name.
* **Right:** indicators. The compose state (§3.2) while a compose sequence
  is pending, the time once the clock is set, and SD presence.
* **Why inverted and not a Mac-style bar with a rule under it:** a 16-px
  glyph row leaves no pixel for a separating line, and taking 17 px breaks
  the character grid. A proportional Chicago-like bar belongs to the later
  fonts work.

The alternatives were per-tile title bars (two rows lost in a split, for no
extra information) and no bar at all (no place for the compose indicator,
and nothing that says which program has the screen).

### 1.2 The layouts

With the status bar, the area below it is 800 × 464 px, 100 × 29 cells. A
split puts the canvas on the left and the text on the right, separated by a
**one-column gutter** with a 1-px vertical rule at its centre. The rule never
touches a glyph, and none of the layouts puts anything on x = 799, which is
under the bezel (phase 36 §1.5).

| Layout | Canvas (px) | Gutter | Text (cells) | For |
|---|---|---|---|---|
| `text` | none | — | 100 × 29 | the shell, `e`, the writer: the default |
| `canvas` | 800 × 464 | — | none visible (output still lands in the shadow) | full-screen graphics |
| `split-wide` | **480 × 464** (cols 0–59) | col 60 | 39 × 29 (cols 61–99) | chess, Lisp plots |
| `split-half` | 400 × 464 (cols 0–49) | col 50 | 49 × 29 (cols 51–99) | Lisp with a wider REPL |

480/800 = 0.6, close to the proposed golden-ratio split, and a whole number
of columns. Side-by-side only: a top/bottom split of 29 rows would leave
about 12 rows of text, which is too few.

**The chess board in `split-wide`:** 8 × 56 px = 448 px board, with 16 px for
the file letters below it (448 + 16 = 464) and the rank numbers in the 32 px
to its left (32 + 448 = 480). The 16 × 16 piece bitmaps scaled 3× (48 px) fit
a 56-px square with a margin. Phase 36's plan had a 400 × 400 board in a
400 × 480 split, which wasted an 80-px strip.

### 1.3 Who may change the layout, and when it comes back

* An application requests a layout over the `lcdterm` channel (Lisp:
  `(canvas-window 'split-wide)`; the names of phase 36 §4.6 stay, with
  `'split n` replaced by the two named splits).
* **Layout persists across shell commands.** The shell is Lisp, and a Lisp
  session that splits the screen wants its REPL on the right until it says
  otherwise.
* **It is restored to `text` when an `exec`'d program or `(chess)` returns,
  normally or by a fault**, by the kernel and not by the program. A crashed
  program cannot leave the screen split.
* `screen text` at the shell does it by hand.
* **[sign-off]** Whether a key (for example Ctrl-Alt-Backspace style) forces
  `text` from anywhere. Not needed while the shell always comes back.

---

## 2. Drawing, and the redraw contract

### 2.1 Redraw messages instead of stored canvas pixels *(owner, 2026-10-01)*

When the canvas is hidden (layout `text`) and shown again, its pixels are
gone. **`lcdterm` does not keep a copy. It marks the canvas damaged, and the
application redraws.** This is the original Macintosh's update event, and it
is how X11 and Wayland clients work today. It costs no RAM, and chess and
plots redraw from their own state anyway.

The *text* tile is the exception, because the terminal is the one client
whose state `lcdterm` holds itself: the **cell shadow** (100 × 30 × 2 bytes,
6 KB, phase 36 §4.3) lets `lcdterm` repaint text without asking anyone.

### 2.2 How a redraw reaches the application

There is one foreground client, so the first form is a **damage counter**, not
an event queue:

* `lcdterm` keeps a per-tile damage generation and returns it with **every**
  canvas reply. A client that sees it change redraws.
* Lisp registers a redraw function, `(canvas-on-redraw f)`. The REPL checks
  the counter between top-level commands and calls `f` when it changed.
* Chess redraws its board after every move anyway; it also checks the
  counter after each command.

The message carries **a tile and a kind** (`redraw`, later `resize`, later
mouse events), so that it can become an event queue when there is more than
one client (§6) without changing its shape.

### 2.3 Canvas calls

As phase 36 §4.6 designed them: one channel message per primitive (`pixel`,
`line`, `rect`, `fill`, `circle`, `invert`, `bitmap`, `text`), executed by
`lcdterm` with `canvas1.c`, so a line is one round trip and not 800 pixels.
Applications never write the framebuffer. **All coordinates are tile-local**,
clipped to the tile, and `canvas-size` returns the tile's size, never the
screen's. Colours stay as phase 36 §4.6 settled: 0 is background, non-zero is
foreground.

`lcdterm`'s U-mode text section uses 4 060 of its 8 192 bytes today. The
canvas primitives, the layout code and the status bar go into the same
section; 37.3 measures whether they fit, and the section grows if needed.

### 2.4 Text programs learn their size

`e` and the line editors assume a screen; nothing tells them its size. Every
text program must be able to **ask for the text tile's size**: a console call
(`console_size(&cols, &rows)`) backed by `lcdterm` on this persona and by a
fixed or configured size elsewhere. `e` gets a real viewport from it (the fix
phase 36 deferred), so it also works in a 39-column split.

---

## 3. Text: UTF-8 out, a compose key in

### 3.1 Display

**Already there:** `vtterm.c` has decoded UTF-8 to one cell per code point
since 36.6, drawing `?` where the font has no glyph. This phase widens what
it can draw.

* **An internal 8-bit glyph code**, so the cell shadow stays 2 bytes per
  cell (6 KB):
  * `0x20–0x7E`: ASCII;
  * `0xA0–0xFF`: Latin-1, the same numbers as their code points;
  * `0x80–0x9F` and `0x00–0x1F` (control positions, never drawn as text):
    the box-drawing set (11 glyphs, already in the font), the 12 chess
    figurines U+2654–265F, €, ‘ ’ “ ” …, and a replacement glyph for anything
    else.
* **Coverage checked 2026-10-01 against `tools/fonts/spleen-8x16.bdf`:** all
  96 Latin-1 glyphs, €, and the typographic quotes are in Spleen. **The chess
  figurines are not**; they are drawn here, at 8 × 16.
* **Every glyph is one cell wide.** No combining or double-width characters.
  Column arithmetic is code-point arithmetic.
* About 230 glyphs × 16 bytes ≈ 3.7 KB of font in flash, up from ~1.7 KB.
  No RAM change.

### 3.2 Input: Caps Lock is the compose key *(owner, 2026-10-01)*

US layout stays the only layout. **Caps Lock no longer toggles capitals;
it starts a compose sequence**: `Caps " a` gives `ä`. The translator
(`drivers/usbkbd.c`) emits the result as UTF-8, so every program receives
plain UTF-8 bytes, the same as from a UTF-8 serial terminal on the host.

* **The table** follows the X11 compose conventions, so it needs no manual:
  `" a o u A O U` → ä ö ü Ä Ö Ü, `s s` → ß, `' e` → é, `` ` e `` → è,
  `^ e` → ê, `~ n` → ñ, `, c` → ç, `a e` → æ, `o /` → ø, `o a` → å,
  `= e` → €, `< <` / `> >` → « », and the remaining Latin-1 letters by the
  same pattern. About 100 entries, kept as a sorted table in code.
* **Feedback:** the status bar shows a compose indicator from Caps until the
  sequence completes.
* **An unknown pair produces nothing** and clears the indicator. Esc cancels.
* The Caps Lock LED (SET_REPORT, open since 36.8) is no longer needed.

### 3.3 Strings become UTF-8 aware *(owner, 2026-10-01)*

Text in memory and on the SD card is UTF-8, so a PC opens the writer's files
unchanged. What changes is every place that counts **characters** rather
than **bytes**:

* **Lisp:** `string-length` and `substring` count and index code points.
  That is the whole list today (`string-append`, `string=?` and the number
  conversions are byte-correct already), and `string-ref` is added in the
  same form when it is added. Indexing becomes O(n), which is fine for strings
  of this size. `string-bytes` returns the byte length for file and protocol
  work.
* **The line editors** (`kernel/line_editor.c`): cursor movement, Backspace,
  Delete, Ctrl-K, and the cursor column move by code point. History stores
  bytes and needs nothing.
* **`e` and the writer:** the same, plus line wrapping by code point.
* **C stays bytes.** `strlen` is a byte count in C everywhere, and `cc`'s
  programs are not changed.
* **Malformed input** (a stray continuation byte, a truncated sequence)
  counts as one character per byte and displays as the replacement glyph.
  Nothing rejects it, so a binary file in `e` does not lose bytes.

This is moderate work, as the owner expected: one decoder helper
(`utf8_next()` / `utf8_prev()`), used by maybe a dozen call sites. The
risk is in `line_editor.c`'s cursor arithmetic, so 37.2 tests it on QEMU with
mixed ASCII and multi-byte input.

---

## 4. Resources

| Item | RAM | Flash |
|---|---|---|
| Cell shadow (100 × 30 × 2) | 6 KB, already budgeted in phase 36 | — |
| Layout state, damage counters, tile titles | < 100 B | — |
| Glyph table (~230 × 16) | — | +2 KB |
| Compose table (~100 entries) | — | < 1 KB |
| UTF-8 helpers, layout and canvas code | — | a few KB in `lcdterm`'s section (§2.3) |
| Stored canvas pixels | **0, by decision (§2.1)** | — |

Nothing here competes with chess or the writer for RAM. Their budgets are the
ones phase 36 §4.7–§4.8 set.

---

## 5. Applications

Phase 36 §4.6–§4.8 hold the designs. What this phase changes in them:

* **Lisp graphics:** tile-local coordinates, `canvas-window` takes the named
  layouts of §1.2, `(canvas-on-redraw f)`, `(canvas-title "…")`.
* **Chess:** `split-wide` with 56-px squares (§1.2). The move list uses the
  figurines when the terminal can draw them, and a UTF-8 host terminal can too.
* **Writer:** full-screen `text`, the document in UTF-8, compose for
  diacritics, wrapping by code point. The decision between a new program and
  an `e` mode is still made by reading `e` first (phase 36 §4.8).

---

## 6. The migration path to bigger hardware

The rules above are chosen so that a later window system, on a board with
more RAM or with PSRAM, changes `lcdterm` and not the applications:

1. **Tile-local coordinates, sizes always asked for.** A tile becomes a
   window; the application does not notice.
2. **Drawing is a message protocol, never framebuffer access.** A compositor
   can be put behind it, or a remote display.
3. **Events carry a tile and a kind.** With several clients, the damage
   counter becomes a queue; the mouse (already on the keyboard's hub) adds
   click events in tile coordinates.
4. **Layout is a request, not a command.** On a large screen, `split-wide`
   becomes a size hint for a window.
5. **Backing store is optional, never assumed.** With PSRAM, `lcdterm` can
   keep a copy and send fewer redraws. Applications still have to handle
   redraw, so nothing breaks either way.
6. **Colour:** 0/non-zero today. A palette index or RGB565 tomorrow, mapped
   by `lcdterm` to whatever the panel does (phase 36 §7 sketches 2-bpp text
   and PSRAM colour).

---

## 7. Milestones

37.0 is this document. The order puts the shared pieces (text, input, layout)
before the three applications, so each application is a client of finished
infrastructure.

### 37.0 — The design, signed off

**Done when:** the owner has answered the **[sign-off]** items (the status
bar §1.1, a force-`text` key §1.3), and this document records the answers.

### 37.1 — Text tiles in `vtterm`

The text window as a rectangle (phase 36 §4.3), the cell shadow and repaint
from it, the status bar, `console_size()`, and the internal glyph code with
Latin-1, the figurines and the replacement glyph. **QEMU tests** by
framebuffer hash, as `vtselftest` does: writing into a 39-column window,
scrolling inside it, repainting from the shadow, Latin-1 and figurine
output, malformed UTF-8.

**Done when:** `cat` of a UTF-8 file with German text and chess figurines
shows correctly on the panel and on a UTF-8 host terminal through ACM0; the
status bar shows the program's name; `vtselftest` covers the new cases.

### 37.2 — Compose and UTF-8 aware editing

The compose key in `usbkbd.c` with its table and indicator,
`utf8_next/prev`, and code-point arithmetic in `line_editor.c` and in Lisp's
`string-length` and `substring`. **QEMU tests:** `usbkbdselftest` gains
compose sequences (known, unknown, Esc); a line-editor test edits a line
mixing ASCII and multi-byte characters, with cursor movement, Backspace,
Delete and Ctrl-K at every position.

**Done when:** on the board, `(string-length "Grüße")` typed with compose
returns 5, and the line editor edits that line correctly.

### 37.3 — Layouts and the canvas: Lisp graphics

The layout manager in `lcdterm` (§1), the restore rule, the damage counter,
`canvas1.c` and the canvas calls on the channel, and the Lisp bindings. This
is phase 36's 36.10, with tile-local coordinates.

**Done when:** a Lisp program typed on the board draws a labelled function
plot (axes, a sine curve with `canvas-line`, text with `canvas-text`) in
`split-wide` while the REPL stays usable on the right; `(canvas-window
'text)` restores the terminal intact from its shadow; switching back to the
split calls the registered redraw function and the plot reappears; an
`exec`'d program that faults in `split-wide` leaves the screen in `text`;
the same program, minus anything the ST7735 cannot do, still runs on the
`rp2350-chess` persona.

### 37.4 — The chess board

Phase 36 §4.7 in `split-wide` (§1.2), with redraw on every move from
either side and on damage. This is phase 36's 36.11.

**Done when:** a full game against `(chess)` at a level that answers in a few
seconds is played **from the keyboard, on the board, with no host
attached**; the graphical board matches the ASCII board after every move
(checked on at least one castling, one en-passant and one promotion, which
are the moves that change more than two squares); the PGN lands in
`/sd0/chess/`.

### 37.5 — The writer, and `e`'s viewport

Phase 36 §4.8, which is phase 36's 36.12, plus `e`'s viewport and flicker
fix and PgUp/PgDn (deferred there), built on `console_size()` and the UTF-8
helpers.

**Done when:** a text of several pages **with umlauts typed by compose** is
written on the board from the keyboard, saved to `/sd0`, the board is
power-cycled, and the file reopens intact; the same file opened on a PC shows
paragraphs as single lines with no inserted line breaks, and the umlauts
correct; saving while the card is removed reports an error and keeps the
document in memory; the QEMU tests for wrap/cursor and atomic save pass;
`e` scrolls a file longer than the screen without repainting it line by
line.

### 37.6 — Documents

README: Lisp graphics, the chess board and the writer under the terminal
persona; the compose key in the stand-alone checklist (`tests/hw/README.md`).
`plan/hardware_seams.md`: the second canvas implementation noted (phase 36
§4.6's rule: extract at the third), and `console_size()` if it became a seam.
The Spleen notice extended to the glyphs this phase took from it.

---

## 8. Risks

* **`lcdterm`'s 8 KB U-mode section overflows** with canvas, layout and
  status bar code. The section is a linker constant; it grows, and
  `check_umode_text.py` keeps it honest.
* **Code-point arithmetic in `line_editor.c` breaks an edge case** (a
  multi-byte character at the end of the line, history recall of one).
  Answered by 37.2's position-by-position test.
* **Lisp programs that relied on `string-length` being a byte count.** Any
  in `/sd0/system/` or `user/` are found by grep in 37.2 and moved to
  `string-bytes` where they meant bytes.
* **The ST7735 persona's canvas diverges.** Its 128 × 160 screen has no
  tiles; `canvas-window` there accepts `canvas` and `text` and refuses the
  splits, which a program can check with `canvas-size`.
