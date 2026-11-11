/* The window manager's Cmd keys. See kernel/include/kernel/wmkeys.h. */

#include "kernel/wmkeys.h"
#include "kernel/console.h"
#include "kernel/keyseq.h"

#include <stdbool.h>

#define WM_ANY_SHIFT 0x80u   /* in wm_key_t.mods: Shift or not, the same key */

typedef struct {
    uint32_t key;     /* unshifted */
    uint8_t  mods;    /* KMOD_SHIFT / KMOD_CTRL beside Cmd, exactly; or WM_ANY_SHIFT */
    uint8_t  hotkey;  /* CONSOLE_HOTKEY_* */
} wm_key_t;

/* Cmd+1..9 are the jumps, outside the table. */
static const wm_key_t k_wm_keys[] = {
    { '\r',      WM_ANY_SHIFT, CONSOLE_HOTKEY_NEW_TERM },
    { 'w',       WM_ANY_SHIFT, CONSOLE_HOTKEY_CLOSE },
    { 'o',       WM_ANY_SHIFT, CONSOLE_HOTKEY_ORIENT },
    { '3',       KMOD_SHIFT,   CONSOLE_HOTKEY_SCREENSHOT },
    { 'f',       0,            CONSOLE_HOTKEY_TOGGLE_MAX },
    { 'f',       KMOD_SHIFT,   CONSOLE_HOTKEY_FULLSCREEN },
    { 'r',       0,            CONSOLE_HOTKEY_WIDTH_NEXT },
    { 'r',       KMOD_SHIFT,   CONSOLE_HOTKEY_HEIGHT_NEXT },
    { '-',       0,            CONSOLE_HOTKEY_WIDTH_DOWN },
    { '=',       0,            CONSOLE_HOTKEY_WIDTH_UP },
    { '-',       KMOD_SHIFT,   CONSOLE_HOTKEY_HEIGHT_DOWN },
    { '=',       KMOD_SHIFT,   CONSOLE_HOTKEY_HEIGHT_UP },
    { '[',       0,            CONSOLE_HOTKEY_JOIN_LEFT },
    { ']',       0,            CONSOLE_HOTKEY_JOIN_RIGHT },
    { ',',       0,            CONSOLE_HOTKEY_SETTINGS },
    { KEY_LEFT,  0,            CONSOLE_HOTKEY_FOCUS_LEFT },
    { KEY_RIGHT, 0,            CONSOLE_HOTKEY_FOCUS_RIGHT },
    { KEY_UP,    0,            CONSOLE_HOTKEY_FOCUS_UP },
    { KEY_DOWN,  0,            CONSOLE_HOTKEY_FOCUS_DOWN },
    { KEY_LEFT,  KMOD_CTRL,    CONSOLE_HOTKEY_MOVE_LEFT },
    { KEY_RIGHT, KMOD_CTRL,    CONSOLE_HOTKEY_MOVE_RIGHT },
    { KEY_UP,    KMOD_CTRL,    CONSOLE_HOTKEY_MOVE_UP },
    { KEY_DOWN,  KMOD_CTRL,    CONSOLE_HOTKEY_MOVE_DOWN },
};

/* A shifted ASCII character as its key and Shift (US layout, as the
 * keyboard driver's). */
static uint32_t unshift(uint32_t key, uint8_t *mods) {
    if (key >= 'A' && key <= 'Z') { *mods |= KMOD_SHIFT; return key + 32u; }
    switch (key) {
    case '_': *mods |= KMOD_SHIFT; return '-';
    case '+': *mods |= KMOD_SHIFT; return '=';
    case '{': *mods |= KMOD_SHIFT; return '[';
    case '}': *mods |= KMOD_SHIFT; return ']';
    case '#': *mods |= KMOD_SHIFT; return '3';
    case '\n': return '\r';
    default:  return key;
    }
}

unsigned wm_hotkey(uint32_t key, uint8_t mods) {
    if (!(mods & KMOD_SUPER) || (mods & KMOD_ALT)) return CONSOLE_HOTKEY_NONE;
    mods &= (uint8_t)(KMOD_SHIFT | KMOD_CTRL);
    key = unshift(key, &mods);
    for (unsigned i = 0; i < sizeof k_wm_keys / sizeof k_wm_keys[0]; i++) {
        const wm_key_t *e = &k_wm_keys[i];
        if (e->key != key) continue;
        bool match = (e->mods & WM_ANY_SHIFT) ? !(mods & KMOD_CTRL) : mods == e->mods;
        if (match) return e->hotkey;
    }
    if (mods == 0 && key >= '1' && key <= '9') return CONSOLE_HOTKEY_JUMP(key - '0');
    return CONSOLE_HOTKEY_NONE;
}

uint32_t wm_usage_key(uint32_t u) {
    if (u >= 0x04u && u <= 0x1du) return 'a' + (u - 0x04u);
    if (u >= 0x1eu && u <= 0x26u) return '1' + (u - 0x1eu);
    switch (u) {
    case 0x27u: return '0';
    case 0x28u: case 0x58u: return '\r';
    case 0x2du: return '-';
    case 0x2eu: return '=';
    case 0x2fu: return '[';
    case 0x30u: return ']';
    case 0x4fu: return KEY_RIGHT;
    case 0x50u: return KEY_LEFT;
    case 0x51u: return KEY_DOWN;
    case 0x52u: return KEY_UP;
    default:    return 0;
    }
}
