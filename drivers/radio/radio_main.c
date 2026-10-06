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

/* IDF's wpa_supplicant (compiled into this domain, cmake/radio_esp32c6.cmake): registers its
 * callbacks with the blob and starts its timer loop. */
extern int esp_supplicant_init(void);

/* IDF's esp_wifi_connect() is a thin wrapper over the blob's internal entry (esp_wifi/src). */
extern int esp_wifi_connect_internal(void);
int esp_wifi_connect(void) { return esp_wifi_connect_internal(); }

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
        r = esp_supplicant_init();
        RLOG("esp_supplicant_init -> 0x%x", r);
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

    if (r == 0 && ctx->join) {
        wifi_config_t wc = { 0 };
        for (int i = 0; i < 32 && ctx->ssid[i]; i++) wc.sta.ssid[i] = (uint8_t)ctx->ssid[i];
        for (int i = 0; i < 64 && ctx->pass[i]; i++) wc.sta.password[i] = (uint8_t)ctx->pass[i];   /* 64 hex = a raw PSK */
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wc.sta.pmf_cfg.capable = true;
        r = esp_wifi_set_config(WIFI_IF_STA, &wc);
        RLOG("esp_wifi_set_config -> 0x%x", r);
        r = esp_wifi_connect();
        RLOG("esp_wifi_connect -> 0x%x", r);
        for (int t = 0; t < 200 && !ctx->connected; t++) {           /* 20 s */
            int32_t id; uint8_t d[48]; uint32_t sz;
            while (radio_osi_event_pop(&id, d, &sz)) {
                RLOG("event %d (%u bytes)", (int)id, (unsigned)sz);
                if (id == WIFI_EVENT_STA_CONNECTED) ctx->connected = 1;
                if (id == WIFI_EVENT_STA_DISCONNECTED) ctx->disc_reason = sz > 39 ? d[39] : 255;
            }
            radio_osi_task_delay(100);
        }
        RLOG("join %s", ctx->connected ? "succeeded" : "FAILED");
        if (ctx->connected) ctx->stage = RADIO_STAGE_JOINED;
    }

    register long a0 __asm__("a0") = SYS_UEXIT;        /* returning would jump to 0 */
    __asm__ volatile("ecall" : "+r"(a0) :: "memory");
    for (;;) { }
}
