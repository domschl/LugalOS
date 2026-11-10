#ifndef LUGALOS_KERNEL_WMKEYS_H
#define LUGALOS_KERNEL_WMKEYS_H

#include <stdint.h>

/* The window manager's Cmd keys: one table for every input path (the USB
 * keyboard, a serial terminal's CSI-u through the line editor, a view's keys),
 * so they cannot disagree. The full map is in doc/user/ribbon.md.
 *
 * key is a code point or KEY_* (kernel/keyseq.h), mods KMOD_*. A shifted
 * character is matched as its unshifted key with KMOD_SHIFT ('F' as Shift+f,
 * '+' as Shift+=, '#' as Shift+3), so a USB usage and any terminal's CSI-u
 * spelling of the same chord agree.
 *
 * Returns the CONSOLE_HOTKEY_* code the chord stands for, or
 * CONSOLE_HOTKEY_NONE when it is an application's. Up/Down codes are
 * returned as such: the console resolves them by the ribbon's axis. */
unsigned wm_hotkey(uint32_t key, uint8_t mods);

/* The USB HID usage `u` (keyboard page) as the unshifted key wm_hotkey()
 * takes, or 0 when the window manager has no key there. */
uint32_t wm_usage_key(uint32_t u);

#endif /* LUGALOS_KERNEL_WMKEYS_H */
