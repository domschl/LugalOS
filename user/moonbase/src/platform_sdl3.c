#include "moon_platform.h"
#include "moon_blitter.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    uint16_t     *framebuffer;
    int           width;
    int           height;
    int           stride;
    bool          initialized;
} s_plat;

static uint32_t map_sdl_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_ESCAPE:    return MOON_KEY_ESCAPE;
        case SDLK_RETURN:    return MOON_KEY_RETURN;
        case SDLK_SPACE:     return MOON_KEY_SPACE;
        case SDLK_BACKSPACE: return MOON_KEY_BACKSPACE;
        case SDLK_TAB:       return MOON_KEY_TAB;
        case SDLK_UP:        return MOON_KEY_UP;
        case SDLK_DOWN:      return MOON_KEY_DOWN;
        case SDLK_LEFT:      return MOON_KEY_LEFT;
        case SDLK_RIGHT:     return MOON_KEY_RIGHT;
        case SDLK_W:         return MOON_KEY_W;
        case SDLK_A:         return MOON_KEY_A;
        case SDLK_S:         return MOON_KEY_S;
        case SDLK_D:         return MOON_KEY_D;
        case SDLK_Q:         return MOON_KEY_Q;
        case SDLK_E:         return MOON_KEY_E;
        case SDLK_R:         return MOON_KEY_R;
        case SDLK_0:         return MOON_KEY_0;
        case SDLK_1:         return MOON_KEY_1;
        case SDLK_2:         return MOON_KEY_2;
        case SDLK_3:         return MOON_KEY_3;
        case SDLK_4:         return MOON_KEY_4;
        case SDLK_5:         return MOON_KEY_5;
        case SDLK_6:         return MOON_KEY_6;
        case SDLK_7:         return MOON_KEY_7;
        case SDLK_8:         return MOON_KEY_8;
        case SDLK_9:         return MOON_KEY_9;
        case SDLK_T:         return MOON_KEY_T;
        case SDLK_Z:         return MOON_KEY_Z;
        case SDLK_P:         return MOON_KEY_P;
        case SDLK_Y:         return MOON_KEY_Y;
        case SDLK_N:         return MOON_KEY_N;
        case SDLK_C:         return MOON_KEY_C;
        case SDLK_O:         return MOON_KEY_O;
        case SDLK_F:         return MOON_KEY_F;
        case SDLK_MINUS:     return MOON_KEY_MINUS;
        case SDLK_KP_MINUS:  return MOON_KEY_MINUS;
        case SDLK_EQUALS:    return MOON_KEY_PLUS;
        case SDLK_PLUS:      return MOON_KEY_PLUS;
        case SDLK_KP_PLUS:   return MOON_KEY_PLUS;
        default:             return MOON_KEY_UNKNOWN;
    }
}

static void *s_tile_cache;   /* single task: the main loop */

bool moon_platform_init(int width, int height, const char *title) {
    if (s_plat.initialized) return true;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[moon_platform] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    s_plat.width = width;
    s_plat.height = height;
    s_plat.stride = width;

    s_plat.window = SDL_CreateWindow(title ? title : "Moonbase Lambda",
                                     width, height, 0);
    if (!s_plat.window) {
        fprintf(stderr, "[moon_platform] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    s_plat.renderer = SDL_CreateRenderer(s_plat.window, NULL);
    if (!s_plat.renderer) {
        fprintf(stderr, "[moon_platform] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_plat.window);
        SDL_Quit();
        return false;
    }

    /* 16-bit RGB565 streaming texture */
    s_plat.texture = SDL_CreateTexture(s_plat.renderer,
                                       SDL_PIXELFORMAT_RGB565,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       width, height);
    if (!s_plat.texture) {
        fprintf(stderr, "[moon_platform] SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(s_plat.renderer);
        SDL_DestroyWindow(s_plat.window);
        SDL_Quit();
        return false;
    }

    s_plat.framebuffer = (uint16_t *)malloc(width * height * sizeof(uint16_t));
    if (!s_plat.framebuffer) {
        fprintf(stderr, "[moon_platform] Failed to allocate framebuffer\n");
        SDL_DestroyTexture(s_plat.texture);
        SDL_DestroyRenderer(s_plat.renderer);
        SDL_DestroyWindow(s_plat.window);
        SDL_Quit();
        return false;
    }

    /* Baked terrain tops (see moon_tile_cache_attach); without them the host just samples slower */
    s_tile_cache = malloc(moon_tile_cache_bytes());
    if (s_tile_cache) moon_tile_cache_attach(s_tile_cache);

    s_plat.initialized = true;
    return true;
}

void moon_platform_shutdown(void) {
    if (!s_plat.initialized) return;

    moon_tile_cache_attach(NULL);
    free(s_tile_cache);
    s_tile_cache = NULL;

    if (s_plat.framebuffer) {
        free(s_plat.framebuffer);
        s_plat.framebuffer = NULL;
    }
    if (s_plat.texture) {
        SDL_DestroyTexture(s_plat.texture);
        s_plat.texture = NULL;
    }
    if (s_plat.renderer) {
        SDL_DestroyRenderer(s_plat.renderer);
        s_plat.renderer = NULL;
    }
    if (s_plat.window) {
        SDL_DestroyWindow(s_plat.window);
        s_plat.window = NULL;
    }
    SDL_Quit();
    s_plat.initialized = false;
}

int moon_platform_get_width(void) {
    return s_plat.width;
}

int moon_platform_get_height(void) {
    return s_plat.height;
}

uint32_t moon_platform_get_ticks_ms(void) {
    return (uint32_t)SDL_GetTicks();
}

uint32_t moon_platform_get_ticks_us(void) {
    return (uint32_t)(SDL_GetTicksNS() / 1000u);
}

void moon_platform_delay_ms(uint32_t ms) {
    SDL_Delay(ms);
}

uint16_t* moon_platform_get_framebuffer(int *stride_pixels) {
    if (stride_pixels) *stride_pixels = s_plat.stride;
    return s_plat.framebuffer;
}

uint16_t* moon_platform_get_display_framebuffer(int *stride_pixels) {
    return moon_platform_get_framebuffer(stride_pixels);
}

void moon_platform_present_rows(int y0, int y1) {
    (void)y0; (void)y1;
    moon_platform_present();
}

void moon_platform_present(void) {
    if (!s_plat.initialized || !s_plat.framebuffer) return;

    SDL_UpdateTexture(s_plat.texture, NULL, s_plat.framebuffer, s_plat.stride * sizeof(uint16_t));
    SDL_RenderClear(s_plat.renderer);
    SDL_RenderTexture(s_plat.renderer, s_plat.texture, NULL, NULL);
    SDL_RenderPresent(s_plat.renderer);
}

bool moon_platform_poll_event(moon_event_t *ev) {
    if (!ev) return false;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
                ev->type = MOON_EVENT_QUIT;
                return true;

            case SDL_EVENT_KEY_DOWN:
                ev->type = MOON_EVENT_KEY_DOWN;
                ev->key = map_sdl_key(e.key.key);
                ev->mods = (e.key.mod & SDL_KMOD_SHIFT) ? MOON_MOD_SHIFT : 0;
                return true;

            case SDL_EVENT_KEY_UP:
                ev->type = MOON_EVENT_KEY_UP;
                ev->key = map_sdl_key(e.key.key);
                return true;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                ev->type = MOON_EVENT_POINTER_DOWN;
                ev->x = (int32_t)e.button.x;
                ev->y = (int32_t)e.button.y;
                ev->button = e.button.button;
                ev->touch = (e.button.which == SDL_TOUCH_MOUSEID);
                return true;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                ev->type = MOON_EVENT_POINTER_UP;
                ev->x = (int32_t)e.button.x;
                ev->y = (int32_t)e.button.y;
                ev->button = e.button.button;
                ev->touch = (e.button.which == SDL_TOUCH_MOUSEID);
                return true;

            case SDL_EVENT_MOUSE_MOTION:
                ev->type = MOON_EVENT_POINTER_MOVE;
                ev->x = (int32_t)e.motion.x;
                ev->y = (int32_t)e.motion.y;
                ev->dx = (int32_t)e.motion.xrel;
                ev->dy = (int32_t)e.motion.yrel;
                ev->touch = (e.motion.which == SDL_TOUCH_MOUSEID);
                return true;

            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                ev->type = MOON_EVENT_POINTER_LEAVE;
                return true;

            case SDL_EVENT_MOUSE_WHEEL:
                ev->type = MOON_EVENT_SCROLL;
                ev->wheel_x = (int16_t)e.wheel.x;
                ev->wheel_y = (int16_t)e.wheel.y;
                return true;

            default:
                break;
        }
    }

    ev->type = MOON_EVENT_NONE;
    return false;
}

void moon_platform_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fflush(stdout);
}
