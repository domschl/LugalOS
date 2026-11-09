# Phase 78 — The Book Reader: Large Documents, epub Conversion, Book Mode

**Status: PLANNED (split from phase 71 on 2026-11-09; its milestones were 71.8, 71.9 and 71.10 there).**
**Target Hardware**:
- **Primary**: M5Stack Tab5 (`esp32p4-tab5`, portrait for reading), Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`)
- **Regression**: Waveshare RP2350-LCD-7 (`rp2350-terminal`): no books, no Latin-1 font unless the flash budget allows (phase 71 decision 5); the reader's existing documents render identically.

**Follows**: Phase 61 (restricted Markdown reader), Phase 71 (compositor, finger-following scroll, reader layout checkpoints, view framework, settings store, icons). **Needs**: 76.1 (long names for the book tree), 76.3 (portrait, for reading on the Tab5), 77.1 (text size from Settings), 77.2 (the "Continue reading" shortcut tile). 78.1 is independent of all of them and can start at once.

| Old (phase 71) | New |
| :--- | :--- |
| 71.8 Reader core for large documents | 78.1 |
| 71.9 epub converter, catalog, fonts (+ 71.7's Books root) | 78.2 |
| 71.10 Reader book mode | 78.3 |
| 71.11, for this phase's items | 78.4 |

**Decisions (confirmed 2026-11-05, from phase 71 §4):** books live in `/sd0/BOOKS`, synced by `p9sync.py`, not part of the default `sd_root` image; the owner provides a DRM-free source tree for testing. Images in books are alt text only in this phase (§3).

---

## 1. Design: converter, layout, reading

**Converter `tools/epub2lmd.py`** (in a `uv` project under `host/booktools/`, typed for `pyrefly`, with tests):
- Input: a directory tree of `.epub` files. Output: `<out>/BOOKS/`, merged into `build/<preset>/sd_root/` or synced with `p9sync.py` (additive and idempotent, as for the other SD content).
- Per book a directory named for people browsing the card (owner, 2026-11-06; needs 76.1's long names): `BOOKS/<Author>/<Title>/`, e.g. `BOOKS/Goethe, Johann Wolfgang/Faust I/` -- the first author for several, `Various` for none. Names are made FAT-safe (`\ / : * ? " < > |` replaced, control characters dropped, trailing dots and spaces trimmed), cut to about 64 characters at a word boundary, and a second book that would get the same path gets a short suffix from its id. Inside:
  - `META` — `id=` (the book's identity: a hash of the epub's `dc:identifier`, or of title+author; stable across runs), `title=`, `author=`, `lang=`, `chapters=`, `source=` (original file name), `converter=` version. A re-conversion finds a book by `id`, not by its path, so the owner may rename or move book directories and the next run updates them in place rather than adding a copy.
  - `001 <Chapter title>.md` … — one file per spine item, numbered so they sort, titled from the chapter's heading (`001 Chapter 1.md` when it has none), in the reader's restricted Markdown. Chapters below 2 KB are merged with the next.
  - `INDEX.md` — the table of contents (from the epub nav/NCX) as links to the chapter files' anchors.
  - `COVER.GIC` — cover icon (phase 71 §2.6), or one generated from title/author when the epub has no cover.
- `BOOKS/CATALOG` — one line per book: id, path, title, author, chapter count. This is the launcher's data source, so it never has to open 500 directories; a book moved by hand is found again by the next conversion or a `books rescan`, which rewrites the catalog from the `META` files.
- **XHTML → Markdown mapping:** `h1-h4` → `#`…, `p` → paragraph, `em/i` → `*`, `strong/b` → `**`, `blockquote` → `>`, `ul/ol/li` → lists, `pre/code` → fences, `a href` → links rewritten to the chapter files' anchors, footnotes (`epub:type="noteref"/"footnote"`) → `[^n]`, simple tables → pipe tables, `img` → `*[Image: alt]*`, `br` → line break. Everything else is reduced to text. CSS is ignored.
- **Character folding:** every character outside the target font set is mapped through a fold table (’‘ → ', “” → ", – → -, — → --, … → ..., non-breaking space → space, ligatures → letters). Remaining unknown characters become `?` and are counted in the converter's report. The target set is Latin-1 + the punctuation added in 78.2.
- DRM-protected epubs (`META-INF/encryption.xml` with content encryption) are skipped with a message. No attempt to decrypt.
- Report per book: chapters, bytes, folded/unknown characters, dropped elements.

**Reader core for large documents** (`drivers/reader_view.c`, split into `reader_layout.c` + `reader_view.c`):
- **Layout index:** one pass over the file builds an array of blocks `{file_off, y, h, kind, toc_level}` (12-16 bytes each) in PSRAM, for the current text width. A 500 KB chapter of ~5000 paragraphs needs ~80 KB. The pass reads the file in 4 KB chunks and yields between chunks. A progress bar is shown for long files, and the first screen is drawn as soon as its blocks are laid out. Phase 71.1's layout checkpoints (every half screen) are its first form.
- **Draw** = binary search of the first visible block, `vfs_pread` from its offset, render only blocks that intersect the viewport. No whole-file re-parse.
- **Invalidation:** a width change (rotation, maximise, text size) rebuilds the index. The reading position is restored by byte offset.
- **Links/TOC** are collected during the index pass, not per draw.
- **Limit:** the index is bounded by available PSRAM, not by a fixed constant. A file above that limit opens with a clear message, never a crash.

**Book mode (reader opened on a book directory or a chapter file inside one):**
- **Paging:** page = viewport height minus one line of overlap, cut at line boundaries (no line split across pages). Page table derived from the layout index.
- **Gestures:** tap on the right third / swipe left → next page; tap on the left third / swipe right → previous page; tap in the middle third → toggle the toolbar (Back, Contents, Text size, Rotate, Close). The reader claims `SWIPE_H` in book mode only (phase 71 §2.2), so the ribbon still moves from chrome and edges. The mouse: a click in the side thirds, the wheel by pages.
- **Keys:** Space/PgDn/→ next page, PgUp/← previous page, `t`/`c` contents, `+`/`-` text size, Home/End chapter start/end, `n`/`p` next/previous chapter.
- **Chapters:** the next page at the end of a chapter opens the next chapter file (by its number). The status line shows the chapter title and "page x of y" for the chapter, plus a book-wide percentage estimated from chapter sizes.
- **Position:** stored per book as `chapter + byte offset` in the book's `POS` file, written on page turn (debounced) and on close. Reopening continues there.
- **Rendering:** page turns are instant by default (one surface redraw + composite). A slide animation is an option in Settings, using composite-only frames.

**Budget targets (from phase 71 §2.1):** reader page turn ≤ 60 ms tap-to-glass on the 7B; on the Tab5 the composite of a full-width window costs ≥ 40 ms of PSRAM bandwidth (phase 71.1's finding), so its target is what that allows.

---

## 2. Milestones

### 78.1 — Reader core for large documents
- Layout index, chunked build with progress, visible-block rendering, byte-offset positions, link/TOC collection during indexing (§1).
- **Acceptance:** a 1 MB generated Markdown file opens with the first screen in ≤ 1 s on the Tab5; scrolling anywhere in it meets the targets; memory use is reported by `reader list`; the existing reader tests (`tests/test_markdown.py`, showcase document) render identically.
- **Status:** planned; independent, can start first.

### 78.2 — epub converter, catalog, fonts
- `host/booktools/` uv project: `epub2lmd.py`, fold table, catalog writer, cover → `.GIC`, report; tests on small generated epubs (checked in under `tests/books/`). The owner's DRM-free source tree stays outside the repository and is referenced by a path option (`--books-src`, or `LUGALOS_BOOKS_SRC`). The converter runs over the whole tree, and its report is this milestone's acceptance record.
- Standard-density serif and its AA variant extended to Latin-1 plus `– — ‘ ’ “ ” … •` (`tools/gen_serif_font.py`); the reader decodes UTF-8 for both font sets; `ugui`'s text field takes characters past ASCII.
- `tools/create_sd_image.py` / `p9sync.py`: an option to include a `BOOKS` tree.
- The launcher's Files face shows a "Books" root as a cover grid from `BOOKS/CATALOG` (`ugui_icon_grid()` with the covers).
- **Acceptance:** converter tests pass under `pyrefly`; the owner's book tree converts and syncs to `/sd0/BOOKS` with `p9sync.py`; a German and an English book from it convert with zero unknown characters and render umlauts on the Tab5 and the 7B; RP2350 flash growth from the font extension is recorded and stays within budget (or the extension is P4-only).
- **Status:** planned; needs 76.1, and 77.2 for the Books root.

### 78.3 — Reader book mode
- Paging, tap zones, claimed horizontal swipe, middle-tap toolbar, chapter chaining, status line, per-book position, "Continue reading" shortcut, text size from settings (§1).
- **Acceptance:** a person reads ten chapters on the Tab5 (portrait) by touch only and on the 7B by keyboard only; position survives close, rotation and reboot; page-turn latency meets the targets.
- **Status:** planned; needs 76.3, 77.1, 77.2.

### 78.4 — Verification and documentation
- `tests/hw/test_esp32p4.py`: book-mode checks and the `uxbench` regression check (fail if a bench row gets > 20 % slower than the recorded baseline).
- `tests/hw/lcdterm_shots.py`: a book page scenario.
- Docs: `doc/user/reader.md` (book mode), new `doc/user/books.md` (converter usage); `python3 tools/gendocs.py --all` and `--check`.
- RAM baselines re-recorded; `sizereport` for RP2350 shows no SRAM growth.
- **Status:** planned.

---

## 3. Out of scope — future extensions

- **Book images and inline figures**: the converter writes epub images as gray image files next to the chapters (4-bpp, sized for the panel, cover-style dithering for 1-bpp). The reader draws them as image blocks through the `.GIC` blit (`ugui_image4()`), and the layout index gets an `image` block kind with known height. The groundwork is there: alt text keeps the image's place in the text, the icon format and blit exist, and the converter's report counts the dropped images.
- Hyphenation; justified text.
