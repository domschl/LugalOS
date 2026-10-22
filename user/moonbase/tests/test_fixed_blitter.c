#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "moon_fixed.h"
#include "moon_blitter.h"

static void test_fixed_math(void) {
    printf("[test] Checking fixed-point arithmetic...\n");

    /* Multiplication */
    fixed32_t a = INT_TO_FIXED(3);
    fixed32_t b = INT_TO_FIXED(4);
    assert(FIXED_TO_INT(fixed_mul(a, b)) == 12);

    /* Fractional multiplication: 1.5 * 2.0 = 3.0 */
    fixed32_t f1 = INT_TO_FIXED(1) + FIXED_HALF;
    fixed32_t f2 = INT_TO_FIXED(2);
    assert(FIXED_TO_INT(fixed_mul(f1, f2)) == 3);

    /* Division */
    assert(FIXED_TO_INT(fixed_div(INT_TO_FIXED(10), INT_TO_FIXED(2))) == 5);
    assert(FIXED_TO_INT(fixed_div(INT_TO_FIXED(7), INT_TO_FIXED(2))) == 3);

    /* Square root */
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(0))) == 0);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(1))) == 1);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(4))) == 2);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(9))) == 3);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(16))) == 4);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(25))) == 5);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(100))) == 10);
    assert(FIXED_TO_INT(fixed_sqrt(INT_TO_FIXED(144))) == 12);

    /* Trigonometry */
    assert(fixed_sin_deg(0) == 0);
    assert(fixed_sin_deg(90) == FIXED_ONE);
    assert(fixed_sin_deg(180) == 0);
    assert(fixed_sin_deg(270) == -FIXED_ONE);
    assert(fixed_sin_deg(360) == 0);

    assert(fixed_cos_deg(0) == FIXED_ONE);
    assert(fixed_cos_deg(90) == 0);
    assert(fixed_cos_deg(180) == -FIXED_ONE);
    assert(fixed_cos_deg(270) == 0);
    assert(fixed_cos_deg(360) == FIXED_ONE);

    /* Lerp */
    fixed32_t lerp_mid = fixed_lerp(INT_TO_FIXED(10), INT_TO_FIXED(20), FIXED_HALF);
    assert(FIXED_TO_INT(lerp_mid) == 15);

    printf("[test] Fixed-point arithmetic: PASS\n");
}

static void test_blitter_and_clipping(void) {
    printf("[test] Checking blitter routines & boundary clipping...\n");

    const int W = 64;
    const int H = 64;
    uint16_t *pixels = (uint16_t *)calloc(W * H, sizeof(uint16_t));
    assert(pixels != NULL);

    moon_surface_t surf = moon_surface_make(pixels, W, H, W);

    /* Clear */
    moon_blit_clear(&surf, COLOR_SPACE_BLACK);
    assert(surf.pixels[0] == COLOR_SPACE_BLACK);

    /* Pixel within & out of bounds */
    moon_blit_pixel(&surf, 10, 10, COLOR_GOLD_FOIL);
    assert(surf.pixels[10 * W + 10] == COLOR_GOLD_FOIL);
    moon_blit_pixel(&surf, -5, 10, COLOR_GOLD_FOIL); /* Should clip safely */
    moon_blit_pixel(&surf, 10, 80, COLOR_GOLD_FOIL); /* Should clip safely */

    /* Filled and outline rectangles with boundary clipping */
    moon_blit_fill_rect(&surf, -10, -10, 20, 20, COLOR_ALERT_RED);
    assert(surf.pixels[0] == COLOR_ALERT_RED);

    moon_blit_rect_outline(&surf, 5, 5, 20, 20, COLOR_LASER_CYAN);

    /* Lines intersecting borders */
    moon_blit_line(&surf, -50, -50, 100, 100, COLOR_ENERGY_YELLOW);

    /* Circles */
    moon_blit_circle(&surf, 32, 32, 10, COLOR_STATUS_GREEN);
    moon_blit_circle_filled(&surf, 32, 32, 5, COLOR_SOLAR_BLUE);

    /* Isometric tile and block */
    moon_blit_iso_tile(&surf, 32, 20, 16, 8, COLOR_REGOLITH_MID, COLOR_REGOLITH_LIGHT);
    moon_blit_iso_block(&surf, 32, 45, 16, 8, 12,
                        COLOR_GOLD_FOIL, COLOR_REGOLITH_DARK,
                        COLOR_REGOLITH_MID, COLOR_LUNAR_WHITE);

    /* Text & formatted text */
    moon_blit_text(&surf, 0, 0, "TEST", COLOR_LUNAR_WHITE, 0, true);
    moon_blit_text_fmt(&surf, 0, 16, COLOR_GOLD_FOIL, 0, true, "VAL=%d", 42);

    /* Sprite with transparency key */
    uint16_t sprite_buf[16] = {
        COLOR_ALERT_RED, 0x0000,          0x0000,          COLOR_ALERT_RED,
        0x0000,          COLOR_ALERT_RED, COLOR_ALERT_RED, 0x0000,
        0x0000,          COLOR_ALERT_RED, COLOR_ALERT_RED, 0x0000,
        COLOR_ALERT_RED, 0x0000,          0x0000,          COLOR_ALERT_RED
    };
    moon_surface_t sprite = moon_surface_make(sprite_buf, 4, 4, 4);

    /* Blit sprite overlapping right edge */
    moon_blit_sprite(&surf, W - 2, H - 2, &sprite, 0x0000);

    /* Verify color dim and lerp */
    uint16_t c_dim = moon_color_dim(RGB565(200, 200, 200), 128);
    assert(c_dim != 0);
    uint16_t c_lerp = moon_color_lerp(COLOR_SPACE_BLACK, COLOR_LUNAR_WHITE, FIXED_HALF);
    assert(c_lerp != COLOR_SPACE_BLACK);

    free(pixels);
    printf("[test] Blitter routines & boundary clipping: PASS\n");
}

int main(void) {
    test_fixed_math();
    test_blitter_and_clipping();
    printf("[test] All Milestone 64.2 tests passed cleanly.\n");
    return 0;
}
