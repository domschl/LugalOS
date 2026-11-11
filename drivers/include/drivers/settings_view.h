#ifndef LUGALOS_DRIVERS_SETTINGS_VIEW_H
#define LUGALOS_DRIVERS_SETTINGS_VIEW_H

#include <stdbool.h>

#include "drivers/view.h"

/* The Settings app (drivers/settings_view.c): a view of its own window type
 * (RIBBON_WIN_SETTINGS) over the settings store (kernel/settings.h).
 *
 * Sections Display, Sound, Input, Power, Network, Storage, About: a list of
 * them beside the open one, or -- in a narrow window -- the list, and a
 * section with a Back button over it. Every control by touch, by keys
 * through ugui's focus (Tab, arrows, Enter; Left / Right step a slider; Esc
 * goes back) and by mouse. A change is the store's: applied at once, saved
 * two seconds later, told to every window as WIN_EV_SETTINGS. */

extern const view_ops_t g_settings_view;

/* The sections, in the order they are listed. */
enum {
    SETTINGS_SECTION_DISPLAY, SETTINGS_SECTION_SOUND, SETTINGS_SECTION_INPUT, SETTINGS_SECTION_POWER,
    SETTINGS_SECTION_NETWORK, SETTINGS_SECTION_STORAGE, SETTINGS_SECTION_ABOUT, SETTINGS_SECTION_COUNT
};

/* Opens the window, or focuses it when it is open, at the section it was
 * last left at. From any task. False when there is no room for it (or no
 * screen). */
bool settings_view_open(void);
/* The same at section (SETTINGS_SECTION_*; -1: as it was left): the menu
 * bar's status icons, the battery. */
bool settings_view_open_at(int section);

/* `settings view`: open or not, the section, and the widgets' hit-boxes in
 * window coordinates (as `launcher` prints them, for `touch tap`). */
void settings_view_report(void);

#endif /* LUGALOS_DRIVERS_SETTINGS_VIEW_H */
