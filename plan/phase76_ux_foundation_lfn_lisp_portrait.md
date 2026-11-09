# Phase 76 — Touch UX Foundation III: Long File Names, `ugui` for Lisp, the Portrait Ribbon

**Status: PLANNED (split from phase 71 on 2026-11-09; its milestones were 71.6b, 71.4b and 71.5 there).**
**Target Hardware**:
- **Primary**: M5Stack Tab5 (`esp32p4-tab5`, portrait and landscape), Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`)
- **Also**: Waveshare RP2350-LCD-7 (`rp2350-terminal`: long names and Lisp `ugui`, no portrait), StarFive VisionFive 2 (`visionfive2`: long names on `/sd0` and `/nvme0`)
- **Regression**: every FAT target builds; RP2350 SRAM does not grow.

**Follows**: Phase 71 (compositor, input pipeline, `ugui` v2, view framework, system events, registry, icons, settings store, text input, mouse).
**Needed by**: phase 77 (the Files face wants long names), phase 78 (the book tree's directory names), phase 74/75 (`/nvme0` inherits long names).

| Old (phase 71) | New |
| :--- | :--- |
| 71.6b FAT32 long file names | 76.1 |
| 71.4b `ugui` for Lisp programs | 76.2 |
| 71.5 Portrait major-axis ribbon | 76.3 |
| — (new, 2026-11-09) window controls by touch and mouse | 76.4 |
| 71.4's damage tracking (optional) | 76.5 |
| 71.11, for this phase's items | 76.6 |

---

## 1. Design

### 1.1 Portrait: the major-axis ribbon

- `ribbon_t` gets an `axis` field (`RIBBON_AXIS_X` | `RIBBON_AXIS_Y`). `rx0`/`rx1`/`x_view` become major-axis coordinates (renamed `m0`/`m1`/`m_view`). Window size along the major axis is counted in text rows when the axis is Y (`RIBBON_ROWS_*` next to `RIBBON_COLS_*`).
- **Portrait defaults (Tab5 720×1280):** one window per screen. `Cmd+\` (maximise toggle) switches between one-up and a **top/bottom pair** (about 18 terminal rows each at 16×32). Never side-by-side slices.
- **Navigation:** swipe on chrome or the edge band along Y (rule 1 of phase 71 §2.2). `Cmd+Up/Down` in portrait, and `Cmd+Left/Right` keep working in both orientations as "previous/next window" (so muscle memory carries over). The minimap moves to a vertical strip at the right edge of the menu bar area, or stays horizontal as window order. Decided with a mock-up on the device.
- **Rotation keeps the window order and focus.** Landscape pairs become portrait stacks. A one-up portrait window stays one-up.
- The 7B and RP2350 never use `AXIS_Y`. On RP2350 the field is constant and folds away.

### 1.2 Window size and order by touch and mouse

Sizing (`Cmd+[`, `Cmd+]`, `Cmd+\`) and order (`Ctrl+Cmd+Arrows`) are keyboard-only today. Everything below sits on the title bar, which is chrome: by phase 71 §2.2 rule 1 its gestures always go to the window manager, so no view claim is affected. It stays in the Classic Mac look, so it is not the deferred touch-first chrome (§3).

- **Zoom box** at the right end of the title bar, opposite the close box: a tap or click is `Cmd+\` (`ribbon_toggle_maximize()`). In portrait it switches between one-up and the top/bottom pair.
- **Title-bar menu:** a long press or right click on a title bar opens a menu: **Width** 2/5 · 1/2 · 2/3 · Full (the current one ticked; in portrait: One-up · Pair), **Move Left / Move Right** (portrait: Up / Down), **Close**. Each item shows its shortcut, Mac-style, so the menu teaches the keyboard. Items that cannot apply (Move Left on the first window) are dimmed. The menu uses the action-sheet code the launcher already has.
- **Grow box (optional part):** a grip at the far end of the title bar along the major axis. Dragging it draws a gray outline that snaps to the width steps; the width is applied on release (`ribbon_layout()` once). Only the outline and chrome are redrawn during the drag, no view. Its hit area is enlarged beyond what is drawn; on the 7B (`CHROME_SCALE = 1`, title bar about 3 mm) it is measured on the device first, and dropped there if it cannot be hit reliably next to the zoom box.
- **Not in this phase:** reordering by dragging a title bar or minimap blocks. The menu covers order; drag reorder is reconsidered after 76.4 is in use.

---

## 2. Milestones

### 76.1 — FAT32 long file names
- **Why:** `fs/fat32.c` is 8.3-only. It skips VFAT long-name entries on read, so files named on a PC appear as their aliases (`LUGALO~1.MD`), and it truncates on write, so two long names sharing their first 8 characters land in one file (`tests/hw/lcdterm_shots.py` works around it). The Files face (77.2), the book tree (78.2) and readable document names all want real names.
- **Scope:** read and write VFAT long names: UTF-16 entries assembled to UTF-8 paths in the VFS (255 UTF-16 units max); case preserved, matching case-insensitive for ASCII and Latin-1; a name that fits 8.3 is stored as 8.3 only; a long one gets its entry set with checksum and a unique `~N` alias; delete and rename remove or rewrite the whole set. Characters outside Latin-1 are stored exactly and shown as a replacement glyph until fonts cover them (78.2 brings Latin-1).
- **Large volumes (`/nvme0`, 256 GB):** about 8 million clusters at 32 KB (a 32 MB FAT). Allocation starts from the FSInfo next-free hint and keeps the free count there, rather than scanning the FAT; directory walks stay linear in the directory, not in the volume. Checked on the 64-bit build too (the host harness builds for both widths).
- **Cross-phase:** phase 74 mounts `/nvme0` through `block_dev_t` and `fat32_init()` and leaves `fs/fat32.c` to this milestone; `/nvme0` gets long names when it lands.
- **Decision for the owner, before it starts:** which targets. Proposal: the SD + PSRAM boards (Tab5, LCD-7B, NANO, RP2350-LCD-7) and the VisionFive 2 at least; whether the code stays shared for every FAT target (one code path) or is compiled out where flash is tight (Pico 2 presets, C6) is measured first (`sizereport`, flash delta).
- **Acceptance:** host harness (`tests/host`, ASan/UBSan) on images made by `mkfs.fat`/`mtools` with long, mixed-case and Latin-1 names: listed, opened and written back exactly; files created here read correctly by `mtools` and `fsck.fat -n` is clean; alias collisions resolved; `p9sync.py` syncs a tree with long names to `/sd0`, and to `/nvme0` on the VisionFive 2; creating, listing and deleting in a directory of 1000 long names stays fast on a 256 GB image (sparse, in the host harness); the screenshot test drops its 8.3 note.
- **Status:** planned.

### 76.2 — `ugui` for Lisp programs
- Lisp primitives over the phase 71 widgets, in the immediate-mode model the toolkit already uses: the program owns its loop, `(ui-begin)`, one call per widget (drawn into the program's own surface, its hit-box registered: `(ui-button id "OK" x y w h)`, cards, lists, steppers, text fields...), `(ui-end)` presents; `(ui-event)` takes the next event from the window's own ring, `(ui-hit ev)` names the widget it hit. No callbacks from system tasks into the program.
- The surface: a canvas window (GRAY8 or RGB565, as the board's surfaces are), so a Lisp UI app is a ribbon window like any other and gets focus, keyboard parity and the full-screen convention if it asks for full screen. Its text fields get the on-screen keyboard and the clipboard as the views' do (71.6).
- Primitives in the builtins table (`.rodata`, sorted), documented in `doc/dev/on_device/lisp_scripting.md`; a sample app ("hello ugui": a few controls and a list) in `tools/sd_root/demos/`.
- **Acceptance:** the sample runs by touch, by mouse and by keys only on the Tab5, the 7B and the RP2350-LCD-7; QEMU's RAM screen runs it in the test suite with injected events.
- **Status:** planned.

### 76.3 — Portrait major-axis ribbon
- `ribbon_t.axis`, renamed major-axis fields, row-based sizes; one-up and top/bottom pair in portrait; rotation preserves order and focus (§1.1).
- Gesture arbitration along Y; `Cmd+Up/Down`; minimap variant chosen on the device.
- **Acceptance:** `tests/test_ribbon.py` covers both axes (layout, viewport, insert/remove, rotation round trip); on the Tab5 portrait a terminal + reader pair is usable by touch and keys; landscape behaviour unchanged.
- **Status:** planned.

### 76.4 — Window size and order by touch and mouse
- **Why:** the ribbon's width steps, maximise and window order are keyboard-only, which leaves the Tab5 and the 7B without a keyboard unable to change them (owner, 2026-11-09).
- **Scope:** zoom box, title-bar menu (width, move, close) by long press and right click, and the optional grow box, on both ribbon axes (§1.2). Built on 76.3's major-axis fields, so it is written once for landscape and portrait. Shared by every panel with chrome: Tab5, 7B, RP2350-LCD-7 (mouse; touch where present), VisionFive 2 (mouse).
- **Acceptance:** `tests/test_ribbon.py`: the menu's width and move actions give the same layout as their shortcuts on both axes. Runner test on QEMU's RAM screen with `ms click` / `ms click right` / `ms drag` and `ts tap` / `ts` long press: zoom box, every menu item, grow-box snapping to each step, dimmed items. On the Tab5 (both orientations) and the 7B: every action by finger and by mouse; a horizontal swipe on a title bar away from the grow box still pans the ribbon. RP2350 SRAM does not grow.
- **Status:** planned.

### 76.5 — Damage tracking in `ugui` (optional)
- `ugui_end()` returns the union of rectangles whose state changed since the previous pass (widget id + state hash per hit-box); the view hands it to the compositor, so a stepper tap recomposites a few percent of the screen instead of the whole window.
- Taken up only when a view needs it: phase 71's double-buffered surfaces and partial redraws made it an optimisation. Measure first with `uxbench` (launcher redraw on the 7B, now dominated by the launcher's own drawing).
- **Acceptance:** a measured gain on a named interaction; `uguiselftest` checks the damage union; glass check (`lcd check`) clean.
- **Status:** optional, not scheduled.

### 76.6 — Verification and documentation
- `tests/hw/lcdterm_shots.py`: a portrait pair scenario; a long-name directory listing; the title-bar menu open, and the grow-box outline mid-drag.
- `tests/hw/test_esp32p4.py`: a gesture-claim check for portrait.
- Docs: `doc/user/ribbon.md` (portrait; zoom box, title-bar menu, grow box), `doc/dev/on_device/lisp_scripting.md` (the `ui-` primitives), `doc/admin` storage notes for long names; `python3 tools/gendocs.py --all` and `--check`.
- RAM baselines re-recorded; `sizereport` for RP2350 shows no SRAM growth.
- **Status:** planned.

---

## 3. Out of scope — future extensions

- **UX 2.0: touch-first chrome** (owner, 2026-11-06): window decoration and UI sizing redesigned around touch — menu bar, title bars, close box and minimap at ≥ 7 mm, and an on-screen keyboard that stays usable in a half-width window (e.g. a floating or full-width keyboard over a split). Deferred: it clashes with the Classic Mac look. What fits that look is done: touch areas grown past what is drawn where nothing else is there, the keyboard's gaps going to the nearest key.
- **On-screen keyboard diacritics** (owner, 2026-11-06): a long press on a character key shows its variants in a strip above it (a → à á â ä å æ …, e → è é ê ë, s → ß, …), the finger slides to one and lifts to type it as UTF-8. Needs 78.2's Latin-1 fonts and UTF-8 decoding in the terminal; the long press on Delete and the arrows stays key repeat.
- **`ugui` for C programs (chibicc).** C programs run in U-mode under memory domains, so this needs its own design: `ugui` as a user-space library compiled into the program (it is freestanding and allocation-free; the question is the font tables' visibility from U-mode), a surface mapped into the program's domain, and an event syscall for the program's window. Same immediate-mode API as 76.2's Lisp one. Decided later whether and when (owner, 2026-11-06).
- Multi-touch (pinch zoom) — the ST7123 reports up to 10 points, the GT911 5; the recogniser reads only the first point, but its contact model is written so a second point can be added.
- Colour UI accents on RGB565 surfaces.
