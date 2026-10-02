# Phase 41 — The chess pane as a dashboard

**Status: noted, not scheduled (owner, 2026-10-02).** Raised during phase
38's 38.9, kept out of it: 38.9 is the chess search's speed only, and this
is a rewrite of what the chess console prints. Elaborated into a plan when
the phase starts.

## The problem

On the RP2350-LCD-7, `(chess)` runs in the wide split: the board on the
canvas, the chess console in a 38 x 27 text pane. The pane is a scrolling
teletype -- it still prints text board diagrams that the canvas already
shows, scrolls `info depth ...` lines while the engine thinks, and long
output (`help`, `moves`, `games`) wraps at 38 columns and scrolls the game
out of sight.

## Decided (owner, 2026-10-02)

The pane becomes fixed sections, drawn with cursor addressing and a scroll
region -- both already in `vtterm` (37.5b, the editor uses them), and the
font has the chess figurines (U+2654..265F):

1. **No text board.** The engine's current best move is shown *on the
   canvas board* while it thinks, as outlined from and to squares.
2. **Search status**, about three rows, rewritten in place at each finished
   depth: depth, score, best line (and nodes/time). Replaces the scrolling
   `info depth` lines.
3. **Game history** in figurine SAN, two moves a row (`12. ♘f3  ♝c5`), the
   latest moves in view.
4. **Command entry** in the bottom 5-6 rows, a scroll region. Long output:
   `help` becomes a compact list with `help <cmd>` for the detail, `moves`
   prints in columns, and anything still longer takes over the pane above
   the prompt until the next command or key, then the dashboard returns.
   Not over the board: the board stays visible while playing.

**Where:** only when chess has its 38-column split. On a serial console,
on QEMU, and on boards without a screen, the line-oriented output stays as
it is.

**The USB link:** the console is teed to USB ACM0, and a host terminal
would receive cursor movements meant for a 38 x 27 pane. As soon as the
output is formatted, it is **dashboard-only**: the formatted output goes to
the panel and not to the tee.

## To settle when the phase starts

* Whether the outlined squares need a canvas primitive (an outline exists:
  request `'o'`) or only chess's own board code.
* How the tee is suppressed for the dashboard's writes -- a console flag
  around them, or chess writing to the screen through `lcdterm` directly.
* Redraw: the dashboard is repainted on entry and after damage, like the
  board.
