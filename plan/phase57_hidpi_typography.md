# Phase 57: HiDPI & Anti-Aliased Typography (Noto Sans Mono & Noto Serif for High-Density Displays)

**Status**: COMPLETED (2026-10-16)  
**Target Hardware**:
- **Primary**: M5Stack Tab5 (`esp32p4-tab5` / `esp32p4-tab5x`, 5.0" 720×1280 native portrait IPS, ~294 DPI)
- **Secondary / Regression**: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`, 7.0" 1024×600, ~170 DPI), Waveshare RP2350-LCD-7 (`rp2350-terminal`, 7.0" 800×480, ~133 DPI), QEMU (`rv32`, `rv64`)
**Reference Typefaces**:
- `NotoSansMono-Regular.ttf` & `NotoSansMono-Bold.ttf` (`/usr/share/fonts/noto/`)
- `NotoSerif-Regular.ttf` & `NotoSerif-Bold.ttf` (`/usr/share/fonts/noto/`)

---

## 1. Executive Summary & Problem Statement

### 1.1 The Physical Display Density Gap
LugalOS runs on hardware displays ranging from 7-inch desktop terminals down to 5-inch handheld tablets. While display resolutions have increased, our core typography system has remained anchored to a fixed $8 \times 16$ pixel character cell inherited from the 1-bpp monochrome RP2350-LCD-7.

On high-density handheld screens like the M5Stack Tab5 ($294\text{ DPI}$), this fixed sizing produces microscopic characters:

| Target Display | Diagonal & Aspect | Resolution | Pixel Density | 8×16 Character Physical Size | User Experience & Readability |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **RP2350-LCD-7** | 7.0" (5:3) | $800 \times 480$ | **$133\text{ DPI}$** | **$1.53\text{ mm} \times 3.06\text{ mm}$ (~8.6 pt)** | Highly readable desk terminal ($98 \times 27$ cells) |
| **ESP32-P4-LCD-7B**| 7.0" (16:9 approx)| $1024 \times 600$ | **$170\text{ DPI}$** | **$1.19\text{ mm} \times 2.39\text{ mm}$ (~6.8 pt)** | Readable on desk ($126 \times 34$ cells) |
| **M5Stack Tab5** | 5.0" (16:9) | $720 \times 1280$ | **$294\text{ DPI}$** | **$0.69\text{ mm} \times 1.38\text{ mm}$ (< 3.9 pt)** | **Microscopic, severe eye strain; unusable handheld** |

### 1.2 Visual Comparison
* On the **RP2350**, a capital letter 'A' is **$3.06\text{ mm}$** tall.
* On the **Tab5**, the same capital letter 'A' is only **$1.38\text{ mm}$** tall—less than half the physical size!
* In Tab5 Landscape mode ($1280 \times 720$), an $8 \times 16$ font yields $158$ columns $\times$ $42$ rows. Text appears like tiny dots on the screen.
* In Tab5 Portrait mode ($720 \times 1280$), an $8 \times 16$ font yields $88$ columns $\times$ $77$ rows. A handheld e-reader with 77 text rows per page violates standard typographic ergonomics (which recommend 30–45 lines per page and 50–70 characters per line).

### 1.3 Goals of Phase 57
1. **DPI-Aware Typography Framework**: Introduce formal display density profiles to scale both monospace and proportional fonts according to the physical panel DPI.
2. **Universal Noto Font Family**: Adopt Google's open-source **Noto** family for both monospace (`Noto Sans Mono`) and proportional serif (`Noto Serif`) typography.
3. **HiDPI Handheld Profile for Tab5**:
   - Monospace terminal shell: Scale from $8 \times 16$ to **$16 \times 32$ pixels** (exact $2\times$ integer scale).
     - Landscape ($1280 \times 720$): **$80\text{ cols} \times 21\text{ rows}$** (the classic universal terminal standard).
     - Portrait ($720 \times 1280$): **$45\text{ cols} \times 38\text{ rows}$** (ergonomic portrait terminal).
   - Proportional Gemtext reader: Scale from $16\text{ px}$ cell height to **$32\text{ px}$ cell height** ($36\text{ px}$ line height), yielding comfortable book-grade reading at ~11–12 pt print equivalent.
4. **Anti-Aliasing on ESP32-P4 Platforms**:
   - **Reader**: Full 4-bpp anti-aliasing for both prose (Noto Serif) and preformatted code blocks (Noto Sans Mono).
   - **Terminal Shell**: Direct 4-bpp anti-aliased character blitting onto RGB565 / GRAY8 surfaces, eliminating harsh monochrome pixelation while preserving 60 Hz frame-paced scanout.
5. **Zero SRAM Overhead & Backward Compatibility**:
   - All font tables stored in Flash `.rodata`.
   - Zero floating point in kernel / M-mode.
   - Maintain 100% compatibility and zero memory overhead on the RP2350-LCD-7 (1-bpp Spleen $8 \times 16$) and ESP32-P4-LCD-7B.

---

## 2. Density Profiles & Target Geometries

### 2.1 Density Tiers

```
                 ┌─────────────────────────────────────────────────────────────┐
                 │                LUGALOS DISPLAY DENSITY TIERS                │
                 └──────────────────────────────┬──────────────────────────────┘
                                                │
                 ┌──────────────────────────────┴──────────────────────────────┐
                 │                                                             │
  ┌──────────────▼─────────────┐                                ┌──────────────▼─────────────┐
  │  STANDARD DENSITY PROFILE  │                                │    HiDPI DENSITY PROFILE   │
  │    (~130 - 170 DPI)        │                                │      (~250 - 300 DPI)      │
  ├────────────────────────────┤                                ├────────────────────────────┤
  │ RP2350-LCD-7  (133 DPI)    │                                │ M5Stack Tab5  (294 DPI)    │
  │ ESP32-P4-LCD7B (170 DPI)   │                                │ Future high-res panels     │
  ├────────────────────────────┤                                ├────────────────────────────┤
  │ Mono: 8x16 px              │                                │ Mono: 16x32 px             │
  │ Serif: 16 px (18 px line)  │                                │ Serif: 32 px (36 px line)  │
  └────────────────────────────┘                                └────────────────────────────┘
```

### 2.2 Geometry Breakdown by Board & Orientation

| Preset / Orientation | Display Resolution | Cell Size ($W \times H$) | Grid Columns | Grid Rows | Physical Char Height | Characters / Line | Lines / Screen |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **RP2350-LCD-7** | $800 \times 480$ | $8 \times 16$ | 98 cols | 27 rows | $3.06\text{ mm}$ (~8.6 pt) | 98 | 27 |
| **P4 LCD-7B** | $1024 \times 600$ | $8 \times 16$ | 126 cols | 34 rows | $2.39\text{ mm}$ (~6.8 pt) | 126 | 34 |
| **Tab5 Landscape (Old)** | $1280 \times 720$ | $8 \times 16$ | 158 cols | 42 rows | $1.38\text{ mm}$ (< 3.9 pt) | 158 | 42 (Too dense!) |
| **Tab5 Landscape (HiDPI)**| $1280 \times 720$ | **$16 \times 32$** | **78 cols** | **21 rows** | **$2.77\text{ mm}$ (~11 pt)** | **78** | **21 (Optimal standard)**|
| **Tab5 Portrait (Old)** | $720 \times 1280$ | $8 \times 16$ | 88 cols | 77 rows | $1.38\text{ mm}$ (< 3.9 pt) | 88 | 77 (Too tall!) |
| **Tab5 Portrait (HiDPI)** | $720 \times 1280$ | **$16 \times 32$** | **43 cols** | **38 rows** | **$2.77\text{ mm}$ (~11 pt)** | **43** | **38 (Ideal paperback)**|

---

## 3. Font Asset Pipeline & Storage Architecture

### 3.1 Typeface Selection
* **Noto Sans Mono**:
  * Clean, humanist monospace design by Google.
  * Explicit distinction between `0` (slashed/dotted), `O`, `1`, `l`, `I`.
  * Complete coverage of ASCII, Latin-1, Box Drawing (`0x2500..0x253C`), Staunton Chess Figurines (`0x2654..0x265F`), and typographic punctuation.
* **Noto Serif**:
  * Elegant, high-legibility proportional serif designed specifically for digital displays and books.
  * Proportional advance widths with natural kerning for long-form reading in `reader`.

### 3.2 Dual Representation in Flash `.rodata`
For each font and scale, the font generator utility (`tools/gen_noto_fonts.py`) emits:
1. **1-bpp Bilevel Bitmaps**:
   - Packed bits for low-memory 1-bpp targets and fast boolean bitwise blits.
   - For $16 \times 32$: Each glyph row is a `uint16_t` (2 bytes, leftmost pixel in bit 0).
   - $32\text{ rows} \times 2\text{ bytes} = 64\text{ bytes}$ per glyph.
2. **4-bpp Alpha Coverage Masks**:
   - 16 levels of anti-aliasing ($0 = \text{transparent}, 15 = 100\% \text{ ink opacity}$).
   - 2 pixels packed per byte.
   - For $16 \times 32$: $16 \times 32 / 2 = 256\text{ bytes}$ per glyph.
   - Blended in integer arithmetic:
     $$\text{dst\_gray} = \frac{\text{fg} \cdot \alpha + \text{bg} \cdot (15 - \alpha)}{15}$$
     For RGB565: channel-separated alpha blending for R (5-bit), G (6-bit), and B (5-bit).

### 3.3 Flash Memory Budget Analysis

All tables reside in Flash `.rodata` (XIP):

| Font Table Asset | Format | Glyph Count | Storage Size |
| :--- | :--- | :--- | :--- |
| `font_noto_mono_16x32_1bpp` | 1-bpp (uint16_t rows) | 224 glyphs | $14.3\text{ KB}$ |
| `font_noto_mono_16x32_4bpp` | 4-bpp coverage (2 px/byte)| 224 glyphs | $57.3\text{ KB}$ |
| `font_noto_serif_32_4bpp` | 4-bpp proportional | 96 ASCII glyphs | $36.8\text{ KB}$ |
| `font_noto_serif_32_widths`| uint8_t advance widths | 128 entries | $128\text{ bytes}$ |
| `font_noto_serif_32_1bpp` | 1-bpp proportional | 96 ASCII glyphs | $7.2\text{ KB}$ |
| **Total HiDPI Font Footprint** | | | **$\approx 115.7\text{ KB}$** |

> [!NOTE]
> On the M5Stack Tab5, total flash size is **16 MB**. A total typography footprint of $116\text{ KB}$ consumes **$0.72\%$** of flash, leaving over $15.8\text{ MB}$ for kernel, drivers, applications, and root filesystems. SRAM consumption is **zero bytes**.

---

## 4. Phase 57 Implementation Milestones

### Milestone 57.1: Multi-Density Font Generator Utility (`tools/gen_noto_fonts.py`)
* Implement `tools/gen_noto_fonts.py` using Python `Pillow` / `FreeType`.
* Rasterize `/usr/share/fonts/noto/NotoSansMono-Regular.ttf` and `NotoSerif-Regular.ttf` at:
  - Standard scale: $8 \times 16$ monospace, $16\text{ px}$ serif.
  - HiDPI scale: $16 \times 32$ monospace, $32\text{ px}$ serif.
* Synthesize box-drawing characters and Staunton chess figurines for the $16 \times 32$ grid to match LugalOS's internal glyph table (`0x80..0x9F`).
* Emit C tables and headers into:
  - `drivers/include/drivers/font_noto_mono.h` & `drivers/font_noto_mono.c`
  - `drivers/include/drivers/font_noto_serif.h` & `drivers/font_noto_serif.c`
* Provide fast inline rendering routines for 1-bpp canvas, 8-bpp GRAY8, and 16-bpp RGB565.

### Milestone 57.2: Parameterized Terminal Cell Geometry (`fbtext` & `screen`)
* Parameterize terminal cell width and height:
  - Introduce `CONFIG_FONT_CELL_W` (8 or 16) and `CONFIG_FONT_CELL_H` (16 or 32) in preset configs (`board_esp32p4_tab5.h`).
  - Abstract `FONT8X16_W`/`FONT8X16_H` usages in `drivers/include/drivers/screen.h`, `drivers/fbtext.h`, and `drivers/ribbon.h`.
* Generalize `drivers/fbtext.c`:
  - When `FONT_CELL_W == 16`: update `fbtext_putcode()` to write 16-bit row words (`uint16_t *p`).
  - Update `fbtext_cursor_xor()` and scrolling primitives to handle 32-row cell steps.
* Update Ribbon layout calculations in `drivers/include/drivers/ribbon.h`:
  - `RIBBON_COLS_FULL(w)` $= (w / \text{CELL\_W}) - 2$
  - `RIBBON_COLS_PAIR(w)` $= (w / \text{CELL\_W}) - 4$
  - Verify layout formulas on Tab5 Landscape ($1280\text{px} \rightarrow 78\text{ cols}$) and Portrait ($720\text{px} \rightarrow 43\text{ cols}$).

### Milestone 57.3: HiDPI Anti-Aliased Reader Engine
* Upgrade `drivers/reader_view.c` for Tab5:
  - Select 32 px Noto Serif for proportional text when `CONFIG_FONT_CELL_H == 32`.
  - Set `READER_LINE_H = 36` (comfortable $1.2\times$ line spacing).
  - Update heading scales: H1 ($40\text{ px}$ bold), H2 ($36\text{ px}$ bold), H3 ($32\text{ px}$ bold/italic), Body ($30\text{ px}$ regular).
  - Replace raw 1-bpp fallback in preformatted code blocks (`rdraw_mono_text()`) with 4-bpp anti-aliased Noto Sans Mono ($16 \times 32$).
* Verify word-wrapping, link hit-testing, and touch fling inertia scrolling with the larger line box.

### Milestone 57.4: Direct Anti-Aliased Terminal Compositor on P4
* Implement direct 4-bpp anti-aliased character rendering for terminal windows on RGB565 surfaces:
  - In `drivers/screen.c` and `drivers/lcdterm.c`, export `lcdterm_get_visible_terms()` with window geometry, shadow buffer, cursor coordinates, and scroll offsets.
  - In `drivers/dsi_esp32p4.c`, composite visible anti-aliased terminal cells onto `fb_back()` using `font_noto_mono_aa_glyphs` (4-bpp grayscale coverage masks) from `NotoSansMono-Medium`.
  - Implement 16-entry precomputed alpha blending LUTs for foreground and background ANSI/palette colors in pure integer arithmetic.
  - Implement orientation-aware blits for Landscape $270^\circ$, Landscape $90^\circ$, and Portrait $0^\circ$.
  - Render cursor underline on bottom scanlines (`dy >= 30u`) during compositing.
  - Optimize scanout writebacks: track horizontal dirty extents (`g_dirty_x0`/`g_dirty_x1`), restrict 1-bpp band conversions to modified columns, and flush only modified physical scanlines to PSRAM via `esp32p4_extmem_writeback`.
  - Result: single character latency reduced from 9109 µs to 5200 µs (well below the 8000 µs HIL threshold) while achieving 1358 lines/s throughput.

### Milestone 57.5: Hardware Verification & Regression Testing
* **Bug Fixes & Refinements**:
  - Fix typed character visibility: explicit interactive damage trigger in `lcdterm_console_flush()`; batched character writes during bulk output flush on newlines only.
  - Fix Reader kinetic scroll clipping: updated visible window row bounds in `lcdterm_get_visible_readers()` and `lcdterm_get_visible_canvases565()` to use `(int)FONT_CELL_H * text_rows`.
  - Reader Typography Upgrade: switched proportional font to `Gentium Book` at 26pt (`GentiumBook-Regular.ttf`, 95 ASCII glyphs with FreeType grayscale thresholding >= 96), delivering classic book typography with optical proportion matching 280 DPI; tightened line height from 38 px to 30 px ($1.15\times$ classic print leading) and calibrated heading/paragraph breaks.
  - Terminal Shell Typography: set `Noto Sans Mono Medium` at 26pt (`NotoSansMono-Medium.ttf`), achieving natural advance width (15.59 px matching the 16x32 cell) with cohesive 2-3 px inter-character gaps and zero horizontal tracking distortion.
* **M5Stack Tab5 Live Hardware Verification**:
  - Verify visual legibility of $16 \times 32$ terminal shell in Landscape ($1280 \times 720$, $78 \times 21$ cells).
  - Verify reading comfort of Gentium Book 26pt Gemtext reader in Portrait ($720 \times 1280$, $43 \times 38$ cells) and Landscape split ribbon.
  - Validate touch swipe inertia, link tapping, and `Super+O` orientation switching.
  - Capture PGM/PPM screenshots to `/sd0/screenshots/` and inspect typography quality (`shot-tab5-noto-condensed-aa.png`, `shot-tab5-gentium-book-full.png`).
* **Regression Testing**:
  - Run `tests/hw/test_esp32p4.py --board tab5` (all 46/46 tests pass).
  - Verify clean builds for `esp32p4-lcd7b` and `rp2350-terminal` (zero compiler warnings or regressions).

---

## 5. Verification Checklist & Success Criteria

1. **[x] Typography Scalability**:
   - Tab5 boots with crisp, large, highly readable $16 \times 32$ Noto Sans Mono in the shell ($78 \times 21$ Landscape, $43 \times 38$ Portrait).
   - Upgraded to `NotoSansMono-Medium` with 4-bpp anti-aliasing directly composited onto RGB565 scanout buffer.
   - Character physical height on Tab5 is $\approx 2.77\text{ mm}$ (~11 pt), completely resolving microscopic eye strain.
   - Textbox margins properly aligned with 12 px desktop padding and 4 px interior padding relative to window borders.
2. **[x] Reader Aesthetics**:
   - Gemtext `.gmi` documents render with 32 px anti-aliased Noto Serif for prose and 32 px anti-aliased Noto Sans Mono for code blocks.
   - Smooth 60 Hz touch scrolling and link navigation are preserved.
   - Dynamic orientation switching (`Super+O` / `Cmd+O` / `'o'`) reflows typography instantly.
3. **[x] Zero Memory & System Regressions**:
   - Zero dynamic SRAM allocations in font rendering; all font tables reside in Flash `.rodata`.
   - Zero floating point operations in kernel / M-mode.
   - RP2350 and LCD-7B presets retain their existing $8 \times 16$ geometries and zero performance loss.
4. **[x] Hardware Test Suite**:
   - All tests pass cleanly in `tests/hw/test_esp32p4.py --board tab5` (46/46 passed).
