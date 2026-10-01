#include "kernel/line_editor.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/scratch.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include <string.h>

/* Eight, not thirty-two. At MAX_LINE_LEN each, the ring was 16 KB of a
 * 512 KB machine spent on scrollback (C5) -- and it is only the *navigable*
 * history: add_history() also appends every command to
 * <vol>/system/history.lisp, which keeps everything and is where a long
 * record belongs. Up-arrow through the last eight is what an interactive
 * session actually uses. */
#define MAX_HIST_ITEMS 8
#define MAX_LINE_LEN 512

/* strncpy() doesn't null-terminate when src is exactly dst_size-1 or more
 * characters long, and several call sites in this file relied on that
 * happening anyway -- a stack-local destination buffer with no guaranteed
 * terminator is an out-of-bounds read waiting to happen the next time it's
 * treated as a C string (see B13 in
 * plan/completed/2026-08-07_review_and_remediation.md). Mirrors strncpy_local() in
 * user/lisp/lisp.c: dst_size is the *full* destination buffer size, and the
 * result is always terminated within it. */
static void safe_strncpy(char *dst, const char *src, int dst_size) {
    int i = 0;
    while (i < dst_size - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static char history_stack[MAX_HIST_ITEMS][MAX_LINE_LEN];
static int history_count = 0;

/* One redraw is one whole write, and is held as one (2026-09-14).
 *
 * This file reaches the console through thirty console_puts() calls and never
 * took console_lock() for any of them, while klog_drain() -- klogd's, every
 * 50 ms -- does take it. Two writers, one of them unlocked, so they were
 * never serialised against each other: a log record could be emitted between
 * two escape sequences of a single redraw. That is a spliced echo, and it is
 * the path a typed command is echoed on, which is why the suite saw it as
 * `identity provision` echoing wrong or not at all rather than as a console
 * bug. See plan/open_issues.md.
 *
 * Held across the writes and nothing else -- never across console_getc(),
 * which waits for a human. A ylock is the right primitive precisely because
 * a console write ends in a block (kernel/console.c). */
/* 37.2, plan/phase37_screen_layouts_and_apps.md §3.3: the line is UTF-8.
 * It stays a byte buffer, but the cursor only ever stands on a character
 * boundary: every movement and deletion steps over a whole character, and a
 * multi-byte character typed in is collected and inserted whole (line_feed).
 * Every character is one cell on the screen (no combining or double-width
 * characters, §3.1), so cursor columns are character counts. */
static bool u8_cont(char c) {
    return ((unsigned char)c & 0xc0u) == 0x80u;
}

static int u8_prev(const char *buf, int pos) {
    if (pos > 0) pos--;
    while (pos > 0 && u8_cont(buf[pos])) pos--;
    return pos;
}

static int u8_next(const char *buf, int len, int pos) {
    if (pos < len) pos++;
    while (pos < len && u8_cont(buf[pos])) pos++;
    return pos;
}

static int u8_chars(const char *buf, int from, int to) {
    int n = 0;
    for (int i = from; i < to; i++) if (!u8_cont(buf[i])) n++;
    return n;
}

/* Removes buf[from, to) from a line of *len bytes. */
static void line_cut(char *buf, int *len, int from, int to) {
    for (int i = to; i < *len; i++) buf[i - (to - from)] = buf[i];
    *len -= to - from;
    buf[*len] = '\0';
}

static void redraw_line(const char *prompt, const char *buf, int len, int pos) {
    console_lock();
    console_sync();   /* backlog before the redraw, never inside it */
    console_puts("\033[?25l"); // Hide cursor during display redraw
    console_puts("\r");
    console_puts(prompt);
    for (int i = 0; i < len; i++) {
        console_putc(buf[i]);
    }
    console_puts("\033[K"); // Clear remaining line to the right

    // Move cursor back to pos using ANSI Cursor Left (\033[D), one per character
    for (int i = 0; i < u8_chars(buf, pos, len); i++) {
        console_puts("\033[D");
    }
    console_puts("\033[?25h"); // Show cursor at final target position
    console_unlock();
}


void line_editor_init(void) {
    history_count = 0;
    printk("[LineEditor] Online.\n");
}


static void add_history(const char *line) {
    if (!line || strlen(line) == 0) return;

    // Avoid duplicate contiguous entries
    if (history_count > 0 && strcmp(history_stack[history_count - 1], line) == 0) {
        return;
    }

    if (history_count < MAX_HIST_ITEMS) {
        safe_strncpy(history_stack[history_count], line, MAX_LINE_LEN);
        history_count++;
    } else {
        // Shift left
        for (int i = 0; i < MAX_HIST_ITEMS - 1; i++) {
            strcpy(history_stack[i], history_stack[i + 1]);
        }
        safe_strncpy(history_stack[MAX_HIST_ITEMS - 1], line, MAX_LINE_LEN);
    }

    /* Append just the new line to the persistent log instead of
     * reconstructing and rewriting the whole accumulated history buffer on
     * every command (see B8 in plan/completed/2026-08-07_review_and_remediation.md).
     * history_stack[] above still independently tracks only the last
     * MAX_HIST_ITEMS entries for Up/Down navigation; the on-disk log is a
     * simple ever-growing record of everything typed since boot (or since
     * the last explicit clear), not a mirror of that ring buffer --
     * reconciling the two would require the same full-rewrite-per-command
     * this change exists to eliminate. */
    char entry_buf[MAX_LINE_LEN + 1];
    int elen = 0;
    const char *p = line;
    while (*p && elen < MAX_LINE_LEN - 1) {
        entry_buf[elen++] = *p++;
    }
    entry_buf[elen++] = '\n';

    /* A card fresh out of the packet has no /system (36.0a,
     * plan/phase36_rp2350_lcd7_terminal.md). The append used to fail on every
     * single command, and each failure logged fat32.c's "no directory to
     * create 'system/history.lisp' in" -- a board with a new card printed that
     * line after everything anyone typed, forever. The directory belongs to
     * this file's own record, so this file creates it.
     *
     * Checked once, not per command: a stat on the card for every line typed
     * would be a cost paid only to rediscover a fact. The latch clears when an
     * append fails, which is what a swapped card looks like from here. No card
     * at all: vfs_mkdir() on an inactive mount fails silently, and so does
     * this -- history is a convenience, not something to warn about. */
    static bool history_dir_ready = false;
    if (!history_dir_ready) {
        vfs_stat_t st;
        if (vfs_stat("/sd0/system", &st) != 0 || !st.is_dir) {
            if (vfs_mkdir("/sd0/system") != 0) return;
            printk("[LineEditor] created /sd0/system for the command history\n");
        }
        history_dir_ready = true;
    }
    if (vfs_append("/sd0/system/history.lisp", entry_buf, elen) < 0) {
        history_dir_ready = false;
    }
}



static int g_prev_target_line = 1;

/* Same whole-write rule as redraw_line() above, and for the same reason. */
static void redraw_box(const char *filename, const char *buf, int len, int pos, const char *status_msg) {
    /* 37.1: the status bar names the file. Every redraw, because Ctrl-X
     * Ctrl-F and Ctrl-W change it; an unchanged title costs a strcmp. */
    char title[80];
    ksnprintf(title, sizeof(title), "e: %s", filename);
    console_set_title(title);
    console_lock();
    console_sync();
    console_puts("\033[?25l"); // Hide cursor during box redraw
    if (g_prev_target_line > 0) {
        for (int m = 0; m < g_prev_target_line; m++) {
            console_puts("\033[A");
        }
        console_puts("\n");
    }

    int line_num = 1;
    int line_start = 0;
    int target_line = 1;
    int target_col = 0;

    for (int i = 0; i <= len; i++) {
        if (i == pos) {
            target_line = line_num;
            target_col = i - line_start;
        }
        if (i < len && buf[i] == '\n') {
            line_num++;
            line_start = i + 1;
        }
    }

    int i = 0;
    int current_line = 1;
    while (1) {
        cprintf("\033[1;36m%3d │ \033[0m", current_line);
        while (i < len && buf[i] != '\n') {
            console_putc(buf[i]);
            i++;
        }
        console_puts("\033[K\n");
        if (i < len && buf[i] == '\n') {
            i++;
            current_line++;
        } else {
            break;
        }
    }

    // Status Line Format: ─── <filename> ─── C-X C-E: eval | C-X C-S: save | C-X C-C: exit ───
    console_puts("\033[1;36m─── \033[1;33m");
    const char *fn = (filename && strlen(filename) > 0) ? filename : "/ram0/system/scratch.lisp";
    console_puts(fn);
    console_puts("\033[1;36m ");

    int fn_len = strlen(fn);
    int mid_fill = 79 - 50 - fn_len;
    if (mid_fill < 1) mid_fill = 1;
    for (int m = 0; m < mid_fill; m++) console_puts("─");

    if (status_msg && strlen(status_msg) > 0) {
        cprintf(" \033[1;32m%s\033[1;36m ───\033[0m", status_msg);
    } else {
        console_puts(" C-X C-E: eval | C-X C-S: save | C-X C-C: exit ───\033[0m");
    }

    /* Erase everything below the status line.
     *
     * Each content line above is cleared with \033[K, which only clears the
     * line the cursor is on -- so a redraw painted exactly as many lines as
     * the buffer currently has and left anything beyond them untouched.
     * Loading a 5-line file after a 10-line one therefore drew the new
     * content and status line over the first six rows and left lines 7-10 of
     * the previous file sitting below, looking like part of the document.
     *
     * \033[J clears from the cursor (end of the status line, the last thing
     * this function draws) to the end of the screen, so the box always ends
     * where it says it ends. Must come before the cursor is walked back up to
     * the edit position below, or it would erase the box itself. */
    console_puts("\033[J");

    int total_content_lines = current_line;
    int move_up_lines = (total_content_lines - target_line) + 1;
    for (int m = 0; m < move_up_lines; m++) {
        console_puts("\033[A");
    }
    console_puts("\r");
    cprintf("\033[1;36m%3d │ \033[0m", target_line);
    for (int c = 0; c < target_col; c++) {
        console_puts("\033[C");
    }

    g_prev_target_line = target_line;
    console_puts("\033[?25h"); // Show cursor at final position
    console_unlock();
}

static bool read_status_prompt(int total_lines, int target_line, const char *prompt, char *out_buf, int max_len) {
    int move_down = (total_lines - target_line) + 1;
    for (int m = 0; m < move_down; m++) {
        console_puts("\033[B");
    }
    console_puts("\r\033[K\033[1;33m");
    console_puts(prompt);
    console_puts("\033[0m");

    int len = 0;
    out_buf[0] = '\0';
    while (1) {
        char c = console_getc();
        if (c == '\r' || c == '\n') {
            out_buf[len] = '\0';
            return (len > 0);
        } else if (c == 0x07 || c == 0x1B) { // Ctrl-G or Esc to cancel
            out_buf[0] = '\0';
            return false;
        } else if (c == 0x08 || c == 0x7F) { // Backspace
            if (len > 0) {
                len--;
                out_buf[len] = '\0';
                console_puts("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (len < max_len - 1) {
                out_buf[len++] = c;
                out_buf[len] = '\0';
                console_putc(c);
            }
        }
    }
}

static void exit_editor_cleanup(int len, const char *buf) {
    int total_lines = 1;
    for (int i = 0; i < len; i++) if (buf[i] == '\n') total_lines++;
    int move_down = (total_lines - g_prev_target_line) + 1;
    for (int m = 0; m < move_down; m++) {
        console_puts("\033[B");
    }
    console_puts("\n");
}

int edit_multiline_box(const char *initial_filename, char *out_buf, int max_len) {
    char active_filename[128];
    if (initial_filename && strlen(initial_filename) > 0) {
        safe_strncpy(active_filename, initial_filename, sizeof(active_filename));
    } else {
        strcpy(active_filename, "/ram0/system/scratch.lisp");
    }

    int len = 0;
    int pos = 0;
    bool modified = false;

    // Pre-fill buffer if file exists
    int rbytes = vfs_read(active_filename, out_buf, max_len - 1);
    if (rbytes > 0) {
        out_buf[rbytes] = '\0';
        len = rbytes;
        pos = len;
    } else {
        out_buf[0] = '\0';
    }

    char status_msg[64];
    status_msg[0] = '\0';

    // Print top optical separator line ONCE at editor start
    console_puts("\r\033[1;36m─────────────────────────────────────────────────────────────────────────────\033[0m\n");
    g_prev_target_line = 1;

    redraw_box(active_filename, out_buf, len, pos, status_msg);

    while (1) {
        char c = console_getc();

        int num_lines = 1;
        for (int i = 0; i < len; i++) if (out_buf[i] == '\n') num_lines++;

        if (c == 0x18) { // Ctrl-X
            char c2 = console_getc();
            if (c2 == 0x05 || c2 == 'e' || c2 == 'E') { // Ctrl-E: Eval
                out_buf[len] = '\0';
                exit_editor_cleanup(len, out_buf);
                return len;
            } else if (c2 == 0x03 || c2 == 0x11 || c2 == 'c' || c2 == 'C' || c2 == 'q' || c2 == 'Q') { // Ctrl-C or Ctrl-Q: Exit
                if (modified) {
                    char choice[16];
                    if (read_status_prompt(num_lines, g_prev_target_line, "Buffer modified. Discard changes? (y/n): ", choice, sizeof(choice))) {
                        if (choice[0] != 'y' && choice[0] != 'Y') {
                            redraw_box(active_filename, out_buf, len, pos, status_msg);
                            continue;
                        }
                    } else {
                        redraw_box(active_filename, out_buf, len, pos, status_msg);
                        continue;
                    }
                }
                out_buf[0] = '\0';
                exit_editor_cleanup(len, out_buf);
                return 0;
            } else if (c2 == 0x13 || c2 == 's' || c2 == 'S') { // Ctrl-S: Save
                /* The return value is the point. This used to be discarded,
                 * and the two lines under it ran unconditionally -- so a
                 * write to an unmounted /sd0, a read-only /flash0 or a full
                 * volume reported "Wrote file", cleared `modified`, and thus
                 * disarmed the "Buffer modified. Discard changes?" guard on
                 * the way out. The editor told you it had saved and then let
                 * you leave without another word. Losing the buffer was the
                 * *quiet* outcome; vfs_write() had been returning -1 the
                 * whole time and nothing looked. */
                if (vfs_write(active_filename, out_buf, len) == 0) {
                    modified = false;
                    strcpy(status_msg, "Wrote file");
                } else {
                    strcpy(status_msg, "WRITE FAILED -- not saved (unmounted? read-only?)");
                }
                redraw_box(active_filename, out_buf, len, pos, status_msg);
                status_msg[0] = '\0';
                continue;
            } else if (c2 == 0x06 || c2 == 'f' || c2 == 'F') { // Ctrl-F: Find / Load File
                char fn_in[128];
                if (read_status_prompt(num_lines, g_prev_target_line, "Find file: ", fn_in, sizeof(fn_in))) {
                    safe_strncpy(active_filename, fn_in, sizeof(active_filename));
                    int r = vfs_read(active_filename, out_buf, max_len - 1);
                    if (r >= 0) {
                        out_buf[r] = '\0';
                        len = r;
                        pos = 0;
                        modified = false;
                        strcpy(status_msg, "Loaded file");
                    } else {
                        len = 0;
                        pos = 0;
                        out_buf[0] = '\0';
                        modified = false;
                        strcpy(status_msg, "New file");
                    }
                }
                redraw_box(active_filename, out_buf, len, pos, status_msg);
                status_msg[0] = '\0';
                continue;
            } else if (c2 == 0x12 || c2 == 'r' || c2 == 'R') { // Ctrl-R: Insert File at cursor
                char fn_in[128];
                if (read_status_prompt(num_lines, g_prev_target_line, "Insert file: ", fn_in, sizeof(fn_in))) {
                    /* §3.1: on-demand rather than 2 KB of .bss for a
                     * keystroke almost nobody presses. */
                    scratch_t ins_sc;
                    if (!scratch_acquire(&ins_sc, 2048)) {
                        strcpy(status_msg, "Insert failed: out of memory");
                    } else {
                        char *ins_buf = (char *)ins_sc.base;
                        int r = vfs_read(fn_in, ins_buf, 2048 - 1);
                        if (r > 0) {
                            ins_buf[r] = '\0';
                            if (len + r < max_len - 1) {
                                for (int i = len - 1; i >= pos; i--) out_buf[i + r] = out_buf[i];
                                memcpy(out_buf + pos, ins_buf, r);
                                pos += r;
                                len += r;
                                out_buf[len] = '\0';
                                modified = true;
                                strcpy(status_msg, "Inserted file");
                            }
                        } else {
                            strcpy(status_msg, "File not found");
                        }
                        scratch_release(&ins_sc);
                    }
                }
                redraw_box(active_filename, out_buf, len, pos, status_msg);
                status_msg[0] = '\0';
                continue;
            } else if (c2 == 0x17 || c2 == 'w' || c2 == 'W') { // Ctrl-W: Write File As
                char fn_in[128];
                if (read_status_prompt(num_lines, g_prev_target_line, "Write file: ", fn_in, sizeof(fn_in))) {
                    safe_strncpy(active_filename, fn_in, sizeof(active_filename));
                    /* Same as Ctrl-S above: report what actually happened,
                     * and keep `modified` set when it didn't. */
                    if (vfs_write(active_filename, out_buf, len) == 0) {
                        modified = false;
                        strcpy(status_msg, "Wrote file as");
                    } else {
                        strcpy(status_msg, "WRITE FAILED -- not saved (unmounted? read-only?)");
                    }
                }
                redraw_box(active_filename, out_buf, len, pos, status_msg);
                status_msg[0] = '\0';
                continue;
            }
        }

        // Control Keys

        if (c == 0x01) { // Ctrl-A: Start of line
            while (pos > 0 && out_buf[pos - 1] != '\n') pos--;
            redraw_box(active_filename, out_buf, len, pos, status_msg);
            continue;
        } else if (c == 0x05) { // Ctrl-E: End of line
            while (pos < len && out_buf[pos] != '\n') pos++;
            redraw_box(active_filename, out_buf, len, pos, status_msg);
            continue;
        } else if (c == 0x02) { // Ctrl-B: Left
            pos = u8_prev(out_buf, pos);
            redraw_box(active_filename, out_buf, len, pos, status_msg);
            continue;
        } else if (c == 0x06) { // Ctrl-F: Right
            pos = u8_next(out_buf, len, pos);
            redraw_box(active_filename, out_buf, len, pos, status_msg);
            continue;
        } else if (c == 0x10) { // Ctrl-P: Up line
            int line_start = pos;
            while (line_start > 0 && out_buf[line_start - 1] != '\n') line_start--;
            if (line_start > 0) {
                int col = pos - line_start;
                int prev_line_end = line_start - 1;
                int prev_line_start = prev_line_end;
                while (prev_line_start > 0 && out_buf[prev_line_start - 1] != '\n') prev_line_start--;
                int prev_line_len = prev_line_end - prev_line_start;
                if (col > prev_line_len) col = prev_line_len;
                pos = prev_line_start + col;
            }
            redraw_box(active_filename, out_buf, len, pos, status_msg);
            continue;
        } else if (c == 0x0E) { // Ctrl-N: Down line
            int line_end = pos;
            while (line_end < len && out_buf[line_end] != '\n') line_end++;
            if (line_end < len) {
                int line_start = pos;
                while (line_start > 0 && out_buf[line_start - 1] != '\n') line_start--;
                int col = pos - line_start;
                int next_line_start = line_end + 1;
                int next_line_end = next_line_start;
                while (next_line_end < len && out_buf[next_line_end] != '\n') next_line_end++;
                int next_line_len = next_line_end - next_line_start;
                if (col > next_line_len) col = next_line_len;
                pos = next_line_start + col;
            }
            redraw_box(active_filename, out_buf, len, pos, status_msg);
            continue;
        }

        // Escape Sequences (Arrow keys, Home, End, Delete)
        if (c == 0x1B) {
            char seq1 = console_getc();
            if (seq1 == '[' || seq1 == 'O') {
                char seq2 = console_getc();
                if (seq2 == 'A') { // Up Arrow
                    int line_start = pos;
                    while (line_start > 0 && out_buf[line_start - 1] != '\n') line_start--;
                    if (line_start > 0) {
                        int col = pos - line_start;
                        int prev_line_end = line_start - 1;
                        int prev_line_start = prev_line_end;
                        while (prev_line_start > 0 && out_buf[prev_line_start - 1] != '\n') prev_line_start--;
                        int prev_line_len = prev_line_end - prev_line_start;
                        if (col > prev_line_len) col = prev_line_len;
                        pos = prev_line_start + col;
                    }
                    redraw_box(active_filename, out_buf, len, pos, status_msg);
                } else if (seq2 == 'B') { // Down Arrow
                    int line_end = pos;
                    while (line_end < len && out_buf[line_end] != '\n') line_end++;
                    if (line_end < len) {
                        int line_start = pos;
                        while (line_start > 0 && out_buf[line_start - 1] != '\n') line_start--;
                        int col = pos - line_start;
                        int next_line_start = line_end + 1;
                        int next_line_end = next_line_start;
                        while (next_line_end < len && out_buf[next_line_end] != '\n') next_line_end++;
                        int next_line_len = next_line_end - next_line_start;
                        if (col > next_line_len) col = next_line_len;
                        pos = next_line_start + col;
                    }
                    redraw_box(active_filename, out_buf, len, pos, status_msg);
                } else if (seq2 == 'C') { // Right Arrow
                    if (pos < len) pos++;
                    redraw_box(active_filename, out_buf, len, pos, status_msg);
                } else if (seq2 == 'D') { // Left Arrow
                    if (pos > 0) pos--;
                    redraw_box(active_filename, out_buf, len, pos, status_msg);
                } else if (seq2 == 'H' || seq2 == '1') { // Home key
                    if (seq2 == '1') console_getc();
                    while (pos > 0 && out_buf[pos - 1] != '\n') pos--;
                    redraw_box(active_filename, out_buf, len, pos, status_msg);
                } else if (seq2 == 'F' || seq2 == '4') { // End key
                    if (seq2 == '4') console_getc();
                    while (pos < len && out_buf[pos] != '\n') pos++;
                    redraw_box(active_filename, out_buf, len, pos, status_msg);
                } else if (seq2 == '3') { // Delete key
                    console_getc();
                    if (pos < len) {
                        for (int i = pos; i < len - 1; i++) out_buf[i] = out_buf[i + 1];
                        len--;
                        out_buf[len] = '\0';
                        redraw_box(active_filename, out_buf, len, pos, status_msg);
                    }
                } else if (seq2 >= '0' && seq2 <= '9') {
                    /* 36.9: Insert, PgUp, PgDn (ESC [ 2/5/6 ~) from the USB
                     * keyboard: not bound yet, but the '~' must not be
                     * typed into the text. */
                    console_getc();
                }
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (len < max_len - 1) {
                for (int i = len; i > pos; i--) out_buf[i] = out_buf[i - 1];
                out_buf[pos] = '\n';
                pos++;
                len++;
                out_buf[len] = '\0';
                modified = true;
                redraw_box(active_filename, out_buf, len, pos, status_msg);
            }
        } else if (c == 0x08 || c == 0x7F) { // Backspace
            if (pos > 0) {
                for (int i = pos - 1; i < len - 1; i++) out_buf[i] = out_buf[i + 1];
                pos--;
                len--;
                out_buf[len] = '\0';
                modified = true;
                redraw_box(active_filename, out_buf, len, pos, status_msg);
            }
        } else if (c >= 32 && c <= 126) {
            if (len < max_len - 1) {
                for (int i = len; i > pos; i--) out_buf[i] = out_buf[i - 1];
                out_buf[pos] = c;
                pos++;
                len++;
                out_buf[len] = '\0';
                modified = true;
                redraw_box(active_filename, out_buf, len, pos, status_msg);
            }
        }

    }
}



/* --- The editing state machine, and its two drivers ---
 *
 * Everything below the state struct used to be the body of
 * readline_interactive()'s own `while (1)`. It was lifted out unchanged so a
 * second driver could exist: readline_poll(), which consumes only the
 * characters already available and returns instead of waiting for the rest of
 * the line.
 *
 * Extracted rather than reimplemented, deliberately. Writing a separate,
 * simpler non-blocking reader was the obvious alternative and would have left
 * two divergent notions of what a line is -- the polled caller quietly losing
 * history, cursor movement, Home/End/Delete and the Ctrl-X multiline escape
 * that the blocking one has. One body, two drivers, so both callers get the
 * same editor.
 *
 * The nested console_getc() calls inside the escape-sequence branches stay
 * blocking even under readline_poll(), which is worth being explicit about
 * rather than leaving as a lurking surprise: they are reached only *after* the
 * ESC that begins a sequence has arrived, and a terminal emits the remainder
 * of an escape sequence in the same burst. The wait is for bytes already in
 * flight -- microseconds -- never for the user to press something.
 *
 * (§"chess_next_event", plan/phase15_memory_reclamation.md.) */

#define LINE_INCOMPLETE (-1)

typedef struct {
    int  len;
    int  pos;
    int  hist_nav_idx;
    char temp_saved_line[MAX_LINE_LEN];
    char pend[4];           /* 37.2: a UTF-8 character still arriving */
    int  pend_len, pend_need;
} line_state_t;

static void line_begin(line_state_t *st, const char *prompt, char *out_buf) {
    st->len = 0;
    st->pos = 0;
    st->hist_nav_idx = history_count;
    st->temp_saved_line[0] = '\0';
    st->pend_len = st->pend_need = 0;
    out_buf[0] = '\0';
    redraw_line(prompt, out_buf, st->len, st->pos);
}

/* Consumes one character. Returns the completed line's length, or
 * LINE_INCOMPLETE while the line is still being edited. */
static int line_feed(line_state_t *st, char c, const char *prompt,
                     char *out_buf, int max_len) {
    /* Aliased so the body below is the original, unedited code. Written back
     * in one place at `done:`, which is also where every former `continue`
     * now lands. */
    int len = st->len;
    int pos = st->pos;
    int hist_nav_idx = st->hist_nav_idx;
    char *temp_saved_line = st->temp_saved_line;
    int ret = LINE_INCOMPLETE;
    if (!u8_cont(c)) st->pend_need = 0;     /* 37.2: anything else abandons a half-arrived character */


        // Control character handling
        if (c == 0x01) { // Ctrl-A: Move to beginning of line
            pos = 0;
            redraw_line(prompt, out_buf, len, pos);
            goto done;
        } else if (c == 0x05) { // Ctrl-E: Move to end of line
            pos = len;
            redraw_line(prompt, out_buf, len, pos);
            goto done;
        } else if (c == 0x02) { // Ctrl-B: Left
            if (pos > 0) pos--;
            redraw_line(prompt, out_buf, len, pos);
            goto done;
        } else if (c == 0x06) { // Ctrl-F: Right
            if (pos < len) pos++;
            redraw_line(prompt, out_buf, len, pos);
            goto done;
        } else if (c == 0x04) { // Ctrl-D: Delete char under cursor
            if (pos < len) {
                line_cut(out_buf, &len, pos, u8_next(out_buf, len, pos));
                redraw_line(prompt, out_buf, len, pos);
            }
            goto done;
        } else if (c == 0x0B) { // Ctrl-K: Kill to end of line
            len = pos;
            out_buf[len] = '\0';
            redraw_line(prompt, out_buf, len, pos);
            goto done;
        } else if (c == 0x0C) { // Ctrl-L: Clear screen
            console_puts("\033[2J\033[H");
            redraw_line(prompt, out_buf, len, pos);
            goto done;
        } else if (c == 0x10) { // Ctrl-P: History Previous
            if (hist_nav_idx > 0) {
                if (hist_nav_idx == history_count) {
                    safe_strncpy(temp_saved_line, out_buf, MAX_LINE_LEN);
                }
                hist_nav_idx--;
                safe_strncpy(out_buf, history_stack[hist_nav_idx], max_len);
                len = strlen(out_buf);
                pos = len;
                redraw_line(prompt, out_buf, len, pos);
            }
            goto done;
        } else if (c == 0x0E) { // Ctrl-N: History Next
            if (hist_nav_idx < history_count) {
                hist_nav_idx++;
                if (hist_nav_idx == history_count) {
                    safe_strncpy(out_buf, temp_saved_line, max_len);
                } else {
                    safe_strncpy(out_buf, history_stack[hist_nav_idx], max_len);
                }
                len = strlen(out_buf);
                pos = len;
                redraw_line(prompt, out_buf, len, pos);
            }
            goto done;
        } else if (c == 0x18) { // Ctrl-X
            char c2 = console_getc();
            if (c2 == 0x0D || c2 == 0x0A || c2 == 0x05 || c2 == 'm' || c2 == 'M' || c2 == 'e' || c2 == 'E') { // Ctrl-M or Ctrl-E
                int mlen = edit_multiline_box("/ram0/system/scratch.lisp", out_buf, max_len);
                add_history(out_buf);
                ret = mlen;
                goto done;
            }
            goto done;
        }


        // Escape Sequences (Arrow keys, Home, End, Delete)
        if (c == 0x1B) {
            char seq1 = console_getc();
            if (seq1 == '[' || seq1 == 'O') {
                char seq2 = console_getc();

                if (seq2 == 'A') { // Up Arrow (History Prev)
                    if (hist_nav_idx > 0) {
                        if (hist_nav_idx == history_count) {
                            safe_strncpy(temp_saved_line, out_buf, MAX_LINE_LEN);
                        }
                        hist_nav_idx--;
                        safe_strncpy(out_buf, history_stack[hist_nav_idx], max_len);
                        len = strlen(out_buf);
                        pos = len;
                        redraw_line(prompt, out_buf, len, pos);
                    }
                } else if (seq2 == 'B') { // Down Arrow (History Next)
                    if (hist_nav_idx < history_count) {
                        hist_nav_idx++;
                        if (hist_nav_idx == history_count) {
                            safe_strncpy(out_buf, temp_saved_line, max_len);
                        } else {
                            safe_strncpy(out_buf, history_stack[hist_nav_idx], max_len);
                        }
                        len = strlen(out_buf);
                        pos = len;
                        redraw_line(prompt, out_buf, len, pos);
                    }
                } else if (seq2 == 'C') { // Right Arrow
                    if (pos < len) {
                        pos = u8_next(out_buf, len, pos);
                        redraw_line(prompt, out_buf, len, pos);
                    }
                } else if (seq2 == 'D') { // Left Arrow
                    if (pos > 0) {
                        pos = u8_prev(out_buf, pos);
                        redraw_line(prompt, out_buf, len, pos);
                    }
                } else if (seq2 == 'H' || seq2 == '1') { // Home key
                    pos = 0;
                    if (seq2 == '1') console_getc(); // consume '~'
                    redraw_line(prompt, out_buf, len, pos);
                } else if (seq2 == 'F' || seq2 == '4') { // End key
                    pos = len;
                    if (seq2 == '4') console_getc(); // consume '~'
                    redraw_line(prompt, out_buf, len, pos);
                } else if (seq2 == '3') { // Delete key
                    console_getc(); // consume '~'
                    if (pos < len) {
                        line_cut(out_buf, &len, pos, u8_next(out_buf, len, pos));
                        redraw_line(prompt, out_buf, len, pos);
                    }
                } else if (seq2 >= '0' && seq2 <= '9') {
                    /* 36.9: Insert, PgUp, PgDn (ESC [ 2/5/6 ~) from the USB
                     * keyboard: not bound here, but the '~' must not become
                     * part of the line. */
                    console_getc();
                }
            }
            goto done;
        }

        // Backspace handling
        if (c == 0x08 || c == 0x7F) {
            if (pos > 0) {
                int from = u8_prev(out_buf, pos);
                line_cut(out_buf, &len, from, pos);
                pos = from;
                redraw_line(prompt, out_buf, len, pos);
            }
            goto done;
        }

        // Enter key
        if (c == '\r' || c == '\n') {
            console_puts("\n");
            out_buf[len] = '\0';
            add_history(out_buf);
            /* A Ctrl-C typed while composing this line cancelled nothing --
             * there was no command running to cancel. Clear it here, as the
             * line is handed over, so it cannot be mistaken for an interrupt
             * aimed at the command about to run. Ctrl-C pressed *during* that
             * command still latches normally, which is the case that matters.
             * (Ctrl-C never arrives here as a character: kernel/console.c
             * latches it at the pump and never delivers it as data.) */
            console_interrupt_clear();
            ret = len;
            goto done;
        }

        // Standard printable character, or 37.2's UTF-8: a lead byte starts
        // a character, continuation bytes complete it, and only a whole one
        // is inserted -- the cursor never stands inside a character. A
        // continuation byte with no lead before it, or a byte that cannot
        // start a character, is dropped.
        {
            unsigned char uc = (unsigned char)c;
            char seq[4];
            int n = 0;
            if (uc >= 32 && uc <= 126) {
                st->pend_need = 0;
                seq[0] = c;
                n = 1;
            } else if (u8_cont(c)) {
                if (st->pend_need > 0) {
                    st->pend[st->pend_len++] = c;
                    if (--st->pend_need == 0) {
                        for (int i = 0; i < st->pend_len; i++) seq[i] = st->pend[i];
                        n = st->pend_len;
                    }
                }
            } else if (uc >= 0xc2 && uc <= 0xf4) {
                st->pend[0] = c;
                st->pend_len = 1;
                st->pend_need = uc >= 0xf0 ? 3 : uc >= 0xe0 ? 2 : 1;
            }
            if (n > 0 && len + n < max_len) {
                for (int i = len - 1; i >= pos; i--) out_buf[i + n] = out_buf[i];
                for (int i = 0; i < n; i++) out_buf[pos + i] = seq[i];
                pos += n;
                len += n;
                out_buf[len] = '\0';
                if (pos == len) {
                    for (int i = 0; i < n; i++) console_putc(seq[i]);
                } else {
                    redraw_line(prompt, out_buf, len, pos);
                }
            }
        }
    
done:
    st->len = len;
    st->pos = pos;
    st->hist_nav_idx = hist_nav_idx;
    return ret;
}

int readline_interactive(const char *prompt, char *out_buf, int max_len) {
    /* Before the prompt is drawn (Y5c). The prompt reaches the console
     * through console_putc(), which no longer drains -- see kernel/console.c
     * -- so without this the tail of a command's diagnostics lands after the
     * next prompt, or in front of the next command's output where it reads as
     * that command's. */
    klog_drain();

    line_state_t st;
    line_begin(&st, prompt, out_buf);

    for (;;) {
        int r = line_feed(&st, console_getc(), prompt, out_buf, max_len);
        if (r != LINE_INCOMPLETE) return r;
    }
}

/* Non-blocking counterpart. Consumes whatever input is already available and
 * returns LINE_INCOMPLETE if that did not finish a line; call it again later.
 *
 * The partial line lives in the caller's own `out_buf` between calls, and the
 * editing state lives here, so **one caller at a time, always the same
 * buffer**. That is a real constraint rather than an oversight: this exists
 * for an event loop that also has other input sources to poll (see
 * chess_next_event() in user/chess/src/chess_ui.c), and such a loop has one
 * line being typed into it by definition. readline_poll_reset() abandons a
 * half-typed line for a caller that gives up on one.
 *
 * The prompt is drawn once, when a fresh line starts. */
static line_state_t g_poll_state;
static bool         g_poll_in_progress = false;

int readline_poll(const char *prompt, char *out_buf, int max_len) {
    if (!g_poll_in_progress) {
        line_begin(&g_poll_state, prompt, out_buf);
        g_poll_in_progress = true;
    }

    while (console_has_char()) {
        int r = line_feed(&g_poll_state, console_getc(), prompt, out_buf, max_len);
        if (r != LINE_INCOMPLETE) {
            g_poll_in_progress = false;
            return r;
        }
    }
    return LINE_INCOMPLETE;
}

void readline_poll_reset(void) {
    /* Only the latch needs clearing: line_begin() rebuilds the rest, and
     * redraws the prompt, on the next call. */
    g_poll_in_progress = false;
}

bool readline_poll_active(void) {
    return g_poll_in_progress && g_poll_state.len > 0;
}
