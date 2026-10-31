/* user/ceres/src/main_host.c — Host Simulator Main Entry Point */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ceres_platform.h"
#include "ceres_config.h"
#include "ceres_game.h"

static ceres_game_t s_game;

int main(int argc, char **argv) {
    int width = CERES_VIRTUAL_W;
    int height = CERES_VIRTUAL_H;
    int max_frames = 0;
    const char *preset_name = "Virtual 16:9 Arcade Canvas (640x360)";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tab5") == 0) {
            width = CERES_PRESET_TAB5_W;
            height = CERES_PRESET_TAB5_H;
            preset_name = "M5Stack Tab5 Landscape (1280x720)";
        } else if (strcmp(argv[i], "--7b") == 0) {
            width = CERES_PRESET_LCD7B_W;
            height = CERES_PRESET_LCD7B_H;
            preset_name = "ESP32-P4-LCD-7B (1024x600)";
        } else if (strcmp(argv[i], "--native") == 0 || strcmp(argv[i], "--virtual") == 0) {
            width = CERES_VIRTUAL_W;
            height = CERES_VIRTUAL_H;
            preset_name = "Virtual 16:9 Arcade Canvas (640x360)";
        } else if (strncmp(argv[i], "--test-frames=", 14) == 0) {
            max_frames = atoi(argv[i] + 14);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Ceres Alpha — Host Simulator\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --native          Target virtual 16:9 canvas (640x360, default)\n");
            printf("  --7b              Target Waveshare LCD-7B preset (1024x600)\n");
            printf("  --tab5            Target M5Stack Tab5 preset (1280x720)\n");
            printf("  --test-frames=N   Run exactly N frames then exit cleanly (for CI/tests)\n");
            return 0;
        }
    }

    char window_title[128];
    snprintf(window_title, sizeof(window_title), "Ceres Alpha — %s", preset_name);

    ceres_platform_log("[main] Booting Ceres Alpha host simulator [%s]\n", preset_name);

    if (!ceres_platform_init(width, height, window_title)) {
        ceres_platform_log("[main] Fatal: failed to initialize platform.\n");
        return 1;
    }

    if (!ceres_game_init(&s_game, width, height)) {
        ceres_platform_log("[main] Fatal: failed to initialize game state.\n");
        ceres_platform_shutdown();
        return 1;
    }

    int stride = 0;
    uint16_t *fb = ceres_platform_get_framebuffer(&stride);
    if (!fb) {
        ceres_platform_log("[main] Fatal: failed to acquire framebuffer.\n");
        ceres_platform_shutdown();
        return 1;
    }

    uint32_t last_time = ceres_platform_get_ticks_ms();
    int frames_run = 0;

    while (s_game.running) {
        uint32_t now = ceres_platform_get_ticks_ms();
        uint32_t delta = now - last_time;
        if (delta < 1) delta = 1;
        last_time = now;

        ceres_event_t ev;
        while (ceres_platform_poll_event(&ev)) {
            ceres_game_handle_event(&s_game, &ev);
        }

        ceres_game_update(&s_game, delta);
        ceres_game_render(&s_game, fb, width, height, stride);
        ceres_platform_present();

        frames_run++;
        if (max_frames > 0 && frames_run >= max_frames) {
            ceres_platform_log("[main] Reached target test frames (%d), exiting cleanly.\n", max_frames);
            break;
        }

        /* 60 FPS frame limiter (~16.6 ms per frame) */
        uint32_t spent = ceres_platform_get_ticks_ms() - now;
        if (spent < CERES_FRAME_TIME_MS) {
            ceres_platform_delay_ms(CERES_FRAME_TIME_MS - spent);
        }
    }

    ceres_platform_shutdown();
    ceres_platform_log("[main] Ceres Alpha host simulator terminated cleanly.\n");
    return 0;
}
