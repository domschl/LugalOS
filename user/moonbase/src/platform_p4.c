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
#include "moon_bench.h"

#include "lugalos_config.h"
#include "kernel/lock.h"
#include "arch/esp32p4_intr.h"
#include "drivers/dsi_esp32p4.h"
#include "drivers/ppa_esp32p4.h"
#include "drivers/lcdterm.h"
#include "drivers/touch.h"
#include "drivers/screen.h"
#include "kernel/console.h"
#include "kernel/keyseq.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#include <stdarg.h>
#include <string.h>

#if defined(CONFIG_BOARD_ESP32P4)

/* 35 KB game state in PSRAM, zero SRAM footprint.
 * single task: the one holding s_moon_session (moonbase_run()). */
static moon_game_t s_p4_game BULK_BSS;

/* One game at a time: its state, the textures and the blitter's caches are
 * statics, and a second terminal starting `moonbase` would share them.
 * accepted: a session_claim_t is claimed and released atomically by itself. */
static session_claim_t s_moon_session;

/* The display geometry and buffers below: single task: the moonbase session
 * holder, which sets them in moon_platform_init() and clears them in
 * moon_platform_shutdown(). The touch hook reads s_scale only: it is set
 * before touch_set_hook() registers the hook and unregistered first. */

#if defined(CONFIG_BOARD_TAB5)
static int       s_scale = 2;         /* accepted: see above. 2x HiDPI integer scale for 5" 294 DPI display (640x360 -> 1280x720) */
#else
static int       s_scale = 1;         /* accepted: see above. 1x native for 7" 170 DPI display */
#endif

static int       s_width = 1024;   /* single task: the session holder */
static int       s_height = 600;   /* single task: the session holder */
static uint32_t  s_stride = 1024;   /* single task: the session holder */
static int       s_virtual_w = 1024;   /* single task: the session holder */
static int       s_virtual_h = 600;   /* single task: the session holder */
static uint16_t *s_fb = NULL;                 /* single task: the session holder.  Canvas slot buffer (visible to compositor) */
static uint16_t *s_render_fb = NULL;          /* single task: the session holder.  Off-screen render buffer (drawn by game) */
static uint16_t *s_canvas_fb_original = NULL; /* single task: the session holder.  Original canvas slot backing buffer */
static uint16_t *s_render_fb_original = NULL; /* single task: the session holder.  Original palloc buffer for safe freeing */
static int       s_cid = -1;   /* single task: the session holder */
static bool      s_allocated_fb = false;   /* single task: the session holder */
static void       *s_tile_cache = NULL;     /* single task: the session holder.  baked terrain tops (moon_tile_cache_attach) */
static uint32_t   s_tile_cache_pages = 0;   /* single task: the session holder */
static bool      s_active = false;   /* single task: the moonbase command; between init and shutdown */
static uint32_t  s_fb_pages = 0;
static uint32_t  s_render_pages = 0;

/* Touch event queue with move-coalescing */
#define EVENT_QUEUE_SIZE 64
/* The touch driver's hook pushes, the game pops: two tasks, on either hart.
 * guarded by: s_qlock, for the ring and both indices -- the move-coalescing
 * push rewrites an entry the game could otherwise be copying out. */
static spinlock_t   s_qlock = SPINLOCK_INIT;
static moon_event_t s_queue[EVENT_QUEUE_SIZE];   /* guarded by: s_qlock */
static int          s_qhead = 0;                 /* guarded by: s_qlock */
static int          s_qtail = 0;                 /* guarded by: s_qlock */
/* single task: p4_touch_hook(), in the touch driver's context */
static int s_last_touch_x = 0;
static int s_last_touch_y = 0;

static void queue_push(const moon_event_t *ev) {
    uintptr_t f = spin_lock_irqsave(&s_qlock);
    /* If the event is POINTER_MOVE, coalesce with previous queued POINTER_MOVE to prevent queue overflow */
    if (ev->type == MOON_EVENT_POINTER_MOVE && s_qhead != s_qtail) {
        int prev = (s_qhead - 1 + EVENT_QUEUE_SIZE) % EVENT_QUEUE_SIZE;
        if (s_queue[prev].type == MOON_EVENT_POINTER_MOVE) {
            s_queue[prev].x = ev->x;
            s_queue[prev].y = ev->y;
            s_queue[prev].dx += ev->dx;
            s_queue[prev].dy += ev->dy;
            spin_unlock_irqrestore(&s_qlock, f);
            return;
        }
    }

    int next = (s_qhead + 1) % EVENT_QUEUE_SIZE;
    if (next != s_qtail) {
        s_queue[s_qhead] = *ev;
        s_qhead = next;
    }
    spin_unlock_irqrestore(&s_qlock, f);
}

static void queue_clear(void) {
    uintptr_t f = spin_lock_irqsave(&s_qlock);
    s_qhead = s_qtail = 0;
    spin_unlock_irqrestore(&s_qlock, f);
}

static bool p4_touch_hook(int x, int y, int kind) {
    moon_event_t ev;
    memset(&ev, 0, sizeof(ev));
    int vx = (s_scale > 1) ? (x / s_scale) : x;
    int vy = (s_scale > 1) ? (y / s_scale) : y;
    ev.x = vx;
    ev.y = vy;
    ev.dx = vx - s_last_touch_x;
    ev.dy = vy - s_last_touch_y;
    s_last_touch_x = vx;
    s_last_touch_y = vy;
    ev.button = 1;
    ev.touch = 1;

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
    queue_clear();

#if defined(CONFIG_BOARD_TAB5)
    s_scale = 2;
    s_width = MOON_PRESET_TAB5_W;   /* 1280 */
    s_height = MOON_PRESET_TAB5_H;  /* 720 */
    /* Ensure Tab5 is in landscape orientation */
    if (dsi_lcd_get_orientation() != 270) {
        dsi_lcd_set_orientation(270);
    }
#else
    s_scale = 1;
    s_width = MOON_PRESET_LCD7B_W;  /* 1024 */
    s_height = MOON_PRESET_LCD7B_H; /* 600 */
#endif
    s_stride = (uint32_t)s_width;
    s_virtual_w = s_width / s_scale;
    s_virtual_h = s_height / s_scale;

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
    s_active = true; /* the terminal has lost its screen: from here on a failure must run the teardown */

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
    s_canvas_fb_original = s_fb;

    /* Allocate secondary off-screen render buffer in PSRAM */
    uint32_t render_bytes = (uint32_t)s_virtual_w * s_virtual_h * sizeof(uint16_t);
    s_render_pages = (render_bytes + 4095u) / 4096u;
    s_render_fb = (uint16_t *)palloc_pages_bulk(s_render_pages);
    s_render_fb_original = s_render_fb;
    if (!s_render_fb) {
        printk("[moonbase] Warning: failed to allocate secondary buffer, using single-buffered rendering\n");
        s_render_fb = s_fb;
        s_render_fb_original = NULL;
        s_scale = 1;
        s_virtual_w = s_width;
        s_virtual_h = s_height;
    }

    /* Clear both framebuffers to black */
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

    /* Baked terrain tops: about 8.7 MB of PSRAM, a lot faster than sampling every pixel */
    s_tile_cache_pages = (uint32_t)((moon_tile_cache_bytes() + 4095u) / 4096u);
    s_tile_cache = palloc_pages_bulk(s_tile_cache_pages);
    if (s_tile_cache) moon_tile_cache_attach(s_tile_cache);
    else printk("[moonbase] No PSRAM for the tile cache: terrain is sampled per pixel (slow)\n");

    /* Hook touch subsystem */
    touch_set_hook(p4_touch_hook);

    printk("[moonbase] Bare-metal platform initialized: %dx%d (virtual %dx%d, scale=%d, stride=%lu) at %p (render_fb=%p, cid=%d)\n",
           s_width, s_height, s_virtual_w, s_virtual_h, s_scale, (unsigned long)s_stride, (void *)s_fb, (void *)s_render_fb, s_cid);
    return true;
}

/* Teardown order: stop the input sources first (the touch hook writes to the event
 * queue and the console still holds key bytes), then give the terminal its canvas
 * back, and only then free the buffers the canvas pointed at. Safe to call twice. */
void moon_platform_shutdown(void) {
    if (!s_active) return;
    s_active = false;

    touch_set_hook(NULL);
    queue_clear();                         /* events queued but never read */
    while (console_has_char()) (void)keyseq_read_console();

    moon_tile_cache_attach(NULL);
    if (s_tile_cache) {
        palloc_free(s_tile_cache, s_tile_cache_pages);
        s_tile_cache = NULL;
        s_tile_cache_pages = 0;
    }

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

    /* Restore original canvas backing pointer and full resolution if double-buffering swapped backing */
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

    /* Force full terminal repaint and hardware VSYNC scanout flip to purge residual game graphics */
    lcdterm_repaint();
    lcdterm_sync_post();
    if (lcdterm_is_double_buffered()) {
        lcdterm_sync_post();
    }
}

int moon_platform_get_width(void) {
    return s_virtual_w;
}

int moon_platform_get_height(void) {
    return s_virtual_h;
}

uint32_t moon_platform_get_ticks_ms(void) {
    return (uint32_t)time_get_ms();
}

uint32_t moon_platform_get_ticks_us(void) {
    return (uint32_t)time_get_us();
}

void moon_platform_delay_ms(uint32_t ms) {
    task_sleep_ms(ms);
}

uint16_t* moon_platform_get_framebuffer(int *stride_pixels) {
    if (stride_pixels) *stride_pixels = s_virtual_w;
    return s_render_fb ? s_render_fb : s_fb;
}

/* 2x pixel doubling of virtual rows [y0, y1) of the 640x360 frame into the 1280x720 canvas, and
 * the cache write-back of destination. Rows are independent, so a partial update costs its rows only. */
static void upscale_rows(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > s_virtual_h) y1 = s_virtual_h;
    if (y0 >= y1) return;
    if (ppa_srm_rgb565(s_render_fb, (uint32_t)s_virtual_w, (uint32_t)s_virtual_h,
                       0u, (uint32_t)y0, (uint32_t)s_virtual_w, (uint32_t)(y1 - y0),
                       s_fb, s_stride, (uint32_t)s_height,
                       0u, (uint32_t)(y0 * 2),
                       2u, 0u, 2u, 0u,
                       0u, true, false)) {
        return;
    }
    for (int y = y0; y < y1; y++) {
        const uint32_t *src_row32 = (const uint32_t *)(s_render_fb + (size_t)y * s_virtual_w);
        uint32_t *dst0 = (uint32_t *)(s_fb + ((size_t)y * 2) * s_stride);
        uint32_t *dst1 = (uint32_t *)(s_fb + ((size_t)y * 2 + 1) * s_stride);
        int half_w = s_virtual_w / 2;
        for (int i = 0; i < half_w; i++) {
            uint32_t p2 = src_row32[i];
            uint32_t p0 = (uint16_t)p2;
            uint32_t p1 = p2 >> 16;
            uint32_t w0 = (p0 << 16) | p0;
            uint32_t w1 = (p1 << 16) | p1;
            dst0[2 * i] = w0;
            dst0[2 * i + 1] = w1;
            dst1[2 * i] = w0;
            dst1[2 * i + 1] = w1;
        }
    }
    esp32p4_extmem_writeback((uintptr_t)(s_fb + (size_t)y0 * 2 * s_stride),
                             (size_t)(y1 - y0) * 2 * s_stride * sizeof(uint16_t));
}

void moon_platform_present(void) {
    if (!s_render_fb) return;

    /* Write back the rendered virtual frame and swap front/back canvas pointers.
     * On Tab5 (s_scale == 2), the display compositor's PPA SRM pass scales the
     * 640x360 canvas by 2x and rotates it 90 deg CCW into fb_back() in a single
     * hardware DMA operation; only fall back to upscale_rows() if no canvas slot. */
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
    } else if (s_scale == 2 && s_fb && s_render_fb && s_render_fb != s_fb) {
        upscale_rows(0, s_virtual_h);
        lcdterm_damage();
    } else {
        esp32p4_extmem_writeback((uintptr_t)s_render_fb,
                                 (size_t)s_virtual_w * s_virtual_h * sizeof(uint16_t));
        lcdterm_damage();
    }
}

/* The buffer currently on screen (after the front/back swap in moon_platform_present). */
uint16_t* moon_platform_get_display_framebuffer(int *stride_pixels) {
    if (stride_pixels) *stride_pixels = s_virtual_w;
    return s_fb;
}

void moon_platform_present_rows(int y0, int y1) {
    if (s_fb) {
        if (y0 < 0) y0 = 0;
        if (y1 > s_virtual_h) y1 = s_virtual_h;
        if (y0 < y1) {
            esp32p4_extmem_writeback((uintptr_t)(s_fb + (size_t)y0 * s_virtual_w),
                                     (size_t)(y1 - y0) * s_virtual_w * sizeof(uint16_t));
        }
    }
    lcdterm_damage();
}

bool moon_platform_poll_event(moon_event_t *ev) {
    /* 1. Touch events */
    uintptr_t f = spin_lock_irqsave(&s_qlock);
    if (s_qhead != s_qtail) {
        *ev = s_queue[s_qtail];
        s_qtail = (s_qtail + 1) % EVENT_QUEUE_SIZE;
        spin_unlock_irqrestore(&s_qlock, f);
        return true;
    }
    spin_unlock_irqrestore(&s_qlock, f);

    /* 2. Keyboard events via kernel keyseq decoder (VT escape sequences, arrow keys, etc.) */
    if (console_has_char()) {
        key_event_t ke = keyseq_read_console();
        memset(ev, 0, sizeof(*ev));
        ev->type = MOON_EVENT_KEY_DOWN;
        ev->mods = (ke.mods & KMOD_SHIFT) ? MOON_MOD_SHIFT : 0;
        if (ke.key >= 'A' && ke.key <= 'Z') ev->mods = MOON_MOD_SHIFT; /* capital letter = Shift */
        switch (ke.key) {
            case KEY_UP:
                ev->key = MOON_KEY_UP;
                return true;
            case KEY_DOWN:
                ev->key = MOON_KEY_DOWN;
                return true;
            case KEY_LEFT:
                ev->key = MOON_KEY_LEFT;
                return true;
            case KEY_RIGHT:
                ev->key = MOON_KEY_RIGHT;
                return true;
            case 0x1B: /* Lone ESC key */
                ev->key = MOON_KEY_ESCAPE;
                return true;
            case 'q': case 'Q':
                ev->key = MOON_KEY_Q;
                return true;
            case ' ':
                ev->key = MOON_KEY_SPACE;
                return true;
            case '\r': case '\n':
                ev->key = MOON_KEY_RETURN;
                return true;
            case '\t':
                ev->key = MOON_KEY_TAB;
                return true;
            case 'w': case 'W':
                ev->key = MOON_KEY_W;
                return true;
            case 'a': case 'A':
                ev->key = MOON_KEY_A;
                return true;
            case 's': case 'S':
                ev->key = MOON_KEY_S;
                return true;
            case 'd': case 'D':
                ev->key = MOON_KEY_D;
                return true;
            case 'r': case 'R':
                ev->key = MOON_KEY_R;
                return true;
            case 't': case 'T':
                ev->key = MOON_KEY_T;
                return true;
            case 'z': case 'Z':
                ev->key = MOON_KEY_Z;
                return true;
            case '0': ev->key = MOON_KEY_0; return true;
            case '1': ev->key = MOON_KEY_1; return true;
            case '2': ev->key = MOON_KEY_2; return true;
            case '3': ev->key = MOON_KEY_3; return true;
            case '4': ev->key = MOON_KEY_4; return true;
            case '5': ev->key = MOON_KEY_5; return true;
            case '6': ev->key = MOON_KEY_6; return true;
            case '7': ev->key = MOON_KEY_7; return true;
            case '8': ev->key = MOON_KEY_8; return true;
            case '9': ev->key = MOON_KEY_9; return true;
            case 'e': case 'E':
                ev->key = MOON_KEY_E;
                return true;
            case 'p': case 'P':
                ev->key = MOON_KEY_P;
                return true;
            case 'y': case 'Y':
                ev->key = MOON_KEY_Y;
                return true;
            case 'n': case 'N':
                ev->key = MOON_KEY_N;
                return true;
            case 'c': case 'C':
                ev->key = MOON_KEY_C;
                return true;
            case 'o': case 'O':
                ev->key = MOON_KEY_O;
                return true;
            case 'f': case 'F':
                ev->key = MOON_KEY_F;
                return true;
            case 'b': case 'B':
                ev->key = MOON_KEY_B;
                return true;
            case 'l': case 'L':
                ev->key = MOON_KEY_L;
                return true;
            case '-': case '_':
                ev->key = MOON_KEY_MINUS;
                return true;
            case '+': case '=':
                ev->key = MOON_KEY_PLUS;
                return true;
            case 0x7F: case '\b':
                ev->key = MOON_KEY_BACKSPACE;
                return true;
            default:
                ev->key = MOON_KEY_UNKNOWN;
                return true;
        }
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
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    cprintf("%s", line);
}

static int moonbase_run_claimed(const char *args);

int moonbase_run(const char *args) {
    if (!session_claim(&s_moon_session, "moonbase")) return -1;
    int rc = moonbase_run_claimed(args);
    session_release(&s_moon_session);
    return rc;
}

static int moonbase_run_claimed(const char *args) {
    if (!moon_platform_init(0, 0, "Moonbase Lambda")) {
        moon_platform_shutdown(); /* gives the terminal its screen back if init got that far */
        cprintf("moonbase: failed to initialize bare-metal platform\n");
        return -1;
    }

    int w = moon_platform_get_width();
    int h = moon_platform_get_height();

    uint32_t seed = 0;
    random_bytes(&seed, sizeof(seed));
    s_p4_game.seed_request = seed ? seed : 1;
    s_p4_game.show_title = true;
    if (!moon_game_init(&s_p4_game, w, h)) {
        cprintf("moonbase: failed to initialize game engine\n");
        moon_platform_shutdown();
        return -1;
    }

    s_p4_game.clock_us = moon_platform_get_ticks_us;

    if (args && strstr(args, "micro")) {
        moon_bench_micro(w, h);
        moon_platform_shutdown();
        return 0;
    }
    if (args && strstr(args, "bench")) {
        static moon_bench_result_t results[MOON_BENCH_SCENARIOS];   /* single task: the session holder */
        static uint32_t samples[4096] BULK_BSS;   /* single task: the moonbase command */
        int n = moon_bench_run(&s_p4_game, w, h, 6, results, samples, 4096);
        moon_platform_shutdown();
        moon_bench_print(results, n);
        return 0;
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

        /* Draw only what is stale: nothing, the top bar and ribbon, or the whole frame */
        int dirty = moon_game_take_dirty(&s_p4_game);
        int stride = 0;
        if (dirty == MOON_DIRTY_WORLD) {
            uint16_t *fb = moon_platform_get_framebuffer(&stride);
            if (fb) {
                uint32_t t0 = moon_platform_get_ticks_us();
                moon_game_render(&s_p4_game, fb, w, h, stride);
                uint32_t t1 = moon_platform_get_ticks_us();
                moon_platform_present();
                uint32_t t2 = moon_platform_get_ticks_us();
                moon_game_note_frame(&s_p4_game, t2 - t1, t2 - t0);
            }
        } else if (dirty == MOON_DIRTY_HUD) {
            uint16_t *fb = moon_platform_get_display_framebuffer(&stride);
            if (fb) {
                moon_game_render_hud(&s_p4_game, fb, w, h, stride);
                moon_platform_present_rows(0, s_p4_game.ui.hud.h + 1);
                moon_platform_present_rows(s_p4_game.ui.ribbon_bar.y, h);
            }
        }

        /* Sleep for what is left of the 16 ms frame, not a fixed 16 ms on top of the work */
        uint32_t spent = moon_platform_get_ticks_ms() - now;
        moon_platform_delay_ms(spent >= 16 ? 1 : 16 - spent);
    }

    moon_platform_shutdown();
    return 0;
}

#else

/* Stub for non-ESP32-P4 targets */
int moonbase_run(const char *args) {
    (void)args;
    cprintf("moonbase: requires ESP32-P4 target with RGB565 display.\n");
    return -1;
}

#endif
