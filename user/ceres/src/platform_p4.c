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

/* Keyboard Autorelease & Event Synthesis (Console VT Streams lack Key-Up events) */
#define CERES_P4_MOVE_INITIAL_MS    380  /* Initial movement duration bridging typematic repeat gap (~23 frames @ 60 FPS) */
#define CERES_P4_MOVE_REPEAT_MS     110  /* Typematic repeat refresh window (~6-7 frames) for crisp stop on release */
#define CERES_P4_WEAPON_TIMEOUT_MS   80  /* Weapon tap duration (stops before 2nd shot) */
#define CERES_P4_MAX_HELD_KEYS        8
#define CERES_P4_QUEUE_SIZE          16

typedef struct {
    ceres_key_t key;
    uint32_t    release_at_ms;
    bool        active;
} p4_key_hold_t;

static p4_key_hold_t s_held_keys[CERES_P4_MAX_HELD_KEYS]; /* single task: the session holder */
static ceres_event_t s_event_queue[CERES_P4_QUEUE_SIZE];  /* single task: the session holder */
static int           s_event_head = 0;                    /* single task: the session holder */
static int           s_event_tail = 0;                    /* single task: the session holder */

static inline bool p4_is_up(ceres_key_t k)     { return k == CERES_KEY_UP || k == CERES_KEY_W; }
static inline bool p4_is_down(ceres_key_t k)   { return k == CERES_KEY_DOWN || k == CERES_KEY_S; }
static inline bool p4_is_left(ceres_key_t k)   { return k == CERES_KEY_LEFT || k == CERES_KEY_A; }
static inline bool p4_is_right(ceres_key_t k)  { return k == CERES_KEY_RIGHT || k == CERES_KEY_D; }
static inline bool p4_is_weapon(ceres_key_t k) {
    return k == CERES_KEY_SPACE || k == CERES_KEY_B || k == CERES_KEY_K || k == CERES_KEY_RETURN;
}
static inline bool p4_is_holdable(ceres_key_t k) {
    return p4_is_up(k) || p4_is_down(k) || p4_is_left(k) || p4_is_right(k) || p4_is_weapon(k);
}

static void p4_queue_push(const ceres_event_t *ev) {
    int next = (s_event_tail + 1) % CERES_P4_QUEUE_SIZE;
    if (next != s_event_head) {
        s_event_queue[s_event_tail] = *ev;
        s_event_tail = next;
    }
}

static void p4_input_reset(void) {
    memset(s_held_keys, 0, sizeof(s_held_keys));
    s_event_head = 0;
    s_event_tail = 0;
}

static void cancel_opposite_directions(ceres_key_t key) {
    for (int i = 0; i < CERES_P4_MAX_HELD_KEYS; i++) {
        if (!s_held_keys[i].active) continue;
        bool cancel = false;
        if (p4_is_up(key) && p4_is_down(s_held_keys[i].key)) cancel = true;
        else if (p4_is_down(key) && p4_is_up(s_held_keys[i].key)) cancel = true;
        else if (p4_is_left(key) && p4_is_right(s_held_keys[i].key)) cancel = true;
        else if (p4_is_right(key) && p4_is_left(s_held_keys[i].key)) cancel = true;

        if (cancel) {
            ceres_event_t up_ev;
            memset(&up_ev, 0, sizeof(up_ev));
            up_ev.type = CERES_EVENT_KEY_UP;
            up_ev.key = s_held_keys[i].key;
            p4_queue_push(&up_ev);
            s_held_keys[i].active = false;
        }
    }
}

static int find_held_slot(ceres_key_t key) {
    for (int i = 0; i < CERES_P4_MAX_HELD_KEYS; i++) {
        if (!s_held_keys[i].active) continue;
        if (s_held_keys[i].key == key) return i;
        if (p4_is_up(key) && p4_is_up(s_held_keys[i].key)) return i;
        if (p4_is_down(key) && p4_is_down(s_held_keys[i].key)) return i;
        if (p4_is_left(key) && p4_is_left(s_held_keys[i].key)) return i;
        if (p4_is_right(key) && p4_is_right(s_held_keys[i].key)) return i;
        if (p4_is_weapon(key) && s_held_keys[i].key == key) return i;
    }
    return -1;
}

static int alloc_held_slot(void) {
    for (int i = 0; i < CERES_P4_MAX_HELD_KEYS; i++) {
        if (!s_held_keys[i].active) return i;
    }
    return -1;
}

static ceres_key_t map_console_key(uint32_t raw_key) {
    switch (raw_key) {
        case KEY_UP:          return CERES_KEY_UP;
        case KEY_DOWN:        return CERES_KEY_DOWN;
        case KEY_LEFT:        return CERES_KEY_LEFT;
        case KEY_RIGHT:       return CERES_KEY_RIGHT;
        case 0x1B:            return CERES_KEY_ESCAPE;
        case 'q': case 'Q':   return CERES_KEY_Q;
        case ' ':             return CERES_KEY_SPACE;
        case '\r': case '\n': return CERES_KEY_RETURN;
        case 'w': case 'W':   return CERES_KEY_W;
        case 'a': case 'A':   return CERES_KEY_A;
        case 's': case 'S':   return CERES_KEY_S;
        case 'd': case 'D':   return CERES_KEY_D;
        case 'b': case 'B':   return CERES_KEY_B;
        case 'k': case 'K':   return CERES_KEY_K;
        case 'p': case 'P':   return CERES_KEY_P;
        case 'r': case 'R':   return CERES_KEY_R;
        default:              return CERES_KEY_UNKNOWN;
    }
}

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

    p4_input_reset();

    /* 1. Open or focus RGB565 true full-screen canvas "ceres" (no chrome/decorations) */
    uint8_t req[SCREEN_CTITLE_MAX + 3];
    req[0] = 'L';
    req[1] = SCREEN_LAYOUT_FULLSCREEN | 0x80u; /* RGB565 True Fullscreen Canvas */
    const char *cname = "ceres";
    size_t nl = strlen(cname);
    memcpy(req + 2, cname, nl);
    req[2 + nl] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 3 + (uint32_t)nl, reply);
    s_active = true;

    /* 2. Retrieve canvas backing buffer and slot ID */
    s_cid = lcdterm_get_canvas565_cid_by_name("ceres");
    s_fb = lcdterm_get_canvas565_by_name("ceres", &s_stride, &s_width, &s_height);
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
        printk("[ceres] Failed to allocate or acquire RGB565 framebuffer in PSRAM\n");
        return false;
    }
    s_canvas_fb_original = s_fb;

    /* 3. Allocate secondary off-screen render buffer in PSRAM */
    uint32_t render_bytes = (uint32_t)s_virtual_w * s_virtual_h * sizeof(uint16_t);
    s_render_pages = (render_bytes + 4095u) / 4096u;
    s_render_fb = (uint16_t *)palloc_pages_bulk(s_render_pages);
    s_render_fb_original = s_render_fb;
    if (!s_render_fb) {
        printk("[ceres] Warning: secondary buffer allocation failed, using single buffer\n");
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

    printk("[ceres] Bare-metal platform initialized: %dx%d (virtual %dx%d, scale=%d) fb=%p render=%p cid=%d\n",
           s_width, s_height, s_virtual_w, s_virtual_h, s_scale, (void *)s_fb, (void *)s_render_fb, s_cid);
    return true;
}

void ceres_platform_shutdown(void) {
    if (!s_active) return;
    s_active = false;

    p4_input_reset();
    while (console_has_char()) (void)keyseq_read_console();

    /* Restore text terminal */
    uint8_t req[SCREEN_CTITLE_MAX + 3];
    req[0] = 'L';
    req[1] = SCREEN_LAYOUT_TEXT;
    const char *cname = "ceres";
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

    /* 1. Drain synthesized events from queue (e.g. key-up before opposite direction down) */
    if (s_event_head != s_event_tail) {
        *ev = s_event_queue[s_event_head];
        s_event_head = (s_event_head + 1) % CERES_P4_QUEUE_SIZE;
        return true;
    }

    /* 2. Check for expired autorelease hold timers */
    uint32_t now = ceres_platform_get_ticks_ms();
    for (int i = 0; i < CERES_P4_MAX_HELD_KEYS; i++) {
        if (s_held_keys[i].active && now >= s_held_keys[i].release_at_ms) {
            s_held_keys[i].active = false;
            memset(ev, 0, sizeof(*ev));
            ev->type = CERES_EVENT_KEY_UP;
            ev->key = s_held_keys[i].key;
            return true;
        }
    }

    /* 3. Read incoming console keystrokes */
    if (console_has_char()) {
        key_event_t ke = keyseq_read_console();
        ceres_key_t key = map_console_key(ke.key);
        if (key == CERES_KEY_UNKNOWN) return false;

        uint8_t mods = (ke.mods & KMOD_SHIFT) ? CERES_MOD_SHIFT : 0;
        if (ke.key >= 'A' && ke.key <= 'Z') mods = CERES_MOD_SHIFT;

        if (p4_is_holdable(key)) {
            cancel_opposite_directions(key);

            int slot = find_held_slot(key);
            if (slot >= 0) {
                uint32_t timeout = p4_is_weapon(key) ? CERES_P4_WEAPON_TIMEOUT_MS : CERES_P4_MOVE_REPEAT_MS;
                uint32_t new_rel = now + timeout;
                if (new_rel > s_held_keys[slot].release_at_ms) {
                    s_held_keys[slot].release_at_ms = new_rel;
                }
                s_held_keys[slot].key = key;
            } else {
                int free_slot = alloc_held_slot();
                if (free_slot >= 0) {
                    uint32_t timeout = p4_is_weapon(key) ? CERES_P4_WEAPON_TIMEOUT_MS : CERES_P4_MOVE_INITIAL_MS;
                    s_held_keys[free_slot].active = true;
                    s_held_keys[free_slot].key = key;
                    s_held_keys[free_slot].release_at_ms = now + timeout;
                }
            }
        }

        ceres_event_t down_ev;
        memset(&down_ev, 0, sizeof(down_ev));
        down_ev.type = CERES_EVENT_KEY_DOWN;
        down_ev.key = key;
        down_ev.mods = mods;

        if (s_event_head != s_event_tail) {
            p4_queue_push(&down_ev);
            *ev = s_event_queue[s_event_head];
            s_event_head = (s_event_head + 1) % CERES_P4_QUEUE_SIZE;
            return true;
        } else {
            *ev = down_ev;
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
    if (!session_claim(&s_ceres_session, "ceres")) return -1;
    int rc = ceres_run_claimed(args);
    session_release(&s_ceres_session);
    return rc;
}

static int ceres_run_claimed(const char *args) {
    int max_frames = 0;
    if (args && *args) {
        const char *p = args;
        while (*p == ' ') p++;
        if (strncmp(p, "--frames=", 9) == 0) p += 9;
        int val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        max_frames = val;
    }

    p4_input_reset();
    if (!ceres_platform_init(0, 0, "Ceres Alpha")) {
        ceres_platform_shutdown();
        cprintf("ceres: failed to initialize bare-metal platform\n");
        return -1;
    }

    int w = ceres_platform_get_width();
    int h = ceres_platform_get_height();

    if (!ceres_game_init(&s_p4_game, w, h)) {
        cprintf("ceres: failed to initialize game engine\n");
        ceres_platform_shutdown();
        return -1;
    }

    uint32_t last_time = ceres_platform_get_ticks_ms();

    while (s_p4_game.running) {
        if (console_interrupt_requested()) break;
        if (max_frames > 0 && s_p4_game.frame_count >= (uint32_t)max_frames) break;

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
    if (max_frames > 0) {
        cprintf("ceres: completed %u frames (%u FPS avg), score=%u, hi=%u\n",
                s_p4_game.frame_count, s_p4_game.fps, s_p4_game.player.score, s_p4_game.player.hi_score);
    }
    return 0;
}

#else

/* Stub for non-ESP32-P4 targets */
int ceres_run(const char *args) {
    (void)args;
    cprintf("ceres: requires ESP32-P4 target with RGB565 display.\n");
    return -1;
}

#endif
