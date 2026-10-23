/* user/moonbase/src/platform_p4.c — Bare-Metal LugalOS HAL for ESP32-P4
 *
 * Implements moon_platform.h on ESP32-P4 (LCD-7B and Tab5 presets).
 * Connects directly to the LugalOS RGB565 PSRAM canvas buffer (buf565),
 * receives native touch events from GT911/ST7121 with gesture suppression,
 * and handles presentation via cache writeback and hardware double-buffering.
 */

#include "moon_platform.h"
#include "moon_config.h"
#include "moon_game.h"
#include "moonbase.h"

#include "lugalos_config.h"
#include "arch/esp32p4_intr.h"
#include "drivers/dsi_esp32p4.h"
#include "drivers/lcdterm.h"
#include "drivers/touch.h"
#include "drivers/screen.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#include <string.h>

#if defined(CONFIG_BOARD_ESP32P4)

/* 35 KB game state in PSRAM, zero SRAM footprint */
static moon_game_t s_p4_game BULK_BSS;

static int       s_width = 1024;
static int       s_height = 600;
static uint32_t  s_stride = 1024;
static uint16_t *s_fb = NULL;         /* Canvas slot buffer (visible to compositor) */
static uint16_t *s_render_fb = NULL;  /* Off-screen render buffer (drawn by game) */
static int       s_cid = -1;
static bool      s_allocated_fb = false;
static uint32_t  s_fb_pages = 0;
static uint32_t  s_render_pages = 0;

/* Touch event queue */
#define EVENT_QUEUE_SIZE 32
static moon_event_t s_queue[EVENT_QUEUE_SIZE];
static volatile int s_qhead = 0;
static volatile int s_qtail = 0;
static int s_last_touch_x = 0;
static int s_last_touch_y = 0;

static void queue_push(const moon_event_t *ev) {
    int next = (s_qhead + 1) % EVENT_QUEUE_SIZE;
    if (next != s_qtail) {
        s_queue[s_qhead] = *ev;
        s_qhead = next;
    }
}

static bool p4_touch_hook(int x, int y, int kind) {
    moon_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.x = x;
    ev.y = y;
    ev.dx = x - s_last_touch_x;
    ev.dy = y - s_last_touch_y;
    s_last_touch_x = x;
    s_last_touch_y = y;
    ev.button = 1;

    if (kind == 1) {
        ev.type = MOON_EVENT_POINTER_DOWN;
        queue_push(&ev);
    } else if (kind == 2) {
        ev.type = MOON_EVENT_POINTER_MOVE;
        queue_push(&ev);
    } else if (kind == 3) {
        ev.type = MOON_EVENT_POINTER_UP;
        queue_push(&ev);
    }
    return true; /* Suppress desktop ribbon swipe gesture while game is active */
}

bool moon_platform_init(int width, int height, const char *title) {
    (void)width;
    (void)height;
    (void)title;
    s_qhead = s_qtail = 0;

#if defined(CONFIG_BOARD_TAB5)
    s_width = MOON_PRESET_TAB5_W;   /* 1280 */
    s_height = MOON_PRESET_TAB5_H;  /* 720 */
    /* Ensure Tab5 is in landscape orientation */
    if (dsi_lcd_get_orientation() != 270) {
        dsi_lcd_set_orientation(270);
    }
#else
    s_width = MOON_PRESET_LCD7B_W;  /* 1024 */
    s_height = MOON_PRESET_LCD7B_H; /* 600 */
#endif
    s_stride = (uint32_t)s_width;

    /* 1. Open or focus RGB565 true full-screen canvas "moonbase" (no chrome/decorations) */
    uint8_t req[SCREEN_CTITLE_MAX + 3];
    req[0] = 'L';
    req[1] = SCREEN_LAYOUT_FULLSCREEN | 0x80u; /* RGB565 True Fullscreen Canvas */
    const char *cname = "moonbase";
    size_t nl = strlen(cname);
    memcpy(req + 2, cname, nl);
    req[2 + nl] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 3 + (uint32_t)nl, reply);

    /* 2. Retrieve canvas backing buffer and slot ID */
    s_cid = lcdterm_get_canvas565_cid_by_name("moonbase");
    s_fb = lcdterm_get_canvas565_by_name("moonbase", &s_stride, &s_width, &s_height);
    if (!s_fb) {
        /* Fallback: try slot 0 */
        s_cid = 0;
        s_fb = lcdterm_get_canvas565_backing(0, &s_stride, &s_width, &s_height);
    }

    uint32_t bytes = (uint32_t)s_width * s_height * sizeof(uint16_t);
    uint32_t pages = (bytes + 4095u) / 4096u;

    if (!s_fb) {
        /* Direct PSRAM allocation fallback */
        s_fb_pages = pages;
        s_fb = (uint16_t *)palloc_pages_bulk(s_fb_pages);
        s_allocated_fb = (s_fb != NULL);
        s_stride = (uint32_t)s_width;
    }

    if (!s_fb) {
        printk("[moonbase] Failed to allocate or acquire RGB565 framebuffer in PSRAM\n");
        return false;
    }

    /* Allocate secondary off-screen render buffer in PSRAM for tear-free presentation */
    s_render_pages = pages;
    s_render_fb = (uint16_t *)palloc_pages_bulk(s_render_pages);
    if (!s_render_fb) {
        printk("[moonbase] Warning: failed to allocate secondary buffer, using single-buffered rendering\n");
        s_render_fb = s_fb;
    }

    /* Clear both framebuffers to black */
    memset(s_fb, 0, (size_t)s_stride * s_height * sizeof(uint16_t));
    esp32p4_extmem_writeback((uintptr_t)s_fb, (size_t)s_stride * s_height * sizeof(uint16_t));
    if (s_render_fb != s_fb) {
        memset(s_render_fb, 0, (size_t)s_stride * s_height * sizeof(uint16_t));
        esp32p4_extmem_writeback((uintptr_t)s_render_fb, (size_t)s_stride * s_height * sizeof(uint16_t));
    }

    /* Hook touch subsystem */
    touch_set_hook(p4_touch_hook);

    printk("[moonbase] Bare-metal platform initialized: %dx%d (stride=%lu) at %p (render_fb=%p, cid=%d)\n",
           s_width, s_height, (unsigned long)s_stride, (void *)s_fb, (void *)s_render_fb, s_cid);
    return true;
}

void moon_platform_shutdown(void) {
    touch_set_hook(NULL);

    /* Close canvas and return to text terminal */
    uint8_t req[SCREEN_CTITLE_MAX + 3];
    req[0] = 'L';
    req[1] = SCREEN_LAYOUT_TEXT;
    const char *cname = "moonbase";
    size_t nl = strlen(cname);
    memcpy(req + 2, cname, nl);
    req[2 + nl] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 3 + (uint32_t)nl, reply);

    if (s_render_fb && s_render_fb != s_fb && s_render_pages > 0) {
        palloc_free(s_render_fb, s_render_pages);
        s_render_fb = NULL;
        s_render_pages = 0;
    }

    if (s_allocated_fb && s_fb && s_fb_pages > 0) {
        palloc_free(s_fb, s_fb_pages);
        s_fb = NULL;
        s_allocated_fb = false;
        s_fb_pages = 0;
    }

    /* Force full terminal repaint and hardware VSYNC scanout flip to purge residual game graphics */
    lcdterm_repaint();
    lcdterm_sync_post();
    if (lcdterm_is_double_buffered()) {
        lcdterm_sync_post();
    }
}

int moon_platform_get_width(void) {
    return s_width;
}

int moon_platform_get_height(void) {
    return s_height;
}

uint32_t moon_platform_get_ticks_ms(void) {
    return (uint32_t)time_get_ms();
}

void moon_platform_delay_ms(uint32_t ms) {
    task_sleep_ms(ms);
}

uint16_t* moon_platform_get_framebuffer(int *stride_pixels) {
    if (stride_pixels) *stride_pixels = (int)s_stride;
    return s_render_fb ? s_render_fb : s_fb;
}

void moon_platform_present(void) {
    if (!s_render_fb) return;

    /* Write back CPU cache lines for the completed off-screen frame */
    esp32p4_extmem_writeback((uintptr_t)s_render_fb, (size_t)s_stride * s_height * sizeof(uint16_t));

    /* Swap canvas backing buffer atomically so compositor sees the completed frame */
    if (s_render_fb != s_fb && s_cid >= 0) {
        console_lock();
        uint16_t *tmp = s_fb;
        s_fb = s_render_fb;
        s_render_fb = tmp;
        lcdterm_set_canvas565_backing((uint8_t)s_cid, s_fb);
        lcdterm_damage();
        console_unlock();
    } else {
        lcdterm_damage();
    }
}

bool moon_platform_poll_event(moon_event_t *ev) {
    /* 1. Touch events */
    if (s_qhead != s_qtail) {
        *ev = s_queue[s_qtail];
        s_qtail = (s_qtail + 1) % EVENT_QUEUE_SIZE;
        return true;
    }

    /* 2. Keyboard events */
    if (console_has_char()) {
        char c = console_getc();
        memset(ev, 0, sizeof(*ev));
        ev->type = MOON_EVENT_KEY_DOWN;
        switch (c) {
            case 'q': case 'Q': case 27: /* ESC */
                ev->key = MOON_KEY_Q;
                ev->type = MOON_EVENT_QUIT;
                break;
            case ' ': ev->key = MOON_KEY_SPACE; break;
            case '\r': case '\n': ev->key = MOON_KEY_RETURN; break;
            case '\t': ev->key = MOON_KEY_TAB; break;
            case 'w': case 'W': ev->key = MOON_KEY_W; break;
            case 'a': case 'A': ev->key = MOON_KEY_A; break;
            case 's': case 'S': ev->key = MOON_KEY_S; break;
            case 'd': case 'D': ev->key = MOON_KEY_D; break;
            case 'r': case 'R': ev->key = MOON_KEY_R; break;
            case '0': ev->key = MOON_KEY_0; break;
            case '1': ev->key = MOON_KEY_1; break;
            case '2': ev->key = MOON_KEY_2; break;
            case '3': ev->key = MOON_KEY_3; break;
            case '4': ev->key = MOON_KEY_4; break;
            case '5': ev->key = MOON_KEY_5; break;
            case '6': ev->key = MOON_KEY_6; break;
            case '7': ev->key = MOON_KEY_7; break;
            default:  ev->key = MOON_KEY_UNKNOWN; break;
        }
        return true;
    }

    /* 3. Ctrl-C interrupt */
    if (console_interrupt_requested()) {
        memset(ev, 0, sizeof(*ev));
        ev->type = MOON_EVENT_QUIT;
        return true;
    }

    return false;
}

void moon_platform_log(const char *fmt, ...) {
    (void)fmt;
}

int moonbase_run(void) {
    if (!moon_platform_init(0, 0, "Moonbase Lambda")) {
        cprintf("moonbase: failed to initialize bare-metal platform\n");
        return -1;
    }

    int w = moon_platform_get_width();
    int h = moon_platform_get_height();

    if (!moon_game_init(&s_p4_game, w, h)) {
        cprintf("moonbase: failed to initialize game engine\n");
        moon_platform_shutdown();
        return -1;
    }

    uint32_t last_time = moon_platform_get_ticks_ms();

    while (s_p4_game.running) {
        if (console_interrupt_requested()) break;

        uint32_t now = moon_platform_get_ticks_ms();
        uint32_t delta = now - last_time;
        if (delta < 1) delta = 1;
        last_time = now;

        moon_event_t ev;
        while (moon_platform_poll_event(&ev)) {
            moon_game_handle_event(&s_p4_game, &ev);
        }

        moon_game_update(&s_p4_game, delta);

        int stride = 0;
        uint16_t *fb = moon_platform_get_framebuffer(&stride);
        if (fb) {
            moon_game_render(&s_p4_game, fb, w, h, stride);
            moon_platform_present();
        }

        moon_platform_delay_ms(16); /* ~60 FPS cap */
    }

    moon_platform_shutdown();
    return 0;
}

#else

/* Stub for non-ESP32-P4 targets */
int moonbase_run(void) {
    cprintf("moonbase: requires ESP32-P4 target with RGB565 display.\n");
    return -1;
}

#endif
