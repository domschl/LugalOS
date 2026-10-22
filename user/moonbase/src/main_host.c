#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "moon_platform.h"
#include "moon_config.h"
#include "moon_game.h"

int main(int argc, char **argv) {
    int width = MOON_PRESET_LCD7B_W;
    int height = MOON_PRESET_LCD7B_H;
    int max_frames = 0;
    const char *preset_name = "ESP32-P4-LCD-7B (1024x600)";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tab5") == 0) {
            width = MOON_PRESET_TAB5_W;
            height = MOON_PRESET_TAB5_H;
            preset_name = "M5Stack Tab5 Landscape (1280x720)";
        } else if (strcmp(argv[i], "--tab5-portrait") == 0) {
            width = MOON_PRESET_TAB5_H;
            height = MOON_PRESET_TAB5_W;
            preset_name = "M5Stack Tab5 Portrait (720x1280)";
        } else if (strcmp(argv[i], "--7b") == 0) {
            width = MOON_PRESET_LCD7B_W;
            height = MOON_PRESET_LCD7B_H;
            preset_name = "ESP32-P4-LCD-7B (1024x600)";
        } else if (strncmp(argv[i], "--test-frames=", 14) == 0) {
            max_frames = atoi(argv[i] + 14);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Moonbase Lambda - Host Simulator\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --7b              Target LCD-7B preset (1024x600, default)\n");
            printf("  --tab5            Target Tab5 landscape preset (1280x720)\n");
            printf("  --tab5-portrait   Target Tab5 portrait preset (720x1280)\n");
            printf("  --test-frames=N   Run exactly N frames then exit cleanly (for CI/tests)\n");
            return 0;
        }
    }

    char window_title[128];
    snprintf(window_title, sizeof(window_title), "Moonbase Lambda — %s", preset_name);

    moon_platform_log("[main] Booting Moonbase Lambda host simulator [%s]\n", preset_name);

    if (!moon_platform_init(width, height, window_title)) {
        moon_platform_log("[main] Fatal: failed to initialize platform.\n");
        return 1;
    }

    moon_game_t game;
    if (!moon_game_init(&game, width, height)) {
        moon_platform_log("[main] Fatal: failed to initialize game state.\n");
        moon_platform_shutdown();
        return 1;
    }

    uint32_t last_time = moon_platform_get_ticks_ms();

    while (game.running) {
        uint32_t current_time = moon_platform_get_ticks_ms();
        uint32_t delta_ms = current_time - last_time;
        if (delta_ms < 1) delta_ms = 1;
        last_time = current_time;

        /* 1. Poll & handle inputs */
        moon_event_t ev;
        while (moon_platform_poll_event(&ev)) {
            moon_game_handle_event(&game, &ev);
        }

        /* 2. Simulation update */
        moon_game_update(&game, delta_ms);

        if (max_frames > 0 && game.frame_count >= (uint32_t)max_frames) {
            game.running = false;
        }

        /* 3. Render frame to RGB565 surface */
        int stride = 0;
        uint16_t *fb = moon_platform_get_framebuffer(&stride);
        if (fb) {
            moon_game_render(&game, fb, width, height, stride);
        }

        /* 4. Present frame */
        moon_platform_present();

        /* 5. Frame rate limiter (target 60 FPS) */
        uint32_t frame_elapsed = moon_platform_get_ticks_ms() - current_time;
        if (frame_elapsed < MOON_FRAME_TIME_MS) {
            moon_platform_delay_ms(MOON_FRAME_TIME_MS - frame_elapsed);
        }
    }

    moon_platform_log("[main] Shutting down Moonbase Lambda.\n");
    moon_game_shutdown(&game);
    moon_platform_shutdown();

    return 0;
}
