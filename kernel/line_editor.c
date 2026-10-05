#include "kernel/line_editor.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/keyseq.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/clipboard.h"
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

/* 37.5a: the selection [sel_a, sel_b) in reverse video (sel_a < 0: none). */
static void redraw_line_sel(const char *prompt, const char *buf, int len, int pos, int sel_a, int sel_b) {
    console_lock();
    console_sync();   /* backlog before the redraw, never inside it */
    console_puts("\033[?25l"); // Hide cursor during display redraw
    console_puts("\r");
    console_puts(prompt);
    for (int i = 0; i < len; i++) {
        if (i == sel_a && sel_a < sel_b) console_puts("\033[7m");
        if (i == sel_b && sel_a >= 0) console_puts("\033[27m");
        console_putc(buf[i]);
    }
    if (sel_a >= 0 && sel_b >= len) console_puts("\033[27m");
    console_puts("\033[K"); // Clear remaining line to the right

    // Move cursor back to pos using ANSI Cursor Left (\033[D), one per character
    for (int i = 0; i < u8_chars(buf, pos, len); i++) {
        console_puts("\033[D");
    }
    console_puts("\033[?25h"); // Show cursor at final target position
    console_unlock();
}

static void redraw_line(const char *prompt, const char *buf, int len, int pos) {
    redraw_line_sel(prompt, buf, len, pos, -1, -1);
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
    static bool history_given_up = false;
    if (history_given_up) return;
    if (!history_dir_ready) {
        vfs_stat_t st;
        if (vfs_stat("/sd0/system", &st) != 0 || !st.is_dir) {
            /* A mkdir that fails on a mounted card means the card is gone
             * or broken, and this boot will not see it come back (a card is
             * mounted at boot only). Trying again after every command made
             * each one log the failure -- after a card pull, forever. */
            if (vfs_mkdir("/sd0/system") != 0) {
                if (vfs_volume_mounted("sd0")) history_given_up = true;
                return;
            }
            printk("[LineEditor] created /sd0/system for the command history\n");
        }
        history_dir_ready = true;
    }
    if (vfs_append("/sd0/system/history.lisp", entry_buf, elen) < 0) {
        history_dir_ready = false;
    }
}



/* `e` and the Ctrl-X box live in kernel/editor.c since 37.5b. */

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
#define LINE_CANCELLED  (-2)

/* 37.5a, plan/phase37_screen_layouts_and_apps.md: the line is edited with
 * keys from kernel/keyseq.c -- one parser for every text input -- and has a
 * selection and the machine's clipboard (kernel/clipboard.h).
 *
 * **Selection.** `mark` is the other end of the selection, or -1. Shift with
 * a movement sets it where the cursor was and moves (a *transient*
 * selection, which the next plain movement ends); Ctrl-Space sets it where
 * the cursor is (the Emacs mark, which movements keep: `mark_sticky`).
 * Ctrl-G or Esc ends it. Typing, Backspace and Delete replace or remove a
 * selection.
 *
 * **Two key families, one command set** (§37.5): cut Ctrl-W / Super+X, copy
 * Alt-W / Super+C, paste Ctrl-Y / Super+V, select all Super+A; Ctrl-K kills
 * to the end of the line into the clipboard. Words: Alt-B/F or Ctrl-arrows
 * to move, Alt-Backspace and Alt-D to delete. */
typedef struct {
    int  len;
    int  pos;
    int  hist_nav_idx;
    int  mark;              /* the selection's other end, or -1 */
    bool mark_sticky;       /* set by Ctrl-Space: movement keeps it */
    uint16_t dropped;       /* bytes that did not fit, since the last edit
                             * (in what was padding: no growth) */
    char temp_saved_line[MAX_LINE_LEN];
} line_state_t;

static void line_begin(line_state_t *st, const char *prompt, char *out_buf) {
    st->len = 0;
    st->pos = 0;
    st->hist_nav_idx = history_count;
    st->mark = -1;
    st->mark_sticky = false;
    st->dropped = 0;
    st->temp_saved_line[0] = '\0';
    out_buf[0] = '\0';
    redraw_line(prompt, out_buf, st->len, st->pos);
}

static void line_show(const line_state_t *st, const char *prompt, const char *buf) {
    if (st->mark >= 0 && st->mark != st->pos) {
        int a = st->mark < st->pos ? st->mark : st->pos, b = st->mark < st->pos ? st->pos : st->mark;
        redraw_line_sel(prompt, buf, st->len, st->pos, a, b);
    } else {
        redraw_line(prompt, buf, st->len, st->pos);
    }
}

/* A word is a run of letters, digits, '_' or any non-ASCII character. */
static bool word_byte(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x80u || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
           (u >= 'A' && u <= 'Z') || u == '_';
}

static int word_left(const char *buf, int pos) {
    while (pos > 0 && !word_byte(buf[pos - 1])) pos--;
    while (pos > 0 && word_byte(buf[pos - 1])) pos--;
    return pos;
}

static int word_right(const char *buf, int len, int pos) {
    while (pos < len && !word_byte(buf[pos])) pos++;
    while (pos < len && word_byte(buf[pos])) pos++;
    return pos;
}

static bool sel_range(const line_state_t *st, int *a, int *b) {
    if (st->mark < 0 || st->mark == st->pos) return false;
    *a = st->mark < st->pos ? st->mark : st->pos;
    *b = st->mark < st->pos ? st->pos : st->mark;
    return true;
}

/* Deletes the selection, if there is one; true if it did. */
static bool sel_delete(line_state_t *st, char *buf) {
    int a, b;
    if (!sel_range(st, &a, &b)) return false;
    line_cut(buf, &st->len, a, b);
    st->pos = a;
    st->mark = -1;
    return true;
}

/* Inserts `n` bytes at the cursor, as far as the line has room, dropping
 * controls (a paste may carry newlines and tabs; they become spaces). Never
 * stops inside a UTF-8 character. What finds no room is counted in
 * `dropped`, for Enter to refuse the line (line_key()). */
static void line_insert(line_state_t *st, char *buf, int max_len, const char *s, int n) {
    for (int i = 0; i < n;) {
        int k = 1;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xc0u) k = c >= 0xf0u ? 4 : c >= 0xe0u ? 3 : 2;
        if (i + k > n) break;
        char one[4];
        int m = k;
        if (k == 1) {
            if (c == '\n' || c == '\r' || c == '\t') one[0] = ' ';
            else if (c < 0x20u || c == 0x7fu || c >= 0x80u) { i++; continue; }
            else one[0] = (char)c;
        } else {
            for (int j = 0; j < k; j++) one[j] = s[i + j];
        }
        if (st->len + m >= max_len) {
            int d = st->dropped + (n - i);
            st->dropped = (uint16_t)(d > 0xffff ? 0xffff : d);
            break;
        }
        for (int j = st->len - 1; j >= st->pos; j--) buf[j + m] = buf[j];
        for (int j = 0; j < m; j++) buf[st->pos + j] = one[j];
        st->pos += m;
        st->len += m;
        i += k;
    }
    buf[st->len] = '\0';
}

static void clip_copy(const line_state_t *st, const char *buf) {
    int a, b;
    if (sel_range(st, &a, &b)) (void)clipboard_set(buf + a, (uint32_t)(b - a));
}

static void clip_paste(line_state_t *st, char *buf, int max_len) {
    uint32_t n;
    const char *s = clipboard_data(&n);
    (void)sel_delete(st, buf);
    if (s) line_insert(st, buf, max_len, s, (int)n);
}

/* A movement: with Shift it extends the selection (setting the mark first);
 * without, it ends a transient one. */
static void line_move(line_state_t *st, int to, bool shift) {
    if (shift) {
        if (st->mark < 0) { st->mark = st->pos; st->mark_sticky = false; }
    } else if (!st->mark_sticky) {
        st->mark = -1;
    }
    st->pos = to;
}

static void hist_show(line_state_t *st, char *out_buf, int max_len, int idx) {
    if (idx == history_count) safe_strncpy(out_buf, st->temp_saved_line, max_len);
    else safe_strncpy(out_buf, history_stack[idx], max_len);
    st->hist_nav_idx = idx;
    st->len = (int)strlen(out_buf);
    st->pos = st->len;
    st->mark = -1;
}

/* Encodes a code point as UTF-8 into out; returns the bytes. */
static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80u) { out[0] = (char)cp; return 1; }
    if (cp < 0x800u) { out[0] = (char)(0xc0u | (cp >> 6)); out[1] = (char)(0x80u | (cp & 0x3fu)); return 2; }
    if (cp < 0x10000u) {
        out[0] = (char)(0xe0u | (cp >> 12)); out[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (cp & 0x3fu));
        return 3;
    }
    out[0] = (char)(0xf0u | (cp >> 18)); out[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3fu)); out[3] = (char)(0x80u | (cp & 0x3fu));
    return 4;
}

/* Consumes one key. Returns the completed line's length, LINE_INCOMPLETE
 * while the line is still being edited, or LINE_CANCELLED (Esc or Ctrl-G
 * with nothing selected, when the options allow it). */
static int line_key(line_state_t *st, key_event_t k, const char *prompt,
                    char *out_buf, int max_len, const readline_opts_t *o) {
    uint32_t key = k.key;
    bool shift = (k.mods & KMOD_SHIFT) != 0, alt = (k.mods & KMOD_ALT) != 0;
    bool ctrl = (k.mods & KMOD_CTRL) != 0, super = (k.mods & KMOD_SUPER) != 0;
    int len = st->len, pos = st->pos;
    (void)len;

    /* A line that lost input (a paste or a sent block longer than the
     * buffer) is refused by the Enter that ends it -- see below -- unless
     * some other key came first: then the person has seen the line as it
     * is, perhaps fixed it, and Enter means it. */
    bool input = (key == '\r' || key == '\n') ||
                 (super ? key == 'v' : !alt && (key == 0x19 || (!ctrl && key >= 0x20 && key != 0x7f && key < 0x110000u)));
    if (!input) st->dropped = 0;

    if (super) {
        if (key == 'c') { clip_copy(st, out_buf); st->mark = -1; }
        else if (key == 'x') { clip_copy(st, out_buf); (void)sel_delete(st, out_buf); }
        else if (key == 'v') clip_paste(st, out_buf, max_len);
        else if (key == 'a') { st->mark = 0; st->mark_sticky = true; st->pos = st->len; }
        else if (key == '\r' || key == '\n') console_hotkey(CONSOLE_HOTKEY_NEW_TERM);
        else if (key == KEY_LEFT) console_hotkey(ctrl ? CONSOLE_HOTKEY_MOVE_LEFT : CONSOLE_HOTKEY_FOCUS_LEFT);
        else if (key == KEY_RIGHT) console_hotkey(ctrl ? CONSOLE_HOTKEY_MOVE_RIGHT : CONSOLE_HOTKEY_FOCUS_RIGHT);
        else if (key == 'w' || key == 'W') console_hotkey(CONSOLE_HOTKEY_CLOSE);
        else if (key == '[') console_hotkey(CONSOLE_HOTKEY_LEFT);
        else if (key == ']') console_hotkey(CONSOLE_HOTKEY_RIGHT);
        else if (key == '\\') console_hotkey(CONSOLE_HOTKEY_SWAP);
        else if (shift && key == '3') console_hotkey(CONSOLE_HOTKEY_SCREENSHOT);   /* before the jumps */
        else if (!shift && key >= '1' && key <= '9') console_hotkey(CONSOLE_HOTKEY_JUMP((unsigned)(key - '0')));
        else return LINE_INCOMPLETE;            /* other Super keys: not ours */
        line_show(st, prompt, out_buf);
        return LINE_INCOMPLETE;
    }
    if (alt) {
        if (key == 'b' || key == 'B') line_move(st, word_left(out_buf, pos), false);
        else if (key == 'f' || key == 'F') line_move(st, word_right(out_buf, st->len, pos), false);
        else if (key == 'w' || key == 'W') { clip_copy(st, out_buf); st->mark = -1; }
        else if (key == 0x7f || key == 0x08) {
            int from = word_left(out_buf, pos);
            line_cut(out_buf, &st->len, from, pos);
            st->pos = from;
            st->mark = -1;
        } else if (key == 'd' || key == 'D') {
            line_cut(out_buf, &st->len, pos, word_right(out_buf, st->len, pos));
            st->mark = -1;
        } else if (key == KEY_LEFT) line_move(st, word_left(out_buf, pos), shift);
        else if (key == KEY_RIGHT) line_move(st, word_right(out_buf, st->len, pos), shift);
        else return LINE_INCOMPLETE;
        line_show(st, prompt, out_buf);
        return LINE_INCOMPLETE;
    }

    switch (key) {
    case KEY_LEFT:
        line_move(st, ctrl ? word_left(out_buf, pos) : u8_prev(out_buf, pos), shift);
        break;
    case KEY_RIGHT:
        line_move(st, ctrl ? word_right(out_buf, st->len, pos) : u8_next(out_buf, st->len, pos), shift);
        break;
    case 0x02: line_move(st, u8_prev(out_buf, pos), false); break;                 /* Ctrl-B */
    case 0x06: line_move(st, u8_next(out_buf, st->len, pos), false); break;        /* Ctrl-F */
    case KEY_HOME: case 0x01: line_move(st, 0, shift); break;                      /* Ctrl-A */
    case KEY_END:  case 0x05: line_move(st, st->len, shift); break;                /* Ctrl-E */
    case KEY_UP: case 0x10:                                                         /* Ctrl-P */
        if (o->no_history) return LINE_INCOMPLETE;
        if (st->hist_nav_idx > 0) {
            if (st->hist_nav_idx == history_count) safe_strncpy(st->temp_saved_line, out_buf, MAX_LINE_LEN);
            hist_show(st, out_buf, max_len, st->hist_nav_idx - 1);
        }
        break;
    case KEY_DOWN: case 0x0E:                                                       /* Ctrl-N */
        if (o->no_history) return LINE_INCOMPLETE;
        if (st->hist_nav_idx < history_count) hist_show(st, out_buf, max_len, st->hist_nav_idx + 1);
        break;
    case 0x00:                                                                      /* Ctrl-Space */
        st->mark = pos;
        st->mark_sticky = true;
        break;
    case 0x07: case 0x1b:                                                           /* Ctrl-G, Esc */
        if (st->mark < 0 && o->cancel_on_esc) {
            out_buf[0] = '\0';
            return LINE_CANCELLED;
        }
        st->mark = -1;
        break;
    case 0x17: clip_copy(st, out_buf); (void)sel_delete(st, out_buf); break;     /* Ctrl-W */
    case 0x19: clip_paste(st, out_buf, max_len); break;                           /* Ctrl-Y */
    case 0x0B:                                                                      /* Ctrl-K */
        if (pos < st->len) (void)clipboard_set(out_buf + pos, (uint32_t)(st->len - pos));
        st->len = pos;
        out_buf[st->len] = '\0';
        st->mark = -1;
        break;
    case 0x04: case KEY_DELETE:                                                     /* Ctrl-D */
        if (!sel_delete(st, out_buf) && pos < st->len) line_cut(out_buf, &st->len, pos, u8_next(out_buf, st->len, pos));
        break;
    case 0x08: case 0x7F:                                                           /* Backspace */
        if (!sel_delete(st, out_buf) && pos > 0) {
            int from = u8_prev(out_buf, pos);
            line_cut(out_buf, &st->len, from, pos);
            st->pos = from;
        }
        break;
    case 0x0C:                                                                      /* Ctrl-L */
        console_lock();
        console_puts("\033[2J\033[H");
        console_unlock();
        break;
    case 0x18: {                                                                    /* Ctrl-X */
        if (o->no_history) return LINE_INCOMPLETE;
        key_event_t k2 = keyseq_read_console();
        if (k2.key == 0x0D || k2.key == 0x0A || k2.key == 0x05 || k2.key == 'm' || k2.key == 'M' ||
            k2.key == 'e' || k2.key == 'E') {
            int mlen = edit_multiline_box("/ram0/system/scratch.lisp", out_buf, max_len);
            add_history(out_buf);
            return mlen;
        }
        return LINE_INCOMPLETE;
    }
    case '\r': case '\n':
        if (!o->no_newline) { console_lock(); console_puts("\n"); console_unlock(); }
        if (st->dropped > 0) {
            /* Silently cut, the tail of a program used to be read as
             * whatever was left: Lisp saw `()p4(m)` at the end of an
             * 1100-byte line. Nothing of it is entered, or kept in the
             * history; the person learns why. */
            cprintf("Line too long: %d bytes did not fit in %d -- not entered."
                    " Longer programs belong in a file (e).\n", (int)st->dropped, max_len - 1);
            st->dropped = 0;
            st->len = 0;
            st->pos = 0;
            out_buf[0] = '\0';
            console_interrupt_clear();
            return 0;
        }
        out_buf[st->len] = '\0';
        if (!o->no_history) add_history(out_buf);
        /* A Ctrl-C typed while composing this line cancelled nothing --
         * there was no command running to cancel. Clear it here, as the
         * line is handed over, so it cannot be mistaken for an interrupt
         * aimed at the command about to run. Ctrl-C pressed *during* that
         * command still latches normally, which is the case that matters. */
        console_interrupt_clear();
        return st->len;
    default:
        if (key >= 0x20 && key != 0x7f && key < 0x110000u && !ctrl) {
            char enc[4];
            int n = utf8_encode(key, enc);
            bool sel = sel_delete(st, out_buf);
            bool at_end = st->pos == st->len;
            int before = st->len;
            line_insert(st, out_buf, max_len, enc, n);
            if (at_end && !sel && st->mark < 0) {
                /* The common case, typing at the end: echo, no redraw --
                 * and nothing for a key that found no room, or the screen
                 * would show a line the buffer does not hold. */
                if (st->len > before) {
                    /* Locked like a redraw (phase31 §7 R4): the echo goes
                     * into the LCD batch every terminal's writers share. */
                    console_lock();
                    for (int i = 0; i < n; i++) console_putc(enc[i]);
                    console_unlock();
                }
                return LINE_INCOMPLETE;
            }
            break;
        }
        return LINE_INCOMPLETE;                 /* Tab, other controls: nothing */
    }
    line_show(st, prompt, out_buf);
    return LINE_INCOMPLETE;
}

static const readline_opts_t g_shell_opts = { false, false, false };

static void (*g_idle)(void);

void readline_set_idle(void (*fn)(void)) {
    g_idle = fn;
}

void readline_idle(void) {
    if (g_idle) g_idle();
}

/* The next key, giving the idle hook its turns while none is there. */
static key_event_t next_key(void) {
    if (g_idle) {
        uint64_t next = time_get_us() + 100000u;
        while (!console_has_char()) {
            if (time_get_us() >= next) {
                g_idle();
                next = time_get_us() + 100000u;
            }
            sched_yield();
        }
    }
    return keyseq_read_console();
}

int readline_ex(const char *prompt, char *out_buf, int max_len, const readline_opts_t *opts) {
    /* Before the prompt is drawn (Y5c). The prompt reaches the console
     * through console_putc(), which no longer drains -- see kernel/console.c
     * -- so without this the tail of a command's diagnostics lands after the
     * next prompt, or in front of the next command's output where it reads as
     * that command's. */
    klog_drain();
    const readline_opts_t *o = opts ? opts : &g_shell_opts;
    line_state_t st;
    line_begin(&st, prompt, out_buf);
    for (;;) {
        int r = line_key(&st, next_key(), prompt, out_buf, max_len, o);
        if (r == LINE_CANCELLED) return -1;
        if (r != LINE_INCOMPLETE) return r;
    }
}

int readline_interactive(const char *prompt, char *out_buf, int max_len) {
    return readline_ex(prompt, out_buf, max_len, NULL);
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
 * A key that arrives as several bytes is read whole once its first byte is
 * there (kernel/keyseq.c): a terminal sends a sequence in one burst, so the
 * wait is for bytes already in flight, never for the user.
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
        int r = line_key(&g_poll_state, keyseq_read_console(), prompt, out_buf, max_len, &g_shell_opts);
        if (r != LINE_INCOMPLETE && r != LINE_CANCELLED) {
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
