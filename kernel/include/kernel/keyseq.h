#ifndef LUGALOS_KERNEL_KEYSEQ_H
#define LUGALOS_KERNEL_KEYSEQ_H

#include <stdbool.h>
#include <stdint.h>

/* Bytes from a terminal to keys -- 37.5a, plan/phase37_screen_layouts_and_apps.md.
 *
 * One parser for every text input, so that every one understands the same
 * keys. It reads what a modern terminal sends -- and since 37.5a what the
 * RP2350-LCD-7's USB keyboard sends, deliberately the same bytes:
 *
 *   text            UTF-8, assembled into one code point
 *   controls        0x00-0x1F and 0x7F as themselves (Ctrl-Space is 0x00)
 *   ESC x           x with KMOD_ALT (Alt sends an ESC prefix)
 *   ESC alone       0x1B, when nothing follows within a short wait
 *   ESC [ A..D      arrows; ESC [ H / F Home / End; ESC O x the same (SS3)
 *   ESC [ n ~       1/7 Home, 2 Insert, 3 Delete, 4/8 End, 5 PgUp, 6 PgDn
 *   ESC [ 1 ; m X   an arrow/Home/End with modifiers (xterm), m = 1 + mods
 *   ESC [ n ; m ~   the same for the ~ keys
 *   ESC [ cp ; m u  CSI-u (kitty, foot, xterm): code point cp with mods --
 *                   how Super (Cmd) arrives, e.g. Super+c is ESC [ 99 ; 9 u
 *
 * Anything else is consumed whole and reported as KEY_UNKNOWN, never typed:
 * a sequence printed as text is how `~` ended up in the line editor in 36.9. */

enum {
    KEY_UP = 0x110000,          /* past the last Unicode code point */
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_INSERT,
    KEY_DELETE,
    KEY_PGUP,
    KEY_PGDN,
    KEY_UNKNOWN,
};

#define KMOD_SHIFT 0x1u
#define KMOD_ALT   0x2u
#define KMOD_CTRL  0x4u
#define KMOD_SUPER 0x8u

typedef struct {
    uint32_t key;               /* a code point, a control, or KEY_* */
    uint8_t  mods;              /* KMOD_* */
} key_event_t;

/* Where the bytes come from. getc blocks for the next byte; more says
 * whether another byte arrives within a short wait (it decides a lone ESC). */
typedef struct {
    int  (*getc)(void *ctx);
    bool (*more)(void *ctx);
    void *ctx;
} keyseq_src_t;

key_event_t keyseq_read(const keyseq_src_t *src);

/* The console as a source: console_getc(), and up to 30 ms for `more`. */
key_event_t keyseq_read_console(void);

/* `keyselftest`: every form above from byte strings. Prints
 * KEYSEQ_SELFTEST_OK/_FAIL; returns the failures. */
int keyseq_selftest(void);

#endif /* LUGALOS_KERNEL_KEYSEQ_H */
