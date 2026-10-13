# Phase 51 — LugalOS Shell Usability, Composability & Pipeline Architecture

**Status: COMPLETE / VERIFIED.** Follows Phase 50 (Documentation System & Codebase Rationalization).  
**Target Version:** 0.18.0  
**Primary Focus:** Interactive Shell Polish, Persistent History, Clean Diagnostic Routing, Unix Pipeline Composability, Generalized Pager, and Responsive Terminal Layouts.

---

## 1. Executive Summary & Problem Statement

Across phases 1 through 50, LugalOS developed into a full-featured, capability-isolated microkernel operating system. However, the interactive user experience in the `lsh` shell retains several legacy rough edges from early bring-up:

1. **Ephemeral Command History**:
   - `line_editor.c` maintains a memory-only history buffer (`MAX_HIST_ITEMS = 20`) that is cleared on reboot.
   - Although `add_history()` appends commands to `/sd0/system/history.lisp`, the boot script `tools/sd_root/system/etc/init.lisp` executes `(write-file "/sd0/system/history.lisp" "")` at startup, obliterating previous session history.
   - Users cannot recall commands entered in prior sessions after a reset or power cycle.

2. **Uncoordinated Kernel Message Interleaving**:
   - The kernel log console sink (`klog_sink_register("console", ...)`) is attached by default.
   - Background tasks (WLAN autostart, DHCP leases, NTP synchronization, USB device polling, watchdog pings) emit `printk()` messages directly to the active terminal while the user is typing, splicing logs through the shell prompt (`lsh> `).
   - Historical bring-up messages (such as `"printk() no longer blocks"`) remain in the kernel log stream, adding noise.

3. **Lack of Shell Composability (Piping)**:
   - LugalOS commands execute in isolation. There is no mechanism to pass output from one command into another (`cmd1 | cmd2`).
   - Long outputs (`help`, `cat /proc/kmsg`, `ps`, `meminfo`, `top`) rapidly scroll off the screen, requiring users to manually capture screenshots or navigate via serial scrollback.

4. **Siloed Paging Logic**:
   - The interactive pager introduced in Phase 50 (`cmd_more` / `pager_view_file`) is strictly bound to reading files from the VFS (`vfs_open`/`vfs_read`).
   - It cannot page arbitrary text streams, in-memory buffers, or command outputs.

5. **Fixed-Width Layout Artifacts on Constrained Displays**:
   - Help banners and section dividers use hardcoded 60-character strings (`------------------------------------------------------------`).
   - On compact physical screens (e.g. 160×128 ST7735 TFT with 20–26 columns) or narrow serial terminal windows, lines wrap prematurely onto subsequent rows, corrupting visual alignment.

Phase 51 resolves these limitations by introducing **bounded persistent history**, **quiet kernel diagnostic routing (`dmesg`)**, a **generalized streaming pager**, **shell command piping (`|`)**, and **responsive terminal dividers**.

---

## 2. Architecture & Design Decisions

### 2.1 Persistent Bounded Command History

```
  [User Enters Command] ────────► [line_editor.c: add_history()]
                                           │
                        ┌──────────────────┴──────────────────┐
                        ▼                                     ▼
             [In-Memory Ring]                      [Append to Storage]
          history_stack[0..19]                  /sd0/system/history.lis
                                                              │
                                                  (Preserved Across Boots)
                                                              │
  [System Boot / Shell Init] ◄────────────────────────────────┘
    Loads last N entries (default: 10)
    Capped on disk (max 100 entries)
```

- **Bootstrapping**:
  - Replace the destructive `(write-file "/sd0/system/history.lisp" "")` in `init.lisp` with a non-destructive check.
  - Implement `line_editor_load_history(int max_entries)` in `kernel/line_editor.c`.
  - When the shell initializes on a mounted volume (`/sd0` or `/flash0`), the last 10 non-empty lines are loaded into `history_stack`.
  - Pressing `Up-Arrow` at the first shell prompt immediately recalls the final command from the previous session.
- **Disk Boundary & Rotation**:
  - Limit `/sd0/system/history.lis` to a maximum of 100 entries. If the file exceeds this threshold, trim the oldest entries to prevent unbounded storage consumption on MicroSD cards.
- **Builtin Tool**:
  - Add shell command `history`:
    - `history`: Lists numbered entries in current history stack.
    - `history clear`: Clears both memory stack and disk history.
    - `!n`: Re-executes command number $n$ (standard shell convention).

---

### 2.2 Clean Diagnostic Routing & `dmesg` Builtin

```
                          ┌──────────────────────────┐
                          │   printk() Diagnostics   │
                          └─────────────┬────────────┘
                                        │
                                        ▼
                          ┌──────────────────────────┐
                          │   klog Memory Ring       │
                          │   (KLOG_RING_SIZE: 16KB) │
                          └─────────────┬────────────┘
                                        │
                   ┌────────────────────┴────────────────────┐
                   ▼                                         ▼
         [/proc/kmsg Filesystem]                   [Console Sink Filter]
                   │                                         │
                   ▼                                         ▼
         [dmesg Shell Command]                     Only CRITICAL / PANIC
         (dmesg | more)                            (Default: Muted info logs)
```

- **Default Console Silence**:
  - Kernel log messages (`printk()`) continue recording into the 16 KB lockless memory ring buffer.
  - When the interactive shell starts, normal diagnostic sinks (`console`, `lcd`) are detached or set to filter out non-critical informational logs.
  - Critical kernel exceptions (`printk_critical()`, panics, traps, assertion failures) bypass the filter and print to the console immediately.
- **`dmesg` Command**:
  - Implement `cmd_dmesg()` in `kernel/shell.c` as a fast shortcut for `/proc/kmsg`.
  - Flags:
    - `dmesg`: Dumps the kernel ring buffer to user console.
    - `dmesg -c`: Dumps and clears/advances the read pointer.
    - `dmesg -f`: Live follow mode (escapes cleanly on `q` or `Ctrl-C`).
- **Log String Audit**:
  - Audit `kernel/`, `drivers/`, `fs/`, and `net/` to eliminate archaic commentary strings from active log statements (e.g., removing historical debugging relics like `"printk() no longer blocks"`, `"B6 originally masked interrupts"`).

---

### 2.3 Shell Pipeline Architecture (`|`)

In a freestanding, single-address-space microkernel where shell commands are built-in C functions rather than distinct processes, classical Unix `fork()` + `pipe()` is inapplicable. LugalOS adopts a **Buffered Stream Pipeline Architecture**:

```
      Command 1               Captured Buffer               Command 2
 ┌─────────────────┐       ┌──────────────────┐       ┌─────────────────┐
 │ cat /proc/kmsg  │ ────► │  Pipe Buffer     │ ────► │      more       │
 │   (cprintf)     │       │  (4KB - 16KB)    │       │ (pager_engine)  │
 └─────────────────┘       └──────────────────┘       └─────────────────┘
```

1. **Pipeline Syntax Parsing**:
   - `parse_and_eval_cmd()` parses the command line for unquoted pipe operators (`|`).
   - Commands are split into pipeline stages: `stage[0] | stage[1] | ... | stage[N]`.

2. **Sequential Buffered Execution**:
   - Stage 0 runs with `console_capture(pipe_buf, pipe_cap)` active.
   - Any text emitted by `cprintf()` during Stage 0 accumulates in `pipe_buf`.
   - When Stage 0 completes, `console_capture_end()` finalizes the stream.
   - Stage 1 is invoked with `pipe_buf` assigned as its standard input stream (`stdin`).

3. **Memory Management & Safety**:
   - Dedicated pipe buffer allocated from the kernel page zone (NAPOT aligned, 4 KB to 16 KB).
   - If output exceeds buffer capacity, the pipe engine switches to chunked paging or signals overflow gracefully without kernel panic.

---

### 2.4 Generalized Pager Engine (`pager_engine_t`)

Phase 50 introduced file-based paging in `pager_view_file()`. Phase 51 abstracts this into a unified **Pager Engine**:

```c
typedef enum {
    PAGER_SOURCE_FILE,
    PAGER_SOURCE_BUFFER,
    PAGER_SOURCE_STREAM
} pager_source_type_t;

typedef struct {
    pager_source_type_t type;
    union {
        int fd;
        struct {
            const char *buf;
            size_t      len;
            size_t      pos;
        } mem;
        struct {
            void *ctx;
            int (*read)(void *ctx, char *buf, size_t max);
        } stream;
    } src;
    bool is_help_topic;
    char topic_name[32];
} pager_stream_t;
```

- **Supported Modes**:
  1. **File Paging**: `more <file_path>` (reads directly from VFS).
  2. **Pipeline Paging**: `cmd | more` (reads from captured pipe buffer).
  3. **Topic Manual**: `man <topic>` / `help <topic>` (integrates numbered `[1..9]` link jumping and history navigation `b`).
  4. **Direct Shell Help**: `help` automatically pages when output exceeds the detected terminal height.

- **Interactive Keybindings**:
  | Key | Action |
  | :--- | :--- |
  | `Space` / `PageDown` | Advance by one full screen |
  | `Enter` / `Down` / `j` | Advance by one line |
  | `b` / `PageUp` | Back to previous screen (or back to previous help topic) |
  | `q` / `Ctrl-C` / `ESC` | Exit pager and return to shell prompt |
  | `1` .. `9` | Jump to referenced manual topic (in help mode) |
  | `/` | Search forward for pattern |

---

### 2.5 Core Shell Filter Utilities

With pipeline support enabled, LugalOS introduces lightweight, freestanding text filter commands to enable Unix-style composability:

1. **`grep [-i] [-v] <pattern>`**:
   - Filters incoming lines matching `<pattern>`.
   - Supports case-insensitive matching (`-i`) and inversion (`-v`).
   - Example: `ps | grep radio`, `cat /proc/net | grep wlan0`.

2. **`head [-n lines]`**:
   - Emits the first $N$ lines (default: 10).
   - Example: `cat /sd0/log.txt | head -n 5`.

3. **`tail [-n lines]`**:
   - Emits the last $N$ lines (default: 10).
   - Example: `dmesg | tail -n 20`.

4. **`wc [-l|-w|-c]`**:
   - Counts lines (`-l`), words (`-w`), or characters (`-c`).
   - Example: `ls /sd0/games | wc -l`.

---

### 2.6 Responsive Terminal Dividers & Dynamic Width Adaptation

- **Terminal Size Discovery**:
  - The microkernel already exposes `console_size(unsigned *cols, unsigned *rows)`.
  - On LCD terminals (`rp2350-terminal`, `esp32p4-lcd7b`), `cols` and `rows` report exact geometry (e.g. 100×30 or 126×34).
  - On serial lines, if geometry is unnegotiated, defaults to 80×24.
  - On compact displays (Pico 2 ST7735: 20–26 columns), `cols` reflects the narrow window.

- **Dynamic Divider Routine**:
  ```c
  void console_print_divider(char ch) {
      unsigned cols = 80, rows = 24;
      if (!console_size(&cols, &rows) || cols == 0) cols = 80;
      if (cols > 72) cols = 72; /* Clean aesthetic cap for wide workstations */
      if (cols > 2) cols -= 1;  /* Prevent autowrap at right margin */
      for (unsigned i = 0; i < cols; i++) console_putc(ch);
      console_putc('\n');
  }
  ```
- **Banner Rationalization**:
  - Replace all hardcoded 60-character dash strings (`------------------------------------------------------------`) in `cmd_help_summary()`, `cmd_help_test()`, and diagnostics with `console_print_divider('-')`.
  - Formatted tables (`ps`, `ls`, `df`) adjust column padding dynamically based on available width, avoiding line wrap on compact hardware targets.

---

## 3. Milestone Breakdown & Implementation Plan

```
       M51.1                M51.2               M51.3               M51.4               M51.5               M51.6
     Persistent           Diagnostic          Universal          Pipeline &          Filter &           Docs & Full
      History              Routing              Pager           Composability        Utilities          Verification
  ───────●────────────────────●───────────────────●───────────────────●───────────────────●───────────────────●───► main
     (Loaded @ boot)      (dmesg builtin)     (Buffer/File)       (cmd1 | cmd2)       (grep/wc/head)      (490+ tests)
```

### Milestone M51.1: Persistent & Bounded Shell History
- **Tasks**:
  1. Implement `line_editor_load_history(int max_entries)` in `kernel/line_editor.c`.
  2. Implement bounded disk rotation (capping `/sd0/system/history.lis` to 100 entries).
  3. Modify `tools/sd_root/system/etc/init.lisp` to preserve existing history across boots.
  4. Implement `history` shell builtin (`history`, `history clear`).
  5. Add QEMU regression tests verifying history persistence across simulated reboots.
- **Deliverables**: Previous session commands available via `Up-Arrow` on boot; `history` builtin.

### Milestone M51.2: Quiet Diagnostic Routing & `dmesg` Builtin
- **Tasks**:
  1. Detach console sinks (`console`, `lcd`) for routine informational logging once the shell launches, keeping ring buffer recording active.
  2. Ensure `printk_critical()` continues to reach the screen immediately during exceptions.
  3. Implement `dmesg` builtin in `kernel/shell.c` (supports standard dump, `-c` clear, and `-f` follow).
  4. Audit kernel/drivers source code to scrub obsolete bring-up remarks from `printk()` format strings.
  5. Verify that background tasks (WLAN, DHCP, NTP) no longer corrupt the active shell prompt.
- **Deliverables**: Clean interactive prompt with background logs isolated to `/proc/kmsg` and `dmesg`.

### Milestone M51.3: Universal Pager Engine & Responsive Layouts
- **Tasks**:
  1. Refactor `pager_view_file()` into abstract `pager_engine_t` supporting both file paths and memory buffers.
  2. Implement `console_print_divider(char ch)` in `kernel/console.c`.
  3. Replace hardcoded 60-character divider strings across `kernel/shell.c` with responsive dividers.
  4. Add pattern search (`/`) and smooth line scrolling (`j`/`Enter`) to the pager.
  5. Update `cmd_help()` to automatically page when line count exceeds terminal height.
- **Deliverables**: Pager capable of paging any stream; zero visual line-wrap on narrow screens.

### Milestone M51.4: Shell Pipeline Architecture (`|`)
- **Tasks**:
  1. Extend `parse_and_eval_cmd()` in `kernel/shell.c` to parse pipeline operators (`|`).
  2. Allocate and manage page-aligned kernel pipe buffers.
  3. Implement captured execution flow: Stage 0 captures to buffer, Stage 1 consumes from buffer.
  4. Wire `cmd | more` end-to-end (e.g. `help | more`, `dmesg | more`, `cat /proc/kmsg | more`).
  5. Verify pipeline error handling (unknown command in pipeline, buffer overflow).
- **Deliverables**: Functional `|` pipeline operator in `lsh`.

### Milestone M51.5: Core Shell Filter Utilities
- **Tasks**:
  1. Implement freestanding `cmd_grep()` supporting standard pattern matching, `-i`, and `-v`.
  2. Implement `cmd_head()` and `cmd_tail()` supporting line count arguments (`-n N`).
  3. Implement `cmd_wc()` supporting line, word, and byte counting.
  4. Add pipeline integration tests in `tests/runner.py` testing filter chaining.
- **Deliverables**: `grep`, `head`, `tail`, `wc` available as both standalone tools and pipeline stages.

### Milestone M51.6: Documentation, On-Device Help & Final Audit
- **Tasks**:
  1. Update User Manual: [`doc/user/shell.md`](file:///home/dsc/Source/gith/domschl/lugalos/doc/user/shell.md) with pipeline examples, `dmesg`, `history`, and filter syntax.
  2. Run `tools/gendocs.py` to regenerate plain-text help files in `tools/sd_root/help/` (`help/shell.txt`).
  3. Run full automated test suite ([`tests/runner.py`](file:///home/dsc/Source/gith/domschl/lugalos/tests/runner.py)) and verify 100% pass rate.
  4. Verify clean builds across all presets (`rv32`, `rv64`, `rp2350-terminal`, `esp32p4-lcd7b`, `esp32c6`).
- **Deliverables**: Fully verified and documented Phase 51 release.

---

## 4. Definition of Done

Phase 51 is complete when:
1. Shell command history persists across reboots (up to 10 entries immediately recalled via `Up-Arrow`).
2. Kernel informational logs no longer splice across the active shell prompt; `dmesg` cleanly displays the log ring.
3. Obsolete historical commentary is removed from `printk()` statements.
4. Shell supports `cmd1 | cmd2` piping, seamlessly connecting commands to `more`, `grep`, `head`, `tail`, and `wc`.
5. `help` and large file reads page cleanly without runaway terminal scrolling.
6. Dividers and tables render responsively without visual line wrapping on compact displays.
7. All regression tests in `tests/runner.py` pass 100% with zero memory leaks or regressions.

---

## 5. Execution & Verification Summary

All milestones (M51.1 through M51.6) have been implemented and verified:

1. **Persistent Bounded History (M51.1)**:
   - Implemented bounded history loading (`line_editor_load_history(10)`) in `kernel/line_editor.c` and disk rotation capping at 4 KB (~100 entries).
   - Preserved `/sd0/system/history.lisp` on boot in `tools/sd_root/system/etc/init.lisp`.
   - Added `history`, `history clear`, and `!n` / `!!` history recall without recursive loops in `kernel/shell.c`.

2. **Clean Diagnostic Routing & `dmesg` (M51.2)**:
   - Spliced console sinks during interactive `readline_interactive` to prevent background logs (WLAN, DHCP, NTP, USB, WDT) from interrupting the user prompt. Sinks temporarily re-attach during foreground command execution for selftest compatibility.
   - Added `cmd_dmesg()` supporting standard dump, `-c` (clear buffer via `klog_clear()`), and `-f` (live follow).
   - Scrubbed obsolete remarks (`"printk() no longer blocks"`) from `kernel/klogd.c`.

3. **Universal Pager & Responsive Dividers (M51.3)**:
   - Added `console_print_divider(char ch)` in `kernel/console.c` using dynamic terminal width (`cols - 1`, capped at 72).
   - Generalized pager in `kernel/shell.c` with `pager_view_buf()` and `pager_view_file()`, featuring smooth line scroll (`j`/`J`/`Enter`), half-page scroll (`d`/`D`), and quit (`q`/`ESC`/`Ctrl-C`).
   - Integrated dynamic divider and terminal-height-aware paging in `cmd_help()`.

4. **Piping & Core Filters (M51.4 & M51.5)**:
   - Extended `kernel/console.c` capture engine (`console_capture`) to guarantee head retention and null-termination on buffer saturation.
   - Implemented pipeline execution engine in `kernel/shell.c` ping-ponging across 16 KB (or fallback 4 KB) scratch buffers.
   - Implemented standalone and filter utilities: `grep` (`-i`, `-v`), `head` (`-n N`), `tail` (`-n N`), `wc` (`-l`, `-w`, `-c`), and `echo`.
   - Connected `cmd | more` end-to-end.

5. **Documentation, Tooling & Silicon Verification (M51.6)**:
   - Updated User Manual in `doc/user/shell.md`.
   - Regenerated on-device help in `tools/sd_root/help/shell.txt` via `tools/gendocs.py`.
   - Added automated regression tests to `tests/runner.py`.
   - Silicon verified on RP2350 hardware (`rp2350-terminal`): 32/32 tests passed (100%).

