/* user/ceres/include/ceres_platform.h — Ceres Alpha Platform Hardware Abstraction Layer (HAL) */
#ifndef CERES_PLATFORM_H
#define CERES_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Abstract Input Event Types */
typedef enum {
    CERES_EVENT_NONE = 0,
    CERES_EVENT_QUIT,
    CERES_EVENT_KEY_DOWN,
    CERES_EVENT_KEY_UP,
} ceres_event_type_t;

/* Standard Key Codes for cross-platform arcade controls */
typedef enum {
    CERES_KEY_UNKNOWN = 0,
    CERES_KEY_ESCAPE,
    CERES_KEY_RETURN,
    CERES_KEY_SPACE,     /* Primary Laser Fire */
    CERES_KEY_B,         /* Secondary Bomb Drop */
    CERES_KEY_K,         /* Alternate Bomb Drop */
    CERES_KEY_UP,        /* Thrust Up */
    CERES_KEY_DOWN,      /* Thrust Down */
    CERES_KEY_LEFT,      /* Thrust Backward */
    CERES_KEY_RIGHT,     /* Thrust Forward */
    CERES_KEY_W,         /* WASD Up */
    CERES_KEY_A,         /* WASD Left */
    CERES_KEY_S,         /* WASD Down */
    CERES_KEY_D,         /* WASD Right */
    CERES_KEY_P,         /* Pause / Unpause */
    CERES_KEY_Q,         /* Quit */
    CERES_KEY_R,         /* Restart / Retry */
    CERES_KEY_V,         /* Volume Adjust Mode / Cycle */
    CERES_KEY_0,
    CERES_KEY_1,
    CERES_KEY_2,
    CERES_KEY_3,
    CERES_KEY_4,
    CERES_KEY_5,
    CERES_KEY_6,
    CERES_KEY_7,
    CERES_KEY_8,
    CERES_KEY_9,
    CERES_KEY_MINUS,     /* Volume Down */
    CERES_KEY_PLUS,      /* Volume Up */
} ceres_key_t;

#define CERES_MOD_SHIFT 1

typedef struct {
    ceres_event_type_t type;
    uint32_t              key;
    uint8_t               mods;
} ceres_event_t;

/* Platform Lifecycle & Display Management */
bool      ceres_platform_init(int width, int height, const char *title);
void      ceres_platform_shutdown(void);
int       ceres_platform_get_width(void);
int       ceres_platform_get_height(void);

/* Timing & Delays */
uint32_t  ceres_platform_get_ticks_ms(void);
uint32_t  ceres_platform_get_ticks_us(void);
void      ceres_platform_delay_ms(uint32_t ms);

/* 16-Bit RGB565 Framebuffer Access & Hardware Present */
uint16_t* ceres_platform_get_framebuffer(int *stride_pixels);
void      ceres_platform_present(void);

/* Input Event Polling */
bool      ceres_platform_poll_event(ceres_event_t *out_ev);

/* Audio Hardware Volume (0..100 percent) */
void      ceres_platform_set_volume(int percent);

/* Diagnostic Logging */
void      ceres_platform_log(const char *fmt, ...);

#endif /* CERES_PLATFORM_H */
