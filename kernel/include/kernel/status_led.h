#ifndef KERNEL_STATUS_LED_H
#define KERNEL_STATUS_LED_H

/* The status LED of a headless node: one light that says which mode the node is in.
 *
 * Patterns, on a 100 ms grid:
 *   SETUP    1 Hz blink                   the setup access point is (about to be) up
 *   SOLID    on                           the button is held long enough to open setup on an unclaimed node; let go now
 *   RESET    5 Hz blink                   the owner reset went through
 *   JOINING  two short flashes per second  a Wi-Fi network is configured and not joined yet
 *   JOINED   one short flash per 3 s      the network is up
 *   FAILED   three short flashes per 3 s  a configured network was not joined for FAILED_AFTER_S
 *
 * The light is a plain GPIO (CONFIG_STATUS_LED_GPIO, active low with CONFIG_STATUS_LED_ACTIVE_LOW),
 * the Pico 2 W's LED behind the Wi-Fi chip (CONFIG_STATUS_LED_CYW43) or the C6-Zero's RGB pixel
 * (CONFIG_STATUS_LED_WS2812, where each pattern has a colour). A board with none of them has no task. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_SETUP,
    STATUS_LED_SOLID,
    STATUS_LED_RESET,
    STATUS_LED_JOINING,
    STATUS_LED_JOINED,
    STATUS_LED_FAILED,
} status_led_pattern_t;

/* How long the setup button is held to open a setup window, and to reset the owner part (kernel/setup_button.c). */
#define SETUP_BUTTON_SETUP_MS   5000u
#define SETUP_BUTTON_RESET_MS   15000u

#define STATUS_LED_STEP_MS      100u
#define STATUS_LED_FAILED_AFTER_S 30u
#define STATUS_LED_RESET_SHOW_MS 3000u

typedef struct {
    uint32_t button_held_ms;     /* how long the setup button has been down, 0 if up */
    bool     setup_possible;     /* the node is unclaimed, so a setup window can do something */
    bool     reset_shown;        /* an owner reset happened within STATUS_LED_RESET_SHOW_MS */
    bool     setup_ap;           /* setup_ap_wanted() */
    bool     net_up;             /* an interface holds an address */
    bool     wlan_set;           /* a Wi-Fi network is configured */
    uint32_t net_down_s;         /* seconds since the network was last up (or since boot) */
} status_led_in_t;

/* Pure: the pattern to show for these inputs, whether the light is on at time `t_ms`, and the colour
 * of a pattern on an RGB light. */
status_led_pattern_t status_led_choose(const status_led_in_t *in);
bool status_led_level(status_led_pattern_t p, uint32_t t_ms);
void status_led_rgb(status_led_pattern_t p, uint8_t *r, uint8_t *g, uint8_t *b);

/* The button task reports its hold, and an owner reset; the LED task reads them. A no-op without a light. */
void status_led_button(uint32_t held_ms);
void status_led_reset_done(void);

/* Boot task: starts the LED task on a board that has a light. */
void status_led_start(void);

#endif
