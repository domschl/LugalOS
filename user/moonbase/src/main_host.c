#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "moon_platform.h"
#include "moon_config.h"
#include "moon_game.h"
#include "moon_bench.h"

static moon_game_t s_game;

int main(int argc, char **argv) {
    int width = MOON_PRESET_LCD7B_W;
    int height = MOON_PRESET_LCD7B_H;
    int max_frames = 0;
    const char *preset_name = "ESP32-P4-LCD-7B (1024x600)";
    const char *dump_ppm = NULL;
    uint32_t seed = (uint32_t)time(NULL); /* a new map each run */
    int difficulty = MOON_DIFFICULTY_NORMAL;
    bool bench = false, micro = false;
    bool difficulty_given = false;

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
        } else if (strcmp(argv[i], "--bench") == 0) {
            bench = true;
        } else if (strcmp(argv[i], "--micro") == 0) {
            micro = true;
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            seed = (uint32_t)strtoul(argv[i] + 7, NULL, 0);
        } else if (strncmp(argv[i], "--difficulty=", 13) == 0) {
            const char *d = argv[i] + 13;
            difficulty_given = true;
            difficulty = (d[0] == 'e' || d[0] == 'c' || d[0] == '0') ? MOON_DIFFICULTY_EASY
                       : (d[0] == 'h' || d[0] == '2') ? MOON_DIFFICULTY_HARD : MOON_DIFFICULTY_NORMAL;
        } else if (strncmp(argv[i], "--dump-ppm=", 11) == 0) {
            dump_ppm = argv[i] + 11;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Moonbase Lambda - Host Simulator\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --7b              Target LCD-7B preset (1024x600, default)\n");
            printf("  --tab5            Target Tab5 landscape preset (1280x720)\n");
            printf("  --tab5-portrait   Target Tab5 portrait preset (720x1280)\n");
            printf("  --seed=N          Map seed (default: the current time)\n");
            printf("  --difficulty=L    easy|normal|hard (also cadet|engineer|commander, 0..2); default normal\n");
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

    s_game.seed_request = seed;
    s_game.difficulty_request = (uint8_t)(difficulty + 1);
    s_game.show_title = !difficulty_given && max_frames == 0; /* automated runs skip the title */
    if (!moon_game_init(&s_game, width, height)) {
        moon_platform_log("[main] Fatal: failed to initialize game state.\n");
        moon_platform_shutdown();
        return 1;
    }
    if (!s_game.show_title && max_frames == 0 && !bench && !micro) {
        s_game.landing_active = true;
        s_game.landing_elapsed_ms = 0;
        s_game.landing_y_offset = 260;
    }

    s_game.clock_us = moon_platform_get_ticks_us;
    if (micro) {
        moon_bench_micro(width, height);
        moon_platform_shutdown();
        return 0;
    }
    if (bench) {
        moon_bench_result_t results[MOON_BENCH_SCENARIOS];
        static uint32_t samples[4096];
        int n = moon_bench_run(&s_game, width, height, 4, results, samples, 4096);
        moon_bench_print(results, n);
        moon_platform_shutdown();
        return 0;
    }

    uint32_t last_time = moon_platform_get_ticks_ms();

    while (s_game.running) {
        uint32_t current_time = moon_platform_get_ticks_ms();
        uint32_t delta_ms = current_time - last_time;
        if (delta_ms < 1) delta_ms = 1;
        last_time = current_time;

        /* 1. Poll & handle inputs */
        moon_event_t ev;
        while (moon_platform_poll_event(&ev)) {
            moon_game_handle_event(&s_game, &ev);
        }

        /* 2. Simulation update */
        moon_game_update(&s_game, delta_ms);

        if (max_frames > 0 && s_game.frame_count >= (uint32_t)max_frames) {
            s_game.running = false;
        }

        /* 3. Render frame to RGB565 surface, if anything changed (automated runs draw every frame) */
        if (max_frames > 0) s_game.dirty = MOON_DIRTY_WORLD;
        int dirty = moon_game_take_dirty(&s_game);
        int stride = 0;
        uint16_t *fb = (dirty == MOON_DIRTY_WORLD) ? moon_platform_get_framebuffer(&stride) : NULL;
        if (dirty == MOON_DIRTY_HUD) {
            uint16_t *disp = moon_platform_get_display_framebuffer(&stride);
            moon_game_render_hud(&s_game, disp, width, height, stride);
        }
        if (fb) {
            moon_game_render(&s_game, fb, width, height, stride);

            if (dump_ppm && max_frames > 0 && s_game.frame_count >= (uint32_t)max_frames) {
                FILE *fp = fopen(dump_ppm, "wb");
                if (fp) {
                    fprintf(fp, "P6\n%d %d\n255\n", width, height);
                    for (int y = 0; y < height; y++) {
                        for (int x = 0; x < width; x++) {
                            uint16_t c = fb[y * stride + x];
                            uint8_t r = ((c >> 11) & 0x1F) * 255 / 31;
                            uint8_t g = ((c >> 5) & 0x3F) * 255 / 63;
                            uint8_t b = (c & 0x1F) * 255 / 31;
                            fputc(r, fp);
                            fputc(g, fp);
                            fputc(b, fp);
                        }
                    }
                    fclose(fp);
                    moon_platform_log("[main] Dumped frame to %s\n", dump_ppm);
                }
            }
        }

        /* 4. Present frame */
        if (dirty != MOON_DIRTY_NONE) {
            uint32_t t1 = moon_platform_get_ticks_us();
            moon_platform_present();
            if (dirty == MOON_DIRTY_WORLD) moon_game_note_frame(&s_game, moon_platform_get_ticks_us() - t1, moon_platform_get_ticks_us() - t1);
        }

        /* 5. Frame rate limiter (target 60 FPS) */
        uint32_t frame_elapsed = moon_platform_get_ticks_ms() - current_time;
        if (frame_elapsed < MOON_FRAME_TIME_MS) {
            moon_platform_delay_ms(MOON_FRAME_TIME_MS - frame_elapsed);
        }
    }

    moon_platform_log("[main] Shutting down Moonbase Lambda.\n");
    moon_game_shutdown(&s_game);
    moon_platform_shutdown();

    return 0;
}
