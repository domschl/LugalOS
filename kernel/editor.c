/*
 * `e` -- the editor, and the writer. 37.5b, plan/phase37_screen_layouts_and_apps.md.
 * See kernel/include/kernel/editor.h for what it does and its keys.
 *
 * **The buffer** is one run of UTF-8 bytes on the heap, grown by doubling.
 * Every change, from a typed letter to an undo, is one call to ed_change():
 * replace [at, at + dn) with `in` new bytes. A memmove per keystroke is a
 * fraction of a millisecond for anything a person writes on this machine,
 * and one primitive is what keeps undo, the viewport and the selection
 * honest.
 *
 * **Rows.** What the screen shows is a sequence of display rows. In code
 * mode a row is a line; in text mode a line (a paragraph) is cut into rows
 * at word boundaries, a row at most tcols - 1 cells wide so the cursor
 * still fits after the last character. Everything about the screen is
 * phrased in four functions -- row_end(), next_row(), row_start_of(),
 * prev_row() -- and nothing above them knows which mode is on.
 *
 * **Drawing** is a diff. Each frame renders every visible row to bytes
 * (cheap: a few KB of the buffer) and hashes it; only rows whose hash
 * changed are sent. Before that, a row shift is looked for -- the old rows
 * reappearing k rows lower or higher, which is what scrolling, Enter and
 * joining lines all look like -- and done by the terminal with an insert or
 * delete of lines inside the scroll region (rows 1 .. rows-1; the last row
 * is the status line), so a scroll by one sends one row, not a screenful.
 * On the RP2350-LCD-7 that is the difference between ~1.5 ms and ~90 ms.
 *
 * **Undo** is a stack of the changes' inverses in 8 KB (taken on the first
 * edit): each record is where, how much was inserted, and the bytes that
 * were removed. When it is full the oldest records go. Typing coalesces
 * into one record a word at a time.
 */

#include "kernel/editor.h"
#include "kernel/line_editor.h"
#include "kernel/console.h"
#include "kernel/keyseq.h"
#include "kernel/clipboard.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/klog.h"
#include "drivers/screen.h"
#include "fs/vfs.h"

#include <string.h>

#define ED_ROWS_MAX   64u               /* text rows: the panel has 27 */
#define ED_COLS_MAX   160u
#define ED_LINE_BYTES (ED_COLS_MAX * 4u + 64u)
#define ED_UNDO_MAX   8192u
#define ED_NONE       0xffffffffu
#define ED_TAB        8u
#define ED_EMPTY_HASH 0x811c9dc5u       /* FNV-1a of nothing: a blank row */

typedef struct {
    char    *buf;
    uint32_t len, cap;              /* cap: bytes allocated (whole pages) */
    uint32_t pos;                   /* the cursor, on a character boundary */
    int32_t  mark;                  /* the selection's other end, or -1 */
    bool     mark_sticky;           /* Ctrl-Space: movement keeps it */
    uint32_t top;                   /* the first visible row's start */
    uint32_t hleft;                 /* code mode: the first visible column */
    int32_t  goal;                  /* the column vertical moves aim for, -1 */
    bool     text_mode, modified;
    bool     kill_append;           /* the last command was Ctrl-K */
    bool     typing;                /* the last command typed a character */
    char     last_typed;
    unsigned cols, rows, trows, gutter, tcols;
    bool     drawn;                 /* the screen holds this frame's region */
    uint32_t hash[ED_ROWS_MAX];     /* what each text row shows now */
    uint32_t rs[ED_ROWS_MAX], nh[ED_ROWS_MAX], lns[ED_ROWS_MAX];   /* the frame being drawn */
    uint32_t status_hash;
    /* Line counts, kept rather than recounted (38.7): every frame of a code
     * file asked for the lines before the end, the top and the cursor, three
     * scans of the whole text -- 51 ms a keystroke for 200 KB in SRAM, 88 ms
     * in PSRAM. `nl` is the newlines in the text; `ln_n` the newlines before
     * `ln_at`, the last position asked about. ed_raw() keeps both; a text
     * replaced any other way calls ed_lines_reset(). */
    bool     ln_ok;
    uint32_t nl, ln_at, ln_n;
    uint8_t *undo;
    uint32_t undo_len;
    bool     undo_lost;             /* records were dropped, or never kept */
    const editor_hooks_t *hooks;
    char     file[128];
    char     msg[96];
    char     find[64];
    char     line[ED_LINE_BYTES];   /* one rendered row */
} ed_t;

#define ED_STATE_PAGES ((sizeof(ed_t) + PAGE_SIZE - 1u) / PAGE_SIZE)
#define ED_UNDO_PAGES  (ED_UNDO_MAX / PAGE_SIZE)

/* --- Characters, lines, rows ------------------------------------------- */

static bool u8_cont(char c) {
    return ((unsigned char)c & 0xc0u) == 0x80u;
}

static uint32_t nextc(const ed_t *e, uint32_t p) {
    if (p < e->len) p++;
    while (p < e->len && u8_cont(e->buf[p])) p++;
    return p;
}

static uint32_t prevc(const ed_t *e, uint32_t p) {
    if (p > 0) p--;
    while (p > 0 && u8_cont(e->buf[p])) p--;
    return p;
}

static unsigned width_at(const ed_t *e, uint32_t p, unsigned col) {
    return e->buf[p] == '\t' ? ED_TAB - col % ED_TAB : 1u;
}

static uint32_t line_start(const ed_t *e, uint32_t p) {
    while (p > 0 && e->buf[p - 1u] != '\n') p--;
    return p;
}

static uint32_t line_end(const ed_t *e, uint32_t p) {
    while (p < e->len && e->buf[p] != '\n') p++;
    return p;
}

/* The end of the row that starts at s. If the line ends in this row, its
 * end -- the '\n', or the buffer's end -- and *eol; if the row wraps, where
 * the next row starts: after the last space that fits, or mid-word for a
 * word longer than the row. A space that would overflow stays at the row's
 * end, where it is invisible (the row has the one spare column). */
static uint32_t row_end(const ed_t *e, uint32_t s, bool *eol) {
    *eol = true;
    if (!e->text_mode) return line_end(e, s);
    unsigned wrap = e->tcols > 2u ? e->tcols - 1u : 1u, col = 0;
    uint32_t p = s, brk = ED_NONE;
    while (p < e->len && e->buf[p] != '\n') {
        unsigned w = width_at(e, p, col);
        if (col + w > wrap && p > s) {
            if (e->buf[p] == ' ') {
                uint32_t n = p + 1u;
                if (n >= e->len || e->buf[n] == '\n') return n;
                *eol = false;
                return n;
            }
            *eol = false;
            return brk != ED_NONE ? brk : p;
        }
        col += w;
        char c = e->buf[p];
        p = nextc(e, p);
        if (c == ' ' || c == '\t') brk = p;
    }
    return p;
}

static uint32_t next_row(const ed_t *e, uint32_t s) {
    bool eol;
    uint32_t r = row_end(e, s, &eol);
    if (!eol) return r;
    return r < e->len ? r + 1u : ED_NONE;
}

static uint32_t row_start_of(const ed_t *e, uint32_t p) {
    uint32_t s = line_start(e, p);
    if (!e->text_mode) return s;
    for (;;) {
        bool eol;
        uint32_t r = row_end(e, s, &eol);
        if (eol || p < r) return s;
        s = r;
    }
}

static uint32_t prev_row(const ed_t *e, uint32_t s) {
    return s == 0 ? ED_NONE : row_start_of(e, s - 1u);
}

static unsigned col_of(const ed_t *e, uint32_t rs, uint32_t p) {
    unsigned col = 0;
    for (uint32_t q = rs; q < p; q = nextc(e, q)) col += width_at(e, q, col);
    return col;
}

/* The position in row rs nearest column `goal`, never past the row. */
static uint32_t pos_at(const ed_t *e, uint32_t rs, unsigned goal) {
    bool eol;
    uint32_t r = row_end(e, rs, &eol), p = rs;
    unsigned col = 0;
    while (p < r) {
        unsigned w = width_at(e, p, col);
        if (col + w > goal) break;
        col += w;
        p = nextc(e, p);
    }
    if (!eol && p >= r) p = prevc(e, r);
    return p;
}

static uint32_t count_nl(const char *s, uint32_t n) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) if (s[i] == '\n') c++;
    return c;
}

static void ed_lines_reset(ed_t *e) {
    e->ln_ok = false;
}

/* Newlines before `p`: counted from the last position asked about, from
 * the start, or not at all for the end -- whichever is nearest. */
static uint32_t lines_before(ed_t *e, uint32_t p) {
    if (!e->ln_ok) {
        e->nl = count_nl(e->buf, e->len);
        e->ln_at = e->ln_n = 0;
        e->ln_ok = true;
    }
    if (p >= e->len) return e->nl;
    uint32_t n;
    if (p >= e->ln_at) n = e->ln_n + count_nl(e->buf + e->ln_at, p - e->ln_at);
    else if (e->ln_at - p <= p) n = e->ln_n - count_nl(e->buf + p, e->ln_at - p);
    else n = count_nl(e->buf, p);
    e->ln_at = p;
    e->ln_n = n;
    return n;
}

static bool word_byte(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x80u || (u >= '0' && u <= '9') || ((u | 0x20u) >= 'a' && (u | 0x20u) <= 'z') || u == '_';
}

static uint32_t word_left(const ed_t *e, uint32_t p) {
    while (p > 0 && !word_byte(e->buf[p - 1u])) p--;
    while (p > 0 && word_byte(e->buf[p - 1u])) p--;
    return p;
}

static uint32_t word_right(const ed_t *e, uint32_t p) {
    while (p < e->len && !word_byte(e->buf[p])) p++;
    while (p < e->len && word_byte(e->buf[p])) p++;
    return p;
}

static bool sel_range(const ed_t *e, uint32_t *a, uint32_t *b) {
    if (e->mark < 0 || (uint32_t)e->mark == e->pos) return false;
    uint32_t m = (uint32_t)e->mark;
    *a = m < e->pos ? m : e->pos;
    *b = m < e->pos ? e->pos : m;
    return true;
}

static void say(ed_t *e, const char *s) {
    strncpy(e->msg, s, sizeof(e->msg) - 1u);
    e->msg[sizeof(e->msg) - 1u] = '\0';
}

static bool text_ext(const char *f) {
    const char *dot = NULL;
    for (const char *p = f; *p; p++) if (*p == '.') dot = p;
    return dot && (strcmp(dot, ".txt") == 0 || strcmp(dot, ".md") == 0 ||
                   strcmp(dot, ".TXT") == 0 || strcmp(dot, ".MD") == 0);
}

/* --- The buffer, and undo ---------------------------------------------- */

/* The text's pages -- the buffer, undo and an inserted file's staging --
 * are the bulk class (38.7, plan/phase38_psram.md): PSRAM on the LCD-7.
 * `edbench 200` there: a keystroke at the start of the text 28 ms (5 in
 * SRAM), a frame's line counts 3 us, a search to the end 63 ms (50).
 * The state (ed_t) stays in SRAM: it is small and touched on every key. */
static void *ed_pages(uint32_t pages) {
    return palloc_pages_bulk(pages);
}

static bool ed_reserve(ed_t *e, uint32_t more) {
    if (e->len + more + 1u <= e->cap) return true;
    uint32_t want = e->cap * 2u;
    if (want < e->len + more + 1u) want = e->len + more + 1u;
    uint32_t pages = (want + PAGE_SIZE - 1u) / PAGE_SIZE;
    char *nb = (char *)ed_pages(pages);
    if (!nb) return false;
    memcpy(nb, e->buf, e->len + 1u);
    palloc_free(e->buf, e->cap / PAGE_SIZE);
    e->buf = nb;
    e->cap = pages * PAGE_SIZE;
    return true;
}

/* Replace [at, at + dn) with in bytes of `ins`, keeping the view's start and
 * the mark where they belong. No undo record: ed_change() and undo use it. */
static bool ed_raw(ed_t *e, uint32_t at, uint32_t dn, const char *ins, uint32_t in) {
    if (in > dn && !ed_reserve(e, in - dn)) {
        say(e, "Out of memory -- the change was not made");
        return false;
    }
    if (e->ln_ok) {
        uint32_t gone = count_nl(e->buf + at, dn), come = count_nl(ins, in);
        e->nl = e->nl + come - gone;
        if (at < e->ln_at) {
            if (e->ln_at >= at + dn) {
                e->ln_at = e->ln_at + in - dn;
                e->ln_n = e->ln_n + come - gone;
            } else {
                e->ln_at = e->ln_n = 0;     /* the remembered spot went */
            }
        }
    }
    memmove(e->buf + at + in, e->buf + at + dn, e->len - at - dn);
    if (in) memcpy(e->buf + at, ins, in);
    e->len = e->len + in - dn;
    e->buf[e->len] = '\0';
    if (e->top > at) e->top = e->top >= at + dn ? e->top + in - dn : at;
    if (e->mark >= 0 && (uint32_t)e->mark > at)
        e->mark = (uint32_t)e->mark >= at + dn ? (int32_t)((uint32_t)e->mark + in - dn) : (int32_t)at;
    e->modified = true;
    return true;
}

static uint32_t pad4(uint32_t n) {
    return (n + 3u) & ~3u;
}

/* A record: at, in, dn (one word each), the dn removed bytes padded to a
 * word, and the record's size again, so the stack can be walked down. */
static uint32_t *undo_word(ed_t *e, uint32_t off) {
    return (uint32_t *)(void *)(e->undo + off);
}

static void undo_drop_oldest(ed_t *e) {
    uint32_t size = 12u + pad4(undo_word(e, 0)[2]) + 4u;
    memmove(e->undo, e->undo + size, e->undo_len - size);
    e->undo_len -= size;
    e->undo_lost = true;
}

static void undo_push(ed_t *e, uint32_t at, uint32_t dn, uint32_t in, bool merge) {
    if (!e->undo) {
        e->undo = (uint8_t *)ed_pages(ED_UNDO_PAGES);
        e->undo_len = 0;
        if (!e->undo) {
            e->undo_lost = true;
            return;
        }
    }
    if (merge && dn == 0 && e->undo_len) {
        uint32_t size = undo_word(e, e->undo_len - 4u)[0];
        uint32_t *r = undo_word(e, e->undo_len - size);
        if (r[2] == 0 && r[0] + r[1] == at) {
            r[1] += in;
            return;
        }
    }
    uint32_t size = 12u + pad4(dn) + 4u;
    if (size > ED_UNDO_MAX) {           /* larger than all of undo: start over */
        e->undo_len = 0;
        e->undo_lost = true;
        return;
    }
    while (e->undo_len + size > ED_UNDO_MAX) undo_drop_oldest(e);
    uint32_t *r = undo_word(e, e->undo_len);
    r[0] = at;
    r[1] = in;
    r[2] = dn;
    if (dn) memcpy(e->undo + e->undo_len + 12u, e->buf + at, dn);
    undo_word(e, e->undo_len + size - 4u)[0] = size;
    e->undo_len += size;
}

static bool ed_change(ed_t *e, uint32_t at, uint32_t dn, const char *ins, uint32_t in, bool merge) {
    if (in > dn && !ed_reserve(e, in - dn)) {
        say(e, "Out of memory -- the change was not made");
        return false;
    }
    undo_push(e, at, dn, in, merge);
    return ed_raw(e, at, dn, ins, in);
}

static void ed_undo(ed_t *e) {
    if (!e->undo || e->undo_len == 0) {
        say(e, e->undo_lost ? "No further undo (older changes were dropped)" : "No further undo");
        return;
    }
    uint32_t size = undo_word(e, e->undo_len - 4u)[0];
    uint32_t *r = undo_word(e, e->undo_len - size);
    uint32_t at = r[0], in = r[1], dn = r[2];
    if (!ed_raw(e, at, in, (const char *)(r + 3), dn)) return;
    e->undo_len -= size;
    e->pos = at + dn;
    e->mark = -1;
    say(e, "Undo");
}

/* Type, paste or insert: replaces the selection, if there is one. */
static void ed_insert(ed_t *e, const char *s, uint32_t n, bool merge) {
    uint32_t a, b;
    if (sel_range(e, &a, &b)) {
        merge = false;
    } else {
        a = b = e->pos;
    }
    if (ed_change(e, a, b - a, s, n, merge)) e->pos = a + n;
    e->mark = -1;
}

static void ed_delete(ed_t *e, uint32_t a, uint32_t b) {
    if (a >= b) return;
    if (ed_change(e, a, b - a, NULL, 0, false)) e->pos = a;
    e->mark = -1;
}

static bool ed_copy(ed_t *e) {
    uint32_t a, b;
    if (!sel_range(e, &a, &b)) {
        say(e, "No selection");
        return false;
    }
    if (!clipboard_set(e->buf + a, b - a)) {
        say(e, "The selection is larger than the clipboard (8 KB)");
        return false;
    }
    return true;
}

static void ed_paste(ed_t *e) {
    uint32_t n;
    const char *s = clipboard_data(&n);
    if (!s) {
        say(e, "The clipboard is empty");
        return;
    }
    ed_insert(e, s, n, false);
}

/* --- Files ---------------------------------------------------------------- */

static void ed_title(ed_t *e) {
    char t[80];
    ksnprintf(t, sizeof(t), "e: %s", e->file);
    console_set_title(t);
}

/* The safe save's copy of `path`: the extension's FIRST character becomes
 * '~' (notes.txt -> notes.~xt, lorenz.lisp -> lorenz.~isp; none: notes ->
 * notes.~). The card's FAT32 keeps 8.3 names -- an extension cut to three
 * letters -- so anything changed past the third letter names the file
 * itself: `path~` did for every name, and the extension's last letter did
 * for four-letter ones (lorenz.lis~ is LORENZ.LIS, which is lorenz.lisp;
 * removing "the copy" deleted the file, found on the panel, 37.5b). */
static void copy_name(const char *path, char *o, uint32_t cap) {
    ksnprintf(o, cap, "%s", path);
    uint32_t n = (uint32_t)strlen(o);
    char *dot = NULL;
    for (char *p = o; *p; p++) {
        if (*p == '/') dot = NULL;
        else if (*p == '.') dot = p;
    }
    if (dot && dot[1]) dot[1] = dot[1] == '~' ? '_' : '~';
    else if (n + 2u < cap) ksnprintf(o + n, cap - n, dot ? "~" : ".~");
}

/* Replaces the buffer with the file (an empty one if there is none). */
static bool ed_load(ed_t *e, const char *path) {
    vfs_stat_t st;
    uint32_t size = (vfs_stat(path, &st) == 0 && !st.is_dir) ? st.size : 0;
    uint32_t pages = (size + 4096u + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (pages < 2u) pages = 2u;
    char *nb = (char *)ed_pages(pages);
    if (!nb) {
        say(e, "No memory for the file");
        return false;
    }
    if (e->buf) palloc_free(e->buf, e->cap / PAGE_SIZE);
    e->buf = nb;
    e->cap = pages * PAGE_SIZE;
    int r = size ? vfs_read(path, e->buf, e->cap - 1u) : -1;
    e->len = r > 0 ? (uint32_t)r : 0;
    e->buf[e->len] = '\0';
    ed_lines_reset(e);
    strncpy(e->file, path, sizeof(e->file) - 1u);
    e->file[sizeof(e->file) - 1u] = '\0';
    e->pos = e->top = e->hleft = 0;
    e->mark = e->goal = -1;
    e->modified = false;
    e->undo_len = 0;
    e->undo_lost = false;
    e->text_mode = text_ext(path);
    say(e, r > 0 ? "" : "New file");
    char tmp[136];
    copy_name(path, tmp, sizeof(tmp));
    if (vfs_stat(tmp, &st) == 0) {
        char m[96];
        ksnprintf(m, sizeof(m), "Note: %s holds the copy of an interrupted save", tmp);
        say(e, m);
    }
    ed_title(e);
    return true;
}

/* The safe save (editor.h): the copy first, then the file, then the copy
 * goes. A volume that cannot hold the copy's name still gets the file. */
static bool ed_save(ed_t *e, const char *path) {
    char tmp[136];
    copy_name(path, tmp, sizeof(tmp));
    vfs_stat_t st;
    bool copy = vfs_write(tmp, e->buf, e->len) == 0 && vfs_stat(tmp, &st) == 0 && st.size == e->len;
    if (vfs_write(path, e->buf, e->len) != 0 || vfs_stat(path, &st) != 0 || st.size != e->len) {
        say(e, copy ? "WRITE FAILED -- the text is in the ~ copy, and still here"
                    : "WRITE FAILED -- not saved (card out? read-only?); the text is still here");
        return false;
    }
    if (copy) {
        (void)vfs_remove(tmp);
        /* The last line of defence: if a volume's naming ever makes the
         * copy and the file one entry again, removing the copy removed the
         * file -- so check, and write it once more. */
        if (vfs_stat(path, &st) != 0 || st.size != e->len) {
            if (vfs_write(path, e->buf, e->len) != 0) {
                say(e, "WRITE FAILED -- the file could not be written back; the text is still here");
                return false;
            }
        }
    }
    if (path != e->file) {
        strncpy(e->file, path, sizeof(e->file) - 1u);
        e->file[sizeof(e->file) - 1u] = '\0';
        ed_title(e);
    }
    e->modified = false;
    say(e, "Saved");
    return true;
}

/* --- The screen ----------------------------------------------------------- */

static void out(const char *s) {
    console_puts(s);
}

static void outf_cup(unsigned row, unsigned col) {
    char b[16];
    ksnprintf(b, sizeof(b), "\033[%u;%uH", row, col);
    out(b);
}

static uint32_t fnv(const char *s, uint32_t n) {
    uint32_t h = ED_EMPTY_HASH;
    for (uint32_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static void ed_layout(ed_t *e) {
    unsigned c = 0, r = 0;
    if (!console_size(&c, &r) || c < 20u || r < 4u) {
        c = 80u;
        r = 24u;
    }
    if (c > ED_COLS_MAX) c = ED_COLS_MAX;
    if (r > ED_ROWS_MAX + 1u) r = ED_ROWS_MAX + 1u;
    if (c != e->cols || r != e->rows) {
        e->cols = c;
        e->rows = r;
        e->trows = r - 1u;
        e->drawn = false;
    }
    if (e->text_mode) {
        e->gutter = 0;
    } else {
        uint32_t lines = lines_before(e, e->len) + 1u;
        unsigned d = 3;
        while (lines >= 1000u) { d++; lines /= 10u; }
        e->gutter = d + 1u;
    }
    e->tcols = e->cols - e->gutter;
}

static void ed_center(ed_t *e, uint32_t cr) {
    uint32_t s = cr;
    for (unsigned i = 0; i < e->trows / 2u; i++) {
        uint32_t p = prev_row(e, s);
        if (p == ED_NONE) break;
        s = p;
    }
    e->top = s;
}

/* Moves the view as little as it must to show the cursor: a few rows by
 * scrolling, further by centring it. */
static void ed_follow(ed_t *e) {
    if (e->top > e->len) e->top = e->len;
    e->top = row_start_of(e, e->top);
    uint32_t cr = row_start_of(e, e->pos);
    if (cr < e->top) {
        uint32_t s = cr;
        unsigned k = 0;
        while (s != ED_NONE && s < e->top && k < e->trows) { s = next_row(e, s); k++; }
        if (s == e->top) e->top = cr;
        else ed_center(e, cr);
    } else {
        uint32_t s = e->top;
        unsigned k = 0;
        while (s != cr && s != ED_NONE && k < 2u * e->trows) { s = next_row(e, s); k++; }
        if (s != cr) ed_center(e, cr);
        else if (k >= e->trows)
            for (unsigned i = 0; i <= k - e->trows; i++) e->top = next_row(e, e->top);
    }
    if (e->text_mode) {
        e->hleft = 0;
    } else {
        unsigned col = col_of(e, cr, e->pos);
        if (col < e->hleft || col >= e->hleft + e->tcols)
            e->hleft = col > e->tcols / 2u ? col - e->tcols / 2u : 0;
    }
}

/* One row into e->line; returns its bytes, and the cells it covers. */
static uint32_t render_row(ed_t *e, uint32_t rs, uint32_t lineno, uint32_t sa, uint32_t sb, unsigned *cells) {
    char *o = e->line;
    uint32_t n = 0;
    *cells = 0;
    if (rs == ED_NONE) return 0;
    if (e->gutter) {
        char d[12];
        unsigned dn = 0;
        do { d[dn++] = (char)('0' + lineno % 10u); lineno /= 10u; } while (lineno && dn < sizeof(d));
        for (unsigned i = dn; i < e->gutter - 1u; i++) o[n++] = ' ';
        while (dn) o[n++] = d[--dn];
        memcpy(o + n, "\xe2\x94\x82", 3u);                       /* the bar, U+2502 */
        n += 3u;
        *cells = e->gutter;
    }
    bool eol;
    uint32_t r = row_end(e, rs, &eol);
    unsigned col = 0, shown = 0;
    bool rev = false;
    for (uint32_t q = rs; q < r && shown < e->tcols; ) {
        uint32_t nq = nextc(e, q);
        unsigned w = width_at(e, q, col);
        bool in_sel = q >= sa && q < sb;
        for (unsigned i = 0; i < w && shown < e->tcols; i++, col++) {
            if (col < e->hleft) continue;
            if (in_sel != rev) {
                memcpy(o + n, in_sel ? "\033[7m" : "\033[27m", in_sel ? 4u : 5u);
                n += in_sel ? 4u : 5u;
                rev = in_sel;
            }
            unsigned char c = (unsigned char)e->buf[q];
            if (c == '\t' || (c < 0x20u) || c == 0x7fu) {
                o[n++] = c == '\t' ? ' ' : '?';
            } else {
                for (uint32_t k = q; k < nq; k++) o[n++] = e->buf[k];
            }
            shown++;
        }
        q = nq;
    }
    /* A selected line break shows as one selected cell past the line. */
    if (eol && r < e->len && r >= sa && r < sb && shown < e->tcols && col >= e->hleft) {
        if (!rev) { memcpy(o + n, "\033[7m", 4u); n += 4u; rev = true; }
        o[n++] = ' ';
        shown++;
    }
    if (rev) { memcpy(o + n, "\033[27m", 5u); n += 5u; }
    *cells += shown;
    return n;
}

static uint32_t render_status(ed_t *e) {
    char right[24];
    uint32_t ln = lines_before(e, e->pos) + 1u;
    unsigned col = (unsigned)(e->pos - line_start(e, e->pos));
    for (uint32_t q = line_start(e, e->pos); q < e->pos; q++) if (u8_cont(e->buf[q])) col--;
    ksnprintf(right, sizeof(right), " %s L%u:C%u ", e->text_mode ? "text" : "", (unsigned)ln, col + 1u);
    const char *hint = e->hooks && e->hooks->eval ? "^X^E eval  ^X^S save  ^X^C quit"
                                                   : "^X^E run  ^X^S save  ^X^C quit";
    char left[200];
    ksnprintf(left, sizeof(left), " %s%s  %s", e->file, e->modified ? " *" : "", e->msg[0] ? e->msg : hint);
    unsigned room = e->cols - 1u, rw = 0, lw = 0, need = 0;
    for (const char *p = right; *p; p++) if (!u8_cont(*p)) rw++;
    for (const char *p = left; *p; p++) if (!u8_cont(*p)) need++;
    /* A message -- a question above all -- must be readable in a narrow
     * tile: the file name goes first, then the position. */
    if (e->msg[0] && need + rw > room) {
        ksnprintf(left, sizeof(left), " %s", e->msg);
        need = 0;
        for (const char *p = left; *p; p++) if (!u8_cont(*p)) need++;
        if (need + rw > room) {
            rw = 0;
            right[0] = '\0';
        }
    }
    char *o = e->line;
    uint32_t n = 0;
    memcpy(o, "\033[7m", 4u);
    n = 4;
    for (const char *p = left; *p; p++) {
        if (!u8_cont(*p)) {
            if (lw + rw >= room) break;
            lw++;
        }
        o[n++] = *p;
    }
    while (lw + rw < room) { o[n++] = ' '; lw++; }
    for (const char *p = right; *p && lw < room; p++) {
        if (!u8_cont(*p)) lw++;
        o[n++] = *p;
    }
    memcpy(o + n, "\033[27m\033[K", 8u);
    return n + 8u;
}

/* The best row shift at the first changed row `first`: k > 0, the old rows
 * reappear k lower (insert k lines); k < 0, higher (delete). 0 if no shift
 * saves at least three rows of drawing. Blank rows never count. */
static int best_shift(const uint32_t *old, const uint32_t *nw, unsigned first, unsigned n) {
    unsigned same = 0;
    for (unsigned j = first; j < n; j++) if (nw[j] == old[j] && nw[j] != ED_EMPTY_HASH) same++;
    int best = 0;
    unsigned best_gain = 2;
    for (unsigned k = 1; first + k < n; k++) {
        unsigned down = 0, up = 0;
        for (unsigned j = first; j < n; j++) {
            if (nw[j] == ED_EMPTY_HASH) continue;
            if (j >= first + k && nw[j] == old[j - k]) down++;
            if (j + k < n && nw[j] == old[j + k]) up++;
        }
        if (down > same + best_gain) { best_gain = down - same; best = (int)k; }
        if (up > same + best_gain) { best_gain = up - same; best = -(int)k; }
    }
    return best;
}

static void ed_draw(ed_t *e) {
    ed_layout(e);
    ed_follow(e);
    uint32_t sa = ED_NONE, sb = ED_NONE;
    (void)sel_range(e, &sa, &sb);
    uint32_t *rs = e->rs, *nh = e->nh, *lns = e->lns;
    uint32_t ln = e->gutter ? lines_before(e, e->top) + 1u : 0;
    uint32_t cr = row_start_of(e, e->pos);
    unsigned crow = 0, cells;
    uint32_t s = e->top;
    for (unsigned i = 0; i < e->trows; i++) {
        rs[i] = s;
        lns[i] = ln;
        if (s == cr) crow = i;
        nh[i] = fnv(e->line, render_row(e, s, ln, sa, sb, &cells));
        if (s != ED_NONE) {
            s = next_row(e, s);
            if (s != ED_NONE && s > 0 && e->buf[s - 1u] == '\n') ln++;
        }
    }
    console_lock();
    console_sync();
    out("\033[?25l");
    if (!e->drawn) {
        char b[24];
        ksnprintf(b, sizeof(b), "\033[r\033[2J\033[1;%ur", e->trows);
        out(b);
        for (unsigned i = 0; i < ED_ROWS_MAX; i++) e->hash[i] = ED_EMPTY_HASH;
        e->status_hash = 0;
        e->drawn = true;
    }
    unsigned first = 0;
    while (first < e->trows && nh[first] == e->hash[first]) first++;
    if (first + 1u < e->trows) {
        int k = best_shift(e->hash, nh, first, e->trows);
        if (k) {
            unsigned a = (unsigned)(k > 0 ? k : -k);
            char b[24];
            ksnprintf(b, sizeof(b), "\033[%u;1H\033[%u%c", first + 1u, a, k > 0 ? 'L' : 'M');
            out(b);
            if (k > 0) {
                for (unsigned j = e->trows; j-- > first + a;) e->hash[j] = e->hash[j - a];
                for (unsigned j = first; j < first + a; j++) e->hash[j] = ED_EMPTY_HASH;
            } else {
                for (unsigned j = first; j + a < e->trows; j++) e->hash[j] = e->hash[j + a];
                for (unsigned j = e->trows - a; j < e->trows; j++) e->hash[j] = ED_EMPTY_HASH;
            }
        }
    }
    for (unsigned i = first; i < e->trows; i++) {
        if (nh[i] == e->hash[i]) continue;
        uint32_t n = render_row(e, rs[i], lns[i], sa, sb, &cells);
        outf_cup(i + 1u, 1u);
        e->line[n] = '\0';
        out(e->line);
        if (cells < e->cols) out("\033[K");
        e->hash[i] = nh[i];
    }
    uint32_t n = render_status(e);
    uint32_t h = fnv(e->line, n);
    if (h != e->status_hash) {
        outf_cup(e->rows, 1u);
        e->line[n] = '\0';
        out(e->line);
        e->status_hash = h;
    }
    unsigned ccol = col_of(e, cr, e->pos) - e->hleft + e->gutter;
    outf_cup(crow + 1u, ccol + 1u);
    out("\033[?25h");
    console_unlock();
    console_flush();
}

/* The next key; while none comes, the shell's idle hook (Lisp's canvas
 * redraw) gets its turns, and a resize -- the divider moved -- redraws. */
static key_event_t ed_key(ed_t *e) {
    uint64_t next = time_get_us() + 100000u;
    while (!console_has_char()) {
        if (time_get_us() >= next) {
            char junk[64];                  /* a redraw's prints: not over the text */
            console_capture(junk, sizeof(junk));
            readline_idle();
            (void)console_capture_end();
            unsigned c, r;
            if (console_size(&c, &r) && (c != e->cols || r != e->rows)) ed_draw(e);
            next = time_get_us() + 100000u;
        }
        sched_yield();
    }
    return keyseq_read_console();
}

/* A question or a prompt needs the text tile: with the canvas full screen
 * (Super+] or Super+\\), it comes back full screen -- the same toggle. */
static void ed_show_text(void) {
    uint8_t req[1] = { 'S' }, reply[SCREEN_REPLY_LEN];
    if (console_canvas(req, 1, reply) && reply[8] == SCREEN_LAYOUT_CANVAS) {
        req[0] = 'X';
        (void)console_canvas(req, 1, reply);
    }
}

/* A question on the status line, answered by one key. */
static uint32_t ed_ask(ed_t *e, const char *q) {
    ed_show_text();
    say(e, q);
    ed_draw(e);
    key_event_t k = ed_key(e);
    e->msg[0] = '\0';
    uint32_t c = k.key;
    return (c >= 'A' && c <= 'Z') ? c + 32u : c;
}

/* A line typed on the status row, with the shared line editor. -1: cancelled. */
static int ed_prompt(ed_t *e, const char *prompt, char *buf, int max) {
    static const readline_opts_t opts = { true, true, true };
    ed_show_text();
    ed_draw(e);
    console_lock();
    outf_cup(e->rows, 1u);
    out("\033[K");
    console_unlock();
    int n = readline_ex(prompt, buf, max, &opts);
    e->status_hash = 0;
    return n;
}

static bool ed_confirm_discard(ed_t *e) {
    return !e->modified || ed_ask(e, "Discard changes? (y/n)") == 'y';
}

/* --- Search and replace ---------------------------------------------------- */

static char fold(char c, bool f) {
    return (f && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Smart case: a query with no capital letter matches either case. */
static bool find_at(const ed_t *e, const char *q, uint32_t qn, uint32_t p, bool f) {
    for (uint32_t i = 0; i < qn; i++)
        if (fold(e->buf[p + i], f) != q[i]) return false;
    return true;
}

static bool ed_find(const ed_t *e, const char *query, uint32_t from, bool fwd, uint32_t *at) {
    char q[64];
    uint32_t qn = 0;
    bool f = true;
    for (const char *s = query; *s && qn < sizeof(q); s++) if (*s >= 'A' && *s <= 'Z') f = false;
    for (const char *s = query; *s && qn < sizeof(q); s++) q[qn++] = fold(*s, f);
    if (qn == 0 || qn > e->len) return false;
    if (fwd) {
        for (uint32_t p = from; p + qn <= e->len; p++)
            if (find_at(e, q, qn, p, f)) { *at = p; return true; }
    } else {
        uint32_t p = from + qn > e->len ? e->len - qn : from;
        for (;;) {
            if (find_at(e, q, qn, p, f)) { *at = p; return true; }
            if (p == 0) break;
            p--;
        }
    }
    return false;
}

/* Next (or previous) match from the cursor, wrapping once around the ends. */
static bool ed_find_wrap(ed_t *e, const char *q, uint32_t from, bool fwd, uint32_t *at) {
    if (ed_find(e, q, from, fwd, at)) return true;
    if (fwd ? ed_find(e, q, 0, true, at) : (e->len && ed_find(e, q, e->len, false, at))) {
        say(e, "Wrapped");
        return true;
    }
    return false;
}

static void ed_show_match(ed_t *e, uint32_t at, uint32_t n, bool fwd) {
    e->mark = (int32_t)(fwd ? at : at + n);
    e->mark_sticky = false;
    e->pos = fwd ? at + n : at;
}

static uint32_t utf8_put(uint32_t cp, char *o) {
    if (cp < 0x80u) { o[0] = (char)cp; return 1; }
    if (cp < 0x800u) { o[0] = (char)(0xc0u | cp >> 6); o[1] = (char)(0x80u | (cp & 0x3fu)); return 2; }
    if (cp < 0x10000u) {
        o[0] = (char)(0xe0u | cp >> 12); o[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        o[2] = (char)(0x80u | (cp & 0x3fu));
        return 3;
    }
    o[0] = (char)(0xf0u | cp >> 18); o[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
    o[2] = (char)(0x80u | ((cp >> 6) & 0x3fu)); o[3] = (char)(0x80u | (cp & 0x3fu));
    return 4;
}

static bool is_text_key(key_event_t k) {
    return k.key >= 0x20u && k.key != 0x7fu && k.key < 0x110000u &&
           !(k.mods & (KMOD_CTRL | KMOD_ALT | KMOD_SUPER));
}

static bool is_super(key_event_t k, char c) {
    uint32_t key = (k.key >= 'A' && k.key <= 'Z') ? k.key + 32u : k.key;
    return (k.mods & KMOD_SUPER) && key == (uint32_t)c;
}

static bool is_shifted(key_event_t k) {
    return (k.mods & KMOD_SHIFT) || (k.key >= 'A' && k.key <= 'Z');
}

/* Incremental search. Returns a key that ended it and still has to be
 * done (an arrow, say), or KEY_UNKNOWN for none. */
static key_event_t ed_isearch(ed_t *e, bool fwd) {
    uint32_t origin = e->pos;
    int32_t omark = e->mark;
    bool osticky = e->mark_sticky;
    char q[64];
    uint32_t qn = 0, ms = e->pos;
    bool found = true;
    q[0] = '\0';
    key_event_t none = { KEY_UNKNOWN, 0 };
    for (;;) {
        char m[96];
        ksnprintf(m, sizeof(m), "%sI-search%s: %s", found ? "" : "Failing ", fwd ? "" : " backward", q);
        say(e, m);
        ed_draw(e);
        key_event_t k = ed_key(e);
        e->msg[0] = '\0';
        bool next = k.key == 0x13u || (is_super(k, 'g') && !is_shifted(k)) || is_super(k, 'f');
        bool prev = k.key == 0x12u || (is_super(k, 'g') && is_shifted(k));
        if (is_text_key(k)) {
            char enc[4];
            uint32_t n = utf8_put(k.key, enc);
            if (qn + n < sizeof(q)) {
                memcpy(q + qn, enc, n);
                qn += n;
                q[qn] = '\0';
            }
            uint32_t at;
            found = ed_find(e, q, ms, fwd, &at);
            if (found) { ms = at; ed_show_match(e, at, qn, fwd); }
        } else if (k.key == 0x7fu || k.key == 0x08u) {
            if (qn) {
                qn--;
                while (qn && u8_cont(q[qn])) qn--;
                q[qn] = '\0';
            }
            uint32_t at;
            found = qn == 0 || ed_find(e, q, origin, fwd, &at);
            if (qn == 0) { e->pos = origin; e->mark = -1; ms = origin; }
            else if (found) { ms = at; ed_show_match(e, at, qn, fwd); }
        } else if (next || prev) {
            if (qn == 0 && e->find[0]) {
                strncpy(q, e->find, sizeof(q) - 1u);
                q[sizeof(q) - 1u] = '\0';
                qn = (uint32_t)strlen(q);
            }
            fwd = next;
            uint32_t at;
            uint32_t from = fwd ? (qn ? ms + 1u : e->pos) : (ms ? ms - 1u : 0);
            found = qn && ed_find_wrap(e, q, from, fwd, &at);
            if (found) { ms = at; ed_show_match(e, at, qn, fwd); }
        } else if (k.key == 0x07u || k.key == 0x1bu) {
            e->pos = origin;
            e->mark = omark;
            e->mark_sticky = osticky;
            say(e, "Search cancelled");
            return none;
        } else {
            if (qn) memcpy(e->find, q, qn + 1u);
            e->mark = -1;
            if (k.key == 0x0du || k.key == 0x0au) return none;
            return k;
        }
    }
}

/* Super+G / Super+Shift+G: the last search again, without the prompt. */
static void ed_find_again(ed_t *e, bool fwd) {
    if (!e->find[0]) {
        say(e, "No search yet");
        return;
    }
    uint32_t a, b, at, n = (uint32_t)strlen(e->find);
    uint32_t from = fwd ? e->pos : (sel_range(e, &a, &b) ? (a ? a - 1u : 0) : e->pos);
    if (ed_find_wrap(e, e->find, from, fwd, &at)) ed_show_match(e, at, n, fwd);
    else say(e, "Not found");
}

static void ed_replace(ed_t *e) {
    char from[64], to[64];
    if (ed_prompt(e, "Replace: ", from, sizeof(from)) <= 0) return;
    if (ed_prompt(e, "Replace with: ", to, sizeof(to)) < 0) return;
    memcpy(e->find, from, sizeof(from));
    uint32_t fn = (uint32_t)strlen(from), tn = (uint32_t)strlen(to), p = e->pos, at;
    unsigned count = 0;
    bool all = false;
    while (ed_find(e, from, p, true, &at)) {
        bool last = false;
        if (!all) {
            ed_show_match(e, at, fn, true);
            uint32_t c = ed_ask(e, "Replace? y n ! . q");
            if (c == 'n' || c == 0x7fu) { p = at + fn; continue; }
            if (c == '!') all = true;
            else if (c == '.') last = true;
            else if (c != 'y' && c != ' ') break;
        }
        if (!ed_change(e, at, fn, to, tn, false)) break;
        count++;
        p = at + tn;
        e->pos = p;
        if (last) break;
    }
    e->mark = -1;
    char m[48];
    ksnprintf(m, sizeof(m), "Replaced %u", count);
    say(e, m);
}

/* --- Commands ---------------------------------------------------------------- */

/* Moves the cursor; Shift extends (or starts) the selection, a plain
 * movement ends a selection that Shift made. */
static void ed_move(ed_t *e, uint32_t to, bool shift) {
    if (shift) {
        if (e->mark < 0) {
            e->mark = (int32_t)e->pos;
            e->mark_sticky = false;
        }
    } else if (e->mark >= 0 && !e->mark_sticky) {
        e->mark = -1;
    }
    e->pos = to;
}

static uint32_t ed_vertical(ed_t *e, int rows) {
    uint32_t rs = row_start_of(e, e->pos);
    if (e->goal < 0) e->goal = (int32_t)col_of(e, rs, e->pos);
    int moved = 0;
    while (rows != 0) {
        uint32_t n = rows > 0 ? next_row(e, rs) : prev_row(e, rs);
        if (n == ED_NONE) break;
        rs = n;
        if (rows > 0) rows--; else rows++;
        moved++;
    }
    if (moved == 0) return rows > 0 ? e->len : 0;
    return pos_at(e, rs, (unsigned)e->goal);
}

static void ed_page(ed_t *e, bool down) {
    int n = e->trows > 2u ? (int)e->trows - 2 : 1;
    for (int i = 0; i < n; i++) {
        uint32_t t = down ? next_row(e, e->top) : prev_row(e, e->top);
        if (t == ED_NONE) break;
        e->top = t;
    }
}

static void ed_goto_line(ed_t *e) {
    char b[16];
    if (ed_prompt(e, "Go to line: ", b, sizeof(b)) <= 0) return;
    uint32_t n = 0;
    for (const char *p = b; *p >= '0' && *p <= '9'; p++) n = n * 10u + (uint32_t)(*p - '0');
    uint32_t p = 0;
    for (uint32_t l = 1; l < n && p < e->len; p++) if (e->buf[p] == '\n') l++;
    e->mark = -1;
    e->pos = p;
    ed_center(e, row_start_of(e, p));
}

static void ed_insert_file(ed_t *e) {
    char fn[128];
    if (ed_prompt(e, "Insert file: ", fn, sizeof(fn)) <= 0) return;
    vfs_stat_t st;
    if (vfs_stat(fn, &st) != 0 || st.is_dir) {
        say(e, "File not found");
        return;
    }
    uint32_t pages = (st.size + 1u + PAGE_SIZE - 1u) / PAGE_SIZE;
    char *t = (char *)ed_pages(pages);
    if (!t) {
        say(e, "Out of memory");
        return;
    }
    int r = vfs_read(fn, t, pages * PAGE_SIZE - 1u);
    if (r > 0) {
        ed_insert(e, t, (uint32_t)r, false);
        say(e, "Inserted");
    } else {
        say(e, "File not found");
    }
    palloc_free(t, pages);
}

static void ed_eval(ed_t *e) {
    uint32_t a, b;
    bool sel = sel_range(e, &a, &b);
    if (!sel) { a = 0; b = e->len; }
    char keep = e->buf[b];
    e->buf[b] = '\0';
    e->msg[0] = '\0';
    e->hooks->eval(e->buf + a, e->msg, sizeof(e->msg));
    e->buf[b] = keep;
    e->drawn = false;                   /* anything it printed is on the screen */
}

enum { ED_GO_ON, ED_QUIT, ED_RUN };

static int ed_ctrl_x(ed_t *e) {
    say(e, "C-x-");
    ed_draw(e);
    key_event_t k = ed_key(e);
    e->msg[0] = '\0';
    uint32_t c = k.key;
    if (c >= 'A' && c <= 'Z') c += 32u;
    switch (c) {
    case 0x13u: case 's': (void)ed_save(e, e->file); break;
    case 0x17u: case 'w': {
        char fn[128];
        if (ed_prompt(e, "Write file: ", fn, sizeof(fn)) > 0) (void)ed_save(e, fn);
        break;
    }
    case 0x06u: case 'f': {
        char fn[128];
        if (ed_confirm_discard(e) && ed_prompt(e, "Find file: ", fn, sizeof(fn)) > 0) {
            (void)ed_load(e, fn);
            e->drawn = false;
        }
        break;
    }
    case 0x12u: case 'r': case 'i': ed_insert_file(e); break;
    case 0x05u: case 'e':
        if (!(e->hooks && e->hooks->eval)) return ED_RUN;
        ed_eval(e);
        break;
    case 0x03u: case 0x11u: case 'c': case 'q':
        if (ed_confirm_discard(e)) return ED_QUIT;
        break;
    case 0x14u: case 't':
        e->text_mode = !e->text_mode;
        e->drawn = false;
        say(e, e->text_mode ? "Text mode: soft wrap" : "Code mode");
        break;
    case 'h': e->mark = 0; e->mark_sticky = true; e->pos = e->len; break;
    case 'u': ed_undo(e); break;
    default: break;
    }
    return ED_GO_ON;
}

/* One key. Returns ED_GO_ON, or how the editor ends. */
static int ed_key_cmd(ed_t *e, key_event_t k) {
    uint32_t key = k.key;
    bool shift = (k.mods & KMOD_SHIFT) != 0, alt = (k.mods & KMOD_ALT) != 0;
    bool ctrl = (k.mods & KMOD_CTRL) != 0, sup = (k.mods & KMOD_SUPER) != 0;
    bool was_kill = e->kill_append, was_typing = e->typing, vertical = false;
    e->kill_append = e->typing = false;
    e->msg[0] = '\0';
    uint32_t a, b;

    if (is_text_key(k)) {
        char enc[4];
        uint32_t n = utf8_put(key, enc);
        bool space = key == ' ';
        bool merge = was_typing && !(e->last_typed == ' ' && !space);
        ed_insert(e, enc, n, merge);
        e->typing = true;
        e->last_typed = space ? ' ' : 'x';
        e->goal = -1;
        return ED_GO_ON;
    }

    if (sup && key < 0x80u && key != 0x0du) {
        if (is_super(k, 'c')) (void)ed_copy(e);
        else if (is_super(k, 'x')) { if (ed_copy(e) && sel_range(e, &a, &b)) ed_delete(e, a, b); }
        else if (is_super(k, 'v')) ed_paste(e);
        else if (is_super(k, 'a')) { e->mark = 0; e->mark_sticky = true; e->pos = e->len; }
        else if (is_super(k, 'z')) ed_undo(e);
        else if (is_super(k, 'f') && alt) ed_replace(e);
        else if (is_super(k, 'r')) ed_replace(e);
        else if (is_super(k, 'f')) {
            key_event_t r = ed_isearch(e, true);
            if (r.key != KEY_UNKNOWN) return ed_key_cmd(e, r);
        }
        else if (is_super(k, 'g')) ed_find_again(e, !is_shifted(k));
        else if (is_super(k, 's')) {
            if (is_shifted(k)) {
                char fn[128];
                if (ed_prompt(e, "Write file: ", fn, sizeof(fn)) > 0) (void)ed_save(e, fn);
            } else {
                (void)ed_save(e, e->file);
            }
        }
        else if (is_super(k, 'o')) {
            char fn[128];
            if (ed_confirm_discard(e) && ed_prompt(e, "Find file: ", fn, sizeof(fn)) > 0) {
                (void)ed_load(e, fn);
                e->drawn = false;
            }
        }
        else if (is_super(k, 'q')) { if (ed_confirm_discard(e)) return ED_QUIT; }
        else if (is_super(k, 'l')) ed_goto_line(e);
        else if (is_super(k, 'e')) {
            if (!(e->hooks && e->hooks->eval)) return ED_RUN;
            ed_eval(e);
        }
        e->goal = -1;
        return ED_GO_ON;
    }

    switch (key) {
    case KEY_LEFT:
        if (sup) ed_move(e, e->text_mode ? row_start_of(e, e->pos) : line_start(e, e->pos), shift);
        else if (ctrl || alt) ed_move(e, word_left(e, e->pos), shift);
        else if (!shift && sel_range(e, &a, &b)) ed_move(e, a, false);
        else ed_move(e, prevc(e, e->pos), shift);
        break;
    case KEY_RIGHT:
        if (sup) goto end_of_row;
        else if (ctrl || alt) ed_move(e, word_right(e, e->pos), shift);
        else if (!shift && sel_range(e, &a, &b)) ed_move(e, b, false);
        else ed_move(e, nextc(e, e->pos), shift);
        break;
    case KEY_UP:
        if (sup || ctrl) { ed_move(e, 0, shift); break; }
        ed_move(e, ed_vertical(e, -1), shift);
        vertical = true;
        break;
    case KEY_DOWN:
        if (sup || ctrl) { ed_move(e, e->len, shift); break; }
        ed_move(e, ed_vertical(e, 1), shift);
        vertical = true;
        break;
    case KEY_PGUP:
    case KEY_PGDN: {
        bool down = key == KEY_PGDN;
        int n = e->trows > 2u ? (int)e->trows - 2 : 1;
        uint32_t to = ed_vertical(e, down ? n : -n);
        ed_page(e, down);
        ed_move(e, to, shift);
        vertical = true;
        break;
    }
    case KEY_HOME:
        if (ctrl) ed_move(e, 0, shift);
        else ed_move(e, e->text_mode ? row_start_of(e, e->pos) : line_start(e, e->pos), shift);
        break;
    case KEY_END:
        if (ctrl) { ed_move(e, e->len, shift); break; }
    end_of_row:
        if (e->text_mode) {
            bool eol;
            uint32_t r = row_end(e, row_start_of(e, e->pos), &eol);
            ed_move(e, eol ? r : prevc(e, r), shift);
        } else {
            ed_move(e, line_end(e, e->pos), shift);
        }
        break;
    case KEY_DELETE:
        if (sel_range(e, &a, &b)) ed_delete(e, a, b);
        else ed_delete(e, e->pos, nextc(e, e->pos));
        break;
    default:
        if (alt) {
            switch (key) {
            case 'b': case 'B': ed_move(e, word_left(e, e->pos), shift); break;
            case 'f': case 'F': ed_move(e, word_right(e, e->pos), shift); break;
            case 'd': case 'D': ed_delete(e, e->pos, word_right(e, e->pos)); break;
            case 0x7fu: case 0x08u: ed_delete(e, word_left(e, e->pos), e->pos); break;
            case 'w': case 'W': (void)ed_copy(e); break;
            case 'v': case 'V': {
                int n = e->trows > 2u ? (int)e->trows - 2 : 1;
                uint32_t to = ed_vertical(e, -n);
                ed_page(e, false);
                ed_move(e, to, false);
                vertical = true;
                break;
            }
            case '<': ed_move(e, 0, shift); break;
            case '>': ed_move(e, e->len, shift); break;
            case '%': ed_replace(e); break;
            case 'g': case 'G': ed_goto_line(e); break;
            default: break;
            }
            break;
        }
        switch (key) {
        case 0x00u: e->mark = (int32_t)e->pos; e->mark_sticky = true; say(e, "Mark set"); break;
        case 0x01u: ed_move(e, e->text_mode ? row_start_of(e, e->pos) : line_start(e, e->pos), false); break;
        case 0x02u: ed_move(e, prevc(e, e->pos), false); break;
        case 0x04u: ed_delete(e, e->pos, nextc(e, e->pos)); break;
        case 0x05u: goto end_of_row;
        case 0x06u: ed_move(e, nextc(e, e->pos), false); break;
        case 0x07u: case 0x1bu: e->mark = -1; break;
        case 0x08u: case 0x7fu:
            if (sel_range(e, &a, &b)) ed_delete(e, a, b);
            else ed_delete(e, prevc(e, e->pos), e->pos);
            break;
        case 0x09u: ed_insert(e, "  ", 2u, false); break;
        case 0x0bu: {                                           /* Ctrl-K */
            a = e->pos;
            b = line_end(e, a);
            if (a == b && b < e->len) b++;
            if (a == b) break;
            uint32_t have;
            bool ok = was_kill && clipboard_data(&have)
                    ? clipboard_write_at(e->buf + a, b - a, have) >= 0
                    : clipboard_set(e->buf + a, b - a);
            if (!ok) say(e, "Killed, but too large for the clipboard (8 KB)");
            e->mark = -1;
            ed_delete(e, a, b);
            e->kill_append = true;
            break;
        }
        case 0x0cu: e->drawn = false; ed_center(e, row_start_of(e, e->pos)); break;
        case 0x0du: case 0x0au: {
            if (sup) {
                if (!(e->hooks && e->hooks->eval)) return ED_RUN;
                ed_eval(e);
                break;
            }
            char ind[66];
            uint32_t n = 1;
            ind[0] = '\n';
            if (!e->text_mode) {
                for (uint32_t p = line_start(e, e->pos); p < e->pos && n < sizeof(ind) && e->buf[p] == ' '; p++)
                    ind[n++] = ' ';
            }
            ed_insert(e, ind, n, false);
            break;
        }
        case 0x0eu: ed_move(e, ed_vertical(e, 1), false); vertical = true; break;
        case 0x10u: ed_move(e, ed_vertical(e, -1), false); vertical = true; break;
        case 0x12u: case 0x13u: {
            key_event_t r = ed_isearch(e, key == 0x13u);
            if (r.key != KEY_UNKNOWN) return ed_key_cmd(e, r);
            break;
        }
        case 0x16u: {                                           /* Ctrl-V */
            int n = e->trows > 2u ? (int)e->trows - 2 : 1;
            uint32_t to = ed_vertical(e, n);
            ed_page(e, true);
            ed_move(e, to, false);
            vertical = true;
            break;
        }
        case 0x17u:                                             /* Ctrl-W */
            if (ed_copy(e) && sel_range(e, &a, &b)) ed_delete(e, a, b);
            break;
        case 0x18u: {
            int r = ed_ctrl_x(e);
            if (r != ED_GO_ON) return r;
            break;
        }
        case 0x19u: ed_paste(e); break;
        case 0x1fu: ed_undo(e); break;
        default: break;
        }
    }
    if (!vertical) e->goal = -1;
    return ED_GO_ON;
}

/* --- Running it ---------------------------------------------------------------- */

static ed_t *ed_new(void) {
    ed_t *e = (ed_t *)palloc_pages(ED_STATE_PAGES);
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->mark = e->goal = -1;
    return e;
}

static void ed_free(ed_t *e) {
    if (e->undo) palloc_free(e->undo, ED_UNDO_PAGES);
    if (e->buf) palloc_free(e->buf, e->cap / PAGE_SIZE);
    palloc_free(e, ED_STATE_PAGES);
}

/* While the editor runs, the kernel log keeps to its ring: a log line
 * printed into the editor's tile (the first screenshot's "directory
 * created", found on the panel) is text the editor did not draw. Returns
 * which sinks were attached, to give back exactly those. */
static uint32_t log_quiet(void) {
    uint32_t mask = 0;
    const char *name;
    bool on;
    for (uint32_t i = 0; i < 32u && klog_sink_info(i, &name, &on); i++) {
        if (on && klog_sink_detach(name) == 0) mask |= 1u << i;
    }
    return mask;
}

static void log_restore(uint32_t mask) {
    const char *name;
    bool on;
    for (uint32_t i = 0; i < 32u && klog_sink_info(i, &name, &on); i++)
        if (mask & (1u << i)) (void)klog_sink_attach(name);
}

int editor_run(const char *filename, const editor_hooks_t *hooks, char *outbuf, int out_max) {
    ed_t *e = ed_new();
    if (!e) {
        cprintf("e: no memory\n");
        return -1;
    }
    e->hooks = hooks;
    if (!ed_load(e, filename && filename[0] ? filename : "/ram0/system/scratch.lisp")) {
        cprintf("e: no memory for %s\n", filename);
        ed_free(e);
        return -1;
    }
    if (!hooks) e->pos = e->len;        /* the shell's Ctrl-X box: go on typing */
    int result = -1;
    klog_drain();                       /* what is already logged is shown first */
    uint32_t sinks = log_quiet();
    for (;;) {
        ed_draw(e);
        int r = ed_key_cmd(e, ed_key(e));
        if (r == ED_QUIT) break;
        if (r == ED_RUN) {
            uint32_t n = e->len < (uint32_t)out_max ? e->len : (uint32_t)out_max - 1u;
            memcpy(outbuf, e->buf, n);
            outbuf[n] = '\0';
            result = (int)n;
            break;
        }
    }
    console_lock();
    out("\033[r\033[2J\033[H");
    console_unlock();
    console_flush();
    log_restore(sinks);
    ed_free(e);
    return result;
}

/* The shell's Ctrl-X Ctrl-E box (kernel/line_editor.c): the editor without
 * an evaluator, so Ctrl-X Ctrl-E hands the text back to be run as a line. */
int edit_multiline_box(const char *initial_filename, char *out_buf, int max_len) {
    int n = editor_run(initial_filename, NULL, out_buf, max_len);
    if (n < 0) {
        out_buf[0] = '\0';
        return 0;
    }
    return n;
}

/* --- editselftest ------------------------------------------------------------- */

static void st_check(int *fail, const char *name, bool ok) {
    if (!ok) (*fail)++;
    cprintf("  [%s] %s\n", ok ? "ok" : "FAIL", name);
}

static void st_set(ed_t *e, const char *s, bool text, unsigned tcols) {
    e->len = 0;
    e->buf[0] = '\0';
    ed_lines_reset(e);
    (void)ed_raw(e, 0, 0, s, (uint32_t)strlen(s));
    e->pos = e->top = 0;
    e->mark = e->goal = -1;
    e->text_mode = text;
    e->tcols = tcols;
    e->trows = 10;
    e->modified = false;
    e->undo_len = 0;
}

static bool st_rows(ed_t *e, const char *const *want, unsigned n) {
    uint32_t s = 0;
    for (unsigned i = 0; i < n; i++) {
        if (s == ED_NONE) return false;
        bool eol;
        uint32_t r = row_end(e, s, &eol);
        uint32_t wl = (uint32_t)strlen(want[i]);
        if (r - s != wl || memcmp(e->buf + s, want[i], wl) != 0) {
            cprintf("    row %u: '%.*s' want '%s'\n", i, (int)(r - s), e->buf + s, want[i]);
            return false;
        }
        s = next_row(e, s);
    }
    return s == ED_NONE;
}

int editor_selftest(void) {
    int fails = 0;
    ed_t *e = ed_new();
    if (!e) {
        cprintf("EDITOR_SELFTEST_FAIL (no memory)\n");
        return 1;
    }
    e->buf = (char *)palloc_pages(2);
    if (!e->buf) {
        ed_free(e);
        cprintf("EDITOR_SELFTEST_FAIL (no memory)\n");
        return 1;
    }
    e->cap = 2u * PAGE_SIZE;
    cprintf("editor selftest:\n");

    /* Text mode, 11 columns: rows of at most 10 cells, cut after spaces. */
    st_set(e, "the quick brown fox jumps\nover", true, 11);
    static const char *const w1[] = { "the quick ", "brown fox ", "jumps", "over" };
    st_check(&fails, "wrap: rows cut after the last space that fits", st_rows(e, w1, 4));

    st_set(e, "abcdefghijklmnopqrstuvw x", true, 11);
    static const char *const w2[] = { "abcdefghij", "klmnopqrst", "uvw x" };
    st_check(&fails, "wrap: a word longer than the row is cut mid-word", st_rows(e, w2, 3));

    st_set(e, "0123456789 abc", true, 11);
    static const char *const w3[] = { "0123456789 ", "abc" };
    st_check(&fails, "wrap: a space that overflows stays at the row's end", st_rows(e, w3, 2));

    st_set(e, "ab\n\ncd\n", true, 11);
    static const char *const w4[] = { "ab", "", "cd", "" };
    st_check(&fails, "rows: an empty line, and the empty row after a final newline", st_rows(e, w4, 4));

    /* Cursor movement over wrapped rows keeps its column. */
    st_set(e, "the quick brown fox jumps", true, 11);
    e->pos = 5;                                         /* the 'u' of quick */
    uint32_t d1 = ed_vertical(e, 1), d2;
    e->pos = d1;
    d2 = ed_vertical(e, 1);
    e->pos = d2;
    uint32_t u1 = ed_vertical(e, -1);
    st_check(&fails, "down, down, up over wrapped rows: column 5 kept, clamped to a short row",
             d1 == 15 && d2 == 25 && u1 == 15 && row_start_of(e, 25) == 20 && row_start_of(e, 10) == 10);

    /* UTF-8: a character is one step and one cell. */
    st_set(e, "a\xc3\xa4\xc3\xb6z", false, 40);
    e->pos = 1;
    uint32_t n1 = nextc(e, 1), n2 = nextc(e, n1);
    st_check(&fails, "UTF-8: steps over whole characters, one column each",
             n1 == 3 && n2 == 5 && prevc(e, 5) == 3 && col_of(e, 0, 5) == 3 && pos_at(e, 0, 2) == 3);

    /* Undo: typing coalesces by word, a deletion comes back. */
    st_set(e, "", false, 40);
    const char *typed = "ab cd";
    bool merge = false;
    char last = 0;
    for (const char *p = typed; *p; p++) {
        ed_insert(e, p, 1, merge && !(last == ' ' && *p != ' '));
        merge = true;
        last = *p;
    }
    ed_delete(e, 0, 2);
    bool u_del = e->len == 3 && memcmp(e->buf, " cd", 3) == 0;
    ed_undo(e);
    bool u1ok = e->len == 5 && memcmp(e->buf, "ab cd", 5) == 0;
    ed_undo(e);
    bool u2ok = e->len == 3 && memcmp(e->buf, "ab ", 3) == 0;
    ed_undo(e);
    bool u3ok = e->len == 0;
    st_check(&fails, "undo: a deletion, then typing a word at a time", u_del && u1ok && u2ok && u3ok);

    /* Undo overflow drops the oldest, never the newest. */
    st_set(e, "", false, 40);
    char big[1000];
    memset(big, 'x', sizeof(big));
    for (int i = 0; i < 12; i++) {
        ed_insert(e, big, sizeof(big), false);
        e->pos = e->len;
        ed_delete(e, 0, sizeof(big));                   /* a 1000-byte record each */
    }
    ed_undo(e);
    st_check(&fails, "undo: a full history drops its oldest records", e->len == 1000 && e->undo_lost);

    /* Search: smart case, forward, backward, wrapped. */
    st_set(e, "Alpha beta ALPHA alpha", false, 40);
    uint32_t at = 0;
    bool s1 = ed_find(e, "alpha", 1, true, &at) && at == 11;
    bool s2 = ed_find(e, "Alpha", 1, true, &at) == false;
    bool s3 = ed_find(e, "beta", 22, false, &at) && at == 6;
    bool s4 = ed_find_wrap(e, "Alpha", 3, true, &at) && at == 0;
    st_check(&fails, "search: any case for a lower-case query, exact otherwise; backward; wrapped", s1 && s2 && s3 && s4);

    /* The safe save, on /ram0 -- or the card, on a persona without one. */
    const char *vol = vfs_write("/ram0/editst.tmp", "x", 1) == 0 ? "/ram0" : "/sd0";
    char path[40], path2[40], copy[40];
    ksnprintf(path, sizeof(path), "%s/editst.txt", vol);
    ksnprintf(path2, sizeof(path2), "%s/editst.lisp", vol);
    if (vol[1] == 'r') (void)vfs_remove("/ram0/editst.tmp");
    st_set(e, "saved text\n", true, 40);
    bool sv = ed_save(e, path);
    char back[32];
    int rb = vfs_read(path, back, sizeof(back));
    vfs_stat_t st;
    ksnprintf(copy, sizeof(copy), "%s/editst.~xt", vol);
    bool tmp_gone = vfs_stat(copy, &st) != 0;
    st_check(&fails, "save: the file written, its safety copy removed, the buffer clean",
             sv && rb == 11 && memcmp(back, "saved text\n", 11) == 0 && tmp_gone && !e->modified);
    (void)vfs_remove(path);
    st_set(e, "(define x 1)\n", false, 40);
    bool sv2 = ed_save(e, path2);
    rb = vfs_read(path2, back, sizeof(back));
    char cn[40];
    copy_name("/sd0/lorenz.lisp", cn, sizeof(cn));
    ksnprintf(copy, sizeof(copy), "%s/editst.~isp", vol);
    bool copy_gone = vfs_stat(copy, &st) != 0;
    bool ok4 = sv2 && rb == 13 && memcmp(back, "(define x 1)\n", 13) == 0 && copy_gone &&
               strcmp(cn, "/sd0/lorenz.~isp") == 0;
    if (!ok4) cprintf("    saved %d, read %d, copy gone %d, copy name %s (%s)\n", sv2, rb, copy_gone, cn, e->msg);
    st_check(&fails, "save: a four-letter extension survives its copy (lorenz.lisp -> lorenz.~isp)", ok4);
    (void)vfs_remove(path2);
    e->modified = true;
    bool fail = !ed_save(e, "/nonexistent-volume/x.txt");
    st_check(&fails, "save: a failed write says so and keeps the buffer modified",
             fail && e->modified && e->len == 13 && strncmp(e->msg, "WRITE FAILED", 12) == 0);

    ed_free(e);
    /* 38.7: the kept line counts against a count from scratch, asked about
     * in an order that walks the remembered spot forward, back, and through
     * changes before it, after it and across it. */
    st_set(e, "a\nb\nc\nd\ne\nf\ng\nh\n", false, 40);
    bool lc = true;
    static const uint32_t probe[] = { 9, 3, 16, 0, 12, 5 };
    for (unsigned k = 0; k < 4u; k++) {
        for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
            uint32_t q = probe[i] < e->len ? probe[i] : e->len;
            if (lines_before(e, q) != count_nl(e->buf, q)) lc = false;
        }
        if (lines_before(e, e->len) != count_nl(e->buf, e->len)) lc = false;
        (void)lines_before(e, 8);
        if (k == 0) (void)ed_raw(e, 2, 0, "x\ny\n", 4);       /* before the spot */
        if (k == 1) (void)ed_raw(e, 12, 0, "\n\n", 2);        /* after it */
        if (k == 2) (void)ed_raw(e, 4, 8, "z", 1);            /* across it */
    }
    st_check(&fails, "line counts: kept through changes before, after and across the remembered spot", lc);

    cprintf(fails ? "EDITOR_SELFTEST_FAIL\n" : "EDITOR_SELFTEST_OK\n");
    return fails;
}

/* `edbench [kb] [dir]` (38.7, plan/phase38_psram.md): what a document of
 * `kb` costs the editor where its buffer lives -- the save and load (the
 * file I/O itself, as ed_save()/ed_load() do it), a keystroke at the start
 * (one memmove of the whole text), a search that runs to the end, and the
 * scans a frame of a code file makes (ed_layout()'s line count of the whole
 * buffer, the gutter's and the status line's up to the cursor), with the
 * cursor at the end. Prints EDBENCH lines; returns 0 or -1. */
int editor_bench(unsigned kb, const char *dir) {
    if (kb == 0) kb = 200u;
    uint32_t want = (uint32_t)kb * 1024u;
    uint32_t pages = (want + 4096u + PAGE_SIZE - 1u) / PAGE_SIZE;
    ed_t *e = ed_new();
    if (!e) { cprintf("edbench: no memory\n"); return -1; }
    e->buf = (char *)ed_pages(pages);
    if (!e->buf) { ed_free(e); cprintf("edbench: no memory for %u KB\n", kb); return -1; }
    e->cap = pages * PAGE_SIZE;
    while (e->len + 64u < want) {
        e->len += (uint32_t)ksnprintf(e->buf + e->len, e->cap - e->len,
                                      "line %06u: the quick brown fox jumps over it\n",
                                      (unsigned)(e->len / 48u));
    }
    e->len += (uint32_t)ksnprintf(e->buf + e->len, e->cap - e->len, "needle\n");
    e->buf[e->len] = '\0';
    ed_lines_reset(e);
    uint32_t len = e->len;
    cprintf("edbench: %u KB document (%u bytes) in %s\n", kb, (unsigned)len,
            palloc_is_bulk(e->buf) ? "the bulk zone" : "the fast zone");

    char path[64];
    ksnprintf(path, sizeof(path), "%s/edbench.txt", dir && *dir ? dir : "/ram0");
    uint64_t t0 = time_get_us();
    int w = vfs_write(path, e->buf, e->len);
    uint64_t t_save = time_get_us() - t0;
    memset(e->buf, 0, e->len);
    t0 = time_get_us();
    int r = vfs_read(path, e->buf, e->cap - 1u);
    uint64_t t_load = time_get_us() - t0;
    (void)vfs_remove(path);
    if (w != 0 || r != (int)len) {
        cprintf("edbench: save/load through %s failed (%d, %d)\n", path, w, r);
        ed_free(e);
        return -1;
    }

    const int keys = 20;
    t0 = time_get_us();
    for (int i = 0; i < keys; i++) {
        e->pos = 0;
        ed_insert(e, "x", 1, true);
    }
    uint64_t t_key = (time_get_us() - t0) / (uint64_t)keys;

    uint32_t at = 0;
    t0 = time_get_us();
    bool found = ed_find(e, "needle", 0, true, &at);
    uint64_t t_find = time_get_us() - t0;

    /* A frame's scans with the cursor at the end: the first after a load
     * (every count from scratch), then the next frame after a keystroke. */
    ed_lines_reset(e);
    e->pos = e->len;
    e->top = e->len > 2000u ? e->len - 2000u : 0;
    t0 = time_get_us();
    uint32_t n = lines_before(e, e->len);
    (void)lines_before(e, e->top);
    (void)lines_before(e, e->pos);
    uint64_t t_scan = time_get_us() - t0;
    ed_insert(e, "y", 1, true);
    t0 = time_get_us();
    (void)lines_before(e, e->len);
    (void)lines_before(e, e->top);
    (void)lines_before(e, e->pos);
    uint64_t t_scan2 = time_get_us() - t0;

    cprintf("EDBENCH save %u us, load %u us, key at start %u us, find to end %u us (%s), "
            "frame scans at end %u us first, %u us after a key (%u lines)\n",
            (unsigned)t_save, (unsigned)t_load, (unsigned)t_key, (unsigned)t_find,
            found ? "found" : "NOT FOUND", (unsigned)t_scan, (unsigned)t_scan2, (unsigned)n);
    ed_free(e);
    return found ? 0 : -1;
}
