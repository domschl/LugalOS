#ifndef LUGALOS_KERNEL_CURSOR_H
#define LUGALOS_KERNEL_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

#define CURSOR_W 16
#define CURSOR_H 16

typedef struct {
    int      x;
    int      y;
    bool     visible;
    uint8_t  buttons;
    int      wheel;
    int      speed_factor; /* default 100 = 1.0x */
    uint32_t moves_count;
    uint32_t clicks_count;
} cursor_state_t;

/* Initialize cursor engine */
void cursor_init(void);

/* Update cursor position and state from mouse driver reports */
void cursor_update(int x, int y, uint8_t buttons, int wheel);

/* Blit / refresh cursor on the given scanout buffer */
void cursor_render(uint16_t *fb, unsigned fb_w, unsigned fb_h);
void cursor_restore(uint16_t *fb, unsigned fb_w, unsigned fb_h);

/* Custom background restore callback (e.g. for ground-truth bitmap backing) */
typedef void (*cursor_restore_fn)(uint16_t *fb, unsigned fb_w, unsigned fb_h, int x, int y);
void cursor_set_restore_cb(cursor_restore_fn cb);

/* Diagnostics and configuration */
void cursor_cmd_status(void);
void cursor_set_speed(int speed);
void cursor_set_visible(bool visible);
const cursor_state_t *cursor_get_state(void);

#endif /* LUGALOS_KERNEL_CURSOR_H */
