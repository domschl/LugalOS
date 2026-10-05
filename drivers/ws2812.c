/* The portable half of the WS2812 driver: wire-format encoding (45.5). */
#include "drivers/ws2812.h"

void ws2812_encode(uint32_t *out, uint8_t r, uint8_t g, uint8_t b, bool rgb_order) {
    uint32_t v = rgb_order ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | b
                           : ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    for (int i = 0; i < 24; i++) {                       /* MSB first */
        bool one = (v >> (23 - i)) & 1u;
        out[i] = one ? WS2812_ITEM(1, WS2812_T1H, 0, WS2812_T1L)
                     : WS2812_ITEM(1, WS2812_T0H, 0, WS2812_T0L);
    }
    out[24] = 0;                                         /* duration 0: end of transmission */
}
