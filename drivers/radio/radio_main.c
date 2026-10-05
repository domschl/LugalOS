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

/* The supplicant's callback table (struct wpa_funcs: 29 slots). Scan results are handed
 * to the supplicant for IE parsing through it, and the blob dereferences the table
 * pointer itself -- unregistered, the first access point found was a null load in
 * scan_add_ssid_do (45.6). Every slot is a stub that
 * says "nothing" (the two init/deinit slots say "fine"); the real table is 45.7's. */
extern int esp_wifi_register_wpa_cb_internal(void *cb);
static int wpa_stub_true(void) { return 1; }
static int wpa_stub_zero(void) { return 0; }
static void *g_wpa_funcs_none[32];
static void wpa_stubs_install(void) {
    for (int i = 0; i < 29; i++) g_wpa_funcs_none[i] = (void *)(uintptr_t)wpa_stub_zero;
    g_wpa_funcs_none[0] = (void *)(uintptr_t)wpa_stub_true;      /* wpa_sta_init */
    g_wpa_funcs_none[1] = (void *)(uintptr_t)wpa_stub_true;      /* wpa_sta_deinit */
    esp_wifi_register_wpa_cb_internal(g_wpa_funcs_none);
}

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

    /* The thread that runs the blob's timer callbacks (esp_timer in IDF). */
    if (radio_thread_create(radio_timer_thread, 0, 3072, 22) < 0) { RLOG("no timer thread"); ctx->stage = RADIO_STAGE_FAILED; return; }
    if (!radio_osi_start_isr_thread()) { RLOG("no interrupt thread"); ctx->stage = RADIO_STAGE_FAILED; return; }

    int r = coex_init();
    RLOG("coex_init -> %d", r);
    ctx->stage = RADIO_STAGE_COEX;

    esp_wifi_internal_set_log_level(WIFI_LOG_VERBOSE);   /* the blob says why it refuses (esp_wifi_set_log_level() in IDF) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    r = esp_wifi_init_internal(&cfg);
    RLOG("esp_wifi_init_internal -> 0x%x", r);
    ctx->rc_init = r;
    ctx->stage = r == 0 ? RADIO_STAGE_INIT : RADIO_STAGE_FAILED;

    if (r == 0) {
        wpa_stubs_install();
        r = esp_wifi_set_mode(WIFI_MODE_STA);
        RLOG("esp_wifi_set_mode(STA) -> 0x%x", r);
        r = esp_wifi_start();
        RLOG("esp_wifi_start -> 0x%x", r);
        ctx->rc_start = r;
        if (r == 0) ctx->stage = RADIO_STAGE_STARTED;
    }

    if (r == 0) {
        radio_osi_task_delay(300);                     /* let the blob's own task run what start queued */
        /* A blocking scan of every channel, then the list. */
        wifi_scan_config_t sc = { 0 };
        r = esp_wifi_scan_start(&sc, true);
        RLOG("esp_wifi_scan_start -> 0x%x", r);
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        ctx->ap_count = n;
        RLOG("scan found %u access points", (unsigned)n);
        static wifi_ap_record_t recs[16];
        uint16_t got = n < 16 ? n : 16;
        if (esp_wifi_scan_get_ap_records(&got, recs) == 0) {
            for (uint16_t i = 0; i < got; i++)
                RLOG("  %-32s ch%2u %4d dBm  auth %u  %02x:%02x:%02x:%02x:%02x:%02x", (char *)recs[i].ssid,
                     (unsigned)recs[i].primary, (int)recs[i].rssi, (unsigned)recs[i].authmode,
                     recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2], recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5]);
        }
        if (n) ctx->stage = RADIO_STAGE_SCANNED;
    }

    register long a0 __asm__("a0") = SYS_UEXIT;        /* returning would jump to 0 */
    __asm__ volatile("ecall" : "+r"(a0) :: "memory");
    for (;;) { }
}
