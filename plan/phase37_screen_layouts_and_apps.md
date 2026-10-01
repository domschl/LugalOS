# Phase 37 — Sharing the screen: layouts, text, and the first graphical applications

**Status: in progress — 37.0, 37.1 and 37.1a done 2026-10-01, 37.2 next. Written 2026-10-01, from the owner's proposal of the same
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

### 1.1 The look: a menu bar and framed tiles *(owner, 2026-10-01)*

37.1 built a status bar as one inverted text row. The owner then chose a
Macintosh-like look over keeping every text row: three designs were
rendered at 800 × 480 with the real font (a 16-px inverted bar; a menu bar
with framed tiles; the same with a striped title bar per tile), and the
third, refined twice by eye, is the design (**"C2"**):

* **The menu bar:** y 0–19, white, a 1-px rule at y 19. The system name in
  bold on the left (`LugalOS`; menus can live there later), the indicators
  on the right ending 16 px from the edge: the time once the clock is set,
  and 37.2's compose state.
* **The desktop:** everything no tile covers, the 50 % grey pattern.
* **A tile:** a 1-px frame with a 1-px drop shadow (right and below).
* **Its title bar, 17 px:** the frame's top border, a white row, six
  stripes with white rows between them, a white row, a border. The title
  is centred **to the pixel** in a white box, in bold, glyph rows 1–14
  drawn from one pixel below the top border, so capitals have two white
  rows above and three below. The application sets it (OSC 0/2, or
  `console_set_title()`), and it defaults to the program's name.
* **Text inside a tile** starts on the 8-px grid with a 3-px margin to
  the frame on both sides (Spleen glyphs leave their rightmost column
  blank, so the frame sits 4 px left of the text, 2 px right of it).

**The rule behind it: terminal text stays on the 8-px column grid,
everything else may sit at any pixel.** Text is drawn, scrolled and
repainted thousands of glyphs at a time, and byte alignment keeps that one
store per glyph row; frames, title bars, the menu bar and canvas text are
drawn rarely, by a glyph routine that shifts and masks (`canvas1.c`).
Vertical placement was never constrained: a text window starts on any
pixel row.

The cost against 37.1's bar: one more text row and two columns (98 × 27
instead of 100 × 29), and 52-px chess squares instead of 56.

### 1.2 The layouts

All geometry is in pixels on the 800 × 480 panel. Nothing is drawn on
x = 799, which is under the bezel (phase 36 §1.5): a frame's right border
is at x 794 and its shadow at 795.

| Layout | Canvas tile (frame, x) | Text tile (frame, x) | Text (cells) | For |
|---|---|---|---|---|
| `text` | none | 4–794 | **98 × 27** (cols 1–98) | the shell, `e`, the writer: the default |
| `canvas` | 4–794 | none (output still lands in the shadow) | — | full-screen graphics |
| `split-wide` | 4–474 | 484–794 | **38 × 27** (cols 61–98) | chess, Lisp plots |
| `split-half` | 4–394 | 404–794 | 48 × 27 (cols 51–98) | Lisp with a wider REPL |

Every tile's frame runs from y 22 to 473, its title bar from y 22 to 38,
and its content from y 39 (text from y 40, 27 rows to y 471). A canvas
tile's drawable area is the frame's interior below the title bar: 469 × 434
px in `split-wide`, 389 × 434 in `split-half`, 789 × 434 in `canvas`.
Side-by-side only: a top/bottom split would leave about 12 rows of text.

**The chess board in `split-wide`:** 8 × 52 px = 416 px, centred in the
tile, **without coordinates** (the owner's call: the labels cut into the
frame, and the move list already names squares). The 48-px pieces fit a
52-px square.

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
* **No key forces `text` from anywhere** *(owner, 2026-10-01)*: the shell
  always comes back, and the restore rule above covers a crash.

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
6. **A tiling-control API, later** *(owner, 2026-10-01: probably not in this
   phase)*. Today only the foreground program requests a layout, through its
   canvas calls. A general API (named tiles, which program owns which,
   moving a client between tiles) is what turns tiles into a window manager,
   and it is shaped by the first time two programs want the screen at once.
7. **Colour:** 0/non-zero today. A palette index or RGB565 tomorrow, mapped
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

**Done, 2026-10-01.** The status bar: yes. A force-`text` key: no. A
tiling-control API is noted for later (§6, item 6).

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

**Done, 2026-10-01.**

* **`drivers/screen.c`** (new, portable): the status bar on the top row over
  a `vtterm` window of 100 × 29. The bar is inverted: a space, the title,
  and the indicators ending one cell short of the right edge (x = 799 is
  under the bezel); a long title is cut, never the indicators.
* **`vtterm.c`:** the cell shadow (`uint16_t` per cell: code + reverse bit)
  kept by four wrappers that draw and record together, so the two cannot
  disagree; `vtterm_repaint()`; **OSC 0/2 titles** with BEL or ST, other
  OSCs swallowed (before this, an OSC printed its text onto the screen).
* **`fbtext.c`:** lookup by internal glyph code; a scroll inside a window
  narrower than the buffer moves only the window's own bytes.
* **The glyph table** (`tools/gen_font_bdf.py`): codes 0x20–0xFF as §3.1
  planned, but with every extra in 0x80–0x9F, so 0x00–0x1F stays unused:
  box drawing (11), the twelve figurines (drawn in the generator; the white
  ones are the black ones with the interior cleared), €, ‘ ’ “ ” …, •, █ and
  ▒. 0x7F is the replacement glyph, a reversed `?`. C1 code points
  (U+0080–009F) draw as the replacement, not as Latin-1.
* **State:** the screen, its terminal and the shadow live in their own
  8 KB, 8 KB-aligned block, the lcdterm domain's fifth region (the most a
  domain may have). The framebuffer's spare tail, which held the terminal
  since 36.6a, was too small. Two heap pages: 52 free at idle against 54.
* **The kernel side:** `console_screen_t` (flush, size, title) replaces
  36.6a's flush hook; `console_size()` and `console_set_title()` are
  portable and answer "no screen" elsewhere. The channel to `lcdterm` gains
  title and indicator operations. Titles: `lsh` at the prompt, the file
  name for `exec`, `Chess`, `e: <file>`. The clock (HH:MM, only once the
  time is set) is checked once a second from the console flush, which now
  runs on every turn of an input wait.
* **Measured:** `lcdterm`'s U-mode section holds 7 524 of 8 192 bytes (the
  font is 3.5 KB now); 37.3 grows it. Static RAM +8 bytes (the clock).
* **Tests:** `vtselftest` 23/23 on QEMU, 8 of them new: Latin-1, figurines
  and C1; repaint restores every pixel; OSC titles and their terminators;
  a narrow window leaves the bytes beside it alone; the bar's layout,
  truncation and full repaint. Full suite 375/376: the one failure is the
  known log-burst flake on rv64-smp (`plan/open_issues.md`).
* **On the board** (the owner, 2026-10-01): the bar shows the title and a
  ticking clock; a UTF-8 file (German, French and Spanish letters, €, °,
  the figurines, quotes, blocks, box lines, and characters outside the
  font) displays correctly on the panel and arrives byte-identical at the
  host through ACM0; `e` shows its file name; output scrolls under the
  bar. The file went to the card over 9P, because **the line editor drops
  bytes above 0x7F**: typing them is 37.2's.
* **Owner's note for 37.5:** `e` should fill the whole text tile, not
  draw a box sized by its content.

### 37.1a — The menu bar and framed tiles *(added 2026-10-01)*

§1.1's design, for the one tile 37.1 has: the menu bar, the desktop, the
`text` tile's frame, shadow and title bar, the text window at 98 × 27.
`drivers/canvas1.c` starts here with what the chrome needs (fills with a
pattern, lines, a glyph at any pixel in bold), and 37.3 extends it.
`lcdterm`'s U-mode section grows to 16 KB. `lcd repaint` redraws the whole
screen from the shadow, which makes the shadow testable on the board.

**Done when:** the panel matches the C2 mockup with the shell in it; the
title follows the program; the clock ticks; `lcd test stripes` then
`lcd repaint` restores the screen exactly; `vtselftest` checks the chrome's
geometry on QEMU.

**Done, 2026-10-01.** The owner checked the panel against the mockup: the
look, the titles, `lcd test stripes` then `lcd repaint` restoring the
screen exactly, and scrolling inside the tile.

* **`drivers/canvas1.c`** (new): fills in white, black or 50 % grey with
  masked edges, lines, and a glyph at any pixel (shifted across two bytes,
  foreground only, a row range, bold as the Mac did it), and UTF-8 text.
* **`screen.c`** draws the menu bar, desktop, frame, shadow and title bar
  from the geometry in `screen.h`; the text window is 98 × 27 from (8, 40).
  `lcd repaint` asks the task to redraw everything from the shadow.
* **Scrolling:** a window one byte in from the buffer's edge is never
  word-aligned as a whole, so `fbtext` copies each row's middle by words;
  and a tile that spans the screen scrolls **whole pixel rows** in one
  word-wise copy, because beside its text are only the frame and the grey
  desktop, which repeat every two rows (`fbtext_t.whole_rows`). The shadow
  scrolls two cells a word.
* **Measured** (`lcd outbench`, 20 lines of 98 characters through the
  console with the USB tee): 36.6 89 800 chars/s; 37.1 77 200 (the
  shadow); 37.1a 60 000 with row-wise scrolling, **66 500** with whole-row
  scrolling. The per-character path is the same as 37.1's and the scroll
  now copies less, so the rest is most likely the code's new place in
  flash: it runs through the XIP cache it shares with the kernel. The
  owner's call: accept it, and look at it with the PSRAM work. 66 K chars/s
  still repaints a full screen in about 40 ms.
* **Cost:** `lcdterm`'s U-mode section is 16 KB (8 784 bytes used); static
  RAM +0.
* **Tests:** `vtselftest` 24/24 (its screen test now checks the chrome's
  pixels: the menu bar's rule, the desktop's phase, the frame and shadow,
  the stripes, the centred box and the bold title, the clock's position,
  and that a scroll and a full repaint agree byte for byte); QEMU suite
  376/376; `test_rp2350` 25/25.
* **Found by the owner, fixed:** `ed` and the `lisp` REPL echoed typed
  characters with `uart_putc()`, past the console, so on the panel the
  input was processed but never shown; `clear` cleared the UART's
  terminal and not the screen. All three write through the console now.
  Since 36.6, when the screen became the console.

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

`e` fills the whole text tile (owner, 37.1), sized by `console_size()`.

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
