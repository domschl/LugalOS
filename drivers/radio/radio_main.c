/* The radio's main thread (45.6, plan/phase45_esp32c6.md): the U-mode code that
 * calls into Espressif's blob. It does what IDF's esp_wifi_init() does, minus
 * everything that is IDF's OS (the event loop, esp_netif, the power-management
 * locks, sleep retention), in the same order.
 *
 * It is the second file of the radio that includes IDF headers (the first is
 * the OS table): the init config's layout is checked by the blob against a
 * magic number and a size at run time, so it has to come from the header the
 * blob was built with, not a transcription. */

#include "radio_redirect.h"
#include "osi_impl.h"
#include "plat.h"
#include "radio_main.h"
#include "kernel/ipc.h"

#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "private/esp_coexist_internal.h"

/* The event base the blob posts under (IDF defines it with ESP_EVENT_DEFINE_BASE). */
esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";

/* The crypto table the blob's WPA hooks call through. The blob refuses to initialise
 * unless it has the right *size and version* (it said so: "expected size=52
 * version=1, actual size=0"); the functions are only reached when a station
 * authenticates, which is the supplicant's work (45.7, where this becomes IDF's
 * crypto_ops.c table over the supplicant's crypto). A scan never calls them. */
const wpa_crypto_funcs_t g_wifi_default_wpa_crypto_funcs = {
    .size = sizeof(wpa_crypto_funcs_t),
    .version = ESP_WIFI_CRYPTO_VERSION,
};

/* A ROM function, under the radio_ name tools/c6_radio_libs.py gives it an address for. */
extern void rom_update_cpu_frequency(uint32_t ticks_per_us) __asm__("radio_ets_update_cpu_frequency");

#define RLOG(...) radio_osi_log_write(3, "radio", __VA_ARGS__)

void radio_main(uintptr_t arg) {
    radio_ctx_t *ctx = (radio_ctx_t *)arg;

    if (!radio_osi_init(ctx->arena, ctx->arena_bytes)) { ctx->stage = RADIO_STAGE_FAILED; return; }
    ctx->stage = RADIO_STAGE_OSI;

    /* The ROM's own idea of the CPU frequency: 1000 us of ets_delay_us measured as
     * 330 us until told (45.3a). The PHY uses ets_delay_us. */
    rom_update_cpu_frequency(160);

    int r = coex_init();
    RLOG("coex_init -> %d", r);
    ctx->stage = RADIO_STAGE_COEX;

    esp_wifi_internal_set_log_level(WIFI_LOG_DEBUG);   /* the blob says why it refuses (esp_wifi_set_log_level() in IDF) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    r = esp_wifi_init_internal(&cfg);
    RLOG("esp_wifi_init_internal -> 0x%x", r);
    ctx->rc_init = r;
    ctx->stage = r == 0 ? RADIO_STAGE_INIT : RADIO_STAGE_FAILED;

    if (r == 0) {
        r = esp_wifi_set_mode(WIFI_MODE_STA);
        RLOG("esp_wifi_set_mode(STA) -> 0x%x", r);
        r = esp_wifi_start();
        RLOG("esp_wifi_start -> 0x%x", r);
        ctx->rc_start = r;
        if (r == 0) ctx->stage = RADIO_STAGE_STARTED;
    }

    radio_osi_task_delay(2000);                        /* let the blob's own task run what start queued */

    register long a0 __asm__("a0") = SYS_UEXIT;        /* returning would jump to 0 */
    __asm__ volatile("ecall" : "+r"(a0) :: "memory");
    for (;;) { }
}
