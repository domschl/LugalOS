# Phase 53 — Gemtext Hypertext Engine, Spartan Protocol & Ribbon Reader

**Status: IMPLEMENTED / VERIFIED.**  
**Target Version:** 0.19.0  
**Follows:** Phase 50 (Documentation System), Phase 51 (Shell Usability & Composability), Phase 52 (Remote Admin & TLS).  
**Primary Focus:** Gemtext (`.gmi`) lightweight hypertext standard, complete documentation migration, terminal viewer (`gem`), dedicated GUI Ribbon Reader (`RIBBON_WIN_READER`) with proportional serif typography, and sandboxed Spartan protocol (`spartan://`, port 300) client/server with strict access discrimination.

---

## 1. Executive Summary & Problem Statement

Across Phases 44, 47, 49, and 50, LugalOS gained a high-resolution horizontal window ribbon (1024×600 and 800×480), Wi-Fi networking, and an on-device documentation browser. However, several friction points and architectural gaps remain:

1. **Fragile Link Parsing in Phase 50 Help Browser**:
   - The Phase 50 documentation browser (`cmd_man` / `pager_view_file` in `kernel/shell.c`) attempted to parse numbered reference links from plain text files using informal string patterns (`[1]`, `[2]`).
   - In files like `editors.txt`, inline prose such as `Use e for writing C programs (C programming [1])` collided with the reference table at the bottom (`[1] c_programming`), causing the parser to match `[1]` in the text and extract the trailing `)` as the target filename (`/sd0/help/).txt`).
   - Heuristic text-link parsing is fragile and conflates content with navigational metadata.

2. **Absence of a Native Hypertext Representation**:
   - LugalOS has no native, structured, cross-linked document format. Markdown is too complex and heavy for freestanding on-device parsing ($O(N)$ AST allocation, complex inline spanning rules, arbitrary HTML embedding).
   - Plain text lacks semantics for headers, quotes, lists, preformatted code, and explicit navigation.

3. **Suboptimal Typography for Long-Form Reading**:
   - The GUI ribbon currently supports only terminal windows (`RIBBON_WIN_TERM`) and graphics canvases (`RIBBON_WIN_CANVAS`), both constrained to fixed-pitch monospace fonts (8×16 Spleen).
   - While monospace is essential for code and shell output, long-form reading (system manuals, literature, books, articles) is visually tiring in fixed-pitch fonts. The 7" displays have ample resolution (1024×600) to render comfortable, variable-pitch serif typography.

4. **Lack of a Lightweight, Minimalist Networked Document Protocol**:
   - HTTP/1.1 and modern web formats (HTML/CSS/JS) impose heavy parser overhead and bloat.
   - Gemini (`gemini://`, port 1965) is the ideal modern hyperminimalist web protocol, but strictly mandates TLS 1.2/1.3 and Client mTLS Certificates for authentication (developed in Phase 52).
   - **Spartan** (`spartan://`, port 300) provides the exact same lightweight hypermedia architecture (Gemtext documents, line-oriented navigation, input/upload support) over plain TCP without cryptographic overhead, serving as the immediate network hypermedia transport.

Phase 53 resolves these challenges by adopting **Gemtext (`text/gemini`, `.gmi`)** as LugalOS's standard linked hypertext format, migrating all system documentation to `.gmi`, providing both a **terminal viewer (`gem`)** and a **dedicated GUI Ribbon Reader (`RIBBON_WIN_READER`)** with proportional serif typography, and implementing a **sandboxed Spartan client and server** governed by a strict access discriminator.

---

## 2. Architecture & Design Decisions

### 2.1 The Gemtext Hypertext Standard

Gemtext is a line-oriented, stripped-down text format with strict syntactic invariants:
* Every line is typed by its first 1 to 3 characters.
* There is **zero inline styling** (no bold, italic, or inline links inside paragraphs), which guarantees $O(1)$ memory consumption and streaming single-pass rasterization.
* **Line Types**:
  * `Text`: Regular paragraph text.
  * `=> target [label]`: Hyperlink. Unambiguously starts with `=>` at column 0.
  * `# Heading 1`, `## Heading 2`, `### Heading 3`: Structural section headers.
  * `* Item`: Unordered bullet list item.
  * `> Quote`: Blockquote.
  * ```` ``` ```` [alt]: Preformatted code block toggle. All lines within are rendered in monospace without word wrapping.

```
+-------------------------------------------------------------------------+
| GEMTEXT STREAM: fs/gemtext.c                                            |
|                                                                         |
| Line 0: # C Programming in LugalOS          -> GEM_LINE_H1              |
| Line 1: LugalOS includes a native C compiler-> GEM_LINE_TEXT            |
| Line 2: ```c                                -> GEM_LINE_PRE_START       |
| Line 3: int main(void) { return 0; }        -> GEM_LINE_PRE_TEXT        |
| Line 4: ```                                 -> GEM_LINE_PRE_END         |
| Line 5: => editors.gmi Text Editors         -> GEM_LINE_LINK            |
|            Target: "editors.gmi", Label: "Text Editors"                 |
+-------------------------------------------------------------------------+
```

Because links *must* occupy their own dedicated line starting with `=>`, reference collisions like the Phase 50 `find_link_target()` bug are mathematically impossible.

---

### 2.2 Dual Viewer Architecture: Terminal Monospace vs. Ribbon Proportional Reader

LugalOS serves diverse environments, from resource-constrained serial consoles to rich 7" LCD touchscreens. Phase 53 implements two coordinated viewers:

```
                            ┌────────────────────────┐
                            │   Gemtext Source File  │
                            │   (/sd0/help/*.gmi)    │
                            └───────────┬────────────┘
                                        │
                         ┌──────────────┴──────────────┐
                         ▼                             ▼
            ┌─────────────────────────┐   ┌─────────────────────────┐
            │ Terminal Viewer (`gem`) │   │  Ribbon Reader Window   │
            │ (Console / VT100 / TTY) │   │   (RIBBON_WIN_READER)   │
            ├─────────────────────────┤   ├─────────────────────────┤
            │ • 8x16 Monospace font   │   │ • Proportional Serif    │
            │ • ANSI bold / headers   │   │ • 8x16 Mono Code Blocks │
            │ • Streaming pager       │   │ • Variable Word Wrap    │
            │ • Keys: [1]..[9], 'b'   │   │ • Touch Tap Links & Pan │
            └─────────────────────────┘   └─────────────────────────┘
```

1. **Terminal Viewer (`gem <path_or_url>` / `man <topic>`)**:
   - Operates in any standard terminal (console UART, USB CDC, virtual terminals).
   - Renders in 8×16 monospace font with ANSI color accents (bright headings, highlighted link numbers).
   - Displays links with dynamic index tags: `[1] => c_programming.gmi  C Programming Guide`.
   - Keys: `1`..`9` to follow links, `b` to pop history stack, `Space`/`PageDown` to scroll, `q` to exit.
2. **Dedicated Ribbon Reader Window (`RIBBON_WIN_READER` = 3)**:
   - Operates in the horizontal tiling ribbon alongside terminal and canvas windows.
   - Target displays: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (1024×600) and RP2350-LCD-7 (800×480).
   - Renders prose and headings in an elegant, readable **variable-pitch serif font**.
   - Renders preformatted blocks in 8×16 monospace font.
   - **Direct Touch Navigation**: Tap a link directly on the GT911 touchscreen to navigate; vertical swipe to pan smoothly.

---

### 2.3 Proportional Typography & Word-Wrap Engine

To support variable-pitch serif rendering on 1-bpp and GRAY8 framebuffers without floating-point math:
* **Glyph Representation**: A compact 1-bpp bitmap font table in Flash `.rodata`, accompanied by a 1-byte per character width table `char_width[128]`.
* **Proportional Word Wrapping**:
  - The rasterizer measures words by summing character widths in integer pixels:
    $$\text{width}(word) = \sum_{c \in word} \text{char\_width}[c]$$
  - If `x + width(word) > right_margin`, the rasterizer issues a carriage return/line feed (`x = left_margin; y += line_height`).
* **Visual Hierarchy**:
  - H1 (`#`): Scaled or bold serif glyphs, with 4 px accent underline.
  - H2 (`##`): Medium serif glyphs.
  - H3 (`###`): Small caps / italic serif glyphs.
  - Blockquotes (`>`): Indented with a 2 px vertical bar (`x_bar = left_margin - 6`).
  - Preformatted (```` ``` ````): Fixed 8 px advance per glyph (Spleen 8×16) on a shaded background.

---

### 2.4 Access Control & The Security Discriminator

The Spartan protocol (`spartan://`) is plaintext over TCP port 300 and does not feature built-in authentication headers or bearer tokens. To preserve LugalOS's strict capability isolation (Phase 21):

```
                                  ┌──────────────────────────┐
                                  │    Incoming Request      │
                                  └─────────────┬────────────┘
                                                │
                         ┌──────────────────────┴──────────────────────┐
                         ▼                                             ▼
             [Local Execution Context]                    [Network Execution Context]
             (Terminal Shell / Ribbon)                    (spartand Daemon on TCP 300)
                         │                                             │
               Full VFS Capabilities                      Strict Chroot Sandbox (/sd0/pub/)
               - file:///sd0/help/*.gmi                   - Traversal Rejected (.., //, NUL)
               - file:///sd0/books/*.gmi                  - Zero access to /sd0/system/
               - Future: sys:// dynamic status            - Zero access to /proc or keys
```

1. **Local Domain (Shell `gem`, `man`, GUI Ribbon Reader)**:
   - Runs with local interactive process credentials.
   - Can access any authorized local VFS file (`file:///sd0/help/...`, `/sd0/books/...`, `/flash0/...`).
   - Synthetic namespaces (e.g. `sys://status` for live memory/tasks, `sys://wifi`, `sys://kmsg`) are accessible *exclusively* from the local domain.
2. **Network Domain (`spartand` daemon)**:
   - Listens on TCP port 300 (`net/tcp.c`).
   - **Strict Chroot Jail**: Bounded exclusively to `/sd0/pub/` (e.g. `/sd0/pub/help/`, `/sd0/pub/books/`).
   - **Path Traversal Prevention**: Canonicalizes paths; any path containing `..`, null bytes, or escaping the root is immediately rejected with Spartan status `40 Client Error`.
   - **Protected Namespace**: Never serves `/sd0/system/identity.*`, private peer keys, or internal `/proc/` diagnostics to unauthenticated network clients.
   - **Restricted Subtrees**: For private content under `/sd0/pub/restricted/`, `spartand` checks for a URL query token (`?key=<hex_grant>`) verified against `peers` grants.

---

## 3. Milestones & Implementation Roadmap

```
Phase 53 Milestones:
  M53.1: Gemtext Streaming Parser & Library (`fs/gemtext.c`, `include/fs/gemtext.h`)
    │
  M53.2: Pipeline & Help Migration (`tools/gendocs.py`, `/sd0/help/*.gmi`, `man`/`help`)
    │
  M53.3: Terminal Gemtext Viewer (`gem <path_or_url>`, Link Nav, History Stack)
    │
  M53.4: Spartan Network Client (`spartan://<host>[:port]/<path>`)
    │
  M53.5: Sandboxed Spartan Daemon (`spartand`) with Access Discriminator
    │
  M53.6: Ribbon Reader Window (`RIBBON_WIN_READER`) & Proportional Typography
    │
  M53.7: Hardware Touch Navigation & Full Regression Verification
```

---

### Milestone 53.1: Gemtext Streaming Parser & Library

**Goal**: Build a freestanding, zero-allocation line parser in `fs/gemtext.c` with headers in `include/fs/gemtext.h`.

* **Deliverables**:
  1. `typedef enum { GEM_TEXT, GEM_LINK, GEM_H1, GEM_H2, GEM_H3, GEM_LIST, GEM_QUOTE, GEM_PRE_START, GEM_PRE_TEXT, GEM_PRE_END } gem_line_type_t;`
  2. `typedef struct { gem_line_type_t type; const char *text; const char *url; const char *label; } gem_line_t;`
  3. `bool gemtext_parse_line(const char *raw_line, bool *in_preformatted, gem_line_t *out);`
  4. Unit test suite in `tests/test_gemtext.py` validating all Gemtext line variations and edge cases.
* **Invariants**:
  - Zero dynamic heap allocation (`malloc`/`calloc`).
  - Strict line-length bounds (`GEM_LINE_MAX = 256`).
  - Clean URL and label tokenization for `=>` lines.

---

### Milestone 53.2: Pipeline & Help Migration to `.gmi`

**Goal**: Adapt `tools/gendocs.py` to compile GitHub-Flavored Markdown into `.gmi` files and replace all legacy `.txt` files in `/sd0/help/`.

* **Deliverables**:
  1. Update `tools/gendocs.py` with `--gemtext` generator mode.
  2. Generate `.gmi` files for all manuals in `doc/manual/`:
     - `cas.gmi`, `chess.gmi`, `c_programming.gmi`, `editors.gmi`, `flashing.gmi`, `lisp.gmi`, `networking.gmi`, `presets.gmi`, `ribbon.gmi`, `sensors.gmi`, `shell.gmi`, `updates.gmi`, `zmachine.gmi`.
  3. Deprecate and remove `/sd0/help/*.txt`.
  4. Update `cmd_man()` and `cmd_help()` in `kernel/shell.c` to open `/sd0/help/<topic>.gmi`.
  5. Verify cross-reference resolution: `man editors` -> `c_programming.gmi` cleanly jumps to C programming.

---

### Milestone 53.3: Interactive Terminal Gemtext Viewer (`gem`)

**Goal**: Implement the `gem` shell command for viewing local and remote Gemtext files within standard terminals.

* **Deliverables**:
  1. `cmd_gem(const char *target)` in `kernel/shell.c`:
     - Accepts local paths (`/sd0/help/shell.gmi`, `file:///sd0/...`) and relative links.
     - Parses and renders lines with VT100 ANSI styles (bold headers, bullet points `•`, quotes with vertical bar `│`).
     - Extracts and numbers `GEM_LINK` targets on the visible page: `[1] => editors.gmi Text Editors`.
  2. **Navigation & Paging**:
     - `1`..`9`: Jump to visible link index.
     - `b`: Go back to previous document (maintains a history stack of up to 8 URIs).
     - `Space` / `PageDown`: Scroll down by terminal height.
     - `q`: Exit viewer to shell.
  3. Integrated with `man <topic>`: `man shell` invokes the viewer on `/sd0/help/shell.gmi`.

---

### Milestone 53.4: Spartan Network Client

**Goal**: Extend `gem` to connect to external Spartan servers (`spartan://<host>[:port]/<path>`) over Wi-Fi.

* **Deliverables**:
  1. Spartan client implementation in `net/spartan_client.c` using LugalOS's non-blocking `net/tcp.c` socket API.
  2. **Wire Protocol**:
     - Open TCP connection to host on port 300 (or specified port).
     - Send request: `<host> <path> [content_length]\r\n`.
     - Read status line: `<status> <meta>\r\n`.
       - Status `2` (Success): Stream response body into the Gemtext parser.
       - Status `3` (Redirect): Update URI and follow redirect (up to 3 hops).
       - Status `4` (Client Error) / `5` (Server Error): Display error message to user.
  3. Support browsing public Spartan capsules directly from LugalOS:
     `lsh> gem spartan://spartan.mozz.us/`

---

### Milestone 53.5: Sandboxed Spartan Daemon (`spartand`) & Discriminator

**Goal**: Implement a lightweight Spartan server in LugalOS to serve public documentation and capsules to the network.

* **Deliverables**:
  1. Background daemon `spartand` listening on TCP port 300.
  2. **Access Discriminator & Sandbox**:
     - Document root locked to `/sd0/pub/` (chroot jail).
     - Canonicalization check: rejects any URI containing `..`, null bytes, or backslashes.
     - Serves static `.gmi` and text files.
     - Returns status `40 Not Found` for nonexistent files or directory escape attempts.
  3. Shell management commands:
     - `spartan start [port]` / `spartan stop` / `spartan status`.
  4. Integration with Wi-Fi network nodes (ESP32-P4 and ESP32-C6).

---

### Milestone 53.6: Dedicated Ribbon Reader Window (`RIBBON_WIN_READER`)

**Goal**: Introduce a dedicated long-form reading window in the horizontal window ribbon with proportional serif typography.

* **Deliverables**:
  1. Define `RIBBON_WIN_READER = 3` in `drivers/include/drivers/ribbon.h`.
  2. **Typography Engine**:
     - Add 1-bpp proportional serif font table (e.g. 14 px or 16 px Georgia/Century-style) with per-glyph width table `char_width[128]`.
     - Implement proportional word-wrapping layout in `drivers/reader_view.c`.
     - Monospace fallback (8×16 Spleen) for preformatted blocks (```` ``` ````).
  3. **Ribbon Integration**:
     - Insert/split reader windows with `reader <file.gmi>` or GUI menu.
     - Render into the window's screen geometry with proper margins, clean headers, and styled blockquotes.

---

### Milestone 53.7: Hardware Touch Navigation & Full Regression Verification

**Goal**: Connect GT911 touch gestures to the reader window and verify across all platforms.

* **Deliverables**:
  1. **Touch Navigation**:
     - Touch hit-testing: tapping within the bounding rectangle of a link line activates navigation.
     - Touch swipe: vertical drag pans the reader document smoothly.
  2. **Hardware Verification**:
     - Verify on Waveshare ESP32-P4-WIFI6-Touch-LCD-7B and RP2350-LCD-7.
     - Capture screen dumps (`lcd screen /sd0/reader.pbm`) via `p9sync.py` to verify proportional glyph rendering.
  3. **Automated Test Suite**:
     - Host unit tests (`tests/test_gemtext.py`).
     - QEMU regression tests for Spartan client/server loopback.
     - `tests/hw/test_esp32p4.py` check for reader window creation and touch interaction.

---

## 4. Architectural Rules & Constraints Matrix

| Constraint | Enforcement in Phase 53 |
| :--- | :--- |
| **Freestanding C** | Built strictly with `-nostdlib -ffreestanding`. No `libc` stdio/malloc in parser. |
| **Zero FPU in M-Mode** | All glyph width accumulations and text formatting use pure integer arithmetic. |
| **Zero SRAM Overhead** | Font bitmap tables and glyph width tables reside exclusively in Flash `.rodata`. |
| **Sandbox & Isolation** | Network Spartan daemon strictly chrooted to `/sd0/pub/`; private keys unreachable. |
| **Deterministic Parsing** | Gemtext line types determined strictly by prefix characters; zero inline regex. |

---

## 5. Future Evolution: Gemini & Client mTLS (Phase 52 Dependency)

Gemini (`gemini://`, port 1965) builds on the identical Gemtext document representation and line-oriented semantics established in this phase. However, Gemini mandates TLS 1.2/1.3 and uses Client TLS Certificates (mTLS) for authentication.

Once Phase 52 delivers the LugalOS Certificate Authority and TLS stack:
1. `gemini://` client and server will directly wrap the Gemtext engine built in M53.1 and M53.6.
2. LugalOS client certificates issued by the owner CA will be passed in TLS handshakes, authenticating users against `peers` capability grants without tokens or cookies.
