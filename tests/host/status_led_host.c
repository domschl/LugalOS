/*
 * The status LED's pattern logic (kernel/status_led_pattern.c) on the host: which pattern each state
 * shows, the timing of each pattern on its 100 ms grid, and the colours.
 */
#include "kernel/status_led.h"
#include <stdio.h>

static int g_bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); g_bad = 1; } } while (0)

static unsigned lit_ms(status_led_pattern_t p, unsigned span) {
    unsigned n = 0;
    for (unsigned t = 0; t < span; t += STATUS_LED_STEP_MS) n += status_led_level(p, t) ? STATUS_LED_STEP_MS : 0;
    return n;
}

static unsigned edges(status_led_pattern_t p, unsigned span) {
    unsigned n = 0;
    for (unsigned t = 0; t < span; t += STATUS_LED_STEP_MS)
        if (status_led_level(p, t) && !status_led_level(p, t ? t - STATUS_LED_STEP_MS : span - STATUS_LED_STEP_MS)) n++;
    return n;
}

int main(void) {
    CHECK(lit_ms(STATUS_LED_OFF, 3000) == 0, "off is dark");
    CHECK(lit_ms(STATUS_LED_SOLID, 3000) == 3000, "solid is lit");
    CHECK(lit_ms(STATUS_LED_SETUP, 2000) == 1000 && edges(STATUS_LED_SETUP, 2000) == 2, "setup: 1 Hz, half lit");
    CHECK(lit_ms(STATUS_LED_RESET, 1000) == 500 && edges(STATUS_LED_RESET, 1000) == 5, "reset: 5 Hz");
    CHECK(lit_ms(STATUS_LED_JOINING, 1000) == 200 && edges(STATUS_LED_JOINING, 1000) == 2, "joining: two flashes a second");
    CHECK(lit_ms(STATUS_LED_JOINED, 3000) == 100 && edges(STATUS_LED_JOINED, 3000) == 1, "joined: one short flash in 3 s");
    CHECK(lit_ms(STATUS_LED_FAILED, 3000) == 300 && edges(STATUS_LED_FAILED, 3000) == 3, "failed: three flashes in 3 s");
    CHECK(status_led_level(STATUS_LED_JOINED, 3000) == status_led_level(STATUS_LED_JOINED, 0), "the period repeats");
    CHECK(!status_led_level((status_led_pattern_t)99, 0), "an unknown pattern is dark");

    status_led_in_t in = { 0 };
    in.setup_possible = true;
    CHECK(status_led_choose(&in) == STATUS_LED_OFF, "nothing configured: off");
    in.net_up = true;
    CHECK(status_led_choose(&in) == STATUS_LED_JOINED, "network up: joined");
    in.net_up = false; in.wlan_set = true; in.net_down_s = 5;
    CHECK(status_led_choose(&in) == STATUS_LED_JOINING, "configured, not yet up: joining");
    in.net_down_s = STATUS_LED_FAILED_AFTER_S;
    CHECK(status_led_choose(&in) == STATUS_LED_FAILED, "configured, down too long: failed");
    in.net_up = true;
    CHECK(status_led_choose(&in) == STATUS_LED_JOINED, "up again: joined");
    in.setup_ap = true;
    CHECK(status_led_choose(&in) == STATUS_LED_SETUP, "setup network wanted beats the network state");
    in.button_held_ms = SETUP_BUTTON_SETUP_MS - 1;
    CHECK(status_led_choose(&in) == STATUS_LED_SETUP, "a short hold changes nothing");
    in.button_held_ms = SETUP_BUTTON_SETUP_MS;
    CHECK(status_led_choose(&in) == STATUS_LED_SOLID, "held long enough for setup: solid");
    in.setup_possible = false;
    CHECK(status_led_choose(&in) == STATUS_LED_SETUP, "a claimed node has no setup window to announce");
    in.setup_possible = true;
    in.button_held_ms = SETUP_BUTTON_RESET_MS;
    CHECK(status_led_choose(&in) == STATUS_LED_RESET, "held for the reset: fast blink");
    in.button_held_ms = 0; in.reset_shown = true;
    CHECK(status_led_choose(&in) == STATUS_LED_RESET, "just reset: fast blink after the button is up");
    in.reset_shown = false;
    CHECK(status_led_choose(&in) == STATUS_LED_SETUP, "then the setup pattern");

    for (int p = 0; p <= STATUS_LED_FAILED; p++) {
        uint8_t r, g, b;
        status_led_rgb((status_led_pattern_t)p, &r, &g, &b);
        CHECK((p == STATUS_LED_OFF) == (!r && !g && !b), "pattern %d colour %s", p, p ? "is dark" : "is lit");
    }

    puts(g_bad ? "status_led_host: FAILED" : "status_led_host: all passed");
    return g_bad;
}
