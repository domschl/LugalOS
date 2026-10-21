# Phase 61 — Restricted (No-HTML) Markdown Engine, Rich Typography, LaTeX Math & `ea` Integration

**Status**: PROPOSED / READY FOR IMPLEMENTATION  
**Target Version**: 0.23.0  
**Supersedes / Evolves**: Phase 53 (`plan/phase53_gemtext_spartan.md`)  
**Follows**:
- Phase 42 & 43: Lisp CAS & Symbolic Computation (`cas/`, `cas/format.lisp`)
- Phase 50: Documentation System (`doc/`, `tools/gendocs.py`)
- Phase 53: Gemtext Hypertext Engine, Spartan Protocol & Ribbon Reader (`fs/gemtext.c`, `drivers/reader_view.c`, `net/spartan.c`)
- Phase 56: M5Stack Tab5 Dual-Orientation Reader (`esp32p4-tab5`)
- Phase 57: HiDPI & Anti-Aliased Typography (`drivers/font_noto_serif.c`, `drivers/font_noto_mono.c`)
- Phase 58 & 59: On-Device AI Agent (`ea`) & Gap Closure (`kernel/ea.c`)

---

## 1. Executive Summary & Architectural Rationale

In Phase 53, LugalOS adopted **Gemtext (`.gmi`)** as its on-device documentation and hypertext format to avoid the perceived weight of full CommonMark/HTML parsing. While Gemtext served as a clean initial stepping stone, subsequent phases exposed significant limitations:

1. **Lossy Documentation Pipeline (`doc/**/*.md` $\rightarrow$ `tools/gendocs.py` $\rightarrow$ `/sd0/help/*.gmi`)**:
   - All LugalOS manuals are authored in GitHub-Flavored Markdown (`.md`).
   - Compiling `.md` to `.gmi` strips inline code backticks (`` `...` ``), bold (`**...**`), italic (`*...*`), and inline links (`[label](url)`), flattens tables into raw monospace blocks, and moves cross-references out of context to the bottom of the file.
2. **Mismatch with the On-Device AI Assistant (`ea`, Phases 58–60)**:
   - Markdown is the native lingua franca of LLMs.
   - When `ea` reads system manuals via `Action: help`, stripped Gemtext deprives the model of inline code delimiters and structured links.
   - When `ea` emits answers containing inline code, bold terms, bulleted sublists, and Markdown tables, neither the terminal nor the Gemtext reader can render that inline formatting natively.
3. **Underutilization of HiDPI Proportional Typography (Phases 56 & 57)**:
   - `drivers/reader_view.c` already possesses anti-aliased proportional Serif (regular & bold) and Monospace (`font_noto_serif`, `font_noto_mono`, `font_serif`, `font8x16`) engines, yet Gemtext cannot express a single bold word, italicized term, or inline monospace identifier inside a paragraph.
4. **Long-Form Reading Requirements (Articles, Technical Manuals & Books)**:
   - On handheld tablets like the M5Stack Tab5 (`720×1280` portrait mode) and 7" desk terminals (`1024×600` / `800×480`), reading rich documents requires **inline hyperlinks, footnotes (`[^1]`), structured tables (`|...|`), and a navigable Table of Contents (TOC)**.

### The Solution: Restricted (No-HTML) Markdown
99.9% of CommonMark's parser complexity and dynamic AST memory overhead stems from **embedded raw HTML5 blocks/tags** and pathological nested delimiter backtracking.

By **strictly prohibiting embedded HTML** (treating `<` as a literal character) and bounding inline emphasis to clean, single-pass spans, a **Restricted Markdown Engine (`fs/markdown.c`)** achieves:
* **Zero dynamic heap allocation ($O(1)$ SRAM)** — streaming line-by-line and word-by-word just like `fs/gemtext.c`.
* **Tiny code footprint** (~6–8 KB of `.text` across parser, GUI reader, and terminal viewer).
* **100% backward compatibility with Gemtext (`.gmi`) and Spartan/Gemini**: because `#` headings, `*` lists, `>` quotes, and ```` ``` ```` code fences are identical in Markdown and Gemtext, retaining the 15-line `=> url [label]` rule in the block parser makes every valid `.gmi` document a valid input to the Markdown engine. For external Spartan/Gemini clients requesting `.gmi`, `spartand` performs a lightweight streaming down-conversion on the fly.

---

## 2. Restricted Markdown Specification

### 2.1 Block-Level Elements (`fs/markdown.c`)

| Syntax | Block Type | Rendering in GUI Reader (`reader_view.c`) | Rendering in Terminal (`md` / `man` / `ea`) |
| :--- | :--- | :--- | :--- |
| `# H1` .. `#### H4` | `MD_LINE_H1` .. `MD_LINE_H4` | Bold Serif (scaled/ruled for H1/H2), indexed in TOC | ANSI Bold + color accent + underline |
| ```` ```[lang] ```` | `MD_LINE_CODE_TOGGLE` / `MD_LINE_CODE_TEXT` | Shaded box (`gray=242`) with `font_noto_mono` / `font8x16` | Indented monospace with dim gutter bar `│` |
| `> text` | `MD_LINE_QUOTE` | Left vertical bar (`READER_QUOTE_BAR_GRAY8`) + indented Serif | Vertical bar `│` + ANSI italic/dim |
| `> [!NOTE]` / `[!TIP]` / `[!WARNING]` | `MD_LINE_ALERT` | Highlighted callout header + accented left bar | Bold bracketed `[NOTE]` / `[WARNING]` header |
| `- item`, `* item`, `+ item` | `MD_LINE_ULIST` (indent levels `0..2`) | Filled/hollow bullet disc indented by `level * 20 px` | Indented `•` / `◦` bullet |
| `1. item`, `2. item` | `MD_LINE_OLIST` (indent levels `0..2`) | Bold numeral prefix `1.` indented by `level * 20 px` | Indented numeral `1.` |
| `---`, `***`, `___` | `MD_LINE_HRULE` | Horizontal rule across `max_w` (`rdraw_hline`) | Horizontal `────────` line |
| `\| A \| B \|` + `\|---\|---\|` | `MD_LINE_TABLE_HDR` / `SEP` / `ROW` | Proportional grid with header rule, cell padding, and inline spans | Aligned box-drawing table (`┌─┬─┐`, `│ │ │`) |
| `[^id]: text` | `MD_LINE_FOOTNOTE_DEF` | Registered as `#fn:id` anchor; rendered with `[id]` badge | Rendered with `[id]` prefix |
| `=> url [label]` | `MD_LINE_GEM_LINK` | Legacy Gemtext link line (100% `.gmi` compatibility) | Numbered `[n] => label` |
| `[TOC]` | `MD_LINE_TOC` | Inline clickable Table of Contents block | Indented section list |
| `$$ math $$` / `\[ math \]` | `MD_LINE_MATH_BLOCK` | Centered 2D typeset math formula (Knuth layout, fractions, radices, matrices) | Indented Unicode math / ASCII box art / LaTeX source |

### 2.2 Inline-Level Spans (`md_span_iter_t`)

Within any text-bearing block (`MD_LINE_TEXT`, headings, list items, blockquotes, table cells, footnote definitions), a zero-allocation iterator `md_span_next()` yields contiguous tokens with a style bitmask:

* **Plain Text (`MD_STYLE_NONE`)**: Standard proportional Serif (`font_noto_serif` / `font_serif`).
* **Bold (`**text**` or `__text__`, `MD_STYLE_BOLD`)**: Rendered with `bold = true` in `rdraw_serif_text()` (or ANSI `\033[1m`).
* **Italic (`*text*` or `_text_`, `MD_STYLE_ITALIC`)**:
  - In GUI Reader: rendered using true italic glyphs (`font_noto_serif_italic` / `font_serif_italic`, or a fast integer scanline shear `dx = (baseline - row) >> 2` when italic tables are disabled to save Flash).
  - In Terminal: ANSI italic/underline (`\033[3m` / `\033[4m`).
* **Bold + Italic (`***text***`, `MD_STYLE_BOLD | MD_STYLE_ITALIC`)**: Combined bold + italic attributes.
* **Inline Code (`` `code` ``, `MD_STYLE_CODE`)**:
  - Switched inline to monospace (`font_noto_mono` / `font8x16`) over a subtle shaded background rectangle (`gray = 236`), keeping variable-pitch tracking accurate in integer pixels!
  - In Terminal: ANSI cyan (`\033[36m`).
* **Inline Hyperlink (`[label](url)`, `MD_STYLE_LINK`)**:
  - Rendered with bold/underlined text and a compact `[n]` index prefix/suffix.
  - Records the **exact 2D bounding box `(x0, y0, x1, y1)`** in `r->links[]` (allowing multiple links on the same line or wrapped across two lines).
  - Tapping inside `(x0, y0, x1, y1)` on the touchscreen or pressing key `1..9` navigates to `url` (supporting relative `.md`/`.gmi` files, absolute paths, `spartan://`, and internal `#anchor` targets).
* **Inline Image (`![alt](url)`)**:
  - Rendered cleanly as `[Image: alt]` (or clickable link if `url` points to a `.pbm`/`.pgm`/`.ppm` screenshot viewing target).
* **Footnote Reference (`[^id]`, `MD_STYLE_FOOTNOTE`)**:
  - Rendered as a raised/bracketed `[id]` link pointing to `#fn:id`.
  - Activating it pushes the current `(path, scroll_y)` onto the reader's history stack and scrolls directly to the `[^id]:` definition; pressing `b` (Back) returns to the exact paragraph offset!
* **Inline LaTeX Mathematics (`$math$`, `MD_STYLE_MATH`)**:
  - Evaluated by the Knuth math layout engine in text-style ($T$).
  - Measured with exact height, depth, and advance width, aligning the formula baseline to the running text baseline.
  - Supports subscripts/superscripts (`x_i^2`), fractions (`\frac{a}{b}`), radicals (`\sqrt{x}`), Greek symbols ($\alpha, \beta, \gamma$), operators, and delimiters.
  - In Terminal: Formatted using Unicode math superscripts/symbols (`x²`, `α`, `√x`), ANSI cyan, or raw LaTeX fallback.
* **No Embedded HTML Rule**:
  - Any `<tag>` is rendered literally as text. Character entities `&amp;`, `&lt;`, `&gt;`, `&quot;`, `&rarr;` are decoded inline in the span reader.

---

## 3. Architecture of Key Subsystems

```
                         ┌──────────────────────────────┐
                         │   Markdown / Gemtext Source  │
                         │  (/sd0/help/*.md, books, ea) │
                         └──────────────┬───────────────┘
                                        │
                         ┌──────────────▼───────────────┐
                         │        fs/markdown.c         │
                         │ • Block Line Classifier      │
                         │ • Inline Span Tokenizer      │
                         │ • GFM Table Cell Splitter    │
                         │ • Heading Anchor & TOC Scan  │
                         └──────────────┬───────────────┘
                                        │
        ┌───────────────────────────────┼───────────────────────────────┐
        ▼                               ▼                               ▼
┌───────────────────────┐   ┌───────────────────────┐   ┌───────────────────────┐
│   GUI Ribbon Reader   │   │  Terminal Viewer & ea │   │  Spartan Daemon       │
│ (drivers/reader_view) │   │    (kernel/shell.c)   │   │   (net/spartan.c)     │
├───────────────────────┤   ├───────────────────────┤   ├───────────────────────┤
│ • Serif / Bold / Ital │   │ • ANSI Bold/Ital/Cyan │   │ • Native .md serving  │
│ • Inline Mono `code`  │   │ • Box-drawing Tables  │   │ • Streaming .md->.gmi │
│ • 2D (x0,y0,x1,y1)    │   │ • [1]..[9] Link Jumps │   │   down-converter for  │
│   Touch Link Hit-Test │   │ • Live `ea` Answer    │   │   external Spartan /  │
│ • Proportional Tables │   │   Markdown Formatter  │   │   Gemini clients      │
│ • TOC Overlay ('t')   │   │                       │   │                       │
│ • Footnote Jump/Back  │   │                       │   │                       │
└───────────────────────┘   └───────────────────────┘   └───────────────────────┘
```

### 3.1 2D Hit-Testing & Scroll-Preserving History in `drivers/reader_view.c`

Currently, `reader_link_t` in `drivers/reader_view.c` stores only vertical bounds (`int y0, y1`) because Gemtext links occupy an entire line, and `history[][]` stores only the file path (`char history[8][128]`).

Phase 61 upgrades both structures:
```c
typedef struct {
    int16_t x0, y0, x1, y1;  /* 2D client-area bounding box */
    int     num;             /* 1-based visible link index (1..9) */
    char    url[128];        /* Target path, URL, or #anchor / #fn:id */
} reader_link_t;

typedef struct {
    char    path[128];
    int     scroll_y;        /* Saved vertical scroll position for exact back-navigation */
} reader_hist_entry_t;
```
* **Touch Accuracy**: `reader_tap_instance(rid, client_x, client_y)` checks `client_x >= link.x0 && client_x <= link.x1 && client_y >= link.y0 && client_y <= link.y1`. If an inline link wraps across a line break, both segments register a bounding box sharing the same `num` and `url`.
* **Internal Anchors (`#heading-slug` and `#fn:id`)**: Navigating to `#...` pushes `(current_path, current_scroll_y)` onto `history[]` and sets `r->scroll_y = target_doc_y`. Pressing `b` or `Left` pops the history entry and restores `r->scroll_y`!

### 3.2 Proportional GFM Table Layout in `drivers/reader_view.c`

When `reader_draw()` encounters a `MD_LINE_TABLE_HDR` line:
1. It parses up to `MD_TABLE_MAX_COLS` (8) cells on the row using `md_parse_table_row()`.
2. Column widths are computed cleanly:
   - By default, columns divide `max_w` proportionally based on the header + first row character lengths (clamped to a minimum readable width), or wrap cell text within each column box using `render_wrapped_spans()`.
3. Visual styling:
   - Header row: shaded background (`gray = 238`), bold Serif text, and a 2 px bottom rule (`READER_RULE_GRAY8`).
   - Data rows: alternating subtle row tint (`gray = 246` / `250`), 1 px horizontal separator (`gray = 210`), and 1 px vertical column dividers.

### 3.3 Interactive Table of Contents (TOC) & Book Navigation

Long documents and books benefit from instant structural navigation:
1. Each `reader_state_t` maintains a compact TOC array populated during document scan:
   ```c
   #define READER_TOC_MAX 32
   typedef struct {
       char     title[48];
       uint8_t  level;    /* 1..4 */
       int      doc_y;    /* Vertical pixel offset in document */
   } reader_toc_entry_t;
   ```
2. Pressing `t` (or `T`) in `reader` toggles the **TOC View Mode**:
   - Renders the document's hierarchical outline with dotted leader lines and section numbers.
   - Tapping any TOC entry with the touchscreen (or pressing `1..9`) exits TOC mode, pushes the prior scroll position to history, and jumps straight to `entry.doc_y`!

### 3.4 Spartan / Gemini Down-Converter (`net/spartan.c`)

To ensure LugalOS's `spartand` (and future `geminid` in Phase 52) remains a first-class citizen of the Spartan/Gemini ecosystem while storing documentation in `.md`:
* **Path Resolution in `spartand`**:
  - If a client requests `/index.gmi` or `/help/shell.gmi` and `/sd0/pub/help/shell.md` exists (while `.gmi` does not), `spartand` opens the `.md` file and streams it through `md_to_gemtext_line()` on the fly!
  - `md_to_gemtext_line()` strips inline `**`/`*`/`` ` ``, queues any inline `[label](url)` links found in the line to be emitted immediately after the line as `=> url label`, and wraps contiguous `|...|` table lines inside ```` ``` ```` fences.
* If a client explicitly requests `/help/shell.md`, `spartand` serves the raw Markdown with header `2 text/markdown\r\n`.

### 3.5 Restricted LaTeX Math Typesetting Engine (`fs/latex_math.c`, `drivers/latex_layout.c`)

To support graduate-level mathematics across scientific manuals, the Computer Algebra System (CAS), and on-device AI assistant (`ea`) without the weight of full TeX:

```
                       ┌───────────────────────────────┐
                       │  Markdown Span / Block Token  │
                       │     $ ... $  or  $$ ... $$    │
                       └───────────────┬───────────────┘
                                       │
                       ┌───────────────▼───────────────┐
                       │      fs/latex_math.c          │
                       │ • Recursive-descent LL(1)     │
                       │ • Scratch Arena AST (4–8 KB)  │
                       └───────────────┬───────────────┘
                                       │
                       ┌───────────────▼───────────────┐
                       │     drivers/latex_layout.c    │
                       │ • Knuth Appendix G rules      │
                       │ • Styles: D, T, S, SS         │
                       │ • 2D Box Hierarchy (W, H, D)  │
                       └───────────────┬───────────────┘
                                       │
                 ┌─────────────────────┴─────────────────────┐
                 ▼                                           ▼
      ┌──────────────────────┐                    ┌──────────────────────┐
      │  GUI Ribbon Reader   │                    │   Terminal / Shell   │
      │ (drivers/reader_view)│                    │   (kernel/shell.c)   │
      ├──────────────────────┤                    ├──────────────────────┤
      │ • Proportional math  │                    │ • Unicode math symbols│
      │ • Anti-aliased 4-bpp │                    │   (x², α, ∫ f(x) dx) │
      │ • Fractional rules   │                    │ • Box-drawing ASCII  │
      │ • Dynamic delimiters │                    │   art or raw LaTeX   │
      └──────────────────────┘                    └──────────────────────┘
```

#### 3.5.1 The Restricted Mathematical Subset
Knuth's original mathematical typesetting model (*The TeXbook*, Appendix G) relies on **22 deterministic box layout rules** that operate entirely independently of macro expansion, catcodes, and I/O. By eliminating Turing-complete macros (`\def`, `\let`), custom catcodes, and external packages (`\usepackage`), a lightweight parser handles graduate-level mathematics cleanly:

* **Inline & Display**: `$ ... $` (text style, baseline-aligned) and `$$ ... $$` / `\[ ... \]` (centered block with vertical margins).
* **Scripts & Indices**: Subscripts `x_i`, superscripts `x^2`, combined `x_i^2`, tensor indices `R^{\mu}_{\phantom{\mu}\nu\rho\sigma}`, nested scripts `e^{-\frac{x^2}{2\sigma^2}}`.
* **Fractions & Binomials**: `\frac{numerator}{denominator}`, `\binom{n}{k}`, nested fractions with automatic rule thickness and clearance scaling.
* **Radicals**: `\sqrt{x}`, `\sqrt[n]{x}` with horizontal overline and surd tick.
* **Large Operators with Limits**: `\sum`, `\prod`, `\int`, `\iint`, `\oint`, `\bigcup`, `\bigcap` (limits positioned above/below in display style, and right-shifted in text style).
* **Extensible Delimiters**: `\left( ... \right)`, `\left[ ... \right]`, `\left\{ ... \right\}`, `\left| ... \right|`, `\langle ... \rangle`, `\lfloor ... \rfloor` automatically sized to cover inner box height + depth.
* **Matrices & Environments**: `\begin{matrix}`, `\begin{pmatrix}`, `\begin{bmatrix}`, `\begin{cases}`, `\begin{aligned}` with `&` alignment and `\\` row breaks.
* **Math Alphabets & Symbols**:
  - Math Italic Latin ($a..z, A..Z$) & Greek ($\alpha..\omega, \Gamma..\Omega$).
  - Blackboard Bold ($\mathbb{R}, \mathbb{C}, \mathbb{Z}, \mathbb{Q}, \mathbb{N}$).
  - Calligraphic ($\mathcal{L}, \mathcal{H}, \mathcal{F}$).
  - Relations & Operators: $\le, \ge, \neq, \approx, \equiv, \times, \cdot, \pm, \in, \subset, \forall, \exists, \partial, \nabla, \infty, \to, \implies, \iff$.
  - Named functions: `\sin`, `\cos`, `\ln`, `\exp`, `\det`, `\ker`, `\dim`, `\lim`, `\max`, `\min`.
  - Accents: `\hat{x}`, `\vec{v}`, `\bar{x}`, `\dot{x}`, `\ddot{x}`, `\tilde{x}`.
  - Spacing adjustments: `\,`, `\:`, `\;`, `\!`, `\quad`.

#### 3.5.2 Memory Model: Stack Scratch Arena ($O(1)$ SRAM Overhead)
Formulas are parsed into a compact Abstract Syntax Tree (AST) using a 4–8 KB scratch arena allocated on the stack:
```c
typedef enum {
    MATH_BOX_CHAR,
    MATH_BOX_HLIST,
    MATH_BOX_VLIST,
    MATH_BOX_RULE,
    MATH_BOX_DELIM,
    MATH_BOX_FRAC,
    MATH_BOX_RADICAL,
    MATH_BOX_SCRIPT,
    MATH_BOX_MATRIX,
} math_box_type_t;

typedef struct math_box {
    uint8_t   type;       /* math_box_type_t */
    uint8_t   style;      /* STYLE_DISPLAY, STYLE_TEXT, STYLE_SCRIPT, STYLE_SCRIPTSCRIPT */
    int16_t   w, h, d;    /* Advance width, height above baseline, depth below baseline */
    int16_t   x, y;       /* Relative position within parent */
    union {
        struct { uint16_t glyph_idx; const void *font; } ch;
        struct { struct math_box *head; } list;
        struct { struct math_box *num; struct math_box *den; int16_t thickness; } frac;
        struct { struct math_box *body; struct math_box *deg; } radical;
        struct { struct math_box *nucleus; struct math_box *sub; struct math_box *sup; } script;
        struct { struct math_box **cells; uint8_t rows, cols; int16_t col_w[8]; } matrix;
    } u;
    struct math_box *next;
} math_box_t;
```
Because nodes are ~32 bytes, a 4 KB arena holds >120 nodes—sufficient for complex Hamiltonian operators, Cauchy integral formulas, and 4×4 metric tensors. Once measured and blitted to the target 1-bpp/4-bpp framebuffer canvas, the scratch pointer is reset to 0. Zero heap fragmentation, zero memory leaks.

#### 3.5.3 Pure Integer Knuth Layout Engine (`drivers/latex_layout.c`)
All calculations are performed in **integer millipixels or scaled units**, preserving LugalOS’s strict **Zero Floating-Point in Kernel / M-Mode** invariant:
- **Math Axis Centering**: Fraction rules, operator centers, and radical baselines align with the universal math axis (half x-height).
- **Four Standard Styles**:
  - Display Style ($D$): Large operators with limits above/below; full sizing.
  - Text Style ($T$): Compact inline math; limits placed to the right.
  - Script Style ($S$): Subscripts and superscripts (~70% font scale).
  - Script-Script Style ($SS$): Nested indices (~50% font scale).
- **Extensible Delimiter Construction**: Big parentheses, brackets, and braces are composed of discrete top/extender/bottom glyph segments when exceeding single-glyph height limits.
- **Rule Drawing**: Fraction rules and radical overbars are rendered via `rdraw_fill_rect()`, requiring zero bitmap storage.

#### 3.5.4 Math Typography Pipeline (`tools/gen_math_font.py`, `drivers/font_math.c`)
- Generated from open fonts (Latin Modern Math / STIX Two Math) via `tools/gen_math_font.py`:
  - ~250 glyphs across 3 optical sizes (Normal, Script, Script-Script).
  - Standard Density (1-bpp): ~25–35 KB Flash `.rodata`.
  - HiDPI Density (4-bpp anti-aliased): ~90–120 KB Flash `.rodata`.
  - Stored strictly in Flash `.rodata`, sorted by ASCII/command token for fast binary search.

#### 3.5.5 Ecosystem Synergies
- **Lisp CAS (`cas/format.lisp`)**: Adds `(to-latex expr)` to the CAS, allowing symbolic results from `diff`, `integrate`, `solve`, and `taylor` to be typeset directly on the GUI reader canvas or plot windows!
- **On-Device AI Assistant (`ea`)**: `ea` outputs technical answers containing `$ ... $` and `$$ ... $$` math, rendered into book-grade typography on the M5Stack Tab5 (`esp32p4-tab5`) and desk terminals.
- **Terminal & Shell Fallback**: In `lsh` or serial UART consoles, formulas degrade cleanly to Unicode math symbols (`x²`, `α`, `√x`, `∫`) or multi-line ASCII box-drawing art (similar to SymPy/CAS pretty-printing).

---

## 4. Implementation Milestones

```
Phase 61 Milestones:
  M61.1: Restricted Markdown Parser & Span Tokenizer (`fs/markdown.c`, `tests/test_markdown.py`)
    │
  M61.2: GUI Ribbon Reader Upgrade: Inline Spans, 2D Links, Tables, Footnotes & TOC (`drivers/reader_view.c`)
    │
  M61.3: Terminal Markdown Viewer (`md` / `man`) & `ea` Live Markdown Formatting (`kernel/shell.c`, `kernel/ea.c`)
    │
  M61.4: Documentation Pipeline Migration to Native `.md` & Spartan On-the-Fly Down-Converter (`tools/gendocs.py`, `net/spartan.c`)
    │
  M61.5: Hardware Verification (Tab5, LCD-7B, RP2350-Terminal) & Full Regression Suite
    │
  M61.6: Restricted LaTeX Math Engine: Parser, Knuth Layout, Math Fonts & CAS/`ea` Integration (`fs/latex_math.c`, `drivers/latex_layout.c`)
```

### Milestone 61.1: Restricted Markdown Parser & Span Tokenizer (`fs/markdown.c`, `include/fs/markdown.h`)
* **Goal**: Implement the freestanding, zero-allocation Restricted Markdown parser and inline span tokenizer.
* **Deliverables**:
  1. `include/fs/markdown.h` and `fs/markdown.c`:
     - Block line parser `md_parse_line()` supporting headings (`#`..`####`), fenced code blocks (` ``` `), blockquotes (`>`), GFM alerts (`> [!NOTE]`, etc.), unordered/ordered lists with nesting level, horizontal rules (`---`), pipe table rows/separators (`|...|`), footnote definitions (`[^id]:`), `[TOC]`, and legacy Gemtext `=>` links.
     - Inline span iterator `md_span_init()` / `md_span_next()` supporting `**bold**`, `*italic*`, `***bold-italic***`, `` `inline code` ``, `[label](url)`, `![alt](url)`, `[^footnote]`, backslash escapes (`\*`, `` \` ``, `\|`), and basic HTML entity decoding (`&amp;`, `&lt;`, `&gt;`, `&rarr;`), while treating raw `<tags>` as plain text.
     - Table row splitter `md_parse_table_cells()`.
     - Heading slug generator `md_heading_slug()` for `#anchor` resolution.
     - URL/anchor resolver `md_resolve_url()` (compatible superset of `gemtext_resolve_url()`).
  2. Host unit test suite `tests/test_markdown.py` (compiled under ASan/UBSan) testing all block types, nested/unclosed inline delimiters, table parsing, footnote tokenization, and `.gmi` backward compatibility.
* **Verification**: `tests/test_markdown.py` passes 100% under ASan/UBSan with zero heap allocations.

### Milestone 61.2: GUI Ribbon Reader Upgrade (`drivers/reader_view.c`)
* **Goal**: Upgrade the Ribbon Reader to render full Restricted Markdown with inline typography, 2D touch link boxes, proportional tables, footnotes, and an interactive Table of Contents.
* **Deliverables**:
  1. **Inline Span Word-Wrapper (`render_wrapped_spans()`)**:
     - Measures and renders mixed Serif (regular, bold, italic/slanted) and Monospace (`` `code` `` with shaded pill background) spans within wrapped lines.
     - Computes 2D `(x0, y0, x1, y1)` hit-boxes for inline `[label](url)` and `[^id]` footnote links.
  2. **Proportional Table Renderer**:
     - Renders GFM pipe tables with shaded headers, cell borders, and inline span formatting inside cells.
  3. **Scroll-Preserving History & Footnotes**:
     - Upgrade `reader_state_t` history stack to save `(path, scroll_y)`.
     - Support jumping to `#anchor` headings and `#fn:id` footnotes and returning to the exact prior `scroll_y` via `b` / `Left`.
  4. **Interactive Table of Contents (`t` hotkey)**:
     - Collect document headings into `r->toc[]` and toggle a navigable TOC view on `t`.
* **Verification**: Render a comprehensive test document (`markdown_showcase.md` containing inline formatting, code blocks, GFM tables, footnotes, and TOC) in QEMU `ramscreen` and on hardware panels.

### Milestone 61.3: Terminal Markdown Viewer (`md` / `man`) & `ea` Output Formatting
* **Goal**: Upgrade the terminal viewer and `ea` assistant output to render Restricted Markdown cleanly in ANSI terminals.
* **Deliverables**:
  1. Update terminal pager in `kernel/shell.c` (`cmd_md` / `cmd_gem` / `cmd_man`):
     - Render inline bold (`\033[1m`), italic/underline (`\033[4m`), inline code (`\033[36m`), numbered inline links `[1]`, footnotes, and formatted ASCII/UTF-8 box-drawing tables.
     - Keep `gem` as a command alias for `md` so existing scripts and muscle memory continue to work.
  2. **`ea` Assistant Integration (`kernel/ea.c`)**:
     - Update `Action: help` in `kernel/ea.c` to read `.md` files (with `.gmi` fallback) and support `#subheading` extraction on Markdown headers.
     - Format `ea`'s final `Answer:` output through the terminal Markdown renderer (or optional `ea -r "<prompt>"` to open the answer directly in a Ribbon Reader window!).
* **Verification**: Run `man shell`, `md /sd0/help/cas.md`, and `ea` queries in QEMU and on hardware; verify ANSI styling and link navigation.

### Milestone 61.4: Documentation Pipeline Migration to `.md` & Spartan Down-Converter
* **Goal**: Switch `/sd0/help/` and `/flash0/help/` to native Restricted Markdown (`.md`) and equip `spartand` with on-the-fly `.md` $\rightarrow$ `.gmi` conversion.
* **Deliverables**:
  1. **`tools/gendocs.py` Evolution**:
     - Instead of stripping Markdown into `.gmi`, `tools/gendocs.py` now validates that `doc/**/*.md` contains no raw HTML tags, rewrites relative cross-doc links to clean flat help targets (e.g. `[C Programming](c_programming.md)`), and stages `/sd0/help/*.md` (and `/flash0/help/*.md`).
     - Optionally still supports `--gemtext` export for static `.gmi` generation.
  2. **Path Resolution (`resolve_doc_path()`)**:
     - Update `drivers/reader_view.c`, `kernel/shell.c`, and `kernel/ea.c` to look up `/sd0/help/<topic>.md` and `/flash0/help/<topic>.md` first, falling back to `.gmi` so any user-created `.gmi` files continue to work seamlessly.
  3. **Spartan On-the-Fly Down-Converter (`net/spartan.c`)**:
     - Add streaming `.md` $\rightarrow$ `.gmi` conversion in `spartand` when a client requests a `.gmi` URI (or directory index) backed by a `.md` file, while serving raw `text/markdown` when `.md` is requested directly.
* **Verification**: `python3 tools/gendocs.py --check` passes; `man <topic>` and `reader <topic>` open `.md` manuals with full inline formatting, tables, and links; Spartan loopback test verifies both `.md` and down-converted `.gmi` serving.

### Milestone 61.5: Hardware Verification & Full Regression Suite
* **Goal**: Verify the Restricted Markdown Engine on connected hardware targets and update automated test suites.
* **Deliverables**:
  1. Verify on **M5Stack Tab5** (`esp32p4-tab5`, Portrait & Landscape HiDPI), **ESP32-P4-LCD-7B** (`esp32p4-lcd7b`), and **RP2350-Terminal** (`rp2350-terminal`).
  2. Capture screenshots (`lcd screen`) showing proportional tables, inline code pills, footnotes, and TOC view.
  3. Update `tests/runner.py`, `tests/test_markdown.py`, and `tests/hw/test_esp32p4.py` to validate `.md` help files, 2D inline link tapping, and TOC navigation.

### Milestone 61.6: Restricted LaTeX Math Engine (`fs/latex_math.c`, `drivers/latex_layout.c`, `drivers/font_math.c`)
* **Goal**: Implement the freestanding, zero-allocation LaTeX math parser, Knuth Appendix G box layout engine, math font generator, and CAS/`ea` integration.
* **Deliverables**:
  1. **Math Font Assets (`tools/gen_math_font.py`, `drivers/font_math.c`, `include/drivers/font_math.h`)**:
     - Extracts ~250 math glyphs across 3 optical sizes (Normal, Script, Script-Script) from Latin Modern Math or STIX Two Math.
     - Emits 1-bpp bilevel tables (~25 KB) for standard targets and 4-bpp anti-aliased tables (~100 KB) for HiDPI targets in Flash `.rodata`.
     - Fast lookup tables sorted by token/symbol.
  2. **Restricted LaTeX Math Parser (`fs/latex_math.c`, `include/fs/latex_math.h`)**:
     - Recursive-descent LL(1) parser parsing `$ ... $` and `$$ ... $$` into `math_box_t` nodes using a stack scratch arena (4–8 KB).
     - Handles subscripts/superscripts (`x_i^2`), fractions (`\frac{a}{b}`), radicals (`\sqrt{x}`, `\sqrt[n]{x}`), limits (`\sum`, `\int`), extensible delimiters (`\left( ... \right)`), matrices (`\begin{pmatrix}`), accents (`\hat{x}`), and Greek/Blackboard alphabets.
  3. **Knuth Appendix G Box Layout Engine (`drivers/latex_layout.c`)**:
     - Pure integer millipixel box-and-glue layout computing bounding boxes `(w, h, d)` across Display, Text, Script, and Script-Script styles.
     - Blits assembled character cells, fraction rules, and dynamic delimiters onto 1-bpp canvas and 4-bpp anti-aliased surfaces.
  4. **Reader, Shell & CAS Integration**:
     - Integrates inline math `$ ... $` into `render_wrapped_spans()` in `drivers/reader_view.c` with baseline alignment.
     - Adds centered block rendering for `$$ ... $$` with touch interaction.
     - Terminal viewer fallback in `kernel/shell.c`: formats math using Unicode superscripts and math symbols or multi-line ASCII box art.
     - CAS formatter enhancement in `cas/format.lisp`: implements `(to-latex expr)` to render CAS expressions directly into typeset math.
  5. **Automated Test Suite (`tests/test_latex_math.py`)**:
     - Host test suite verifying AST arena sizing, Appendix G bounding box calculations, and parser edge cases under ASan/UBSan.
* **Verification**: Render complex mathematical equations (Maxwell's equations, Schrödinger equation, Cauchy's residue theorem, and CAS output) in QEMU `ramscreen`, Tab5, and LCD-7B with zero heap allocations.

---

## 5. Architectural Rules & Constraints Matrix

| Constraint | Enforcement in Phase 61 |
| :--- | :--- |
| **Freestanding C** | Built strictly with `-nostdlib -ffreestanding`. Zero `libc` `malloc`/`stdio` in `fs/markdown.c`. |
| **Zero FPU in M-Mode** | All glyph width calculations, table column distributions, scanline shears, and Knuth math box measurements use pure integer arithmetic. |
| **Zero SRAM Overhead** | Streaming parser uses stack-local structs; math expressions allocate from a 4–8 KB stack arena reset per formula; font tables reside in Flash `.rodata`. |
| **No Embedded HTML** | `<` and `>` inside paragraphs are rendered as literal characters, eliminating HTML5 parser complexity and XSS/injection surfaces. |
| **Pure Integer Math Layout** | Knuth Appendix G box measurements, rule alignments, and baseline calculations use pure integer millipixel math; zero floating-point instructions or soft-float calls. |
| **Flash Math Fonts** | Math glyph tables (1-bpp and 4-bpp) reside entirely in Flash `.rodata`, requiring zero runtime heap. |
| **100% `.gmi` Compatibility** | `=> url [label]` remains a supported block line in `fs/markdown.c`, and `spartand` down-converts `.md` $\rightarrow$ `.gmi` on the fly. |
