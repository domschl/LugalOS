/* Host test of the WS2812 wire-format encoder (45.5): every bit's pulse widths
 * against the datasheet's windows, the colour order, and the terminator. Not a
 * check that the part lights -- only the board can say that -- but the encoder
 * is the one place a wrong bit order or a swapped width hides. */
#include "drivers/ws2812.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The data sheet (WS2812B), at 100 ns per tick: T0H 220-380 ns, T0L 580-1000,
 * T1H 580-1000, T1L 220-420; a whole bit 1.25 us +/- 600 ns. */
static unsigned dur0(uint32_t w) { return w & 0x7fff; }
static unsigned lv0(uint32_t w)  { return (w >> 15) & 1; }
static unsigned dur1(uint32_t w) { return (w >> 16) & 0x7fff; }
static unsigned lv1(uint32_t w)  { return w >> 31; }

static unsigned decode(const uint32_t *w, int n_bits_check_timing) {
    unsigned v = 0;
    for (int i = 0; i < 24; i++) {
        CHECK(lv0(w[i]) == 1 && lv1(w[i]) == 0, "bit %d: high then low", i);
        unsigned h = dur0(w[i]) * 100, l = dur1(w[i]) * 100;
        int one = h > 450;
        if (one) CHECK(h >= 580 && h <= 1000 && l >= 220 && l <= 420, "bit %d one: %u/%u ns", i, h, l);
        else     CHECK(h >= 220 && h <= 380 && l >= 580 && l <= 1000, "bit %d zero: %u/%u ns", i, h, l);
        CHECK(h + l >= 1000 && h + l <= 1500, "bit %d period %u ns", i, h + l);
        v = v << 1 | one;
    }
    (void)n_bits_check_timing;
    return v;
}

int main(void) {
    uint32_t w[WS2812_WORDS + 1];
    w[WS2812_WORDS] = 0xdeadbeef;
    /* every colour corner and a few values with distinct bits per channel */
    unsigned vals[][3] = {{0,0,0},{255,255,255},{255,0,0},{0,255,0},{0,0,255},{1,2,4},{0x80,0x40,0x20},{0xa5,0x5a,0xc3}};
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        unsigned r = vals[i][0], g = vals[i][1], b = vals[i][2];
        ws2812_encode(w, r, g, b, false);
        CHECK(decode(w, 1) == (g << 16 | r << 8 | b), "GRB of %u,%u,%u", r, g, b);
        CHECK(w[24] == 0, "terminator");
        ws2812_encode(w, r, g, b, true);
        CHECK(decode(w, 1) == (r << 16 | g << 8 | b), "RGB of %u,%u,%u", r, g, b);
    }
    CHECK(w[WS2812_WORDS] == 0xdeadbeef, "wrote past the table");
    /* exhaustive over one channel: all 256 values come back */
    for (unsigned v = 0; v < 256; v++) {
        ws2812_encode(w, 0, 0, (uint8_t)v, false);
        CHECK(decode(w, 0) == v, "blue %u", v);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ws2812_host OK\n");
    return 0;
}
