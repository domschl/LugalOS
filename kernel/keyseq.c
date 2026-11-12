/*
 * Bytes from a terminal to keys.
 * See kernel/include/kernel/keyseq.h for the forms understood.
 */

#include "kernel/keyseq.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"

static key_event_t ev(uint32_t key, unsigned mods) {
    key_event_t e = { key, (uint8_t)mods };
    return e;
}

/* The rest of a UTF-8 character whose lead byte was `c`. A broken sequence
 * is the replacement character; the byte that broke it is lost, which is
 * the price of not being able to push it back. */
static uint32_t utf8_rest(const keyseq_src_t *src, unsigned c) {
    unsigned need;
    uint32_t cp;
    if ((c & 0xe0u) == 0xc0u)      { cp = c & 0x1fu; need = 1; }
    else if ((c & 0xf0u) == 0xe0u) { cp = c & 0x0fu; need = 2; }
    else if ((c & 0xf8u) == 0xf0u) { cp = c & 0x07u; need = 3; }
    else return 0xfffd;
    while (need--) {
        int b = src->getc(src->ctx);
        if (b < 0 || ((unsigned)b & 0xc0u) != 0x80u) return 0xfffd;
        cp = (cp << 6) | ((unsigned)b & 0x3fu);
    }
    return cp;
}

static uint32_t tilde_key(unsigned n) {
    switch (n) {
    case 1: case 7: return KEY_HOME;
    case 2:         return KEY_INSERT;
    case 3:         return KEY_DELETE;
    case 4: case 8: return KEY_END;
    case 5:         return KEY_PGUP;
    case 6:         return KEY_PGDN;
    default:        return KEY_UNKNOWN;
    }
}

static uint32_t letter_key(int f) {
    switch (f) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default:  return KEY_UNKNOWN;
    }
}

/* After ESC [: parameters, then the final byte. */
static key_event_t csi(const keyseq_src_t *src) {
    unsigned p[4] = { 0, 0, 0, 0 }, np = 0;
    bool any = false;
    for (;;) {
        int b = src->getc(src->ctx);
        if (b < 0) return ev(KEY_UNKNOWN, 0);
        if (b >= '0' && b <= '9') {
            if (np < 4 && p[np] < 0x110000u) p[np] = p[np] * 10u + (unsigned)(b - '0');
            any = true;
        } else if (b == ';') {
            if (np < 3) np++;
            any = true;
        } else if (b >= 0x40 && b <= 0x7e) {
            unsigned mods = (np >= 1 && p[1] >= 1u) ? (p[1] - 1u) & 0xfu : 0u;
            if (b == '~') return ev(tilde_key(p[0]), mods);
            if (b == 'u') return any ? ev(p[0], mods) : ev(KEY_UNKNOWN, 0);
            if (b == 'Z') return ev('\t', mods | KMOD_SHIFT);    /* Shift-Tab (back-tab) */
            return ev(letter_key(b), mods);
        } else if (b < 0x20 || b > 0x7e) {
            return ev(KEY_UNKNOWN, 0);       /* not a sequence after all */
        }
        /* intermediates (0x20-0x2F) and private markers: carry on */
    }
}

key_event_t keyseq_read(const keyseq_src_t *src) {
    int c = src->getc(src->ctx);
    if (c < 0) return ev(KEY_UNKNOWN, 0);
    if (c == 0x1b) {
        if (!src->more(src->ctx)) return ev(0x1b, 0);
        int b = src->getc(src->ctx);
        if (b < 0) return ev(0x1b, 0);
        if (b == '[') return csi(src);
        if (b == 'O') {
            int f = src->getc(src->ctx);
            return ev(letter_key(f), 0);
        }
        if (b == 0x1b) return ev(0x1b, KMOD_ALT);
        if ((unsigned)b >= 0x80u) return ev(utf8_rest(src, (unsigned)b), KMOD_ALT);
        return ev((uint32_t)b, KMOD_ALT);
    }
    if ((unsigned)c >= 0x80u) return ev(utf8_rest(src, (unsigned)c), 0);
    return ev((uint32_t)c, 0);
}

/* --- Pushed bytes -------------------------------------------------------- */

enum { PS_TEXT = 0, PS_ESC, PS_CSI, PS_SS3, PS_UTF8 };

bool keyseq_push(keyseq_push_t *ps, int byte, key_event_t *out) {
    unsigned b = (unsigned)byte & 0xffu;
    switch (ps->state) {
    case PS_ESC:
        if (b == '[') { ps->state = PS_CSI; ps->np = 0; ps->any = false; ps->p[0] = ps->p[1] = 0; return false; }
        if (b == 'O') { ps->state = PS_SS3; return false; }
        ps->state = PS_TEXT;
        if (b >= 0x80u) { ps->state = PS_UTF8; ps->mods = KMOD_ALT; goto utf8_lead; }
        *out = ev(b, KMOD_ALT);
        return true;
    case PS_SS3:
        ps->state = PS_TEXT;
        *out = ev(letter_key((int)b), 0);
        return true;
    case PS_CSI:
        if (b >= '0' && b <= '9') {
            if (ps->np < 2 && ps->p[ps->np] < 0x110000u) ps->p[ps->np] = ps->p[ps->np] * 10u + (b - '0');
            ps->any = true;
            return false;
        }
        if (b == ';') {
            if (ps->np < 1) ps->np++;
            else ps->np = 2;                     /* a third parameter: ignored */
            ps->any = true;
            return false;
        }
        if (b >= 0x40u && b <= 0x7eu) {
            ps->state = PS_TEXT;
            unsigned mods = (ps->np >= 1 && ps->p[1] >= 1u) ? (ps->p[1] - 1u) & 0xfu : 0u;
            if (b == '~') *out = ev(tilde_key(ps->p[0]), mods);
            else if (b == 'u') *out = ps->any ? ev(ps->p[0], mods) : ev(KEY_UNKNOWN, 0);
            else if (b == 'Z') *out = ev('\t', mods | KMOD_SHIFT);
            else *out = ev(letter_key((int)b), mods);
            return true;
        }
        if (b < 0x20u || b > 0x7eu) {
            ps->state = PS_TEXT;
            *out = ev(KEY_UNKNOWN, 0);
            return true;
        }
        return false;                            /* intermediates: carry on */
    case PS_UTF8:
        if ((b & 0xc0u) != 0x80u) {
            ps->state = PS_TEXT;
            *out = ev(0xfffd, 0);
            return true;
        }
        ps->cp = (ps->cp << 6) | (b & 0x3fu);
        if (--ps->need) return false;
        ps->state = PS_TEXT;
        *out = ev(ps->cp, ps->mods);
        return true;
    default:
        break;
    }
    if (b == 0x1bu) { ps->state = PS_ESC; return false; }
    if (b < 0x80u) { *out = ev(b, 0); return true; }
    ps->state = PS_UTF8;
    ps->mods = 0;
utf8_lead:
    if ((b & 0xe0u) == 0xc0u)      { ps->cp = b & 0x1fu; ps->need = 1; }
    else if ((b & 0xf0u) == 0xe0u) { ps->cp = b & 0x0fu; ps->need = 2; }
    else if ((b & 0xf8u) == 0xf0u) { ps->cp = b & 0x07u; ps->need = 3; }
    else { ps->state = PS_TEXT; *out = ev(0xfffd, 0); return true; }
    return false;
}

bool keyseq_push_flush(keyseq_push_t *ps, key_event_t *out) {
    bool lone_esc = ps->state == PS_ESC;
    ps->state = PS_TEXT;
    if (lone_esc) *out = ev(0x1b, 0);
    return lone_esc;
}

/* --- The console as a source ------------------------------------------- */

static int con_getc(void *ctx) {
    (void)ctx;
    return (unsigned char)console_getc();
}

/* A terminal sends a sequence in one burst, so 30 ms after an ESC with
 * nothing more is a key of its own: long enough for a USB or serial burst,
 * short enough that nobody notices Esc waiting. */
static bool con_more(void *ctx) {
    (void)ctx;
    uint64_t until = time_get_us() + 30000u;
    while (!console_has_char()) {
        if (time_get_us() >= until) return false;
        sched_yield();
    }
    return true;
}

key_event_t keyseq_read_console(void) {
    const keyseq_src_t src = { con_getc, con_more, 0 };
    return keyseq_read(&src);
}

static int canvas_con_getc(void *ctx) {
    (void)ctx;
    for (;;) {
        int c = console_canvas_getc();
        if (c >= 0) return c;
        sched_yield();
    }
}

static bool canvas_con_more(void *ctx) {
    (void)ctx;
    uint64_t until = time_get_us() + 30000u;
    while (!console_canvas_has_key()) {
        if (time_get_us() >= until) return false;
        sched_yield();
    }
    return true;
}

key_event_t keyseq_read_canvas(void) {
    const keyseq_src_t src = { canvas_con_getc, canvas_con_more, 0 };
    return keyseq_read(&src);
}

/* --- Reverse encoding: key_event_t -> VT bytes ---------------------------- */

static size_t encode_uint(uint32_t v, uint8_t *out, size_t cap) {
    char tmp[10];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && n < sizeof(tmp));
    if (n > cap) return 0;
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)tmp[n - 1u - i];
    return n;
}

static size_t encode_utf8(uint32_t cp, uint8_t *out, size_t cap) {
    if (cp < 0x80u) {
        if (cap < 1u) return 0;
        out[0] = (uint8_t)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        if (cap < 2u) return 0;
        out[0] = (uint8_t)(0xc0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        if (cap < 3u) return 0;
        out[0] = (uint8_t)(0xe0u | (cp >> 12));
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3fu));
        return 3u;
    }
    if (cp <= 0x10ffffu) {
        if (cap < 4u) return 0;
        out[0] = (uint8_t)(0xf0u | (cp >> 18));
        out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
        out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        out[3] = (uint8_t)(0x80u | (cp & 0x3fu));
        return 4u;
    }
    return 0;
}

size_t keyseq_encode(key_event_t ev, uint8_t *out, size_t cap) {
    if (!out || cap == 0 || ev.key == KEY_UNKNOWN) return 0;
    uint8_t mods = ev.mods & 0x0fu;
    char csi_let = 0;
    uint8_t tilde_num = 0;
    switch (ev.key) {
    case KEY_UP:     csi_let = 'A'; break;
    case KEY_DOWN:   csi_let = 'B'; break;
    case KEY_RIGHT:  csi_let = 'C'; break;
    case KEY_LEFT:   csi_let = 'D'; break;
    case KEY_HOME:   csi_let = 'H'; break;
    case KEY_END:    csi_let = 'F'; break;
    case KEY_INSERT: tilde_num = 2; break;
    case KEY_DELETE: tilde_num = 3; break;
    case KEY_PGUP:   tilde_num = 5; break;
    case KEY_PGDN:   tilde_num = 6; break;
    default: break;
    }
    if (csi_let) {
        if (mods == 0) {
            if (cap < 3u) return 0;
            out[0] = 0x1bu; out[1] = '['; out[2] = (uint8_t)csi_let;
            return 3u;
        }
        if (cap < 6u) return 0;
        out[0] = 0x1bu; out[1] = '['; out[2] = '1'; out[3] = ';';
        size_t p = 4u + encode_uint(1u + mods, out + 4u, cap - 5u);
        if (p == 4u || p >= cap) return 0;
        out[p++] = (uint8_t)csi_let;
        return p;
    }
    if (tilde_num) {
        if (cap < 4u) return 0;
        out[0] = 0x1bu; out[1] = '[';
        size_t p = 2u + encode_uint(tilde_num, out + 2u, cap - 2u);
        if (p == 2u) return 0;
        if (mods) {
            if (p + 2u >= cap) return 0;
            out[p++] = ';';
            size_t m = encode_uint(1u + mods, out + p, cap - p - 1u);
            if (!m) return 0;
            p += m;
        }
        if (p >= cap) return 0;
        out[p++] = '~';
        return p;
    }
    if (ev.key == '\t' && mods == KMOD_SHIFT) {
        if (cap < 3u) return 0;
        out[0] = 0x1bu; out[1] = '['; out[2] = 'Z';
        return 3u;
    }
    if (ev.key >= 0x110000u) return 0;
    if (mods == 0) return encode_utf8(ev.key, out, cap);
    if (mods == KMOD_ALT) {
        if (cap < 2u) return 0;
        out[0] = 0x1bu;
        size_t n = encode_utf8(ev.key, out + 1u, cap - 1u);
        return n ? 1u + n : 0;
    }
    if (mods == KMOD_CTRL) {
        if (ev.key >= 'a' && ev.key <= 'z') {
            out[0] = (uint8_t)(ev.key - 'a' + 1u);
            return 1u;
        }
        if (ev.key >= '@' && ev.key <= '_') {
            out[0] = (uint8_t)(ev.key - '@');
            return 1u;
        }
    }
    /* CSI-u fallback for remaining modified keys (e.g. Super+c). */
    if (cap < 6u) return 0;
    out[0] = 0x1bu; out[1] = '[';
    size_t p = 2u + encode_uint(ev.key, out + 2u, cap - 2u);
    if (p == 2u || p + 2u >= cap) return 0;
    out[p++] = ';';
    size_t m = encode_uint(1u + mods, out + p, cap - p - 1u);
    if (!m || p + m >= cap) return 0;
    p += m;
    out[p++] = 'u';
    return p;
}

/* --- keyselftest -------------------------------------------------------- */

typedef struct {
    const char *s;
    unsigned    i, n;
} st_src_t;

static int st_getc(void *ctx) {
    st_src_t *t = (st_src_t *)ctx;
    return t->i < t->n ? (unsigned char)t->s[t->i++] : -1;
}

static bool st_more(void *ctx) {
    st_src_t *t = (st_src_t *)ctx;
    return t->i < t->n;
}

static bool st_one(const char *s, unsigned n, uint32_t key, unsigned mods) {
    st_src_t t = { s, 0, n };
    const keyseq_src_t src = { st_getc, st_more, &t };
    key_event_t e = keyseq_read(&src);
    return e.key == key && e.mods == mods && t.i == n;
}

#define ONE(s, key, mods) st_one((s), sizeof(s) - 1u, (key), (mods))

/* The same bytes pushed one at a time: exactly one key, at the last byte
 * (or at the flush, for a lone ESC). */
static bool push_one(const char *s, unsigned n, uint32_t key, unsigned mods) {
    keyseq_push_t ps = { 0 };
    key_event_t e = { 0, 0 };
    unsigned got = 0;
    for (unsigned i = 0; i < n; i++) {
        key_event_t k;
        if (keyseq_push(&ps, (unsigned char)s[i], &k)) {
            if (i + 1u != n) return false;
            e = k;
            got++;
        }
    }
    key_event_t k;
    if (keyseq_push_flush(&ps, &k)) { e = k; got++; }
    return got == 1 && e.key == key && e.mods == mods;
}

#define PUSH(s, key, mods) push_one((s), sizeof(s) - 1u, (key), (mods))

static bool enc_roundtrip(uint32_t key, uint8_t mods, const char *exp, unsigned exp_len) {
    uint8_t buf[24];
    size_t n = keyseq_encode((key_event_t){ key, mods }, buf, sizeof(buf));
    if (n != exp_len) return false;
    for (size_t i = 0; i < n; i++)
        if (buf[i] != (uint8_t)exp[i]) return false;
    return st_one((const char *)buf, (unsigned)n, key, mods);
}

#define ENC(key, mods, exp) enc_roundtrip((key), (mods), (exp), sizeof(exp) - 1u)

int keyseq_selftest(void) {
    int fails = 0;
    struct { const char *name; bool ok; } c[] = {
        { "plain byte and a control",       ONE("a", 'a', 0) && ONE("\x03", 3, 0) && ONE("\x7f", 0x7f, 0) },
        { "Ctrl-Space is NUL",              st_one("\0", 1, 0, 0) },
        { "UTF-8: one code point",          ONE("\xc3\xa4", 0xe4, 0) && ONE("\xe2\x82\xac", 0x20ac, 0) },
        { "lone ESC",                       ONE("\x1b", 0x1b, 0) },
        { "Alt: ESC prefix, and Alt-UTF-8", ONE("\x1b" "w", 'w', KMOD_ALT) && ONE("\x1b\xc3\xbc", 0xfc, KMOD_ALT) },
        { "Alt-Backspace",                  ONE("\x1b\x7f", 0x7f, KMOD_ALT) },
        { "arrows, CSI and SS3",            ONE("\x1b[A", KEY_UP, 0) && ONE("\x1b[D", KEY_LEFT, 0) &&
                                            ONE("\x1bOC", KEY_RIGHT, 0) && ONE("\x1b[H", KEY_HOME, 0) },
        { "the ~ keys",                     ONE("\x1b[1~", KEY_HOME, 0) && ONE("\x1b[3~", KEY_DELETE, 0) &&
                                            ONE("\x1b[4~", KEY_END, 0) && ONE("\x1b[5~", KEY_PGUP, 0) &&
                                            ONE("\x1b[6~", KEY_PGDN, 0) && ONE("\x1b[2~", KEY_INSERT, 0) },
        { "xterm modifiers: Shift-Left, Ctrl-Right, Shift-End",
                                            ONE("\x1b[1;2D", KEY_LEFT, KMOD_SHIFT) &&
                                            ONE("\x1b[1;5C", KEY_RIGHT, KMOD_CTRL) &&
                                            ONE("\x1b[1;2F", KEY_END, KMOD_SHIFT) &&
                                            ONE("\x1b[3;2~", KEY_DELETE, KMOD_SHIFT) &&
                                            ONE("\x1b[1;6D", KEY_LEFT, KMOD_SHIFT | KMOD_CTRL) },
        { "CSI-u: Super+c, Super+Shift+z, Super+[",
                                            ONE("\x1b[99;9u", 'c', KMOD_SUPER) &&
                                            ONE("\x1b[122;10u", 'z', KMOD_SUPER | KMOD_SHIFT) &&
                                            ONE("\x1b[91;9u", '[', KMOD_SUPER) },
        { "unknown sequences are consumed whole",
                                            ONE("\x1b[?25h", KEY_UNKNOWN, 0) && ONE("\x1b[15~", KEY_UNKNOWN, 0) &&
                                            ONE("\x1b[200;1;2x", KEY_UNKNOWN, 0) },
        { "Shift-Tab",                      ONE("\x1b[Z", '\t', KMOD_SHIFT) && PUSH("\x1b[Z", '\t', KMOD_SHIFT) },
        { "pushed bytes: the same keys",    PUSH("a", 'a', 0) && PUSH("\x1b", 0x1b, 0) && PUSH("\xc3\xa4", 0xe4, 0) &&
                                            PUSH("\x1b[A", KEY_UP, 0) && PUSH("\x1bOC", KEY_RIGHT, 0) &&
                                            PUSH("\x1b[5~", KEY_PGUP, 0) && PUSH("\x1b[1;2D", KEY_LEFT, KMOD_SHIFT) &&
                                            PUSH("\x1b[99;9u", 'c', KMOD_SUPER) && PUSH("\x1bw", 'w', KMOD_ALT) &&
                                            PUSH("\x1b[?25h", KEY_UNKNOWN, 0) },
        { "reverse encode round-trip",      ENC('a', 0, "a") && ENC(0x20ac, 0, "\xe2\x82\xac") &&
                                            ENC(KEY_UP, 0, "\x1b[A") && ENC(KEY_LEFT, KMOD_SHIFT, "\x1b[1;2D") &&
                                            ENC(KEY_PGUP, 0, "\x1b[5~") && ENC(KEY_DELETE, KMOD_SHIFT, "\x1b[3;2~") &&
                                            ENC('\t', KMOD_SHIFT, "\x1b[Z") && ENC('w', KMOD_ALT, "\x1bw") &&
                                            ENC('c', KMOD_SUPER, "\x1b[99;9u") },
    };
    cprintf("keyseq selftest:\n");
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        cprintf("  [%s] %s\n", c[i].ok ? "ok" : "FAIL", c[i].name);
        if (!c[i].ok) fails++;
    }
    cprintf("%s\n", fails ? "KEYSEQ_SELFTEST_FAIL" : "KEYSEQ_SELFTEST_OK");
    return fails;
}
