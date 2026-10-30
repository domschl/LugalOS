#include "kernel/status_led.h"

/* One bit per 100 ms slot of the period; bit 0 is the first slot. */
typedef struct { uint8_t slots; uint32_t bits; } pat_t;

static const pat_t PATTERNS[] = {
    [STATUS_LED_OFF]     = { 1,  0 },
    [STATUS_LED_SETUP]   = { 10, 0x01fu },
    [STATUS_LED_SOLID]   = { 1,  1 },
    [STATUS_LED_RESET]   = { 2,  1 },
    [STATUS_LED_JOINING] = { 10, 0x005u },
    [STATUS_LED_JOINED]  = { 30, 0x001u },
    [STATUS_LED_FAILED]  = { 30, 0x015u },
};

status_led_pattern_t status_led_choose(const status_led_in_t *in) {
    if (in->button_held_ms >= SETUP_BUTTON_RESET_MS || in->reset_shown) return STATUS_LED_RESET;
    if (in->setup_possible && in->button_held_ms >= SETUP_BUTTON_SETUP_MS) return STATUS_LED_SOLID;
    if (in->setup_ap) return STATUS_LED_SETUP;
    if (in->net_up) return STATUS_LED_JOINED;
    if (in->wlan_set) return in->net_down_s < STATUS_LED_FAILED_AFTER_S ? STATUS_LED_JOINING : STATUS_LED_FAILED;
    return STATUS_LED_OFF;
}

bool status_led_level(status_led_pattern_t p, uint32_t t_ms) {
    if ((unsigned)p >= sizeof PATTERNS / sizeof PATTERNS[0]) return false;
    const pat_t *q = &PATTERNS[p];
    return (q->bits >> ((t_ms / STATUS_LED_STEP_MS) % q->slots)) & 1u;
}

void status_led_rgb(status_led_pattern_t p, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t c[3] = { 0, 0, 0 };
    switch (p) {
    case STATUS_LED_SETUP:   c[2] = 40; break;
    case STATUS_LED_SOLID:   c[0] = 30; c[1] = 30; c[2] = 30; break;
    case STATUS_LED_RESET:   c[0] = 40; break;
    case STATUS_LED_JOINING: c[0] = 30; c[1] = 20; break;
    case STATUS_LED_JOINED:  c[1] = 40; break;
    case STATUS_LED_FAILED:  c[0] = 40; c[1] = 4; break;
    default: break;
    }
    *r = c[0]; *g = c[1]; *b = c[2];
}
