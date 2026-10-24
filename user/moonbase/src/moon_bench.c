#include "moon_bench.h"
#include "moon_platform.h"
#include "moon_sprites.h"
#include "moon_blitter.h"
#include "moon_grid.h"
#include <string.h>

static volatile uint32_t s_sink;   /* keeps the read loop from being optimised away */

static void add_structures(moon_game_t *game, int target) {
    static const moon_structure_type_t cycle[] = {
        MOON_STRUCT_SOLAR_ARRAY, MOON_STRUCT_BATTERY_BANK, MOON_STRUCT_REGOLITH_PLANT, MOON_STRUCT_POWER_CONDUIT
    };
    int n = 0;
    for (int r = 2; r < 16 && game->structs.count < target; r++) {
        for (int y = 32 - r; y <= 32 + r && game->structs.count < target; y++) {
            for (int x = 32 - r; x <= 32 + r && game->structs.count < target; x++) {
                if (x != 32 - r && x != 32 + r && y != 32 - r && y != 32 + r) continue;   /* the ring only */
                if (moon_structure_place(&game->grid, &game->structs, cycle[n % 4], x, y)) n++;
            }
        }
    }
}

static void sort_u32(uint32_t *v, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        uint32_t x = v[i], j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
}

static void fresh_game(moon_game_t *game, int w, int h, bool textured, int structures) {
    uint32_t (*clk)(void) = game->clock_us;
    game->show_title = false;
    game->seed_request = 19690720;
    game->difficulty_request = MOON_DIFFICULTY_NORMAL + 1;
    moon_game_init(game, w, h);
    game->clock_us = clk;
    game->grid.textured_terrain = textured;
    add_structures(game, structures);
    moon_resources_solve_power_grid(&game->grid, &game->structs);
}

/* Pan along a fixed route: back and forth in x, slowly down and up in y */
static void pan(moon_game_t *game, int frame) {
    int phase = frame % 400;
    int dx = (phase < 200 ? phase : 400 - phase) * 8 - 800;         /* -800 .. +800 */
    int dy = ((frame / 400) % 2 ? -1 : 1) * (phase % 200);
    game->glide = false;
    game->cam_x = dx;
    game->cam_y = 32 * MOON_TILE_H + dy;
    moon_game_clamp_camera(game);
}

static void run_pan(moon_game_t *game, int w, int h, int seconds, const char *name, moon_bench_result_t *out,
                    uint32_t *s_samples, uint32_t max_samples) {
    memset(out, 0, sizeof(*out));
    out->name = name;
    uint64_t sum[8] = { 0 };
    uint32_t n = 0;
    uint32_t start = moon_platform_get_ticks_ms();
    uint32_t last = moon_platform_get_ticks_us();
    while (moon_platform_get_ticks_ms() - start < (uint32_t)seconds * 1000u && n < max_samples) {
        uint32_t t0 = moon_platform_get_ticks_us();
        moon_game_update(game, (t0 - last) / 1000u);
        last = t0;
        pan(game, (int)n);
        int stride = 0;
        uint16_t *fb = moon_platform_get_framebuffer(&stride);
        moon_game_render(game, fb, w, h, stride);
        uint32_t t1 = moon_platform_get_ticks_us();
        moon_platform_present();
        uint32_t t2 = moon_platform_get_ticks_us();
        game->present_us = t2 - t1;
        s_samples[n++] = t2 - t0;
        sum[0] += game->phase.world; sum[2] += game->phase.overlay;
        sum[3] += game->phase.minimap; sum[4] += game->phase.ui; sum[5] += t2 - t1;
    }
    out->frames = n;
    if (n) {
        sort_u32(s_samples, n);
        out->median_us = s_samples[n / 2];
        out->p95_us = s_samples[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
        out->max_us = s_samples[n - 1];
        out->phase.world = (uint32_t)(sum[0] / n);
        out->phase.overlay = (uint32_t)(sum[2] / n); out->phase.minimap = (uint32_t)(sum[3] / n);
        out->phase.ui = (uint32_t)(sum[4] / n); out->present_us = (uint32_t)(sum[5] / n);
    }
}

/* The loop the platform runs, with nobody touching anything: how often it draws and what
 * share of the time that takes */
static void run_idle(moon_game_t *game, int w, int h, int seconds, moon_bench_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->name = "idle, nothing pressed";
    uint32_t start_ms = moon_platform_get_ticks_ms();
    uint32_t last_ms = start_ms;
    uint64_t busy_us = 0;
    uint32_t renders = 0, loops = 0, huds = 0;
    while (moon_platform_get_ticks_ms() - start_ms < (uint32_t)seconds * 1000u) {
        uint32_t t0 = moon_platform_get_ticks_us();
        uint32_t now = moon_platform_get_ticks_ms();
        moon_game_update(game, now - last_ms);
        last_ms = now;
        int dirty = moon_game_take_dirty(game);
        if (dirty == MOON_DIRTY_WORLD) {
            int stride = 0;
            uint16_t *fb = moon_platform_get_framebuffer(&stride);
            moon_game_render(game, fb, w, h, stride);
            moon_platform_present();
            renders++;
        } else if (dirty == MOON_DIRTY_HUD) {   /* as the platform loop: top bar and ribbon rows only */
            int stride = 0;
            uint16_t *fb = moon_platform_get_display_framebuffer(&stride);
            moon_game_render_hud(game, fb, w, h, stride);
            moon_platform_present_rows(0, game->ui.hud.h + 1);
            moon_platform_present_rows(game->ui.ribbon_bar.y, h);
            huds++;
        }
        busy_us += moon_platform_get_ticks_us() - t0;
        loops++;
        moon_platform_delay_ms(16);
    }
    out->frames = loops;
    out->renders = renders;
    out->huds = huds;
    out->busy_permille = (uint32_t)(busy_us * 1000u / ((uint64_t)seconds * 1000000u));
}

int moon_bench_run(moon_game_t *game, int width, int height, int seconds, moon_bench_result_t *out,
                   uint32_t *samples, uint32_t max_samples) {
    fresh_game(game, width, height, true, 4);
    run_pan(game, width, height, seconds, "pan, 3 structures, textured", &out[0], samples, max_samples);
    fresh_game(game, width, height, false, 4);
    run_pan(game, width, height, seconds, "pan, 3 structures, flat", &out[1], samples, max_samples);
    fresh_game(game, width, height, true, 34);
    run_pan(game, width, height, seconds, "pan, 30 structures, textured", &out[2], samples, max_samples);
    fresh_game(game, width, height, false, 34);
    run_pan(game, width, height, seconds, "pan, 30 structures, flat", &out[3], samples, max_samples);
    fresh_game(game, width, height, true, 34);
    run_idle(game, width, height, seconds, &out[4]);
    return MOON_BENCH_SCENARIOS;
}

/* Plain-text report through the platform's log */
void moon_bench_print(const moon_bench_result_t *r, int n) {
    for (int i = 0; i < n; i++) {
        if (r[i].renders || r[i].busy_permille || i == n - 1) {
            moon_platform_log("%-30s loops %4u  world %4u  hud %4u  busy %u.%u%%\n", r[i].name, (unsigned)r[i].frames,
                              (unsigned)r[i].renders, (unsigned)r[i].huds, (unsigned)(r[i].busy_permille / 10), (unsigned)(r[i].busy_permille % 10));
            continue;
        }
        moon_platform_log("%-30s %4u frames  median %3u.%u ms  p95 %3u.%u ms  max %3u ms  = %u fps (p95 %u)\n", r[i].name,
                          (unsigned)r[i].frames, (unsigned)(r[i].median_us / 1000), (unsigned)(r[i].median_us % 1000 / 100),
                          (unsigned)(r[i].p95_us / 1000), (unsigned)(r[i].p95_us % 1000 / 100), (unsigned)(r[i].max_us / 1000),
                          (unsigned)(r[i].median_us ? 1000000u / r[i].median_us : 0), (unsigned)(r[i].p95_us ? 1000000u / r[i].p95_us : 0));
        moon_platform_log("%-30s   world %u  overlay %u  minimap %u  ui %u  present %u us\n", "",
                          (unsigned)r[i].phase.world, (unsigned)r[i].phase.overlay,
                          (unsigned)r[i].phase.minimap, (unsigned)r[i].phase.ui, (unsigned)r[i].present_us);
    }
}

/* What the basic operations cost on this memory, on the platform's own frame buffer */
#define MICRO(label, reps, code)                                                         \
    do {                                                                                  \
        uint32_t t_a = moon_platform_get_ticks_us();                                      \
        for (int rep_ = 0; rep_ < (reps); rep_++) { code; }                               \
        uint32_t t_b = moon_platform_get_ticks_us();                                      \
        moon_platform_log("  %-44s %6u us\n", label, (unsigned)((t_b - t_a) / (reps)));   \
    } while (0)

void moon_bench_micro(int width, int height) {
    int stride = 0;
    uint16_t *fb = moon_platform_get_framebuffer(&stride);
    moon_surface_t surf = moon_surface_make(fb, width, height, stride);
    moon_platform_log("micro-benchmarks, %dx%d frame, per operation:\n", width, height);

    MICRO("clear the whole frame", 4, moon_blit_clear(&surf, COLOR_SPACE_BLACK));
    MICRO("store 16-bit pixels, one loop over the frame", 4, {
        for (int y = 0; y < height; y++) { uint16_t *row = fb + (size_t)y * stride; for (int x = 0; x < width; x++) row[x] = (uint16_t)(x ^ y); }
    });
    MICRO("read 16-bit pixels, one loop over the frame", 4, {
        uint32_t acc = 0;
        for (int y = 0; y < height; y++) { const uint16_t *row = fb + (size_t)y * stride; for (int x = 0; x < width; x++) acc += row[x]; }
        s_sink = acc;
    });
    MICRO("memmove the frame by 8 rows (scroll)", 4, memmove(fb + 8 * (size_t)stride, fb, (size_t)(height - 8) * stride * sizeof(uint16_t)));
    MICRO("tint the world area", 2, moon_blit_tint_rect(&surf, 0, 34, width, height - 74, 150, 160, 180));
    MICRO("300 flat iso tiles", 2, {
        for (int i = 0; i < 300; i++) moon_blit_iso_tile(&surf, 40 + (i % 15) * 64, 60 + (i / 15) * 16, 32, 16, RGB565(90, 90, 96), 0);
    });
    MICRO("300 textured iso tiles", 1, {
        for (int i = 0; i < 300; i++) moon_blit_iso_tile_textured(&surf, 40 + (i % 15) * 64, 60 + (i / 15) * 16, 32, 16, i % 8, i / 8, MOON_TERRAIN_REGOLITH_FLAT, 0, 0);
    });
    MICRO("40 text strings of 20 glyphs", 4, {
        for (int i = 0; i < 40; i++) moon_blit_text(&surf, 10, 40 + i * 12, "ABCDEFGHIJKLMNOPQRST", 0xFFFF, 0, true);
    });
    MICRO("20 sprites of 64x48", 4, {
        for (int i = 0; i < 20; i++) moon_blit_sprite(&surf, 20 + (i % 10) * 70, 100 + (i / 10) * 60, &g_surf_sinterer, MOON_KEY_COLOR);
    });
}
