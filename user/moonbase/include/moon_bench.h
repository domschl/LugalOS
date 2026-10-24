#ifndef MOON_BENCH_H
#define MOON_BENCH_H

#include <stdint.h>
#include "moon_game.h"

/* Frame-time benchmark: the same scripted pan over the map, with 3 and with about 30
 * structures, textured and flat, then an idle run. Runs the real render and present
 * path through the platform API (no frame cap), so the numbers are what the board
 * can do. Needs an initialised platform; results are returned, not printed, so the
 * caller can report them after giving the screen back. */
#define MOON_BENCH_SCENARIOS 5

typedef struct {
    const char *name;
    uint32_t frames;
    uint32_t median_us, p95_us, max_us;
    moon_phase_us_t phase;          /* mean per phase */
    uint32_t present_us;            /* mean */
    uint32_t renders;               /* idle run: frames actually drawn */
    uint32_t huds;                  /* idle run: top-bar-and-ribbon-only updates */
    uint32_t busy_permille;         /* idle run: time spent drawing, in parts per thousand */
} moon_bench_result_t;

/* `seconds` per scenario; `samples` holds the frame times of one scenario (the caller
 * places it: on the P4 in PSRAM, as SRAM is short). Returns the number of results
 * written (MOON_BENCH_SCENARIOS). */
int moon_bench_run(moon_game_t *game, int width, int height, int seconds, moon_bench_result_t *out,
                   uint32_t *samples, uint32_t max_samples);

/* Cost of the basic operations on the platform's frame buffer (logged) */
void moon_bench_micro(int width, int height);

void moon_bench_print(const moon_bench_result_t *results, int n);

#endif /* MOON_BENCH_H */
