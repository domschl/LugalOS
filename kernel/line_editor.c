#include "kernel/line_editor.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "drivers/uart.h"
#include "drivers/usb_cdc.h"
#include "fs/vfs.h"
#include <string.h>

#define MAX_HIST_ITEMS 32
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

static void redraw_line(const char *prompt, const char *buf, int len, int pos) {
    console_puts("\033[?25l"); // Hide cursor during display redraw
    console_puts("\r");
    console_puts(prompt);
    for (int i = 0; i < len; i++) {
        console_putc(buf[i]);
    }
    console_puts("\033[K"); // Clear remaining line to the right

    // Move cursor back to pos using ANSI Cursor Left (\033[D)
    for (int i = 0; i < len - pos; i++) {
        console_puts("\033[D");
    }
    console_puts("\033[?25h"); // Show cursor at final target position
}


void line_editor_init(void) {
    history_count = 0;
    printk("[LineEditor] History Engine Initialized.\n");
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
    vfs_append("/sd0/system/history.lisp", entry_buf, elen);
}



static int g_prev_target_line = 1;

static void redraw_box(const char *filename, const char *buf, int len, int pos, const char *status_msg) {
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
        usb_cdc_task();
        char c = uart_getc();
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
        usb_cdc_task();
        char c = uart_getc();

        int num_lines = 1;
        for (int i = 0; i < len; i++) if (out_buf[i] == '\n') num_lines++;

        if (c == 0x18) { // Ctrl-X
            char c2 = uart_getc();
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
                vfs_write(active_filename, out_buf, len);
                modified = false;
                strcpy(status_msg, "Wrote file");
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
                    static char ins_buf[2048];
                    int r = vfs_read(fn_in, ins_buf, sizeof(ins_buf) - 1);
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
                }
                redraw_box(active_filename, out_buf, len, pos, status_msg);
                status_msg[0] = '\0';
                continue;
            } else if (c2 == 0x17 || c2 == 'w' || c2 == 'W') { // Ctrl-W: Write File As
                char fn_in[128];
                if (read_status_prompt(num_lines, g_prev_target_line, "Write file: ", fn_in, sizeof(fn_in))) {
                    safe_strncpy(active_filename, fn_in, sizeof(active_filename));
                    vfs_write(active_filename, out_buf, len);
                    modified = false;
                    strcpy(status_msg, "Wrote file as");
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
            if (pos > 0) pos--;
            redraw_box(active_filename, out_buf, len, pos, status_msg);
            continue;
        } else if (c == 0x06) { // Ctrl-F: Right
            if (pos < len) pos++;
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
            char seq1 = uart_getc();
            if (seq1 == '[' || seq1 == 'O') {
                char seq2 = uart_getc();
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
                    if (seq2 == '1') uart_getc();
                    while (pos > 0 && out_buf[pos - 1] != '\n') pos--;
                    redraw_box(active_filename, out_buf, len, pos, status_msg);
                } else if (seq2 == 'F' || seq2 == '4') { // End key
                    if (seq2 == '4') uart_getc();
                    while (pos < len && out_buf[pos] != '\n') pos++;
                    redraw_box(active_filename, out_buf, len, pos, status_msg);
                } else if (seq2 == '3') { // Delete key
                    uart_getc();
                    if (pos < len) {
                        for (int i = pos; i < len - 1; i++) out_buf[i] = out_buf[i + 1];
                        len--;
                        out_buf[len] = '\0';
                        redraw_box(active_filename, out_buf, len, pos, status_msg);
                    }
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



int readline_interactive(const char *prompt, char *out_buf, int max_len) {
    int len = 0;
    int pos = 0;
    int hist_nav_idx = history_count;
    char temp_saved_line[MAX_LINE_LEN];
    temp_saved_line[0] = '\0';

    out_buf[0] = '\0';
    redraw_line(prompt, out_buf, len, pos);

    while (1) {
        usb_cdc_task();
        char c = uart_getc();

        // Control character handling
        if (c == 0x01) { // Ctrl-A: Move to beginning of line
            pos = 0;
            redraw_line(prompt, out_buf, len, pos);
            continue;
        } else if (c == 0x05) { // Ctrl-E: Move to end of line
            pos = len;
            redraw_line(prompt, out_buf, len, pos);
            continue;
        } else if (c == 0x02) { // Ctrl-B: Left
            if (pos > 0) pos--;
            redraw_line(prompt, out_buf, len, pos);
            continue;
        } else if (c == 0x06) { // Ctrl-F: Right
            if (pos < len) pos++;
            redraw_line(prompt, out_buf, len, pos);
            continue;
        } else if (c == 0x04) { // Ctrl-D: Delete char under cursor
            if (pos < len) {
                for (int i = pos; i < len - 1; i++) {
                    out_buf[i] = out_buf[i + 1];
                }
                len--;
                out_buf[len] = '\0';
                redraw_line(prompt, out_buf, len, pos);
            }
            continue;
        } else if (c == 0x0B) { // Ctrl-K: Kill to end of line
            len = pos;
            out_buf[len] = '\0';
            redraw_line(prompt, out_buf, len, pos);
            continue;
        } else if (c == 0x0C) { // Ctrl-L: Clear screen
            console_puts("\033[2J\033[H");
            redraw_line(prompt, out_buf, len, pos);
            continue;
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
            continue;
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
            continue;
        } else if (c == 0x18) { // Ctrl-X
            char c2 = uart_getc();
            if (c2 == 0x0D || c2 == 0x0A || c2 == 0x05 || c2 == 'm' || c2 == 'M' || c2 == 'e' || c2 == 'E') { // Ctrl-M or Ctrl-E
                int mlen = edit_multiline_box("/ram0/system/scratch.lisp", out_buf, max_len);
                add_history(out_buf);
                return mlen;
            }
            continue;
        }


        // Escape Sequences (Arrow keys, Home, End, Delete)
        if (c == 0x1B) {
            char seq1 = uart_getc();
            if (seq1 == '[' || seq1 == 'O') {
                char seq2 = uart_getc();

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
                        pos++;
                        redraw_line(prompt, out_buf, len, pos);
                    }
                } else if (seq2 == 'D') { // Left Arrow
                    if (pos > 0) {
                        pos--;
                        redraw_line(prompt, out_buf, len, pos);
                    }
                } else if (seq2 == 'H' || seq2 == '1') { // Home key
                    pos = 0;
                    if (seq2 == '1') uart_getc(); // consume '~'
                    redraw_line(prompt, out_buf, len, pos);
                } else if (seq2 == 'F' || seq2 == '4') { // End key
                    pos = len;
                    if (seq2 == '4') uart_getc(); // consume '~'
                    redraw_line(prompt, out_buf, len, pos);
                } else if (seq2 == '3') { // Delete key
                    uart_getc(); // consume '~'
                    if (pos < len) {
                        for (int i = pos; i < len - 1; i++) {
                            out_buf[i] = out_buf[i + 1];
                        }
                        len--;
                        out_buf[len] = '\0';
                        redraw_line(prompt, out_buf, len, pos);
                    }
                }
            }
            continue;
        }

        // Backspace handling
        if (c == 0x08 || c == 0x7F) {
            if (pos > 0) {
                for (int i = pos - 1; i < len - 1; i++) {
                    out_buf[i] = out_buf[i + 1];
                }
                pos--;
                len--;
                out_buf[len] = '\0';
                redraw_line(prompt, out_buf, len, pos);
            }
            continue;
        }

        // Enter key
        if (c == '\r' || c == '\n') {
            console_puts("\n");
            out_buf[len] = '\0';
            add_history(out_buf);
            return len;
        }

        // Standard printable character
        if (c >= 32 && c <= 126) {
            if (len < max_len - 1) {
                if (pos == len) {
                    out_buf[pos] = c;
                    pos++;
                    len++;
                    out_buf[len] = '\0';
                    console_putc(c);
                } else {
                    for (int i = len; i > pos; i--) {
                        out_buf[i] = out_buf[i - 1];
                    }
                    out_buf[pos] = c;
                    pos++;
                    len++;
                    out_buf[len] = '\0';
                    redraw_line(prompt, out_buf, len, pos);
                }
            }
        }
    }
}

