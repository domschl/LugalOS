# Phase 37 — Sharing the screen: layouts, text, and the first graphical applications

**Status: done 2026-10-01 — 37.0 to 37.6. Written 2026-10-01, from the owner's proposal of the same
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

*(Since 38.8, on a board with PSRAM, one canvas per layout is stored and
restored without damage when nothing was drawn meanwhile --
plan/phase38_psram.md. The contract below still holds.)*

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

**Done, 2026-10-01.** The owner composed and edited on the panel.

* **Compose** (`drivers/usbkbd.c`): Caps Lock opens a sequence, the next two
  printable keys pick a character from an 89-entry table (every Latin-1
  letter, €, and the common Latin-1 symbols, X11's pairs), looked up in
  either order; the table was generated with a check that no pair means two
  things either way round. Esc or Caps again cancels; an unknown pair gives
  nothing; any other key cancels and then does its usual thing; nothing in
  a sequence repeats. Caps Lock no longer toggles capitals.
* **The indicator:** a console input source may offer `indicator()`, and
  the screen *polls* it (`console_indicators()`), rebuilding the menu bar's
  right side -- indicators, then the clock -- on every console flush and
  sending it only when it changed. Not pushed from the keyboard, because the
  keyboard is drained under the input lock and drawing takes the console
  lock: a push could deadlock against a program printing while it checks
  for Ctrl-C.
* **The line editor** keeps a byte buffer with the cursor on character
  boundaries: Left/Right, Ctrl-B/F, Backspace, Delete and Ctrl-D step over
  whole characters, a typed multi-byte character is collected and inserted
  whole (anything else abandons a half-arrived one), and the redraw moves
  the screen cursor by characters. `ed`'s and the `lisp` REPL's line
  readers accept UTF-8 and back up over a whole character.
* **Lisp:** `string-length` and `substring` count characters (a stray
  continuation byte belongs to the character before it, so count and index
  agree); `string-bytes` is new. No Lisp file in the tree relied on byte
  counts.
* **Not yet:** `e` and its file-name prompts still drop non-ASCII keys;
  that is 37.5's, with the viewport.
* **Cost:** static RAM +12 bytes on every RP2350 persona (the polled line
  reader's pending character), +30 on the terminal (with the hint and the
  clock string).
* **Tests:** `usbkbdselftest` gains six compose cases (the hint, either
  order, a 3-byte €, an unknown pair, Esc and Caps-twice, Enter); the runner
  gains Lisp's character counting and a line-editor test that inserts
  before, backspaces over and deletes multi-byte characters. QEMU suite
  380/380; `test_rp2350` 25/25.

### 37.3a — Lisp collects inside a form *(added 2026-10-01)*

Found while measuring the interpreter for 37.3's showcase (the owner's
request: a cellular automaton, rule 30 or 110, and maybe Mandelbrot or a
Lorenz attractor). The collector ran only between top-level forms, every
arithmetic result is a node, and the RP2350's pool is 1 024 nodes: a loop
of a few hundred iterations ended in "Node pool exhausted", and so would
37.3's own sine plot. The owner chose to fix the engine first.

**Done, 2026-10-01.**

* **`gc_collect_in_form()`** (`user/lisp/lisp.c`): when `alloc_node()` or
  `intern_string()` finds its pool dry, it collects on the spot. The roots
  are `global_env` and **every word of the evaluating task's stack** that
  points into a pool, interior pointers included (a conservative scan, as
  Boehm's collector does); `s0`–`s11` are stored into the scanning frame
  first, so a pointer held only in a callee-saved register is seen.
  Caller-saved registers live across a call are spilled by their callers.
  The stack's bounds come from the new `sched_current_stack()`: the
  current task's kernel stack, or the linker's boot stack on hart 0.
* **Why the stack is all of it:** Lisp is evaluated only from the shell,
  on one stack, and no static outside `global_env` and the free list holds
  a node (checked). A half-built node's stale fields point at free cells
  (pre-marked, not followed) or valid ones, so marking through them only
  keeps garbage a little longer.
* **Out of memory is decided, not waited for:** a collection inside a form
  that frees less than a thirty-second of the pool counts as exhaustion.
  Without that rule, a list growing into the pool was collected after every
  few allocations near the end -- quadratic, 25 s on RV64 -- before the old
  exhaustion path finally ran.
* **Found by the suite, fixed:** once out of memory was decided, the
  form's last few allocations while unwinding took the old clamp, which
  reuses the pool's last slot -- and in a pool full of live data that slot
  is live. It hung the shell after an exhaustion about one run in six on
  RV64. The cells the deciding collection freed are now handed out first,
  and no further collection runs inside that form: 20/20 and 10/10 since.
* **The exhaustion tests changed with it** (the runner's and
  `test_rp2350`'s): their runaway was `(loop (+ n 1))`, which keeps nothing
  alive and now simply runs until Ctrl-C. They now grow a list without end,
  which is what still exhausts a pool.
* **First run on RV64 found:** `__builtin_unwind_init()` also saves the FP
  registers there (the kernel is built with D, and the FPU is off): an
  illegal instruction. The registers are saved explicitly now.
* **Measured on the board:** `(lp 100000 0)` runs in 15.9 s (~6 300
  iterations/s) with 4 498 collections inside the form, about 27 nodes per
  iteration; `(fib 20)` (21 891 calls) in 2.9 s. `(gc-stats)` reports the
  count and the free nodes.
* **What it means for the showcase:** a rule-30/110 automaton and a Lorenz
  attractor are Lisp-sized (seconds per row, seconds per few thousand
  points). Mandelbrot is not (~200 000 pixels × ~20 iterations: hours in
  this interpreter); it belongs in a C program built by `cc`, which needs a
  canvas path for U-mode programs -- after 37.3b.
* **Cost:** static RAM +5 bytes on every RP2350 persona.
* **Tests:** a runner test per target -- a 30 000-iteration loop, `(fib
  18)`, a list defined before string churn that comes through intact, a
  string held only by an argument across the churn, and `(gc-stats)`
  counting at least one in-form collection; the same, longer, by hand on
  RV32, RV64 and the board.

### 37.3b — Layouts and the canvas: Lisp graphics

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

**Done, 2026-10-01.** The owner ran the layouts, the redraw and the
demos on the panel.

* **Layouts** (`drivers/screen.c`): TEXT, CANVAS, SPLIT_WIDE and
  SPLIT_HALF, by §1.2's table, with a framed canvas tile and its own title
  bar. The text window moves and resizes with the layout (`vtterm_resize()`:
  a narrower window keeps each row's left part); in CANVAS it is hidden but
  still written (`vtterm_set_hidden()`), so nothing printed meanwhile is
  lost. Asking for the layout already showing changes nothing -- else a
  redraw function that starts by asking for its layout would loop.
* **The canvas protocol** (`screen.h`): one request and a 9-byte reply,
  executed by `screen_canvas()` -- in the `lcdterm` task on the panel (a
  new `'C'` op whose reply comes back over the channel), and by a **RAM
  screen** everywhere else (`drivers/ramscreen.c`: the panel's 800 x 480,
  allocated on first use), so Lisp's canvas runs and is tested on QEMU.
  `console_canvas()` is the kernel's entry.
* **`canvas1.c`** gained tile windows (origin and clip), line, circle,
  invert, packed-bit rows with a scale, and pixel read-back.
* **Lisp** (every build without the ST7735): `canvas-fill`, `-pixel`,
  `-rect`, `-text` as before, and `canvas-frame`, `-line`, `-circle`,
  `-invert`, `-row`, `-get`, `-size`, `-window` (`'text`, `'canvas`,
  `'split`, `'split-half`), `-title`, `-on-redraw`. Colours 0 white, 1
  black, 2 grey. **Redraw:** every reply carries the canvas's damage
  count; drawing calls remember it, `canvas-window` does not, and the
  prompt (shell and `lisp` REPL) calls the registered function when the
  count moved on. **Reset:** `exec` and chess put the screen back to text.
* **The showcase** (`tools/sd_root/demos/`): `ca.lisp` -- elementary
  cellular automata, `(ca 30 3)`, `(ca 110 3)`, `(ca 90 3)` -- and
  `lorenz.lisp`, the Lorenz attractor in 1/1000 fixed point (1000 steps in
  2.0 s on the board). **The owner caught a bug in `ca.lisp`:** cells past
  the canvas counted as 0, so rule 30's left side went wrong once the
  pattern reached the edge. It now simulates past both edges by the light
  cone's width and draws shifted, clipped by the canvas; rules 30, 110 and
  90 match a Python reference on an unbounded line exactly (a runner test
  for rule 30). Rule 110 grows only to the left (`001` -> 1, `100` -> 0),
  so from one cell it fills a triangle from the top right.
* **Lisp needed room** (the board ran out of nodes loading the demos): the
  ~170 built-ins left the node pool for a static table looked up after the
  environment (values `static const`, a program's `define` still shadows
  them) -- about 680 nodes and 170 string slots back on every target, for
  1.5 KB of RAM; the integers -16..255 are shared constant nodes; and the
  terminal persona's pool is 2048 nodes (`CONFIG_LISP_NODE_POOL`, a new
  board setting). Measured on the board: 1 869 nodes free at idle (843
  with 1024), 1 184 with both demos loaded.
* **Cost:** static RAM +1.5 KB on every RP2350 persona (the built-ins
  table), +22 KB on the terminal (the pool, five pages: heap 79 -> 74 pages,
  peak 55 in the hardware suite). `lcdterm`'s U-mode section: 11 648 of
  16 384 bytes.
* **Tests:** `vtselftest` 28/28 (layouts and resizing, the canvas
  operations and their clipping, the hidden text, damage, a refused split);
  runner tests for Lisp's canvas through the RAM screen (sizes per layout,
  drawing read back, redraw at the prompt, reset after `exec`) and for the
  demos (Sierpinski cells for rule 90, the rule-30 reference, Lorenz).
  QEMU suite 388/388 on an idle host; `test_rp2350` 25/25.

### 37.4 — The chess board

Phase 36 §4.7 in `split-wide` (§1.2), with redraw on every move from
either side and on damage. This is phase 36's 36.11.

**Done when:** a full game against `(chess)` at a level that answers in a few
seconds is played **from the keyboard, on the board, with no host
attached**; the graphical board matches the ASCII board after every move
(checked on at least one castling, one en-passant and one promotion, which
are the moves that change more than two squares); the PGN lands in
`/sd0/chess/`.

**Done, 2026-10-01.** The owner played on the panel: "all graphics
function and basic game play work well"; further tuning is for a later
phase.

* **The console board is a checkerboard now** (the owner's point): it
  colours squares with 24-bit SGR backgrounds, which the 1-bpp terminal
  used to ignore. `vtterm` turns a **dark background** -- ANSI 40-47 and
  100-107, 256-colour and 24-bit, by brightness below `VT_DARK_LUMA` (160)
  -- into reverse video; SGR 7 on a dark background reverses back, and SGR
  0 or 49 ends it. The board's light squares (brightness ~222) stay white,
  the dark ones (~145) turn black.
* **Figurines in a reversed cell use their opposite-colour glyph** -- the
  "second character set" the owner asked about, which the font already
  had: a white king on a dark square is the solid king reversed (a white
  silhouette), a black one the outline reversed (a white rim around black).
  The same rule will keep pieces readable inside a text selection.
* **The graphical board** (`user/chess/src/chess_ui.c`): `(chess)` on a
  screen switches to the wide split, titles the canvas "Board", and draws
  an 8 x 52 px board, centred, framed, without coordinates: light squares
  white, dark squares the grey pattern, `piece_bitmaps[]` (now outside the
  ST7735 guard) scaled 3x to 48 px with a 1-px rim in the opposite colour.
  Built a pixel row at a time, 416 canvas row requests, from `chess_show()`
  -- so after every change to the position, whoever made it. Leaving chess
  puts the screen back to text (37.3b's rule). It draws only on a real
  screen (`console_size()`) or a canvas already open, so the QEMU targets'
  RAM screen is not allocated by every chess session.
* **Left as they are, for a later phase:** the ASCII board is still printed
  after every move beside the graphical one (12 lines of the 38-column text
  tile); the pieces are the 16-px art scaled, not drawn at 48 px; no
  square-cursor input.
* **Cost:** static RAM +0. `lcdterm`'s U-mode section: 12 076 of 16 384
  bytes. The boot stack's peak in the hardware suite rose from 8 372 to
  11 452 of 16 384 bytes, with `(chess-board-selftest)` (a `Position` and
  the drawing buffers on the stack) run earlier in the same boot.
* **Tests:** `vtselftest` 30/30 (dark backgrounds in all three colour
  forms with SGR 7 on top, and the figurine swap); `(chess-board-selftest)`
  draws the starting position on the canvas and checks an empty light and
  an empty dark square, both kings' bodies and rims, and the frame -- a
  runner test on every target. QEMU suite 390/390; `test_rp2350` 25/25.

### 37.5 — Editing: a clipboard for every text input, and `e` as editor and writer *(reworked with the owner, 2026-10-01)*

The owner's review before starting: `e` has only the basics (movement,
insert, delete; save, save-as, open, insert-file and evaluate already
exist), and editing needs **search/replace** and **select / cut / copy /
paste** -- the latter **for every text input**, the way cursor movement
already is. Settled with the owner, 2026-10-01:

* **Two key families, one command set.** The Emacs keys (Ctrl-Space mark,
  Ctrl-W cut, Alt-W copy, Ctrl-Y paste, Ctrl-K kill to the clipboard,
  Ctrl-_ undo, Ctrl-S/Ctrl-R search) and a **unified clipboard on Super**
  (Super+C/X/V, Super+Z undo, Super+F/G find). Both map to the same internal
  commands. Super cannot cross the serial line (the host's terminal keeps
  Cmd for itself), which is why the Emacs keys stay first-class.
* **Super is sent as CSI-u** (`ESC [ 99 ; 9 u` for Super+c: the code
  point, then 1 + the modifier bits, Super = 8) -- the form kitty, foot and
  xterm use. Alt is an ESC prefix, Shift/Ctrl with a navigation key xterm's
  `ESC [ 1 ; m X`, Ctrl-Space NUL: the same bytes a host terminal sends, so
  editing over serial and on the panel is one protocol.
* **The writer is `e` in text mode** (by file extension: `.txt`, `.md`):
  soft wrap, no line numbers, paragraphs saved as single lines. One editor,
  so the writer gets the viewport, selection, search and undo for free.
* **Undo** belongs to 37.5b, bounded; **the clipboard** holds up to 8 KB.
  Both are allocated on first use.
* **Resizable splits:** the text tile in three widths -- 38 columns (canvas
  469 px, today's `split-wide`, the one the chess board needs), 48 (both
  ~385 px, today's `split-half`) and 64 (canvas 261 px, new) -- stepped with
  **Super+[ and Super+]**, which move the divider left and right (Super+[
  makes the text tile wider: the owner's reading on the panel, 37.5a)
  through five places -- text only, the three splits, canvas only -- so they
  also close a pane; **Super+\\** swaps the panes in a split, and with one
  pane full shows the other one full (both the owner's, 37.5a), caught before any program sees them (the first,
  fixed piece of the tiling control deferred in §6; not Super+Left/Right,
  which are line start/end on the Mac). Lisp gains `'split-narrow`.
* **Editing Lisp beside its canvas:** `e` runs in a split, evaluates the
  buffer **and stays** (result or error on its status line), and redraws
  through the program's redraw function when the canvas is lost. In the 38-
  and 48-column tiles code lines scroll sideways rather than wrap.

### 37.5a — Keys, the clipboard, and selection in every text input

1. **Keyboard** (`drivers/usbkbd.c`): Alt as an ESC prefix, Shift and Ctrl
   with arrows/Home/End/PgUp/PgDn (`ESC [ 1 ; m X` / `ESC [ n ; m ~`),
   Super as CSI-u, Ctrl-Space as NUL. Super+[ and Super+] are taken here and
   cycle the split's width (through the console's screen).
2. **One key-sequence parser** for all text inputs, replacing the line
   editor's ESC-then-two-bytes handling.
3. **Every text input on the shared line editor:** `ed`'s and the `lisp`
   REPL's readers and `e`'s prompts (file names, search) move onto it, so
   all of them get UTF-8, history where it fits, and the clipboard.
4. **The clipboard** in the kernel: up to 8 KB, allocated on first use;
   `/dev/clipboard` (like Plan 9's `/dev/snarf`: `cat`, Lisp files and 9P
   clients get it for free) and `(clipboard)` / `(clipboard-set s)`.
5. **Selection in the line editor:** Shift+movement or Ctrl-Space then
   movement, shown in reverse video; cut/copy/paste in both key families;
   Ctrl-K kills into the clipboard. Word movement (Alt-B/F, Ctrl-arrows).
6. **The split's third width and full-width text** (above): the text
   shadow keeps every row at the full width, so narrowing and widening the
   text tile loses nothing that was written wider.

**Done when:** on the panel, a word selected with Shift+arrows in the shell
is copied with Super+C and pasted with Super+V into `ed`, and the same
through the Emacs keys over the serial line; `cat /dev/clipboard` shows it;
Super+[ / ] cycle a Lisp demo's split through three widths, the demo
redrawing each time, and the text tile's earlier output intact after a
round trip. QEMU tests for the key parser (every sequence form), the
clipboard's file and limit, and selection editing.

**Done, 2026-10-01.** The owner used the clipboard in the shell, `ed` and
the `lisp` REPL, and resized, closed and swapped a demo's split on the
panel.

* **Keys** (`drivers/usbkbd.c`): Alt as an ESC prefix; Shift, Alt, Ctrl or
  Super with a navigation key as xterm's `ESC [ 1 ; m X` / `ESC [ n ; m ~`;
  Super with anything else as CSI-u; Ctrl-Space as NUL. Super+[ , Super+] ,
  Super+\\ and Super+Shift+3 are the screen's hotkeys: the keyboard hands
  them to the console (`console_hotkey()`), which runs them from its next
  input wait, outside every lock.
* **One parser** (`kernel/keyseq.c`): UTF-8 code points, controls, Alt,
  CSI and SS3 arrows, the `~` keys, xterm modifiers, CSI-u; a lone ESC is
  one that nothing follows within 30 ms; anything unknown is consumed whole.
* **The line editor** reads keys through it and has a **selection** (Shift
  with a movement, or Ctrl-Space's sticky mark; reverse video; typing,
  Backspace and Delete replace or remove it), **cut/copy/paste** in both
  families (Ctrl-W / Alt-W / Ctrl-Y and Super+X / C / V, Super+A, Ctrl-K into
  the clipboard), and **words** (Alt-B/F, Ctrl-arrows, Alt-Backspace, Alt-D).
  `readline_ex()` with options serves `ed`, the `lisp` REPL and `e`'s
  prompts, which all moved onto it; `e`'s own keys go through the parser
  too (its editing is 37.5b's).
* **The clipboard** (`kernel/clipboard.c`): 8 KB in two heap pages taken on
  first use; `/dev/clipboard` (read; a write at 0 replaces, at the end
  appends); `(clipboard)` and `(clipboard-set s)`.
* **The divider** -- the owner's design, refined on the panel: Super+[ and
  Super+] move it left and right through five places, text only, the
  38/48/64-column splits, canvas only (so they close either pane);
  **Super+\\** swaps the panes in a split, and with one pane full shows the
  other one full. In Lisp: `'split-narrow`, `(canvas-swap)`. Chess locks the
  layout. The text shadow keeps its full width, so a narrowed text tile
  loses nothing written wide. While the shell waits for a key it checks for
  a lost canvas ten times a second (`readline_set_idle()`), so a program's
  redraw runs at once. **Found on the panel:** the first version's keys were
  "text narrower/wider" (reversed once swapped), and the demos' redraw
  functions reset the layout, undoing the resize; both fixed, and Lorenz
  scales to the canvas's width.
* **Screenshots** (the owner's request, `kernel/screenshot.c`):
  `screenshot [file]`, `(screenshot)`, Super+Shift+3 -- a binary PBM, the
  frame bit for bit, to `/sd0/screenshots/shot-NNN.pbm`. Checked on QEMU by
  cutting one out of the SD image and opening it.
* **The test harness:** `board_config()` retries a `/proc/config` read
  that comes back empty -- the st7735/tm1638 "firmware predates" failures,
  which were the first suite run after a flash, not host load.
* **Logged, not reproduced:** one boot after a flash hung with USB up and
  the panel never started (`plan/open_issues.md`).
* **Cost:** static RAM +12 bytes on every RP2350 persona, +17 on the
  terminal; the clipboard's two pages only once used. `lcdterm`'s section
  12 228 of 16 384 bytes.
* **Tests:** `keyselftest` (11 cases), `usbkbdselftest` +8 (the encodings
  and hotkeys), `vtselftest` 32/32 (the divider's places, the swap, the
  lock, full-width text after a round trip); runner tests for selection and
  the clipboard through real key bytes in both families, the narrow split
  and a screenshot. QEMU suite 396/396; `test_rp2350` 25/25 straight after
  a flash.

### 37.5b — The editor: viewport, search, undo, the canvas beside it, and text mode

1. **A viewport:** `e` fills its tile (`console_size()`), scrolls instead
   of repainting, PgUp/PgDn, sideways scrolling for long code lines.
2. **UTF-8 editing** with 37.2's helpers.
3. **37.5a's selection and clipboard across lines.**
4. **Search and replace:** incremental search (Ctrl-S/Ctrl-R, Super+F/G),
   replace with confirm-each or all.
5. **Undo,** bounded (Ctrl-_ / Super+Z).
6. **Evaluate and stay,** with the canvas beside it and redraws on damage.
7. **Text mode -- the writer:** soft wrap, no line numbers, paragraphs as
   single lines on disk; chosen by extension.

**Done when:** (the writer, from phase 36's 36.12) a text of several pages
**with umlauts typed by compose** is written on the board from the
keyboard, saved to `/sd0`, the board is power-cycled, and the file reopens
intact; the same file opened on a PC shows paragraphs as single lines with
no inserted line breaks, and the umlauts correct; saving while the card is
removed reports an error and keeps the document in memory; the QEMU tests
for wrap/cursor and atomic save pass. **And** (the editor) `e` scrolls a
file longer than its tile without repainting it line by line; a Lisp
graphics program is edited, evaluated and re-evaluated in a split without
leaving `e`; search, replace, cut/paste across lines and undo work in both
key families.

**Done, 2026-10-01.** The owner wrote, evaluated and saved on the panel, with a 9P mount open.

* **A new editor** (`kernel/editor.c`, replacing the box in
  `line_editor.c`): full screen in the text tile at whatever size the
  console reports (80 x 24 on a serial line), following a resize while it
  runs. The buffer is one UTF-8 run on the heap, grown by doubling; every
  change is one primitive, so undo, the view and the selection follow it.
* **Drawing is a diff:** each frame hashes every visible row and sends only
  the changed ones; a row shift (scrolling, Enter, joining lines) is found
  first and done by the terminal with insert/delete-line inside a scroll
  region (rows 1 .. n-1, the status line below it). Typing a character
  sends its row and the status line; scrolling by one sends one row.
  `drivers/vtterm.c` learned DECSTBM, IL/DL and SU/SD for it
  (`fbtext_move_rows()`).
* **Code mode:** line numbers, long lines scroll sideways (by half a tile),
  Enter keeps the indent, Tab is two spaces. **Text mode -- the writer**
  (`.txt`, `.md`; Ctrl-X Ctrl-T switches): soft wrap after the last space
  that fits, no numbers, Up/Down/Home/End by screen row, a paragraph one
  line on disk.
* **Keys in both families** (editor.h lists them): selection with Shift or
  Ctrl-Space, across lines; cut/copy/paste through the clipboard; Ctrl-K
  kills collect; word moves and deletes; PgUp/PgDn, Alt-< / Alt->, go to
  line (Alt-G, Super+L).
* **Search:** incremental (Ctrl-S/Ctrl-R, Super+F), Super+G and
  Super+Shift+G again, wrapping once; smart case. **Replace:** Alt-% /
  Super+R, each match y / n / ! / . / q.
* **Undo:** 8 KB of inverse records, taken on the first edit; typing
  coalesces a word at a time; when full the oldest go (and undo says so).
* **Evaluate and stay:** Ctrl-X Ctrl-E (Super+Enter) evaluates the buffer,
  or the selection, and stays. What it prints is captured
  (`console_capture()`) and the status line shows the value, the last line
  printed, or the error -- "Unbound symbol", or the last `Error` line the
  Lisp engine logged meanwhile. The canvas's redraw runs while `e` waits
  for keys, as in the shell. The shell's Ctrl-X box is the same editor
  without an evaluator: Ctrl-X Ctrl-E hands the text back, as before.
* **The safe save:** the text goes to a copy first, then to the file, and
  the copy is removed once both writes succeeded -- then the file is
  checked once more and rewritten if it is gone. The copy is the file's
  name with the extension's **first** letter made `~` (`notes.~xt`,
  `lorenz.~isp`). The card's FAT32 cuts an extension to three letters, so
  any change past the third names the file itself: `notes.txt~` was found
  by the self-test, and the extension's *last* letter -- `lorenz.lis~`,
  which is `LORENZ.LIS` -- by the owner on the panel, where saving
  `lorenz.lisp` deleted it. Opening a file whose copy is still there says
  so. A failed save says so and keeps the buffer modified.
* **Found on the panel (owner, 2026-10-01), fixed:** in a 48-column tile a
  question on the status line was cut off -- a message now drops the file
  name, then the position, before itself; with the canvas full screen a
  question or prompt brings the text back (the Super+\\ toggle). A kernel
  log line (the first screenshot's "directory created") printed into the
  editor's text: while `e` runs the log's sinks are detached (the ring keeps
  everything, `/proc/kmsg`) and reattached on exit; what a canvas redraw
  prints while `e` waits is captured and dropped.
* **Found on the panel, fixed: a 9P connection made the editor take ~10 s
  a key.** The USB device took *any* port's DTR for the console's --
  `SET_CONTROL_LINE_STATE`'s wIndex was never read -- so a 9P client
  opening ACM1 "opened" ACM0. With nobody reading ACM0, its ring filled
  after ~50 scrolled lines and every byte the LCD-7 tees there waited out
  `usb_cdc_putc_wait()`'s 20 ms. Now only interface 0's DTR is the
  console's (both the kernel and the U-mode USB paths). Measured on the
  board, 15 KB of output with the console port closed and ACM1 held open:
  40.3 s before, 0.43 s after (0.43 s without ACM1 either way).
* **Tests:** `editselftest` (wrapping in four shapes, movement over wrapped
  rows, UTF-8 steps, undo by word and its overflow, search, the save and a
  failed save); `vtselftest` +2 (the region, IL/DL, SU/SD); runner tests
  for evaluate-and-stay, the writer (a wrapped paragraph with umlauts saved
  as one line), replace/undo/cut/paste across lines, and scrolling by
  delete-line. The C0 test now checks the screen handed back on exit.

### 37.6 — Documents

README: Lisp graphics, the chess board and the writer under the terminal
persona; the compose key in the stand-alone checklist (`tests/hw/README.md`).
`plan/hardware_seams.md`: the second canvas implementation noted (phase 36
§4.6's rule: extract at the third), and `console_size()` if it became a seam.
The Spleen notice extended to the glyphs this phase took from it.

**Done, 2026-10-01.** The README has phase 37 under the terminal persona
(and the Spleen notice says which glyphs are Spleen's and which are drawn
here); the stand-alone checklist has a step 9 for compose, the divider,
the clipboard, screenshots, the writer and the editor; `hardware_seams.md`
lists the screen as a device-class contract with its two implementations,
`console_size()` included.

### 37.7 — The Lisp open issues *(added 2026-10-02)*

Three entries `plan/open_issues.md` gained during 37.5b, fixed before the
next phase (the owner's call): no `set!`, Ctrl-C could not stop a `while`,
and a line longer than the editor's buffer was cut silently.

**Done, 2026-10-02.**

* **`set!`** (`user/lisp/lisp.c`): a special form that changes the innermost
  existing binding -- a global, a parameter, a `let`'s variable, and a
  closure that captured it sees the change; an unbound name is an error
  (`set!: unbound variable x`), so a typo cannot create a global. Its value
  is the new value. `(help)` lists it.
* **Calling what is not a function is an error.** An unknown operator says
  `Unbound function: frob`, a value that is not callable `Not a function: 5`,
  both before any argument is evaluated. Before, both handed back the form
  unevaluated -- which is how `(set! k (+ k 1))` "worked" and the loop
  around it never ended. No Lisp file in the tree relied on it.
* **Ctrl-C reached a `while` only after ~2^20 evaluator calls**: some 15 s
  of plain arithmetic on the RP2350, minutes when every iteration printed
  to the panel. The poll is by time now -- the clock every 256 calls, the
  console every 50 ms -- and `while` asks at the top of every iteration.
  The old rarity was for a console that threw away what it drained; it
  queues it since phase 36's pump.
* **Found while reproducing it: one Ctrl-C in `lsh` made every later line
  answer nil.** The latch was cleared only at evaluator depth 0, and inside
  `lsh` -- which runs inside the boot script's `(shell)` form -- the depth
  never gets back to 0. It is cleared at the collector's safe point, which
  every shell, REPL and `e` evaluation passes between commands.
* **An over-long line is refused, not cut** (`kernel/line_editor.c`): Enter
  right after input that found no room prints `Line too long: N bytes did
  not fit in 511 -- not entered` and enters nothing; any other key first
  (the person has seen the line, perhaps fixed it) and Enter means it. Also
  found: a key typed at the end of a full line was *echoed* though not
  stored, so the screen showed a line the buffer did not hold.
* **Found by the new refusal: the `lisp` REPL's line was 128 bytes.** The
  runner's own `while` test sends a 133-byte line; it had been cut all
  along and passed only because the reader forgives missing closing
  parentheses. The REPL's line is the shell's 512 now, taken from scratch
  (a heap page while the REPL is open) rather than from the shell's stack.
* **Cost:** static RAM +1 byte on every RP2350 persona (the poll's
  timestamp, with the call counter shrunk to a byte; the dropped count sits
  in what was padding). The gateway's baseline was exactly at a page
  boundary: the first cut, +12 bytes, cost it a 4 KB heap page.
* **Tests:** runner tests for `set!` (a counting `while`, a closure
  counter, the unbound-name error) and both operator errors; Ctrl-C
  stopping a `while` with the next line evaluating; a 600-byte line refused
  and the next one run. QEMU suite 411/412 (the one, the MQTT client test on RV32, is
  the known one-shot-broker flake); `test_rp2350` 25/25 on the LCD-7. On
  the board, Ctrl-C stops a busy `while` in 0.20 s and one that prints in
  0.24 s.

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
