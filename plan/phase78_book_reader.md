# Phase 78 — The Book Reader: TrueType Fonts, On-Device `.epub`, Book Mode

**Status: PLANNED (split from phase 71 on 2026-11-09; reworked 2026-11-13 for on-device `.ttf` + `.epub` support).**
**Target Hardware**:
- **Primary**: M5Stack Tab5 (`esp32p4-tab5`, portrait for reading, 32 MB PSRAM), Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`, 32 MB PSRAM), StarFive VisionFive 2 (`visionfive2`, 8 GiB LPDDR4 + `/sd0` / `/nvme0`).
- **Regression**: Waveshare RP2350-LCD-7 (`rp2350-terminal`): no TTF engine, no `.epub` books; the reader's existing Markdown/Gemtext documents render identically with the built-in bitmap faces.

**Follows**: Phase 61 (restricted Markdown reader), Phase 71 (compositor, finger-following scroll, reader layout checkpoints, view framework, settings store, icons). **Needs**: 76.1 (long names for `/sd0/BOOKS` and `/sd0/fonts`), 76.3 (portrait, for reading on the Tab5), 77.1 (text size and font selection from Settings), 77.2 (the "Continue reading" shortcut tile). 78.1 and 78.2 are independent of all of them and can start at once.

| Old (phase 71 / initial 78) | New |
| :--- | :--- |
| *(new)* | 78.1 Freestanding TrueType (`.ttf`) engine & UTF-8 pipeline |
| 71.8 / 78.1 Reader core for large documents | 78.2 Reader core for large documents |
| 71.9 / 78.2 *(host converter replaced)* | 78.3 On-device `.epub` reader & book catalog |
| 71.10 / 78.3 Reader book mode | 78.4 Book mode & distraction-free fullscreen |
| 71.11 / 78.4 Verification & docs | 78.5 Verification and documentation |

**Decisions:**
- **No host-based `epub2lmd.py` converter:** `.epub` files are copied directly to `/sd0/BOOKS` (or `/nvme0/BOOKS`) via `p9sync.py` or SD card and parsed on-device. Books are not part of the default `sd_root` image.
- **TrueType fonts on storage:** `.ttf` files live in `/sd0/fonts/` (or `/nvme0/fonts/`), starting with `NotoSerif-Regular.ttf` (plus optional `NotoSerif-Bold.ttf`, `NotoSerif-Italic.ttf`). Built-in bitmap `font_serif` remains the fallback if no `.ttf` is present or on non-PSRAM targets.
- **Images in books** are alt text only in this phase (§3).

---

## 1. Design: TTF Engine, Large-Document Layout, `.epub` Reader, Fullscreen Book Mode

### 1.1 Freestanding TrueType (`.ttf`) Engine (`drivers/ttf.c`, `drivers/ugui.c`)
- **Constraints:** `esp32p4` builds with `-march=rv32imac_zicsr_zifencei -mabi=ilp32 -ffreestanding -nostdlib -fno-builtin` (zero `float` in M-mode, no `libm`, no `malloc`/`free`). Libraries using floating point (e.g. unmodified `stb_truetype.h`) are unsuitable.
- **Scope:** TrueType outlines (`glyf` quadratic Béziers) only — not `.otf` CFF cubic outlines.
  - Minimal SFNT table parser: `head`, `hhea`, `maxp`, `hmtx`, `cmap` (formats 4 and 12 for BMP + supplementary Unicode), `loca` (short/long), `glyf` (simple and compound glyphs).
  - Pure integer / `26.6` (or `16.16`) fixed-point scanline anti-aliased rasterizer producing 4-bit (or 8-bit) coverage bitmaps.
  - Synthetic bold/italic in `drivers/ugui.c` continues to work when only `Regular.ttf` is installed; dedicated `Bold.ttf` / `Italic.ttf` files are used if present.
- **PSRAM Memory & Glyph Cache:**
  - Allocated via `palloc_pages_bulk()` (fixed-slot LRU glyph cache in PSRAM, zero heap fragmentation).
  - Keyed by `(face_id, px_size, codepoint)`, storing coverage bitmap + advance width + `xoff`/`yoff` bearings.
  - Pre-warm `0x20..0xFF` on font load so common Latin text hits the cache immediately; remaining Unicode codepoints rasterize on first miss.
  - For large `.ttf` files (e.g. CJK), keep `cmap` + `loca` + `hmtx` in PSRAM (~100–300 KB) and `vfs_pread()` individual `glyf` records on cache miss; smaller fonts (< 4 MB) can be cached whole in PSRAM.
- **UTF-8 Pipeline:** `ugui_face_width()`, `ugui_face_text()`, and `drivers/reader_view.c`'s word-wrapper iterate by UTF-8 codepoint (`uint32_t`) instead of single bytes, eliminating character folding and `?` replacements.
- **Platform Guard:** Enabled only when `CONFIG_PSRAM_BYTES` (or DRAM budget) is present; `rp2350-terminal` and `esp32c6` pay zero Flash/SRAM cost.

### 1.2 Reader Core for Large Documents (`drivers/reader_view.c`, `drivers/reader_layout.c`)
- **Layout index:** One pass over the document/chapter builds an array of blocks `{file_off, y, h, kind, toc_level}` (12–16 bytes each) in PSRAM for the current text width. A 500 KB chapter of ~5,000 paragraphs needs ~80 KB. Reads in 4 KB chunks and yields between chunks; shows a progress bar for long files and draws the first screen as soon as its blocks are laid out.
- **Draw:** Binary search of the first visible block, read/stream from its offset, render only blocks intersecting the viewport. No whole-file re-parse per frame.
- **Invalidation:** Width changes (rotation, maximize, text size, font change) rebuild the index; reading position is restored by byte offset.
- **Links/TOC** are collected during the index pass, not per draw.
- **Limit:** Bounded by available PSRAM, not a fixed constant. Exceeding it fails with a clear message, never a crash.

### 1.3 On-Device `.epub` Support (`fs/epub.c`, `drivers/reader_view.c`)
- **ZIP + DEFLATE:** Minimal ZIP local-header / central-directory / CRC-32 reader reusing `kernel/zlib.c` (`zlib_inflate_raw()`, `zlib_crc32()`) with a 32 KB sliding window and scratch buffer in PSRAM.
- **DRM check:** Detect `META-INF/encryption.xml` with content encryption and refuse cleanly with a message. No decryption attempts.
- **EPUB structure:**
  - Parse `META-INF/container.xml` → locate `.opf` package file.
  - Parse `.opf`: metadata (`dc:title`, `dc:creator`, `dc:identifier`), `<manifest>`, and `<spine>` (ordered list of chapter XHTML paths).
  - Parse NCX (`toc.ncx`) or EPUB3 `nav` document for chapter titles / Table of Contents.
- **Streaming XHTML → Reader Blocks/Markdown Adapter:**
  - Stream-parse chapter XHTML without building a full DOM in RAM: `h1–h4`, `p`, `em/i`, `strong/b`, `blockquote`, `ul/ol/li`, `pre/code`, `a href` (intra-book chapter/anchor links), footnotes (`epub:type="noteref"/"footnote"`), simple tables, `img` → `*[Image: alt]*`, `br`, and HTML entities (`&nbsp;`, `&mdash;`, numeric `&#...;` / `&#x...;` → UTF-8). CSS is ignored.
- **Book Catalog:** Scan `/sd0/BOOKS` (or `/nvme0/BOOKS`) for `.epub` files, caching a lightweight `BOOKS/CATALOG` index (title, author, path, cover) so the launcher's Books root (`ugui_icon_grid()`) opens without re-parsing every archive on every launch.

### 1.4 Book Mode & Distraction-Free Fullscreen
- **Paging:** Page = viewport height minus one line of overlap, cut at line boundaries (no line split across pages). Page table derived from the layout index.
- **Fullscreen Book Mode (`.epub` / book mode):**
  - When fullscreen is active, **only the book text is visible** — no toolbar, no TOC/Back buttons, no window chrome, no status line or page-number overlay.
  - **Exiting fullscreen:** **Swipe down** exits fullscreen (and reveals the toolbar/status bar); a middle-third tap can also toggle the toolbar/fullscreen overlay.
- **Navigation Gestures & Keys:**
  - Tap right third / swipe left → next page; tap left third / swipe right → previous page. The reader claims `SWIPE_H` in book mode so the ribbon still moves from chrome/edges when not fullscreen.
  - Mouse: click in side thirds, wheel by pages.
  - Keys: `Space`/`PgDn`/`→` next page, `PgUp`/`←` previous page, `t`/`c` contents, `+`/`-` text size, `f` toggle fullscreen, `Home`/`End` chapter start/end, `n`/`p` next/previous chapter.
- **Chapters & Position:** Advancing past the end of a chapter opens the next spine item. Position is saved per book (`spine_idx + byte_offset` keyed by book path/identifier) on page turn (debounced) and on close; reopening resumes there.
- **Budget targets (from phase 71 §2.1):** Reader page turn ≤ 60 ms tap-to-glass on the 7B; on the Tab5, full-width composite costs ≥ 40 ms of PSRAM bandwidth, so page turns target one surface redraw + composite within that envelope.

---

## 2. Milestones

### 78.1 — Freestanding TrueType (`.ttf`) engine & UTF-8 pipeline
- Implement `drivers/ttf.c` (`head`, `hhea`, `maxp`, `hmtx`, `cmap` 4/12, `loca`, `glyf` + integer/fixed-point scanline AA rasterizer, zero `float`).
- PSRAM LRU glyph cache (`palloc_pages_bulk()`) + pre-warming of `0x20..0xFF`; `vfs_pread()` fallback for large `glyf` tables.
- Update `drivers/ugui.c` (`ugui_face_width()`, `ugui_face_text()`) and `drivers/reader_view.c` word-wrapping for UTF-8 codepoint iteration.
- Load `NotoSerif-Regular.ttf` from `/sd0/fonts/` (or `/nvme0/fonts/`), selectable via Settings (`ui.font`, `ui.text_size`), with built-in `font_serif` fallback.
- **Acceptance:** `NotoSerif-Regular.ttf` renders English, German umlauts, typographic punctuation (`– — ‘ ’ “ ” …`), and Greek/Cyrillic glyphs cleanly on Tab5 and 7B at multiple sizes; page-turn glyph blits hit the PSRAM cache with no per-frame rasterization; RP2350/C6 builds have zero Flash/SRAM growth.

### 78.2 — Reader core for large documents
- Layout index, chunked build with progress, visible-block rendering, byte-offset positions, link/TOC collection during indexing (§1.2).
- **Width after a turn:** layout follows the window's content width on every resize (`WIN_EV_RESIZE`), including portrait/landscape rotation on Tab5.
- **Acceptance:** A 1 MB generated Markdown file opens with the first screen in ≤ 1 s on the Tab5; scrolling anywhere meets targets; memory use is reported by `reader list`; existing reader tests (`tests/test_markdown.py`, showcase document) render identically.

### 78.3 — On-device `.epub` reader & book catalog
- Implement `fs/epub.c`: ZIP directory/CRC-32 + `kernel/zlib.c` `zlib_inflate_raw()`, DRM check, `container.xml`/OPF/NCX/NAV parsing, streaming XHTML → reader blocks/Markdown adapter.
- `/sd0/BOOKS` (and `/nvme0/BOOKS`) scanner + `BOOKS/CATALOG` cache for the launcher's Books root (`ugui_icon_grid()`).
- **Test corpus:** Download a small set of DRM-free Project Gutenberg `.epub` files (e.g. English + non-ASCII/German/French/Greek) into `tests/books/` (or `LUGALOS_BOOKS_SRC`) and test with `NotoSerif-Regular.ttf`.
- **Acceptance:** Project Gutenberg `.epub` books open directly from `/sd0/BOOKS` on Tab5, 7B, and VisionFive 2 with chapter TOC, intra-book links, and native UTF-8 typography rendered via `NotoSerif-Regular.ttf`; encrypted epubs report a clear error.

### 78.4 — Book mode & distraction-free fullscreen
- Paging, tap zones, claimed `SWIPE_H`, chapter chaining across spine items, per-book saved position, "Continue reading" shortcut, text size/font from Settings (§1.4).
- **Fullscreen book mode:** hides all chrome, TOC buttons, and status overlays so only book text is visible; swipe down exits fullscreen.
- **Acceptance:** Read ten chapters on the Tab5 (portrait, fullscreen) by touch only and on the 7B by keyboard only; swipe down reliably exits fullscreen; position survives close, rotation, and reboot; page-turn latency meets targets.

### 78.5 — Verification and documentation
- Host/unit tests for SFNT/`glyf` rasterizer and ZIP/EPUB/XHTML parser.
- `tests/hw/test_esp32p4.py`: TTF rendering, `.epub` open/navigation/fullscreen checks, `uxbench` regression check (fail if > 20% slower than baseline).
- `tests/hw/lcdterm_shots.py`: `.ttf` + `.epub` book page and fullscreen scenarios.
- Docs: `doc/user/reader.md` (TTF fonts, `.epub` reading, fullscreen gestures); `python3 tools/gendocs.py --all` and `--check`.
- RAM baselines re-recorded; `sizereport` for RP2350 shows no SRAM growth.

### 78.6 — OpenType Shaping (GSUB/GPOS), Kerning, Ligatures & Complex-Script Fallback
- **Glyph-ID keyed cache & rasterizer:** Key `ttf_cache` by `(font_id, glyph_id, size_px)` so shaped/ligated/stacked glyphs without direct Unicode codepoints are cached in PSRAM alongside `cmap`-mapped glyphs.
- **GSUB shaping:** Parse `GSUB` ScriptList/FeatureList/LookupList and execute Lookup Types `1` (Single), `2` (Multiple), `4` (Ligature), and `6` (Chaining Contextual) in pure integer/fixed-point freestanding C (`ccmp`, `abvs`, `blws`, `calt`, `liga`).
- **GPOS kerning:** Parse `GPOS` Lookup Type `2` (Pair Adjustment, formats 1 & 2) and legacy `kern` table for horizontal advance adjustments (`kern`), with integer `26.6`/px rounding.
- **Script fallback font slot:** Support a secondary Tibetan/complex-script font (`/sd0/fonts/Jomolhari-Regular.ttf`) for `U+0F00..U+0FFF` runs while keeping `/sd0/fonts/NotoSerif-Regular.ttf` for Latin/Greek/Cyrillic, and render shaped glyph runs cleanly in `ugui` + `reader_view.c`.
- **Acceptance:** `ttf_host` ASan/UBSan verifies `Jomolhari-Regular.ttf` Tibetan stacking (`GSUB` `ccmp`/`abvs`/`blws`/`calt`) and `NotoSerif-Regular.ttf` `liga`/`kern`; `Nyengyü Trekchö translation.epub` and `Nyengyü Tögal translation.epub` open and render both English and Tibetan on Tab5.

---

## 3. Out of scope — future extensions
- **Book images and inline figures:** Decode embedded JPEG/PNG images from `.epub` archives into PSRAM surfaces and render them as `image` blocks in the layout index.
- **OpenType CFF (`.otf`) outlines, GPOS mark-to-base/mark-to-mark (`mark`/`mkmk`), and TrueType bytecode hinting.**
- **Hyphenation; justified text.**
