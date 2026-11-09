#ifndef LUGALOS_DRIVERS_UI_SOUND_H
#define LUGALOS_DRIVERS_UI_SOUND_H

/* UI sounds (drivers/ui_sound.c). */

/* A key of the on-screen keyboard was typed: a short click, if the
 * `ui.click_sound` setting is on and the board has sound (the ESP32-P4
 * boards) and no program has the sound. Returns at once, from any task. */
void ui_click(void);

#endif /* LUGALOS_DRIVERS_UI_SOUND_H */
