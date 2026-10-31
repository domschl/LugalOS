/* user/ceres/src/platform_sdl3.c — Host SDL3 Platform Driver for Rapid Prototyping */

#include "ceres_platform.h"
#include "ceres_blitter.h"
#include "ceres_sound.h"
#include "ceres_ambient.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static struct {
    SDL_Window      *window;
    SDL_Renderer    *renderer;
    SDL_Texture     *texture;
    SDL_AudioStream *audio_stream;
    uint16_t        *framebuffer;
    int              width;
    int              height;
    int              stride;
    bool             initialized;
} s_plat;

static uint32_t map_sdl_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_ESCAPE:    return CERES_KEY_ESCAPE;
        case SDLK_RETURN:    return CERES_KEY_RETURN;
        case SDLK_SPACE:     return CERES_KEY_SPACE;
        case SDLK_B:         return CERES_KEY_B;
        case SDLK_K:         return CERES_KEY_K;
        case SDLK_UP:        return CERES_KEY_UP;
        case SDLK_DOWN:      return CERES_KEY_DOWN;
        case SDLK_LEFT:      return CERES_KEY_LEFT;
        case SDLK_RIGHT:     return CERES_KEY_RIGHT;
        case SDLK_W:         return CERES_KEY_W;
        case SDLK_A:         return CERES_KEY_A;
        case SDLK_S:         return CERES_KEY_S;
        case SDLK_D:         return CERES_KEY_D;
        case SDLK_P:         return CERES_KEY_P;
        case SDLK_Q:         return CERES_KEY_Q;
        case SDLK_R:         return CERES_KEY_R;
        case SDLK_V:         return CERES_KEY_V;
        case SDLK_0:         return CERES_KEY_0;
        case SDLK_1:         return CERES_KEY_1;
        case SDLK_2:         return CERES_KEY_2;
        case SDLK_3:         return CERES_KEY_3;
        case SDLK_4:         return CERES_KEY_4;
        case SDLK_5:         return CERES_KEY_5;
        case SDLK_6:         return CERES_KEY_6;
        case SDLK_7:         return CERES_KEY_7;
        case SDLK_8:         return CERES_KEY_8;
        case SDLK_9:         return CERES_KEY_9;
        case SDLK_MINUS:     return CERES_KEY_MINUS;
        case SDLK_EQUALS:    return CERES_KEY_PLUS;
        default:             return CERES_KEY_UNKNOWN;
    }
}

bool ceres_platform_init(int width, int height, const char *title) {
    if (s_plat.initialized) return true;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "[ceres_platform] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    s_plat.width = (width > 0) ? width : CERES_VIRTUAL_W;
    s_plat.height = (height > 0) ? height : CERES_VIRTUAL_H;
    s_plat.stride = s_plat.width;

    s_plat.window = SDL_CreateWindow(title ? title : "Ceres Alpha — Host Simulator",
                                     s_plat.width, s_plat.height, 0);
    if (!s_plat.window) {
        fprintf(stderr, "[ceres_platform] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    s_plat.renderer = SDL_CreateRenderer(s_plat.window, NULL);
    if (!s_plat.renderer) {
        fprintf(stderr, "[ceres_platform] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_plat.window);
        SDL_Quit();
        return false;
    }

    /* 16-bit RGB565 streaming texture */
    s_plat.texture = SDL_CreateTexture(s_plat.renderer,
                                       SDL_PIXELFORMAT_RGB565,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       s_plat.width, s_plat.height);
    if (!s_plat.texture) {
        fprintf(stderr, "[ceres_platform] SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(s_plat.renderer);
        SDL_DestroyWindow(s_plat.window);
        SDL_Quit();
        return false;
    }

    /* Initialize SDL3 Audio Stream */
    SDL_AudioSpec spec;
    spec.channels = 1;
    spec.format = SDL_AUDIO_S16LE;
    spec.freq = CERES_AUDIO_SAMPLE_RATE;
    s_plat.audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (s_plat.audio_stream) {
        SDL_ResumeAudioStreamDevice(s_plat.audio_stream);
    }

    s_plat.framebuffer = (uint16_t *)malloc(s_plat.width * s_plat.height * sizeof(uint16_t));
    if (!s_plat.framebuffer) {
        fprintf(stderr, "[ceres_platform] Failed to allocate framebuffer\n");
        SDL_DestroyTexture(s_plat.texture);
        SDL_DestroyRenderer(s_plat.renderer);
        SDL_DestroyWindow(s_plat.window);
        SDL_Quit();
        return false;
    }

    memset(s_plat.framebuffer, 0, s_plat.width * s_plat.height * sizeof(uint16_t));
    s_plat.initialized = true;
    return true;
}

void ceres_platform_shutdown(void) {
    if (!s_plat.initialized) return;

    if (s_plat.audio_stream) {
        SDL_DestroyAudioStream(s_plat.audio_stream);
        s_plat.audio_stream = NULL;
    }
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

int ceres_platform_get_width(void) {
    return s_plat.width;
}

int ceres_platform_get_height(void) {
    return s_plat.height;
}

uint32_t ceres_platform_get_ticks_ms(void) {
    return (uint32_t)SDL_GetTicks();
}

uint32_t ceres_platform_get_ticks_us(void) {
    return (uint32_t)(SDL_GetTicksNS() / 1000u);
}

void ceres_platform_delay_ms(uint32_t ms) {
    SDL_Delay(ms);
}

uint16_t* ceres_platform_get_framebuffer(int *stride_pixels) {
    if (stride_pixels) *stride_pixels = s_plat.stride;
    return s_plat.framebuffer;
}

void ceres_platform_present(void) {
    if (!s_plat.initialized || !s_plat.framebuffer) return;

    /* Feed Procedural Audio Stream */
    if (s_plat.audio_stream) {
        int queued_bytes = SDL_GetAudioStreamQueued(s_plat.audio_stream);
        int target_bytes = (int)(CERES_AUDIO_SAMPLE_RATE * sizeof(int16_t) / 20); /* ~50ms audio cushion */
        int top_up_chunks = 0;
        while (queued_bytes < target_bytes && top_up_chunks < 8) {
            int16_t pcm[512];
            ceres_sound_render(pcm, 512);
            ceres_ambient_render(pcm, 512);
            SDL_PutAudioStreamData(s_plat.audio_stream, pcm, sizeof(pcm));
            queued_bytes += (int)sizeof(pcm);
            top_up_chunks++;
        }
    }

    SDL_UpdateTexture(s_plat.texture, NULL, s_plat.framebuffer, s_plat.stride * sizeof(uint16_t));
    SDL_RenderClear(s_plat.renderer);
    SDL_RenderTexture(s_plat.renderer, s_plat.texture, NULL, NULL);
    SDL_RenderPresent(s_plat.renderer);
}

void ceres_platform_set_volume(int percent) {
    (void)percent;
}

bool ceres_platform_poll_event(ceres_event_t *ev) {
    if (!ev) return false;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            ev->type = CERES_EVENT_QUIT;
            return true;
        }

        if (event.type == SDL_EVENT_KEY_DOWN) {
            ev->type = CERES_EVENT_KEY_DOWN;
            ev->key = map_sdl_key(event.key.key);
            ev->mods = 0;
            if (event.key.mod & SDL_KMOD_SHIFT) ev->mods |= CERES_MOD_SHIFT;
            return true;
        }

        if (event.type == SDL_EVENT_KEY_UP) {
            ev->type = CERES_EVENT_KEY_UP;
            ev->key = map_sdl_key(event.key.key);
            ev->mods = 0;
            if (event.key.mod & SDL_KMOD_SHIFT) ev->mods |= CERES_MOD_SHIFT;
            return true;
        }
    }
    return false;
}

void ceres_platform_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
