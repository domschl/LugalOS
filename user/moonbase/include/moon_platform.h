#ifndef MOON_PLATFORM_H
#define MOON_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Abstract Input Event Types */
typedef enum {
    MOON_EVENT_NONE = 0,
    MOON_EVENT_QUIT,
    MOON_EVENT_KEY_DOWN,
    MOON_EVENT_KEY_UP,
    MOON_EVENT_POINTER_DOWN,
    MOON_EVENT_POINTER_UP,
    MOON_EVENT_POINTER_MOVE,
    MOON_EVENT_SCROLL,
    MOON_EVENT_POINTER_LEAVE,   /* Pointer left the window */
} moon_event_type_t;

/* Standard Key Codes for cross-platform handling */
enum {
    MOON_KEY_UNKNOWN = 0,
    MOON_KEY_ESCAPE,
    MOON_KEY_RETURN,
    MOON_KEY_SPACE,
    MOON_KEY_BACKSPACE,
    MOON_KEY_TAB,
    MOON_KEY_UP,
    MOON_KEY_DOWN,
    MOON_KEY_LEFT,
    MOON_KEY_RIGHT,
    MOON_KEY_W,
    MOON_KEY_A,
    MOON_KEY_S,
    MOON_KEY_D,
    MOON_KEY_Q,
    MOON_KEY_E,
    MOON_KEY_R,
    MOON_KEY_0,
    MOON_KEY_1,
    MOON_KEY_2,
    MOON_KEY_3,
    MOON_KEY_4,
    MOON_KEY_5,
    MOON_KEY_6,
    MOON_KEY_7,
    MOON_KEY_8,
    MOON_KEY_9,
    MOON_KEY_T,
    MOON_KEY_Z,
    MOON_KEY_P,
    MOON_KEY_MINUS,
    MOON_KEY_PLUS,
    MOON_KEY_Y,
    MOON_KEY_N,
    MOON_KEY_C,
    MOON_KEY_O,
    MOON_KEY_F,
    MOON_KEY_B,
};

#define MOON_MOD_SHIFT 1

typedef struct {
    moon_event_type_t type;
    int32_t x;              /* Screen X coordinate (pixels) */
    int32_t y;              /* Screen Y coordinate (pixels) */
    int32_t dx;             /* Relative motion delta X */
    int32_t dy;             /* Relative motion delta Y */
    uint32_t key;           /* Key code (MOON_KEY_*) */
    uint8_t  touch;         /* 1 if the pointer event comes from a finger */
    uint8_t  mods;          /* MOON_MOD_* held with a key */
    uint8_t  button;        /* Pointer button: 1=primary/touch, 2=middle, 3=secondary */
    int16_t  wheel_x;       /* Scroll wheel X */
    int16_t  wheel_y;       /* Scroll wheel Y */
} moon_event_t;

/* Platform Lifecycle & Window Management */
bool     moon_platform_init(int width, int height, const char *title);
void     moon_platform_shutdown(void);
int      moon_platform_get_width(void);
int      moon_platform_get_height(void);

/* Timing & Delays */
uint32_t moon_platform_get_ticks_ms(void);
uint32_t moon_platform_get_ticks_us(void);   /* wraps after about 71 minutes: use differences */
void     moon_platform_delay_ms(uint32_t ms);

/* 16-Bit RGB565 Framebuffer Access */
uint16_t* moon_platform_get_framebuffer(int *stride_pixels);
void      moon_platform_present(void);

/* Partial updates: draw into the buffer that is being shown (not the one frames are composed
 * in, which on a double-buffered board is a frame old) and publish only some rows of it. */
uint16_t* moon_platform_get_display_framebuffer(int *stride_pixels);
void      moon_platform_present_rows(int y0, int y1);   /* rows [y0, y1) */

/* Input Event Polling */
bool     moon_platform_poll_event(moon_event_t *ev);

/* Debug / Logging output (maps to printf/stderr on host, cprintf/klog on LugalOS) */
void     moon_platform_log(const char *fmt, ...);

#endif /* MOON_PLATFORM_H */
