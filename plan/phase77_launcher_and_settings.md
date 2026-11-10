# Phase 77 — The Launcher's Two Faces and the Settings App

**Status: PLANNED (split from phase 71 on 2026-11-09; its milestones were 71.6 and 71.7 there).**
**Target Hardware**:
- **Primary**: M5Stack Tab5 (`esp32p4-tab5`), Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`)
- **Also**: Waveshare RP2350-LCD-7 (`rp2350-terminal`, reduced: phase 71 decision 5), StarFive VisionFive 2 (`visionfive2`, mouse and keyboard)

**Follows**: Phase 71 (view framework, registry and associations, icons and the icon grid, settings store, text fields with the on-screen keyboard and clipboard, the mouse). **Needs**: 76.1 (long file names) for the Files face.

| Old (phase 71) | New |
| :--- | :--- |
| 71.6 Settings app | 77.1 |
| 71.7 Launcher: Shortcuts and Files (without the Books root) | 77.2 |
| 76.5 Damage tracking in `ugui` (moved here 2026-11-10) | 77.3 |
| 71.11, for this phase's items | 77.4 |

The Books root as a cover grid moved to 78.2, beside the catalog it reads.

---

## 1. Design: the launcher's two faces

The launcher is the home view, with a tab bar for **Shortcuts | Files** (remembered in `launcher.face`). Settings leaves the launcher and becomes its own app with a shortcut tile.

**Shortcuts face**
- Icon grid (`ugui_icon_grid()`, 71.5) of entries from `/sd0/system/SHORTCUT` (text, one per line: `type<TAB>target<TAB>label<TAB>icon`; types `app`, `doc`, `dir`, `book`). Defaults come from `/flash0` when the SD file is missing.
- Tap/Enter/click opens the entry. Long press, right click or `Menu`/`m` opens an action sheet: Open, Rename, Remove, Move left/right. Removing asks for confirmation.
- A "Continue reading" tile appears when a book has a stored position (78.3).

**Files face**
- Real directory traversal starting at a **roots** list (`/sd0`, `/flash0`, `/ram0`, `/nvme0` where present, mounted 9P trees). Path bar with tappable segments; `Backspace`/`←` goes up.
- Virtual list rows: icon by association, display name, size, and type badge. Display names are the long file names (76.1), or from metadata where that is better (book catalog, `# H1` of Markdown files, cached).
- Tap/Enter opens by association. Action sheet: Open, Open with…, Add to shortcuts, Info, and (with confirmation) Delete. Copy/rename are a follow-up unless cheap through existing VFS calls.
- Sorting: directories first, then name.

**Keyboard map (launcher and all `ugui` apps):** Tab/arrows move focus, Enter opens, Esc goes back, `Cmd+1/2` switches faces, `/` focuses search (later), `Cmd+Space` opens the launcher from anywhere (new global hotkey; `Cmd+L` as alternative if `Cmd+Space` collides with the keyboard layout).

---

## 2. Milestones

### 77.1 — Settings app
- Own view: Display (brightness, orientation, text size), Sound (volume, click sound, test beep), Input (OSK preview, keyboard and pointer status, pointer speed `input.pointer_speed`, natural scrolling `input.natural_scroll`), Power (battery, charger, INA226 readings), Network (Wi-Fi state, IP), Storage (mounted volumes, free space), About (version, board, chip revision).
- `WIN_EV_SETTINGS`: a setting a view depends on changed (text size), told to every view and window.
- Fully keyboard-operable through the `ugui` focus, and by mouse.
- **Acceptance:** each setting by touch and by keys only; persistence across a reboot; `WIN_EV_SETTINGS` reaches the reader (a text size change relayouts it).
- **Status:** planned.

### 77.2 — Launcher: Shortcuts and Files
- Tab bar; Shortcuts face with the persisted list, action sheet, defaults from `/flash0` (§1).
- Files face: roots, traversal, path bar, virtual list, association icons, long and metadata names, open/open-with/add-to-shortcuts/info/delete-with-confirm.
- `Cmd+Space` global hotkey.
- **Narrow windows** (owner, 2026-11-10, after 76.4b): the launcher's widgets run into each other when its window is made small (a 2/5 column, a third of a stack's height -- the sizes the menu and the handles give now). The redesigned faces lay out for every size the ribbon gives a window, down to `RIBBON_TERM_MIN_COLS` across and `RIBBON_ROWS_MIN` rows down: fewer columns of icons, then a list.
- **Acceptance:** a scripted keyboard-only session (QEMU with the RAM screen) browses from `/sd0` to a Markdown file, opens it, adds it as a shortcut, removes the shortcut; the same sequence by `ts` on the Tab5 and by `ms` with a mouse; a directory with 500 entries scrolls at frame rate.
- **Status:** planned; needs 76.1.

### 77.3 — Redraw only what changed in `ugui` (from 76.5)
- **Why here** (owner, 2026-11-10): the settings app and the Files face are the first views with many controls and a long list; 76.5 had no use case to measure on.
- **Measure first**, with `uxbench` on the Tab5 and the 7B: the launcher's redraw, a stepper tap, the Files list scrolling -- each stage apart (the view's own drawing, the 1-bpp conversion, the compositor's copy of the surface). In 76 the launcher's redraw on the 7B was dominated by its own drawing, which damage tracking alone does not reduce.
- **Then the part the numbers ask for:**
  - compositing dominant: `ugui_end()` returns the union of the rectangles whose state changed since the previous pass (widget id + state hash per hit box); the view hands it to the compositor, which copies only that (76.5 as planned);
  - drawing dominant: `ugui` skips drawing a widget whose state hash is unchanged since the last pass -- which yields the same rectangles, so the compositor's part comes with it.
- **Correctness first:** a partial pass must equal a full redraw, pixel for pixel -- `uguiselftest` compares both on QEMU's RAM screen, and on the boards a check that reads the panel's frame, not the caches (76.4a: `lcd check` trusted the same cell cache it was to check, and missed the Tab5's stale cells).
- **Acceptance:** a measured gain on the named interactions on the Tab5 and the 7B; the 500-entry list of 77.2 scrolls at frame rate; the partial-vs-full check clean on QEMU and the boards.
- **Status:** planned; after 77.1 and 77.2 have views to measure.

### 77.4 — Verification and documentation
- `tests/hw/test_esp32p4.py`: launcher faces and settings persistence checks.
- `tests/hw/lcdterm_shots.py` scenarios: launcher both faces, settings.
- Docs: new `doc/user/launcher.md`; `doc/user/shell.md` for any new commands; `python3 tools/gendocs.py --all` and `--check`.
- RAM baselines re-recorded; `sizereport` for RP2350 shows no SRAM growth.
- **Status:** planned.

---

## 3. Out of scope — future extensions

- File copy/move/rename across volumes; search across the catalog.
- SD-provided app manifests beyond the minimal form in phase 71 §2.5.
