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
- **Paper warmth** (owner, 2026-11-11), under Display: one slider `display.warmth`, 0..10, from white paper to warm, e-reader-sepia paper. Not a color picker.
  - **Mechanism:** every screen is 1-bpp and gets its color from a foreground/background pair at output: `lcd7_set_colours()` on the RP2350 (instant, PIO pin logic), `term_colours()` on the P4 (LUT rebuild + full redraw, about 100 ms), `dc8200_build_lut()` on the VF2. Warmth only computes that pair. The anti-aliased text ramp (16-step fg/bg blend in `dsi_esp32p4.c`) follows the pair on its own.
  - **Curve** (owner, 2026-11-11, after seeing it: "more sepia in the high settings"): two stages, integer interpolation per RGB channel (no floating point). 0..5: white to a light warm paper `#F4E8D0` (`0xf75a`), the first version's whole range; 5..10: on to a deep, aged sepia `#E3CCA0` (`0xe674`).
  - **Ink:** a second setting, `display.warm_ink` (on by default): the ink goes from black to dark brown `#3B2A1A` (`0x3943`) by 5 and keeps it. Pure black on cream looks harsh, and sepia themes (reading apps, e-readers) warm the text too. Contrast at 10: 8.8:1 with brown ink, 13.4:1 with black, both above WCAG AAA (7:1). Off keeps black ink for users who want the most contrast.
  - **Live preview:** each slider step applies at once. On the P4 a step redraws the whole screen, so dragging applies at most every ~150 ms and always applies the last value on release.
  - **Applied at boot** when the settings store is read (shell start), before the launcher or any view opens. The panel and its boot log come up earlier, before the SD card is read, so the boot log shows in black on white and the screen warms when the shell starts.
  - **Cost on the 7B:** any non-gray pair moves the frame from GRAY8 to RGB565 (`term_colours()`), so warmth > 0 costs scan-out and conversion bandwidth there. Measure it in 77.3. The Tab5 always runs RGB565 and the RP2350 pays nothing.
  - **Out of scope:** warmth for an inverted (dark) screen, an automatic schedule ("warmer after sunset"), per-app colors.
- **Acceptance:** each setting by touch and by keys only; persistence across a reboot; `WIN_EV_SETTINGS` reaches the reader (a text size change relayouts it); warmth 0 gives exactly `0x0000`/`0xffff` (7B back in GRAY8); warmth 10 gives the endpoint pair (`0x3943` / `0xe674`), checked through `lcd` status output on the RP2350, the Tab5, the 7B and the VF2. The 1-bpp screenshot scenarios are unchanged.
- **Menu bar** (owner, 2026-11-11): a gear at the menu bar's right end, always drawn (the clock shows only once the time is set, the battery only where there is one), opens the app as it was left; the SD icon at Storage, Wi-Fi and 9P at Network, the battery text at Power, the clock as the gear. `Cmd+,` as the gear. `drivers/screen.c`'s `status_hit()`, `SCREEN_REPLY_SETTINGS`; `ribbon list` prints the status area's ranges (screen request `'z'`).
- **Status: done 2026-11-11.** QEMU 790/790 (the app by keys, its event, the narrow list; text size relayouts the reader; the gear, the SD icon and `Cmd+,`). Boards on the final build: Tab5 and 7B 13/13 steps (with the glass check in the warm colours), RP2350-LCD-7 and VF2 10/10; text size measured in the reader on the Tab5, the 7B and the VF2. The RP2350 suite's one failure (38.8, first run after a boot) predates this phase: `plan/open_issues.md`.
  - **Done:** the Settings app (`drivers/settings_view.c`, `RIBBON_WIN_SETTINGS`; `settings open`, the app registry's `settings`, More... on the launcher's quick settings) with all seven sections, by touch, mouse and keys, wide (list beside the section) and narrow (list, then the section with Back). Paper warmth (`drivers/paper.c`) on the RP2350, both P4 panels (views tinted through `g_gray565`) and the VF2. `WIN_EV_SETTINGS` sent to every window. QEMU: `settings check` covers the colour pairs; the runner drives the app by keys (warmth, its event, narrow list and Back, Cmd+W). Boards: `tests/hw/settings_app.py` in the P4, RP2350 and VF2 suites (pair read back from `lcd`, P4 glass check, the app by keys), not yet run on hardware.
  - **Text size (owner, 2026-11-11: three sizes on the boards with flash, one on the RP2350):** small / medium / large are 13 / 16 / 24 px on 1x panels (the 7B, QEMU, the VF2) and 24 / 32 / 40 px on the Tab5; medium is the board's own face, unchanged. `tools/gen_serif_faces.py` renders the extra sizes from Gentium Book exactly as the 32 px face was rendered (it reproduces that face byte for byte), 4-bit coverage only; `ugui_face()` / `ugui_face_text()` draw them; each build links its pair (36 KB of flash at 1x, 103 KB on the Tab5). The reader re-reads the size at each full draw, tags its checkpoints with it, and relayouts on `WIN_EV_SETTINGS` at about the same place. Code keeps the board's monospace size (in Small its boxes are taller than the text: to revisit with phase 78's book layout). QEMU: the runner checks small < medium < large document heights and medium again exactly.

### 77.2 — Launcher: Shortcuts and Files
- Tab bar; Shortcuts face with the persisted list, action sheet, defaults from `/flash0` (§1).
- Files face: roots, traversal, path bar, virtual list, association icons, long and metadata names, open/open-with/add-to-shortcuts/info/delete-with-confirm.
- `Cmd+Space` global hotkey.
- **Narrow windows** (owner, 2026-11-10, after 76.4b): the launcher's widgets run into each other when its window is made small (a 2/5 column, a third of a stack's height -- the sizes the menu and the handles give now). The redesigned faces lay out for every size the ribbon gives a window, down to `RIBBON_TERM_MIN_COLS` across and `RIBBON_ROWS_MIN` rows down: fewer columns of icons, then a list.
- **Acceptance:** a scripted keyboard-only session (QEMU with the RAM screen) browses from `/sd0` to a Markdown file, opens it, adds it as a shortcut, removes the shortcut; the same sequence by `ts` on the Tab5 and by `ms` with a mouse; a directory with 500 entries scrolls at frame rate.
- **Owner decisions (2026-11-11):**
  - `Ctrl+Tab` / `Ctrl+Shift+Tab` switch the faces, not §1's `Cmd+1/2`: those are the window manager's jumps (Cmd+1..9).
  - `Shift+Enter` opens the focused item's action sheet in both faces; `m` does too in Shortcuts. In Files the letters are type-ahead (a letter jumps to the next name with it), so §1's `m` cannot be the menu key there.
  - The quick settings and the application cards left the launcher: the applications are default shortcuts (Settings among them), and the menu bar's gear opens Settings.
  - The shortcuts file is lowercase, `/sd0/system/shortcuts` (else `/flash0/system/shortcuts`, else built-in defaults), per the card-naming rule.
- **Done (2026-11-11), tiers 0-2:** the two faces (`drivers/launcher_view.c`), the shortcuts list and file (`drivers/shortcuts.c`, `shortcuts` in the shell), the roots from `vfs_root_at()`, `Cmd+Space`. Files: dirs first, natural order, Markdown by its first heading, up to 512 entries; type-ahead, PgUp/PgDn/Home/End move the focus. Narrow windows: fewer grid columns, a list below two. QEMU `quick.py launcher`: keys-only Files to a document, a shortcut added and removed; rename; 500 entries listed and paged; the mouse. Boards (`launcher_faces.py`, pointer only): Tab5, 7B and RP2350-LCD-7 24/24 steps by touch and mouse, VF2 12/12 by mouse. The 500-entry list's frame rate is 77.3's to measure.
- **Status: done 2026-11-11**, on the tier 3 run: QEMU 799/799 (all targets); full board suites Tab5 67/67, 7B 67/67, RP2350-LCD-7 40/40, VisionFive 2 26/26. Found on the way, not 77.2's: the VF2's cursor test still expected the output from before phase 71 (4c9c8f36), updated; this host's firewall blocked the MQTT test broker on TCP 1883 (owner opened it).

### 77.3 — Redraw only what changed in `ugui` (from 76.5)
- **Why here** (owner, 2026-11-10): the settings app and the Files face are the first views with many controls and a long list; 76.5 had no use case to measure on.
- **Measure first**, with `uxbench` on the Tab5 and the 7B: the launcher's redraw, a stepper tap, the Files list scrolling -- each stage apart (the view's own drawing, the 1-bpp conversion, the compositor's copy of the surface). In 76 the launcher's redraw on the 7B was dominated by its own drawing, which damage tracking alone does not reduce.
- **Then the part the numbers ask for:**
  - compositing dominant: `ugui_end()` returns the union of the rectangles whose state changed since the previous pass (widget id + state hash per hit box); the view hands it to the compositor, which copies only that (76.5 as planned);
  - drawing dominant: `ugui` skips drawing a widget whose state hash is unchanged since the last pass -- which yields the same rectangles, so the compositor's part comes with it.
- **Correctness first:** a partial pass must equal a full redraw, pixel for pixel -- `uguiselftest` compares both on QEMU's RAM screen, and on the boards a check that reads the panel's frame, not the caches (76.4a: `lcd check` trusted the same cell cache it was to check, and missed the Tab5's stale cells).
- **Acceptance:** a measured gain on the named interactions on the Tab5 and the 7B; the 500-entry list of 77.2 scrolls at frame rate; the partial-vs-full check clean on QEMU and the boards.
- **Measured (2026-11-11)**, `uxbench shell.md /sd0/uxb500` (a directory of 500 files) on the Tab5 (landscape, 270) and the 7B, both double-buffered. `uxbench` now times the launcher's and the Settings app's passes by stage (`uxbench_view_note()`: the view's own drawing, its `'W'` copy to the screen, the wait for the compositor's buffer) beside the frame stages it had; new rows `files`, `fscroll` (the Files list dragged 24 px a step) and `stepper` (a tap on the pointer-speed stepper: press, release, tap, as the console sends them). Medians per action, ms:

  | Row | Board | Input to glass | View draws | Copy (`'W'`) | Frame | of which composite |
  | :--- | :--- | ---: | ---: | ---: | ---: | ---: |
  | launcher (Shortcuts) | Tab5 | 122 | 28.4 | 7.4 | 71 | 67.4 |
  | | 7B | 50 | 12.3 | 4.4 | 17 | 15.2 |
  | files (redraw, 500) | Tab5 | 140 | 43.8 | 6.9 | 72 | 67.6 |
  | | 7B | 66 | 24.6 | 5.0 | 17 | 15.2 |
  | fscroll (a step) | Tab5 | 140 | 43.3 | 7.3 | 76 | 67.9 |
  | | 7B | 66 | 24.9 | 4.3 | 18 | 15.3 |
  | stepper (a tap) | Tab5 | 53 | 12.6 in 3 passes | 21.1 | 4 | -- |
  | | 7B | 33 | 8.3 in 3 passes | 12.2 | 1.7 | -- |

  The 1-bpp conversion is under 3 ms everywhere: the launcher is a GRAY8 surface, and the conversion skips what a surface covers. Input to glass moves in steps of one panel frame (16.7 ms on the 7B, about 17.5 ms on the Tab5): 66 ms is four frames, 140 ms eight.
- **What the numbers say:**
  - **Tab5: compositing dominates** -- the launcher's GRAY8 surface into the RGB565 frame, on the CPU (the frame record shows no mirror and no PPA time for it), 67 ms of 122-140, for every pass, whatever changed. The view's drawing is second (28-44 ms).
  - **7B: drawing dominates** -- 12-25 ms against a 15 ms composite and a 4-5 ms copy.
  - **The launcher draws everything twice on the P4**: ugui writes each widget to the 1-bpp backing and to the GRAY8 surface, and `'W'` then copies the 1-bpp backing to the screen canvas, under the surface where the conversion skips it. Measured by drawing GRAY8 only (an experiment, not kept): the 7B's launcher 12.3 -> 8.5 ms, Files 24.6 -> 17.3 ms, input to glass 50 -> 33 and 66 -> 50 ms (a frame less each); the Tab5's 28 -> 19 and 44 -> 32 ms (no change on the glass: its composite still decides). The copy (4-7 ms) would go too. What still reads the launcher's 1-bpp picture on the P4 (the 1-bpp screenshot scenarios, `uxbench`'s screen check) is to settle first.
  - **The Settings tap draws three times**: the press, the act, and again on its own WIN_EV_SETTINGS, which shows the same picture as the act. Its 1-bpp copy (7 ms a pass on the Tab5) costs more than its drawing (4 ms).
  - **Scrolling is not helped by damage tracking**: every row moves, so the union is the whole list. Frame rate (16.7 ms) is 4x away on the 7B and 8x on the Tab5; it needs the reader's way (move the surface, draw only the uncovered rows) and, on the Tab5, a composite that moves pixels rather than converting the window again.
- **Owner decision (2026-11-11): deferred.** None of the parts gives a change a user would notice: (1)-(3) save at most a frame on the 7B (launcher 50 -> 33 ms) and nothing on the Tab5's glass, and a stepper tap at 33-53 ms is already immediate. The one thing a user does notice -- the Files list scrolling at 7 (Tab5) to 15 (7B) frames a second -- damage tracking does not touch. So 77.3 ends with the measurements and `uxbench`'s new rows; the options stay here for when they are wanted:
  1. the launcher GRAY8-only on the P4 and no 1-bpp copy under a surface (first: what still reads its 1-bpp picture -- the 1-bpp screenshot scenarios, `uxbench`'s screen check);
  2. one pass per Settings tap (the act and its own WIN_EV_SETTINGS merged);
  3. ugui's skip-unchanged widgets, the changed rectangles handed to `'W'` and the compositor (the second variant above, which carries the first) -- for taps, focus, hover, on the Tab5 most;
  4. scrolling by moving the surface and drawing only the uncovered rows, as the reader does, with a composite that moves pixels on the Tab5. The acceptance "the 500-entry list scrolls at frame rate" goes with it.
- **Found on the way, not 77.3's:** `uxbench`'s screen check reports rows 0..15 changed "during the check itself" on several rows on both boards: something drew into the menu bar while it compared (most likely the clock; not looked into), not a stale screen: the glass check, which reads the panel's frame, matches each time.
- **Status: done 2026-11-11 as a measurement; the redraw work deferred** (owner decision above).

### 77.4 — Verification and documentation
- `tests/hw/test_esp32p4.py`: launcher faces and settings persistence checks.
- `tests/hw/lcdterm_shots.py` scenarios: launcher both faces, settings.
- Docs: new `doc/user/launcher.md`; `doc/user/shell.md` for any new commands; `python3 tools/gendocs.py --all` and `--check`.
- RAM baselines re-recorded; `sizereport` for RP2350 shows no SRAM growth.
- **Version** (owner, 2026-11-11): the version number goes up -- `kernel/include/kernel/version.h` (`LUGALOS_VERSION` and its parts), 0.20.0 to 0.21.0 (a minor step: new apps and settings, nothing incompatible) -- and every preset is rebuilt so `/proc/version` and `/proc/buildid` say so.
- **Release, the final step** (owner, 2026-11-11): once everything above passes -- the phase's tier 3 run (`doc/dev/testing.md` §0) -- a new GitHub release `0.21.0` is published by `doc/dev/releasing.md`, tagged on the commit that closes the phase, with release notes in the style of the earlier ones (0.20.0: "Touch UX Mode, Classic Mac Micro-Toolkit & HiDPI Ribbon"): the Settings app, paper warmth, text sizes, the launcher's two faces (damage tracking deferred: 77.3).
- **Status:** planned.

---

## 3. Out of scope — future extensions

- File copy/move/rename across volumes; search across the catalog.
- SD-provided app manifests beyond the minimal form in phase 71 §2.5.
