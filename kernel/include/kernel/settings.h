#ifndef LUGALOS_KERNEL_SETTINGS_H
#define LUGALOS_KERNEL_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/* The settings store (kernel/settings.c).
 *
 * Typed keys with defaults and limits, in a sorted .rodata table: an integer
 * in a range, or one of a list of names. Setting one takes effect at once
 * (the volume, the backlight, the Tab5's orientation are applied here) and
 * is written to /sd0/system/settings -- /flash0/system/settings where there
 * is no card -- about two seconds after the last change, so a run of
 * stepper taps is one write. The file is `key=value` lines; it is read and
 * applied at boot (settings_load()). Values are the names for the named
 * kinds ("portrait"), numbers for the others.
 *
 * A change is told to every window as WIN_EV_SETTINGS (kernel/event.h),
 * its key the setting's index (settings_at()).
 *
 * Keys today: audio.volume, display.brightness, display.orient (the Tab5),
 * display.warm_ink, display.warmth (drivers/paper.h), input.natural_scroll,
 * input.pointer_speed, launcher.face, ui.click_sound, ui.osk_preview,
 * ui.text_size. */

typedef enum { SETTING_INT, SETTING_BOOL, SETTING_ENUM } setting_kind_t;

typedef struct {
    const char         *key;
    setting_kind_t      kind;
    int16_t             def, min, max;   /* ENUM: indices into names; BOOL: 0 / 1 */
    const char *const  *names;           /* ENUM's names, max + 1 of them */
} setting_def_t;

/* The setting called key, or NULL. */
const setting_def_t *settings_find(const char *key);
const setting_def_t *settings_at(unsigned i);       /* by key, until NULL */

/* key's value (its default when unknown to the file); -1 for an unknown key. */
int  settings_get(const char *key);

/* key set to v, clamped to its range, applied, and written soon. False for
 * an unknown key. */
bool settings_set(const char *key, int v);

/* The same from text: a number, a name of an ENUM's, or on/off/true/false
 * for a BOOL. False when it is none of those. */
bool settings_set_text(const char *key, const char *text);

/* key's value as text into out ("70", "portrait", "on"). */
void settings_format(const setting_def_t *d, int v, char *out, unsigned cap);

/* At boot, after the volumes are mounted: the file read and every setting
 * applied. */
void settings_load(void);

/* From a task that runs often (the shell's idle turn): the file written when
 * the last change is two seconds old. */
void settings_poll(void);

/* The index of key in the table (settings_at()), or -1. */
int  settings_index(const char *key);

/* `settings check`: the table sorted, every default in range. Returns the faults. */
int settings_check(void);

#endif /* LUGALOS_KERNEL_SETTINGS_H */
