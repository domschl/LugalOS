#include "drivers/lcd2004.h"
#include "drivers/i2c_bus.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* HD44780 20x4 over a PCF8574. See the header for the pin map and the two modes.
 *
 * Timing: the PCF8574 latches each byte at I2C ack, and one I2C byte is ~90 us at
 * 100 kHz, longer than the HD44780's 230 ns E pulse and 37 us command time, so the
 * six-byte encoding (data, E high, E low, per nibble) needs no delays between
 * bytes. Only clear (1.52 ms) and the power-up sequence are waited out.
 *
 * Two locks, so the console can feed the display from any context:
 *   g_buf_lock (spinlock)  the frame buffer, cursor and escape parser; held for a few
 *                          instructions, never across I2C. The console tee updates the
 *                          terminal here, inline.
 *   g_io_lock  (ylock)     what the glass shows, and the bus; held across I2C, so task
 *                          context only. A flush snapshots the buffer under g_buf_lock,
 *                          then talks to the bus under g_io_lock alone.
 * Order: g_io_lock, then g_buf_lock.
 */

#define RS_BIT   0x01u
#define E_BIT    0x04u
#define BL_BIT   0x08u

#define CMD_CLEAR        0x01u
#define CMD_ENTRY_INC    0x06u
#define CMD_DISPLAY      0x08u   /* | 0x04 display on, | 0x02 underline, | 0x01 blink */
#define CMD_FUNCTION_4B  0x28u   /* 4-bit, two line mode (the 4-line glass is two 40-char lines), 5x8 */
#define CMD_SET_CGRAM    0x40u
#define CMD_SET_DDRAM    0x80u

/* The C6 controller moves 30 bytes in one write and fails at 34 (its 32-byte FIFO); five characters. */
#define XFER_MAX  30u

static const uint8_t k_row_base[LCD2004_ROWS] = { 0x00u, 0x40u, 0x14u, 0x54u };

/* The frame buffer and the terminal state machine, which the console tee updates from any context. */
static spinlock_t g_buf_lock = SPINLOCK_INIT;
static struct {
    char     want[LCD2004_ROWS][LCD2004_COLS];
    uint32_t cx, cy;               /* terminal cursor */
    bool     wrap_pending;         /* the last column was written; wrap on the next printable */
    bool     cursor_on;
    bool     dirty;                /* changed since the last snapshot */
    uint8_t  esc;                  /* escape parser: 0 text, 1 after ESC, 2 inside CSI */
    uint8_t  csi_n;                /* parameter being read */
    uint16_t csi[3];
    bool     csi_priv;             /* ESC [ ? ... : modes and the like, ignored */
} t;   /* guarded by: g_buf_lock */

/* What the glass shows and the bus state: a blocking lock, held across I2C. */
static ylock_t g_io_lock;
static struct {
    bool     ready;
    uint8_t  addr;
    bool     backlight;
    bool     hw_cursor_on;         /* what the HD44780 display-control register holds */
    bool     shown_valid;          /* false: what the glass shows is unknown, redraw all */
    bool     cg_dirty;             /* the glyphs below must be written to CGRAM before the next characters */
    uint8_t  cg[8][8];             /* user-defined glyphs, by CGRAM slot */
    char     shown[LCD2004_ROWS][LCD2004_COLS];
    bool     mirror_task;
    uint32_t flush_count, fail_count, chars_sent;
} g;   /* guarded by: g_io_lock */
/* accepted: gates a tee that is bound or not; the shell and Lisp read it racily */
static volatile bool g_mirror_on;

static void wait_ms(uint32_t ms) {
    uint64_t until = time_get_ms() + ms;
    while (time_get_ms() < until) sched_yield();
}

void lcd2004_encode(uint8_t out[6], uint8_t v, bool data, bool backlight) {
    uint8_t ctl = (uint8_t)((data ? RS_BIT : 0u) | (backlight ? BL_BIT : 0u));
    uint8_t hi = (uint8_t)((v & 0xF0u) | ctl);
    uint8_t lo = (uint8_t)(((v << 4) & 0xF0u) | ctl);
    out[0] = hi;  out[1] = (uint8_t)(hi | E_BIT);  out[2] = hi;
    out[3] = lo;  out[4] = (uint8_t)(lo | E_BIT);  out[5] = lo;
}

uint32_t lcd2004_diff_runs(const char *shown, const char *want, lcd2004_run_t *runs, uint32_t max) {
    uint32_t n = 0, x = 0;
    while (x < LCD2004_COLS && n < max) {
        if (shown[x] == want[x]) { x++; continue; }
        uint32_t s = x;
        while (x < LCD2004_COLS && shown[x] != want[x]) x++;
        runs[n].x = (uint8_t)s;
        runs[n].len = (uint8_t)(x - s);
        n++;
    }
    return n;
}

/* Batches encoded bytes into transfers of at most XFER_MAX; a retry covers a bus glitch. */
typedef struct { uint8_t buf[XFER_MAX]; uint32_t len; bool ok; } wr_t;

static bool wr_xfer(wr_t *w) {
    if (!w->ok || w->len == 0) return w->ok;
    bool ok = i2c_xfer(g.addr, w->buf, w->len, NULL, 0u) ||
              i2c_xfer(g.addr, w->buf, w->len, NULL, 0u);
    w->len = 0;
    if (!ok) w->ok = false;
    return w->ok;
}

static void wr_byte(wr_t *w, uint8_t v, bool data) {
    if (w->len + 6u > XFER_MAX && !wr_xfer(w)) return;
    if (!w->ok) return;
    lcd2004_encode(&w->buf[w->len], v, data, g.backlight);
    w->len += 6u;
}

static bool hd_cmd(uint8_t v) {
    wr_t w = { .len = 0, .ok = true };
    wr_byte(&w, v, false);
    return wr_xfer(&w);
}

static uint8_t display_ctl(bool cursor_on) {
    return (uint8_t)(CMD_DISPLAY | 0x04u | (cursor_on ? 0x01u : 0u));
}

static void buf_reset_locked(void) {            /* g_buf_lock held */
    memset(t.want, ' ', sizeof(t.want));
    t.cx = t.cy = 0;
    t.wrap_pending = false;
    t.esc = 0;
    t.dirty = true;
}

static bool init_locked(uint8_t addr) {         /* g_io_lock held */
    g.ready = false;
    static const uint8_t cands[2] = { LCD2004_ADDR, 0x3Fu };
    uint8_t found = 0;
    uint8_t off = 0;
    for (uint32_t i = 0; i < 2u && !found; i++) {
        uint8_t a = addr ? addr : cands[i];
        if (i2c_xfer(a, &off, 1u, NULL, 0u)) found = a;
        if (addr) break;
    }
    if (!found) return false;
    g.addr = found;
    g.backlight = true;

    /* Power-up by instruction (HD44780 datasheet fig. 24): three 8-bit function sets,
     * then 4-bit, with the documented waits. Raw nibbles, E pulsed per nibble. */
    static const struct { uint8_t nib; uint32_t wait; } seq[] = {
        { 0x30u, 5u }, { 0x30u, 1u }, { 0x30u, 1u }, { 0x20u, 1u },
    };
    wait_ms(50u);
    for (uint32_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        uint8_t b[3] = { (uint8_t)(seq[i].nib | BL_BIT), (uint8_t)(seq[i].nib | BL_BIT | E_BIT),
                         (uint8_t)(seq[i].nib | BL_BIT) };
        if (!i2c_xfer(g.addr, b, 3u, NULL, 0u)) return false;
        wait_ms(seq[i].wait);
    }
    if (!hd_cmd(CMD_FUNCTION_4B)) return false;
    if (!hd_cmd(CMD_DISPLAY)) return false;          /* display off while clearing */
    if (!hd_cmd(CMD_CLEAR)) return false;
    wait_ms(3u);
    if (!hd_cmd(CMD_ENTRY_INC)) return false;

    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    buf_reset_locked();
    bool cur = t.cursor_on;
    spin_unlock_irqrestore(&g_buf_lock, f);
    memset(g.shown, ' ', sizeof(g.shown));
    g.cg_dirty = true;
    g.hw_cursor_on = cur;
    if (!hd_cmd(display_ctl(cur))) return false;
    g.shown_valid = true;
    g.ready = true;
    printk("[LCD2004] HD44780 20x4 display at 0x%02x.\n", g.addr);
    return true;
}

static bool ensure_ready(void) {                /* g_io_lock held */
    return g.ready || init_locked(0u);
}

bool lcd2004_init(uint8_t addr) {
    ylock_acquire(&g_io_lock);
    bool r = init_locked(addr);
    ylock_release(&g_io_lock);
    return r;
}

bool    lcd2004_is_ready(void) { return g.ready; }
uint8_t lcd2004_address(void)  { return g.addr; }

const char *lcd2004_row(uint32_t y) { return y < LCD2004_ROWS ? &t.want[y][0] : NULL; }

/* ---- The terminal: every function below assumes g_buf_lock is held ------------------------ */

static char printable(char c) {
    return (c >= 0x20 && c <= 0x7D) ? c : '?';
}

static void scroll_up(void) {
    memmove(t.want[0], t.want[1], (LCD2004_ROWS - 1u) * LCD2004_COLS);
    memset(t.want[LCD2004_ROWS - 1u], ' ', LCD2004_COLS);
}

static void term_char(char c) {
    switch (c) {
    case '\r': t.cx = 0; t.wrap_pending = false; return;
    case '\n':
        t.cx = 0; t.wrap_pending = false;
        if (t.cy + 1u >= LCD2004_ROWS) scroll_up(); else t.cy++;
        return;
    case '\t': {                                        /* to the next multiple of 4 */
        t.wrap_pending = false;
        uint32_t nx = (t.cx + 4u) & ~3u;
        t.cx = nx < LCD2004_COLS ? nx : LCD2004_COLS - 1u;
        return;
    }
    case '\b':
        if (t.wrap_pending) t.wrap_pending = false;
        else if (t.cx > 0) t.cx--;
        return;
    case '\f':
        memset(t.want, ' ', sizeof(t.want));
        t.cx = t.cy = 0; t.wrap_pending = false;
        return;
    default: break;
    }
    if (t.wrap_pending) {
        t.cx = 0; t.wrap_pending = false;
        if (t.cy + 1u >= LCD2004_ROWS) scroll_up(); else t.cy++;
    }
    t.want[t.cy][t.cx] = printable(c);
    if (t.cx + 1u >= LCD2004_COLS) t.wrap_pending = true; else t.cx++;
}

static void erase_cells(uint32_t y, uint32_t from, uint32_t to) {   /* [from, to) */
    if (y < LCD2004_ROWS && from < to) memset(&t.want[y][from], ' ', to - from);
}

static void csi_final(char c) {
    if (t.csi_priv) return;                               /* ?25l, ?2004h and the like */
    uint32_t p0 = t.csi[0], p1 = t.csi[1], n = p0 ? p0 : 1u;
    uint32_t cx = t.wrap_pending ? LCD2004_COLS - 1u : t.cx;
    switch (c) {
    case 'A': t.cy = t.cy > n ? t.cy - n : 0; t.wrap_pending = false; break;
    case 'B': t.cy = t.cy + n < LCD2004_ROWS ? t.cy + n : LCD2004_ROWS - 1u; t.wrap_pending = false; break;
    case 'C': t.cx = cx + n < LCD2004_COLS ? cx + n : LCD2004_COLS - 1u; t.wrap_pending = false; break;
    case 'D': t.cx = cx > n ? cx - n : 0; t.wrap_pending = false; break;
    case 'G': t.cx = (n - 1u) < LCD2004_COLS ? n - 1u : LCD2004_COLS - 1u; t.wrap_pending = false; break;
    case 'H': case 'f': {
        uint32_t r = p0 ? p0 : 1u, k = p1 ? p1 : 1u;
        t.cy = r - 1u < LCD2004_ROWS ? r - 1u : LCD2004_ROWS - 1u;
        t.cx = k - 1u < LCD2004_COLS ? k - 1u : LCD2004_COLS - 1u;
        t.wrap_pending = false;
        break;
    }
    case 'K':
        if (p0 == 0) erase_cells(t.cy, cx, LCD2004_COLS);
        else if (p0 == 1) erase_cells(t.cy, 0, cx + 1u);
        else erase_cells(t.cy, 0, LCD2004_COLS);
        break;
    case 'J':
        if (p0 == 0) { erase_cells(t.cy, cx, LCD2004_COLS); for (uint32_t r = t.cy + 1u; r < LCD2004_ROWS; r++) erase_cells(r, 0, LCD2004_COLS); }
        else if (p0 == 1) { for (uint32_t r = 0; r < t.cy; r++) erase_cells(r, 0, LCD2004_COLS); erase_cells(t.cy, 0, cx + 1u); }
        else memset(t.want, ' ', sizeof(t.want));
        break;
    default: break;                                       /* colours (m), scroll regions, ... */
    }
}

/* Text with the escape sequences a line editor sends: cursor moves, erase, absolute position. */
static void term_feed(char c) {
    t.dirty = true;
    if (t.esc == 0u) {
        if (c == 0x1B) t.esc = 1u; else term_char(c);
        return;
    }
    if (t.esc == 1u) {
        if (c == '[') { t.esc = 2u; t.csi_n = 0; t.csi[0] = t.csi[1] = t.csi[2] = 0; t.csi_priv = false; }
        else t.esc = 0u;                                  /* a two-byte escape: ignored */
        return;
    }
    if (c >= '0' && c <= '9') { if (t.csi[t.csi_n] < 9999u) t.csi[t.csi_n] = (uint16_t)(t.csi[t.csi_n] * 10u + (uint16_t)(c - '0')); return; }
    if (c == ';') { if (t.csi_n < 2u) t.csi_n++; return; }
    if (c == '?' || c == '>' || c == '=') { t.csi_priv = true; return; }
    if (c >= 0x20 && c <= 0x2F) return;                   /* intermediate bytes */
    t.esc = 0u;
    if (c >= 0x40 && c <= 0x7E) csi_final(c);
}

/* ---- Public frame buffer operations --------------------------------------------------------- */

void lcd2004_clear(void) {
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    memset(t.want, ' ', sizeof(t.want));
    t.cx = t.cy = 0; t.wrap_pending = false; t.esc = 0; t.dirty = true;
    spin_unlock_irqrestore(&g_buf_lock, f);
}

void lcd2004_put_at(uint32_t x, uint32_t y, const char *s) {
    if (!s || y >= LCD2004_ROWS) return;
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    for (; *s && x < LCD2004_COLS; s++, x++)
        t.want[y][x] = ((uint8_t)*s >= 1u && (uint8_t)*s <= 8u) ? *s : printable(*s);   /* 1..8: user glyphs */
    t.dirty = true;
    spin_unlock_irqrestore(&g_buf_lock, f);
}

void lcd2004_define_char(uint32_t code, const uint8_t rows[8]) {
    if (!rows) return;
    ylock_acquire(&g_io_lock);
    uint8_t *glyph = g.cg[code & 7u];
    for (uint32_t r = 0; r < 8u; r++) glyph[r] = rows[r] & 0x1Fu;
    g.cg_dirty = true;
    ylock_release(&g_io_lock);
}

void lcd2004_term_write(const char *s, size_t len) {
    if (!s) return;
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    for (size_t i = 0; i < len; i++) term_feed(s[i]);
    spin_unlock_irqrestore(&g_buf_lock, f);
}

void lcd2004_set_cursor(uint32_t x, uint32_t y) {
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    t.cx = x < LCD2004_COLS ? x : LCD2004_COLS - 1u;
    t.cy = y < LCD2004_ROWS ? y : LCD2004_ROWS - 1u;
    t.wrap_pending = false; t.dirty = true;
    spin_unlock_irqrestore(&g_buf_lock, f);
}

void lcd2004_cursor_visible(bool on) {
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    t.cursor_on = on; t.dirty = true;
    spin_unlock_irqrestore(&g_buf_lock, f);
}

void lcd2004_backlight(bool on) {
    ylock_acquire(&g_io_lock);
    g.backlight = on;
    if (g.ready) {
        uint8_t b = on ? BL_BIT : 0u;      /* any write carries the bit; E stays low */
        (void)i2c_xfer(g.addr, &b, 1u, NULL, 0u);
    }
    ylock_release(&g_io_lock);
}

/* ---- Flush ---------------------------------------------------------------------------------- */

static bool flush_locked(void) {                /* g_io_lock held */
    if (!ensure_ready()) return false;

    char want[LCD2004_ROWS][LCD2004_COLS];
    uint32_t cx, cy;
    bool cursor_on;
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    memcpy(want, t.want, sizeof(want));
    cx = t.wrap_pending ? LCD2004_COLS - 1u : t.cx;
    cy = t.cy;
    cursor_on = t.cursor_on;
    t.dirty = false;
    spin_unlock_irqrestore(&g_buf_lock, f);

    wr_t w = { .len = 0, .ok = true };
    if (!g.shown_valid) {
        /* The glass is unknown after a bus failure: clear it and redraw every character. */
        wr_byte(&w, CMD_CLEAR, false);
        if (!wr_xfer(&w)) return false;
        wait_ms(3u);
        memset(g.shown, ' ', sizeof(g.shown));
        g.shown_valid = true;
        g.hw_cursor_on = !cursor_on;       /* force the display-control write below */
        g.cg_dirty = true;
    }
    if (g.cg_dirty) {
        for (uint32_t s = 0; s < 8u; s++) {
            wr_byte(&w, (uint8_t)(CMD_SET_CGRAM | (s << 3)), false);
            for (uint32_t r = 0; r < 8u; r++) wr_byte(&w, g.cg[s][r], true);
        }
        g.cg_dirty = false;                /* cleared again below if the transfer fails */
    }
    if (g.hw_cursor_on != cursor_on) {
        wr_byte(&w, display_ctl(cursor_on), false);
        g.hw_cursor_on = cursor_on;
    }

    for (uint32_t r = 0; r < LCD2004_ROWS; r++) {
        lcd2004_run_t runs[LCD2004_COLS / 2u + 1u];
        uint32_t n = lcd2004_diff_runs(g.shown[r], want[r], runs, LCD2004_COLS / 2u + 1u);
        for (uint32_t i = 0; i < n; i++) {
            wr_byte(&w, (uint8_t)(CMD_SET_DDRAM | (k_row_base[r] + runs[i].x)), false);
            for (uint32_t k = 0; k < runs[i].len; k++)
                wr_byte(&w, (uint8_t)want[r][runs[i].x + k], true);
            g.chars_sent += runs[i].len;
        }
    }
    if (cursor_on) wr_byte(&w, (uint8_t)(CMD_SET_DDRAM | (k_row_base[cy] + cx)), false);
    if (!wr_xfer(&w)) {
        g.shown_valid = false;
        g.cg_dirty = true;
        g.fail_count++;
        f = spin_lock_irqsave(&g_buf_lock);
        t.dirty = true;                    /* try again */
        spin_unlock_irqrestore(&g_buf_lock, f);
        return false;
    }
    memcpy(g.shown, want, sizeof(g.shown));
    g.flush_count++;
    return true;
}

bool lcd2004_flush(void) {
    ylock_acquire(&g_io_lock);
    bool r = flush_locked();
    ylock_release(&g_io_lock);
    return r;
}

/* ---- Console mirror ------------------------------------------------------------------------- */

#define MIRROR_MS  60u

/* console_tee_bind(mirror_tee) feeds every byte a task prints into the terminal inline: a few
 * instructions under g_buf_lock, in whatever context printed. A task does the slow part, the
 * flush, when the buffer changed, at most every MIRROR_MS; a burst of output costs one flush. */
static void mirror_tee(char c) {
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    term_feed(c);
    spin_unlock_irqrestore(&g_buf_lock, f);
}

static void mirror_body(void *arg) {
    (void)arg;
    for (;;) {
        uintptr_t f = spin_lock_irqsave(&g_buf_lock);
        bool dirty = t.dirty;
        spin_unlock_irqrestore(&g_buf_lock, f);
        if (dirty && g_mirror_on && !lcd2004_flush()) { task_sleep_ms(2000u); continue; }   /* absent or glitching: back off */
        task_sleep_ms(MIRROR_MS);
    }
}

bool lcd2004_mirror(bool on) {
    if (!on) {
        console_tee_bind(NULL);
        g_mirror_on = false;
        return true;
    }
    ylock_acquire(&g_io_lock);
    bool ok = ensure_ready();
    if (ok && !g.mirror_task) g.mirror_task = task_create_sized("clcd", mirror_body, NULL, 2) >= 0;
    bool started = g.mirror_task;
    ylock_release(&g_io_lock);
    if (!ok || !started) return false;
    g_mirror_on = true;
    console_tee_bind(mirror_tee);
    return true;
}

bool lcd2004_mirror_active(void) { return g_mirror_on; }

/* ---- Selftest, status ------------------------------------------------------------------------ */

static void feed_str(const char *s) {
    for (; *s; s++) term_feed(*s);
}

uint32_t lcd2004_selftest(bool report) {
    uint32_t failed = 0;

    /* 'A' (0x41) as data with backlight: nibble 4 then 1, RS and BL set, E pulsed. */
    uint8_t e[6];
    lcd2004_encode(e, 0x41u, true, true);
    static const uint8_t want_a[6] = { 0x49, 0x4D, 0x49, 0x19, 0x1D, 0x19 };
    if (memcmp(e, want_a, 6) != 0) {
        failed++;
        if (report) cprintf("  lcd2004 encode data: got %02x %02x %02x %02x %02x %02x\n",
                            e[0], e[1], e[2], e[3], e[4], e[5]);
    }
    /* Clear (0x01) as a command without backlight. */
    lcd2004_encode(e, 0x01u, false, false);
    static const uint8_t want_clr[6] = { 0x00, 0x04, 0x00, 0x10, 0x14, 0x10 };
    if (memcmp(e, want_clr, 6) != 0) {
        failed++;
        if (report) cprintf("  lcd2004 encode command wrong\n");
    }

    /* Diff runs. */
    char a[LCD2004_COLS], b[LCD2004_COLS];
    memset(a, ' ', sizeof a);
    memset(b, ' ', sizeof b);
    lcd2004_run_t runs[4];
    if (lcd2004_diff_runs(a, b, runs, 4) != 0) {
        failed++;
        if (report) cprintf("  lcd2004 diff: equal rows gave runs\n");
    }
    b[0] = 'x'; b[1] = 'y'; b[5] = 'z'; b[19] = 'w';
    uint32_t n = lcd2004_diff_runs(a, b, runs, 4);
    if (n != 3 || runs[0].x != 0 || runs[0].len != 2 || runs[1].x != 5 || runs[1].len != 1 ||
        runs[2].x != 19 || runs[2].len != 1) {
        failed++;
        if (report) cprintf("  lcd2004 diff: got %lu runs\n", (unsigned long)n);
    }

    /* Terminal behaviour on a scratch state, restored afterwards: wrap, deferred wrap at the last
     * cell, scroll, tabs, escape sequences. Results are collected under the lock and reported after. */
    uint32_t bad = 0;
    char got[LCD2004_COLS + 1] = "";
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    char keep[LCD2004_ROWS][LCD2004_COLS];
    memcpy(keep, t.want, sizeof(keep));
    uint32_t keep_cx = t.cx, keep_cy = t.cy;
    bool keep_wrap = t.wrap_pending, keep_dirty = t.dirty;
    uint8_t keep_esc = t.esc;
    buf_reset_locked();
    for (uint32_t i = 0; i < LCD2004_COLS; i++) term_char('a');
    if (t.cy != 0 || !t.wrap_pending) bad |= 1u;              /* no deferred wrap */
    term_char('\n');
    if (t.cy != 1 || t.cx != 0) bad |= 2u;                    /* LF after a full row */
    term_char('b');
    for (uint32_t i = 0; i < 3u; i++) term_char('\n');        /* row 1 -> 3, then a scroll */
    if (t.cy != 3 || t.want[0][0] != 'b' || t.want[3][0] != ' ') bad |= 4u;

    buf_reset_locked();
    feed_str("a\tb");                                         /* tab to column 4 */
    if (t.want[0][4] != 'b') bad |= 8u;

    buf_reset_locked();
    feed_str("lsh> abcdef\r\x1b[?25l\x1b[Klsh> x\x1b[?25h");  /* a prompt redraw */
    memcpy(got, t.want[0], LCD2004_COLS); got[LCD2004_COLS] = '\0';
    if (memcmp(t.want[0], "lsh> x               ", LCD2004_COLS) != 0 || t.cx != 6) bad |= 16u;

    feed_str("\x1b[3;5HZ\x1b[2D\x1b[1CY\x1b[0m");             /* absolute position, left 2, right 1 */
    if (t.want[2][4] != 'Y' || t.cy != 2) bad |= 32u;
    feed_str("\x1b[2J");
    if (t.want[0][0] != ' ' || t.want[2][4] != ' ') bad |= 64u;

    memcpy(t.want, keep, sizeof(keep));
    t.cx = keep_cx; t.cy = keep_cy; t.wrap_pending = keep_wrap; t.dirty = keep_dirty; t.esc = keep_esc;
    spin_unlock_irqrestore(&g_buf_lock, f);
    if (bad) {
        failed++;
        if (report) cprintf("  lcd2004 terminal: failure mask 0x%02lx (row 0 after a redraw: '%s')\n", (unsigned long)bad, got);
    }

    if (report)
        cprintf("lcd2004 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

/* What the terminal buffer holds and where its cursor is, for debugging the mirror. */
void lcd2004_dump(void) {
    char rows[LCD2004_ROWS][LCD2004_COLS];
    uint32_t cx, cy;
    bool wrap;
    uintptr_t f = spin_lock_irqsave(&g_buf_lock);
    memcpy(rows, t.want, sizeof(rows));
    cx = t.cx; cy = t.cy; wrap = t.wrap_pending;
    spin_unlock_irqrestore(&g_buf_lock, f);
    for (uint32_t r = 0; r < LCD2004_ROWS; r++) cprintf("|%.20s|\n", rows[r]);
    cprintf("cursor %lu,%lu%s\n", (unsigned long)cx, (unsigned long)cy, wrap ? " (wrap pending)" : "");
}

void lcd2004_print_status(void) {
    if (!g.ready) {
        cprintf("lcd: not initialized (try `lcd init`)\n");
        return;
    }
    cprintf("lcd: 20x4 HD44780 at 0x%02x, backlight %s; %lu flushes, %lu chars sent, %lu bus failures\n",
            g.addr, g.backlight ? "on" : "off",
            (unsigned long)g.flush_count, (unsigned long)g.chars_sent, (unsigned long)g.fail_count);
    cprintf("lcd: console mirror %s\n", g_mirror_on ? "on" : "off");
}
