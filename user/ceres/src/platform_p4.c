/* user/ceres/src/platform_p4.c — Bare-Metal LugalOS HAL for ESP32-P4
 *
 * Implements ceres_platform.h on ESP32-P4 (Waveshare LCD-7B and M5Stack Tab5).
 * Connects to LugalOS RGB565 PSRAM canvas buffer, manages tear-free double-buffering
 * and hardware PPA SRM 2D-DMA acceleration, and processes keyboard input via keyseq.
 */

#include "ceres_platform.h"
#include "ceres_config.h"
#include "ceres_game.h"
#include "ceres.h"

#include "lugalos_config.h"
#include "kernel/lock.h"
#include "arch/esp32p4_intr.h"
#include "drivers/dsi_esp32p4.h"
#include "drivers/ppa_esp32p4.h"
#include "drivers/lcdterm.h"
#include "drivers/screen.h"
#include "kernel/console.h"
#include "kernel/keyseq.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#include <stdarg.h>
#include <string.h>

#if defined(CONFIG_BOARD_ESP32P4)

/* Game state in PSRAM, zero SRAM footprint.
 * single task: the one holding s_ceres_session (ceres_run()). */
static ceres_game_t s_p4_game BULK_BSS;

/* One game at a time: statics and buffers are owned by the active session.
 * accepted: a session_claim_t is claimed and released atomically. */
static session_claim_t s_ceres_session;

#if defined(CONFIG_BOARD_TAB5)
static int       s_scale = 2;         /* accepted: 2x HiDPI integer scale on 720x1280 panel (640x360 -> 1280x720) */
#else
static int       s_scale = 1;         /* accepted: 1x native for 1024x600 display */
#endif

static int       s_width = 1024;      /* single task: the session holder */
static int       s_height = 600;      /* single task: the session holder */
static uint32_t  s_stride = 1024;     /* single task: the session holder */
static int       s_virtual_w = 640;   /* single task: the session holder */
static int       s_virtual_h = 360;   /* single task: the session holder */
static uint16_t *s_fb = NULL;                 /* single task: front buffer visible to scanout */
static uint16_t *s_render_fb = NULL;          /* single task: off-screen render buffer */
static uint16_t *s_canvas_fb_original = NULL; /* single task: original canvas backing */
static uint16_t *s_render_fb_original = NULL; /* single task: allocated buffer */
static int       s_cid = -1;                  /* single task: canvas slot id */
static bool      s_allocated_fb = false;      /* single task: whether s_fb was heap allocated */
static bool      s_active = false;            /* single task: between init and shutdown */
static uint32_t  s_fb_pages = 0;              /* single task: page count */
static uint32_t  s_render_pages = 0;          /* single task: page count */

bool ceres_platform_init(int width, int height, const char *title) {
    (void)width;
    (void)height;
    (void)title;

#if defined(CONFIG_BOARD_TAB5)
    s_scale = 2;
    s_width = CERES_PRESET_TAB5_W;   /* 1280 */
    s_height = CERES_PRESET_TAB5_H;  /* 720 */
    /* Ensure Tab5 is in landscape orientation */
    if (dsi_lcd_get_orientation() != 270) {
        dsi_lcd_set_orientation(270);
    }
#else
    s_scale = 1;
    s_width = CERES_PRESET_LCD7B_W;  /* 1024 */
    s_height = CERES_PRESET_LCD7B_H; /* 600 */
#endif
    s_stride = (uint32_t)s_width;
    s_virtual_w = s_width / s_scale;
    s_virtual_h = s_height / s_scale;

    /* 1. Open or focus RGB565 true full-screen canvas "scroller" (no chrome/decorations) */
    uint8_t req[SCREEN_CTITLE_MAX + 3];
    req[0] = 'L';
    req[1] = SCREEN_LAYOUT_FULLSCREEN | 0x80u; /* RGB565 True Fullscreen Canvas */
    const char *cname = "scroller";
    size_t nl = strlen(cname);
    memcpy(req + 2, cname, nl);
    req[2 + nl] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 3 + (uint32_t)nl, reply);
    s_active = true;

    /* 2. Retrieve canvas backing buffer and slot ID */
    s_cid = lcdterm_get_canvas565_cid_by_name("scroller");
    s_fb = lcdterm_get_canvas565_by_name("scroller", &s_stride, &s_width, &s_height);
    if (!s_fb) {
        s_cid = 0;
        s_fb = lcdterm_get_canvas565_backing(0, &s_stride, &s_width, &s_height);
    }

    uint32_t bytes = (uint32_t)s_width * s_height * sizeof(uint16_t);
    uint32_t pages = (bytes + 4095u) / 4096u;

    if (!s_fb) {
        s_fb_pages = pages;
        s_fb = (uint16_t *)palloc_pages_bulk(s_fb_pages);
        s_allocated_fb = (s_fb != NULL);
        s_stride = (uint32_t)s_width;
    }

    if (!s_fb) {
        printk("[scroller] Failed to allocate or acquire RGB565 framebuffer in PSRAM\n");
        return false;
    }
    s_canvas_fb_original = s_fb;

    /* 3. Allocate secondary off-screen render buffer in PSRAM */
    uint32_t render_bytes = (uint32_t)s_virtual_w * s_virtual_h * sizeof(uint16_t);
    s_render_pages = (render_bytes + 4095u) / 4096u;
    s_render_fb = (uint16_t *)palloc_pages_bulk(s_render_pages);
    s_render_fb_original = s_render_fb;
    if (!s_render_fb) {
        printk("[scroller] Warning: secondary buffer allocation failed, using single buffer\n");
        s_render_fb = s_fb;
        s_render_fb_original = NULL;
        s_scale = 1;
        s_virtual_w = s_width;
        s_virtual_h = s_height;
    }

    /* 4. Clear both framebuffers to black */
    memset(s_fb, 0, (size_t)s_stride * s_height * sizeof(uint16_t));
    esp32p4_extmem_writeback((uintptr_t)s_fb, (size_t)s_stride * s_height * sizeof(uint16_t));
    esp32p4_extmem_invalidate((uintptr_t)s_fb, (size_t)s_stride * s_height * sizeof(uint16_t));
    if (s_render_fb != s_fb) {
        memset(s_render_fb, 0, (size_t)s_virtual_w * s_virtual_h * sizeof(uint16_t));
        esp32p4_extmem_writeback((uintptr_t)s_render_fb, (size_t)s_virtual_w * s_virtual_h * sizeof(uint16_t));
    }
    if (s_cid >= 0) {
        console_lock();
        console_frame_lock();
        lcdterm_set_canvas565_geometry((uint8_t)s_cid, s_fb, (uint32_t)s_virtual_w,
                                       (uint16_t)s_virtual_w, (uint16_t)s_virtual_h);
        console_frame_unlock();
        console_unlock();
    }

    printk("[scroller] Bare-metal platform initialized: %dx%d (virtual %dx%d, scale=%d) fb=%p render=%p cid=%d\n",
           s_width, s_height, s_virtual_w, s_virtual_h, s_scale, (void *)s_fb, (void *)s_render_fb, s_cid);
    return true;
}

void ceres_platform_shutdown(void) {
    if (!s_active) return;
    s_active = false;

    while (console_has_char()) (void)keyseq_read_console();

    /* Restore text terminal */
    uint8_t req[SCREEN_CTITLE_MAX + 3];
    req[0] = 'L';
    req[1] = SCREEN_LAYOUT_TEXT;
    const char *cname = "scroller";
    size_t nl = strlen(cname);
    memcpy(req + 2, cname, nl);
    req[2 + nl] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 3 + (uint32_t)nl, reply);

    if (s_cid >= 0 && s_canvas_fb_original) {
        console_lock();
        console_frame_lock();
        lcdterm_set_canvas565_geometry((uint8_t)s_cid, s_canvas_fb_original,
                                       (uint32_t)s_width, (uint16_t)s_width, (uint16_t)s_height);
        console_frame_unlock();
        console_unlock();
    }

    if (s_render_fb_original && s_render_fb_original != s_canvas_fb_original && s_render_pages > 0) {
        palloc_free(s_render_fb_original, s_render_pages);
        s_render_fb_original = NULL;
        s_render_fb = NULL;
        s_render_pages = 0;
    }

    if (s_allocated_fb && s_canvas_fb_original && s_fb_pages > 0) {
        palloc_free(s_canvas_fb_original, s_fb_pages);
        s_canvas_fb_original = NULL;
        s_fb = NULL;
        s_allocated_fb = false;
        s_fb_pages = 0;
    }

    lcdterm_repaint();
    lcdterm_sync_post();
    if (lcdterm_is_double_buffered()) {
        lcdterm_sync_post();
    }
}

int ceres_platform_get_width(void) {
    return s_virtual_w;
}

int ceres_platform_get_height(void) {
    return s_virtual_h;
}

uint32_t ceres_platform_get_ticks_ms(void) {
    return (uint32_t)time_get_ms();
}

uint32_t ceres_platform_get_ticks_us(void) {
    return (uint32_t)time_get_us();
}

void ceres_platform_delay_ms(uint32_t ms) {
    task_sleep_ms(ms);
}

uint16_t* ceres_platform_get_framebuffer(int *stride_pixels) {
    if (stride_pixels) *stride_pixels = s_virtual_w;
    return s_render_fb ? s_render_fb : s_fb;
}

void ceres_platform_present(void) {
    if (!s_render_fb) return;

    /* Write back rendered virtual frame and swap front/back canvas pointers */
    if (s_render_fb != s_fb && s_cid >= 0) {
        esp32p4_extmem_writeback((uintptr_t)s_render_fb,
                                 (size_t)s_virtual_w * s_virtual_h * sizeof(uint16_t));
        console_lock();
        console_frame_lock();
        uint16_t *tmp = s_fb;
        s_fb = s_render_fb;
        s_render_fb = tmp;
        lcdterm_set_canvas565_geometry((uint8_t)s_cid, s_fb, (uint32_t)s_virtual_w,
                                       (uint16_t)s_virtual_w, (uint16_t)s_virtual_h);
        lcdterm_damage();
        console_frame_unlock();
        console_unlock();
    } else {
        esp32p4_extmem_writeback((uintptr_t)s_render_fb,
                                 (size_t)s_virtual_w * s_virtual_h * sizeof(uint16_t));
        lcdterm_damage();
    }
}

bool ceres_platform_poll_event(ceres_event_t *ev) {
    if (!ev) return false;

    if (console_has_char()) {
        key_event_t ke = keyseq_read_console();
        memset(ev, 0, sizeof(*ev));
        ev->type = CERES_EVENT_KEY_DOWN;
        ev->mods = (ke.mods & KMOD_SHIFT) ? CERES_MOD_SHIFT : 0;
        if (ke.key >= 'A' && ke.key <= 'Z') ev->mods = CERES_MOD_SHIFT;

        switch (ke.key) {
            case KEY_UP:        ev->key = CERES_KEY_UP;     return true;
            case KEY_DOWN:      ev->key = CERES_KEY_DOWN;   return true;
            case KEY_LEFT:      ev->key = CERES_KEY_LEFT;   return true;
            case KEY_RIGHT:     ev->key = CERES_KEY_RIGHT;  return true;
            case 0x1B:          ev->key = CERES_KEY_ESCAPE; return true;
            case 'q': case 'Q': ev->key = CERES_KEY_Q;      return true;
            case ' ':           ev->key = CERES_KEY_SPACE;  return true;
            case '\r': case '\n': ev->key = CERES_KEY_RETURN; return true;
            case 'w': case 'W': ev->key = CERES_KEY_W;      return true;
            case 'a': case 'A': ev->key = CERES_KEY_A;      return true;
            case 's': case 'S': ev->key = CERES_KEY_S;      return true;
            case 'd': case 'D': ev->key = CERES_KEY_D;      return true;
            case 'b': case 'B': ev->key = CERES_KEY_B;      return true;
            case 'k': case 'K': ev->key = CERES_KEY_K;      return true;
            case 'p': case 'P': ev->key = CERES_KEY_P;      return true;
            case 'r': case 'R': ev->key = CERES_KEY_R;      return true;
            default:
                ev->key = CERES_KEY_UNKNOWN;
                return true;
        }
    }
    return false;
}

void ceres_platform_log(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    cprintf("%s", line);
}

static int ceres_run_claimed(const char *args);

int ceres_run(const char *args) {
    if (!session_claim(&s_ceres_session, "scroller")) return -1;
    int rc = ceres_run_claimed(args);
    session_release(&s_ceres_session);
    return rc;
}

static int ceres_run_claimed(const char *args) {
    (void)args;
    if (!ceres_platform_init(0, 0, "Ceres Alpha")) {
        ceres_platform_shutdown();
        cprintf("scroller: failed to initialize bare-metal platform\n");
        return -1;
    }

    int w = ceres_platform_get_width();
    int h = ceres_platform_get_height();

    if (!ceres_game_init(&s_p4_game, w, h)) {
        cprintf("scroller: failed to initialize game engine\n");
        ceres_platform_shutdown();
        return -1;
    }

    uint32_t last_time = ceres_platform_get_ticks_ms();

    while (s_p4_game.running) {
        if (console_interrupt_requested()) break;

        uint32_t now = ceres_platform_get_ticks_ms();
        uint32_t delta = now - last_time;
        if (delta < 1) delta = 1;
        last_time = now;

        ceres_event_t ev;
        while (ceres_platform_poll_event(&ev)) {
            ceres_game_handle_event(&s_p4_game, &ev);
        }

        ceres_game_update(&s_p4_game, delta);

        int stride = 0;
        uint16_t *fb = ceres_platform_get_framebuffer(&stride);
        if (fb) {
            ceres_game_render(&s_p4_game, fb, w, h, stride);
            ceres_platform_present();
        }

        /* 60 FPS frame delay (~16.6 ms) */
        uint32_t spent = ceres_platform_get_ticks_ms() - now;
        ceres_platform_delay_ms(spent >= CERES_FRAME_TIME_MS ? 1 : CERES_FRAME_TIME_MS - spent);
    }

    ceres_platform_shutdown();
    return 0;
}

#else

/* Stub for non-ESP32-P4 targets */
int ceres_run(const char *args) {
    (void)args;
    cprintf("scroller: requires ESP32-P4 target with RGB565 display.\n");
    return -1;
}

#endif
