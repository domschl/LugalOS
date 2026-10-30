/* The status LED task (kernel/include/kernel/status_led.h): polls what the node is doing every
 * STATUS_LED_STEP_MS, picks the pattern and drives whichever light the board has. */

#include "lugalos_config.h"
#include "kernel/status_led.h"
#include "kernel/identity.h"
#include "kernel/sched.h"
#include "kernel/setup.h"
#include "kernel/time.h"
#include "kernel/printk.h"
#include "net/ip.h"
#include <stddef.h>

/* accepted: the button task is the only writer and the LED task the only reader; each is one word,
 * so a read is never torn and a stale one only delays the light by a step. */
static volatile uint32_t g_held_ms;
static volatile uint32_t g_reset_at_ms;     /* the reset's time plus one; 0 = none */

void status_led_button(uint32_t held_ms) { g_held_ms = held_ms; }
void status_led_reset_done(void) { g_reset_at_ms = (uint32_t)time_get_ms() + 1u; }

#if defined(CONFIG_STATUS_LED_GPIO) || defined(CONFIG_STATUS_LED_CYW43) || defined(CONFIG_STATUS_LED_WS2812)

#if defined(CONFIG_STATUS_LED_GPIO)
#include "drivers/gpio.h"
#ifdef CONFIG_STATUS_LED_ACTIVE_LOW
#define LED_ACTIVE 0
#else
#define LED_ACTIVE 1
#endif
static void led_init(void) {
    (void)gpio_set_mode(CONFIG_STATUS_LED_GPIO, GPIO_MODE_OUT);
    (void)gpio_write_pin(CONFIG_STATUS_LED_GPIO, !LED_ACTIVE);
}
static bool led_put(bool on, status_led_pattern_t p) {
    (void)p;
    return gpio_write_pin(CONFIG_STATUS_LED_GPIO, on ? LED_ACTIVE : !LED_ACTIVE) == 0;
}
#elif defined(CONFIG_STATUS_LED_CYW43)
#include "drivers/cyw43.h"
static void led_init(void) { }
/* The light is on the Wi-Fi chip's own GPIO, so it exists only while the firmware runs. */
static bool led_put(bool on, status_led_pattern_t p) {
    (void)p;
    return cyw43_is_ready() && cyw43_led_set(on);
}
#else
#include "drivers/ws2812.h"
static void led_init(void) { (void)ws2812_init(); }
static bool led_put(bool on, status_led_pattern_t p) {
    uint8_t r = 0, g = 0, b = 0;
    if (on) status_led_rgb(p, &r, &g, &b);
    return ws2812_set(r, g, b);
}
#endif

static void status_led_task(void *arg) {
    (void)arg;
    led_init();

    bool shown = false, shown_valid = false;
    status_led_pattern_t shown_p = STATUS_LED_OFF;
    uint64_t last_up_ms = 0, ssid_at_ms = 0;
    bool wlan_set = false;

    for (;;) {
        task_sleep_ms(STATUS_LED_STEP_MS);
        uint64_t now = time_get_ms();

        status_led_in_t in = { 0 };
        in.button_held_ms = g_held_ms;
        uint32_t reset_at = g_reset_at_ms;
        in.reset_shown = reset_at && (uint32_t)now - (reset_at - 1u) < STATUS_LED_RESET_SHOW_MS;
        in.setup_ap = setup_ap_wanted();
        in.net_up = net_state()->configured;
        if (in.net_up) last_up_ms = now;
        in.net_down_s = (uint32_t)((now - last_up_ms) / 1000u);
        if (now - ssid_at_ms >= 1000u || !ssid_at_ms) {
            char ssid[NODE_WLAN_SSID_MAX + 1];
            wlan_set = node_wlan_ssid(ssid, sizeof ssid);
            ssid_at_ms = now ? now : 1;
        }
        in.wlan_set = wlan_set;

        status_led_pattern_t p = status_led_choose(&in);
        bool on = status_led_level(p, (uint32_t)now);
        if (shown_valid && on == shown && p == shown_p) continue;
        if (led_put(on, p)) { shown = on; shown_p = p; shown_valid = true; }
    }
}

void status_led_start(void) {
    if (task_create_sized("statusled", status_led_task, NULL, 4) < 0) printk("[Led] could not start the status LED task\n");
}

#else
void status_led_start(void) { }
#endif
