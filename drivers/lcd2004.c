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
 */

#define RS_BIT   0x01u
#define E_BIT    0x04u
#define BL_BIT   0x08u

#define CMD_CLEAR        0x01u
#define CMD_ENTRY_INC    0x06u
#define CMD_DISPLAY      0x08u   /* | 0x04 display on, | 0x02 underline, | 0x01 blink */
#define CMD_FUNCTION_4B  0x28u   /* 4-bit, two line mode (the 4-line glass is two 40-char lines), 5x8 */
#define CMD_SET_DDRAM    0x80u

/* The C6 controller moves 30 bytes in one write and fails at 34 (its 32-byte FIFO); five characters. */
#define XFER_MAX  30u

static const uint8_t k_row_base[LCD2004_ROWS] = { 0x00u, 0x40u, 0x14u, 0x54u };

static struct {
    bool     ready;
    uint8_t  addr;
    bool     backlight;
    bool     cursor_on;
    bool     hw_cursor_on;         /* what the HD44780 display-control register holds */
    bool     shown_valid;          /* false: what the glass shows is unknown, redraw all */
    uint32_t cx, cy;               /* terminal cursor */
    bool     wrap_pending;         /* the last column was written; wrap on the next printable */
    char     want[LCD2004_ROWS][LCD2004_COLS];
    char     shown[LCD2004_ROWS][LCD2004_COLS];
    uint32_t flush_count, fail_count, chars_sent;
} g;   /* guarded by: g_lock (below) */
static ylock_t g_lock;

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

static uint8_t display_ctl(void) {
    return (uint8_t)(CMD_DISPLAY | 0x04u | (g.cursor_on ? 0x01u : 0u));
}

static bool init_locked(uint8_t addr) {
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
    g.hw_cursor_on = g.cursor_on;
    if (!hd_cmd(display_ctl())) return false;

    memset(g.want, ' ', sizeof(g.want));
    memset(g.shown, ' ', sizeof(g.shown));
    g.cx = g.cy = 0;
    g.wrap_pending = false;
    g.shown_valid = true;
    g.ready = true;
    printk("[LCD2004] HD44780 20x4 display at 0x%02x.\n", g.addr);
    return true;
}

static bool ensure_ready(void) {
    return g.ready || init_locked(0u);
}

bool lcd2004_init(uint8_t addr) {
    ylock_acquire(&g_lock);
    bool r = init_locked(addr);
    ylock_release(&g_lock);
    return r;
}

bool    lcd2004_is_ready(void) { return g.ready; }
uint8_t lcd2004_address(void)  { return g.addr; }

const char *lcd2004_row(uint32_t y) { return y < LCD2004_ROWS ? &g.want[y][0] : NULL; }

static char printable(char c) {
    return (c >= 0x20 && c <= 0x7D) ? c : '?';
}

static void scroll_up(void) {
    memmove(g.want[0], g.want[1], (LCD2004_ROWS - 1u) * LCD2004_COLS);
    memset(g.want[LCD2004_ROWS - 1u], ' ', LCD2004_COLS);
}

static void term_putc(char c) {
    switch (c) {
    case '\r': g.cx = 0; g.wrap_pending = false; return;
    case '\n':
        g.cx = 0; g.wrap_pending = false;
        if (g.cy + 1u >= LCD2004_ROWS) scroll_up(); else g.cy++;
        return;
    case '\b':
        if (g.wrap_pending) g.wrap_pending = false;
        else if (g.cx > 0) g.cx--;
        return;
    case '\f':
        memset(g.want, ' ', sizeof(g.want));
        g.cx = g.cy = 0; g.wrap_pending = false;
        return;
    default: break;
    }
    if (g.wrap_pending) {
        g.cx = 0; g.wrap_pending = false;
        if (g.cy + 1u >= LCD2004_ROWS) scroll_up(); else g.cy++;
    }
    g.want[g.cy][g.cx] = printable(c);
    if (g.cx + 1u >= LCD2004_COLS) g.wrap_pending = true; else g.cx++;
}

void lcd2004_clear(void) {
    ylock_acquire(&g_lock);
    memset(g.want, ' ', sizeof(g.want));
    g.cx = g.cy = 0; g.wrap_pending = false;
    ylock_release(&g_lock);
}

void lcd2004_put_at(uint32_t x, uint32_t y, const char *s) {
    if (!s || y >= LCD2004_ROWS) return;
    ylock_acquire(&g_lock);
    for (; *s && x < LCD2004_COLS; s++, x++) g.want[y][x] = printable(*s);
    ylock_release(&g_lock);
}

void lcd2004_term_write(const char *s, size_t len) {
    if (!s) return;
    ylock_acquire(&g_lock);
    for (size_t i = 0; i < len; i++) term_putc(s[i]);
    ylock_release(&g_lock);
}

void lcd2004_set_cursor(uint32_t x, uint32_t y) {
    ylock_acquire(&g_lock);
    g.cx = x < LCD2004_COLS ? x : LCD2004_COLS - 1u;
    g.cy = y < LCD2004_ROWS ? y : LCD2004_ROWS - 1u;
    g.wrap_pending = false;
    ylock_release(&g_lock);
}

void lcd2004_cursor_visible(bool on) {
    ylock_acquire(&g_lock);
    g.cursor_on = on;
    ylock_release(&g_lock);
}

void lcd2004_backlight(bool on) {
    ylock_acquire(&g_lock);
    g.backlight = on;
    if (g.ready) {
        uint8_t b = on ? BL_BIT : 0u;      /* any write carries the bit; E stays low */
        (void)i2c_xfer(g.addr, &b, 1u, NULL, 0u);
    }
    ylock_release(&g_lock);
}

static bool flush_locked(void) {
    if (!ensure_ready()) return false;
    wr_t w = { .len = 0, .ok = true };

    if (!g.shown_valid) {
        /* The glass is unknown after a bus failure: clear it and redraw every character. */
        wr_byte(&w, CMD_CLEAR, false);
        if (!wr_xfer(&w)) return false;
        wait_ms(3u);
        memset(g.shown, ' ', sizeof(g.shown));
        g.shown_valid = true;
        g.hw_cursor_on = !g.cursor_on;     /* force the display-control write below */
    }
    if (g.hw_cursor_on != g.cursor_on) {
        wr_byte(&w, display_ctl(), false);
        g.hw_cursor_on = g.cursor_on;
    }

    for (uint32_t r = 0; r < LCD2004_ROWS; r++) {
        lcd2004_run_t runs[LCD2004_COLS / 2u + 1u];
        uint32_t n = lcd2004_diff_runs(g.shown[r], g.want[r], runs, LCD2004_COLS / 2u + 1u);
        for (uint32_t i = 0; i < n; i++) {
            wr_byte(&w, (uint8_t)(CMD_SET_DDRAM | (k_row_base[r] + runs[i].x)), false);
            for (uint32_t k = 0; k < runs[i].len; k++)
                wr_byte(&w, (uint8_t)g.want[r][runs[i].x + k], true);
            g.chars_sent += runs[i].len;
        }
    }
    if (g.cursor_on) {
        uint32_t cx = g.wrap_pending ? LCD2004_COLS - 1u : g.cx;
        wr_byte(&w, (uint8_t)(CMD_SET_DDRAM | (k_row_base[g.cy] + cx)), false);
    }
    if (!wr_xfer(&w)) {
        g.shown_valid = false;
        g.fail_count++;
        return false;
    }
    memcpy(g.shown, g.want, sizeof(g.shown));
    g.flush_count++;
    return true;
}

bool lcd2004_flush(void) {
    ylock_acquire(&g_lock);
    bool r = flush_locked();
    ylock_release(&g_lock);
    return r;
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

    /* Terminal behaviour on a scratch state: wrap, deferred wrap at the last cell, scroll. */
    ylock_acquire(&g_lock);
    char keep[LCD2004_ROWS][LCD2004_COLS];
    memcpy(keep, g.want, sizeof(keep));
    uint32_t keep_cx = g.cx, keep_cy = g.cy;
    bool keep_wrap = g.wrap_pending;
    memset(g.want, ' ', sizeof(g.want));
    g.cx = g.cy = 0; g.wrap_pending = false;
    for (uint32_t i = 0; i < LCD2004_COLS; i++) term_putc('a');
    if (g.cy != 0 || !g.wrap_pending) { failed++; if (report) cprintf("  lcd2004 term: no deferred wrap\n"); }
    term_putc('\n');
    if (g.cy != 1 || g.cx != 0) { failed++; if (report) cprintf("  lcd2004 term: LF after full row\n"); }
    term_putc('b');
    for (uint32_t i = 0; i < 3u; i++) term_putc('\n');   /* row 1 -> 3, then a scroll */
    if (g.cy != 3 || g.want[0][0] != 'b' || g.want[3][0] != ' ') {
        failed++;
        if (report) cprintf("  lcd2004 term: scroll wrong (row0 '%c')\n", g.want[0][0]);
    }
    memcpy(g.want, keep, sizeof(keep));
    g.cx = keep_cx; g.cy = keep_cy; g.wrap_pending = keep_wrap;
    ylock_release(&g_lock);

    if (report)
        cprintf("lcd2004 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

void lcd2004_print_status(void) {
    if (!g.ready) {
        cprintf("lcd: not initialized (try `lcd init`)\n");
        return;
    }
    cprintf("lcd: 20x4 HD44780 at 0x%02x, backlight %s, cursor %lu,%lu%s; %lu flushes, %lu chars sent, %lu bus failures\n",
            g.addr, g.backlight ? "on" : "off", (unsigned long)g.cx, (unsigned long)g.cy,
            g.cursor_on ? " (shown)" : "",
            (unsigned long)g.flush_count, (unsigned long)g.chars_sent, (unsigned long)g.fail_count);
}
