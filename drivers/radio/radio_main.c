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

/* The chip ROM's own debug prints (its PHY and clock code call ets_printf) go to its console -- here the
 * USB-Serial/JTAG peripheral, which the radio's domain has no business touching (a load fault at
 * 0x6000f004 on the first cold run). The ROM's character sink is replaceable; this one collects a line
 * and hands it to the radio's log. */
extern void rom_install_putc1(void (*fn)(char)) __asm__("radio_ets_install_putc1");
extern void rom_install_putc2(void (*fn)(char)) __asm__("radio_ets_install_putc2");
static void rom_putc(char c) {
    static char line[96];
    static unsigned n;
    if (c == '\r') return;
    if (c == '\n' || n == sizeof line - 1) {
        line[n] = 0;
        if (n) radio_osi_log_write(3, "rom", "%s", line);
        n = 0;
        if (c == '\n') return;
    }
    line[n++] = c;
}

/* A ROM function, under the radio_ name tools/c6_radio_libs.py gives it an address for. */
extern void rom_update_cpu_frequency(uint32_t ticks_per_us) __asm__("radio_ets_update_cpu_frequency");

#define RLOG(...) radio_osi_log_write(3, "radio", __VA_ARGS__)

static void radio_exit(void) {
    register long a0 __asm__("a0") = SYS_UEXIT;        /* returning from the entry would jump to 0 */
    __asm__ volatile("ecall" : "+r"(a0) :: "memory");
    for (;;) { }
}

static void radio_fail(radio_ctx_t *ctx) { ctx->stage = RADIO_STAGE_FAILED; ctx->done = 1; radio_exit(); }

void radio_main(uintptr_t arg) {
    radio_ctx_t *ctx = (radio_ctx_t *)arg;

    /* The frame rings occupy the front of the arena; the heap is the rest. */
    ctx->rings = (rn_rings_t *)ctx->arena;
    const uint32_t ring_bytes = (sizeof(rn_rings_t) + 15u) & ~15u;       /* the heap wants 8-byte alignment */
    if (!radio_osi_init((uint8_t *)ctx->arena + ring_bytes, ctx->arena_bytes - ring_bytes)) radio_fail(ctx);
    ctx->stage = RADIO_STAGE_OSI;

    /* The ROM's own idea of the CPU frequency: 1000 us of ets_delay_us measured as
     * 330 us until told (45.3a). The PHY uses ets_delay_us. */
    rom_update_cpu_frequency(160);
    rom_install_putc1(rom_putc);
    rom_install_putc2(0);

    /* The thread that runs the blob's timer callbacks (esp_timer in IDF), and the interrupt thread. */
    if (radio_thread_create(radio_timer_thread, 0, 3072, 22) < 0) { RLOG("no timer thread"); radio_fail(ctx); }
    if (!radio_osi_start_isr_thread()) { RLOG("no interrupt thread"); radio_fail(ctx); }

    int r = coex_init();
    RLOG("coex_init -> %d", r);
    ctx->stage = RADIO_STAGE_COEX;

    esp_wifi_internal_set_log_level(WIFI_LOG_INFO);   /* esp_wifi_set_log_level() in IDF, at CONFIG_LOG_DEFAULT_LEVEL */
    esp_wifi_internal_set_log_mod(WIFI_LOG_MODULE_ALL, WIFI_LOG_SUBMODULE_ALL, true);
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
        esp_wifi_get_mac(WIFI_IF_STA, ctx->mac);
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
        ctx->best_rssi = -127;
        if (esp_wifi_scan_get_ap_records(&got, recs) == 0) {
            for (uint16_t i = 0; i < got; i++)
                RLOG("  %-32s ch%2u %4d dBm  auth %u  %02x:%02x:%02x:%02x:%02x:%02x", (char *)recs[i].ssid,
                     (unsigned)recs[i].primary, (int)recs[i].rssi, (unsigned)recs[i].authmode,
                     recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2], recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5]);
            /* The strongest access point of the wanted SSID: the blob's default connects to the *first* match
             * of its channel sweep (fast scan), which on a mesh is as often a weak repeater as the near one. */
            for (uint16_t i = 0; i < got; i++) {
                bool same = ctx->ssid[0] != 0;
                for (int k = 0; same && k < 32; k++) { if ((char)recs[i].ssid[k] != ctx->ssid[k]) same = false; if (!ctx->ssid[k]) break; }
                if (same && recs[i].rssi > ctx->best_rssi) {
                    ctx->best_rssi = recs[i].rssi;
                    ctx->best_chan = recs[i].primary;
                    for (int k = 0; k < 6; k++) ctx->best_bssid[k] = recs[i].bssid[k];
                }
            }
        }
        if (n) ctx->stage = RADIO_STAGE_SCANNED;
    }

    int retry_in = 0;
    if (r == 0 && ctx->join) {
        /* As esp_netif does when it attaches to the station: the receive callback is in place before the
         * connection, so the first frame after the handshake has somewhere to go. */
        if (!radio_netif_start(ctx)) RLOG("netif start failed");
        wifi_config_t wc = { 0 };
        for (int i = 0; i < 32 && ctx->ssid[i]; i++) wc.sta.ssid[i] = (uint8_t)ctx->ssid[i];
        for (int i = 0; i < 64 && ctx->pass[i]; i++) wc.sta.password[i] = (uint8_t)ctx->pass[i];   /* 64 hex = a raw PSK */
        wc.sta.threshold.authmode = ctx->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;   /* no key: an open network */
        if (ctx->best_rssi > -127) {
            wc.sta.bssid_set = true;
            for (int k = 0; k < 6; k++) wc.sta.bssid[k] = ctx->best_bssid[k];
            RLOG("joining the strongest %s: %d dBm", ctx->ssid, (int)ctx->best_rssi);
        }
        wc.sta.pmf_cfg.capable = true;
        r = esp_wifi_set_config(WIFI_IF_STA, &wc);
        RLOG("esp_wifi_set_config -> 0x%x", r);
        /* No modem sleep: the blob's power save lets the RF and the MAC doze between beacons through the
         * PMU's modem state (sleep retention, REGDMA), none of which this radio has. IDF's default is
         * WIFI_PS_MIN_MODEM; this is NONE until power saving is ported deliberately. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        r = esp_wifi_connect();
        RLOG("esp_wifi_connect -> 0x%x", r);
        /* Like IDF's default event handler: a disconnect while we mean to be connected is answered with
         * esp_wifi_connect() again, after a short pause (a stale session at the AP, say, which kicks a
         * station that rebooted without deauthenticating, is gone a moment later). */
        for (int t = 0; t < 300 && !ctx->connected; t++) {           /* 30 s */
            int32_t id; uint8_t d[48]; uint32_t sz;
            while (radio_osi_event_pop(&id, d, &sz)) {
                RLOG("event %d (%u bytes)", (int)id, (unsigned)sz);
                if (id == WIFI_EVENT_STA_CONNECTED) ctx->connected = 1;
                if (id == WIFI_EVENT_STA_DISCONNECTED) { ctx->disc_reason = sz > 39 ? d[39] : 255; retry_in = 10; }
            }
            if (retry_in && --retry_in == 0) { r = esp_wifi_connect(); RLOG("reconnect -> 0x%x", r); r = 0; }
            radio_osi_task_delay(100);
        }
        RLOG("join %s; radio heap free %u of %u", ctx->connected ? "succeeded" : "FAILED",
             (unsigned)radio_osi_get_free_heap_size(), (unsigned)ctx->arena_bytes);
        if (ctx->connected) ctx->stage = RADIO_STAGE_JOINED;
    }
    ctx->done = 1;

    /* From here the main thread is the event pump: link state and rejoin. Without a join there is nothing
     * more to do and it ends. */
    if (ctx->join && r == 0) {
        uint32_t tick = 0;
        for (;;) {
            int32_t id; uint8_t d[48]; uint32_t sz;
            if ((++tick % 5) == 0) radio_plat_phy_track();      /* IDF's 1 s PLL-tracking timer */
            while (radio_osi_event_pop(&id, d, &sz)) {
                if (id == WIFI_EVENT_STA_CONNECTED) ctx->connected = 1;
                if (id == WIFI_EVENT_STA_DISCONNECTED) { ctx->connected = 0; ctx->disc_reason = sz > 39 ? d[39] : 255; retry_in = 10; }
                RLOG("event %d", (int)id);
            }
            if (retry_in && --retry_in == 0) esp_wifi_connect();
            radio_osi_task_delay(200);
        }
    }
    radio_exit();
}
