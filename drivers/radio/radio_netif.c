/* The radio's side of the Ethernet frame path (45.7): receive callback and transmit thread, in the
 * radio's U-mode domain, connected to the kernel's netif by the two rings in radio_main.h.
 *
 * RX: the blob calls our callback from its own thread with a buffer it owns; the frame is copied
 * into the next free RX slot (dropped if there is none -- a full ring means the stack is behind, and
 * dropping is what a NIC does) and the buffer is handed back. TX: the kernel queues a frame and gives
 * a semaphore; this thread takes it and passes every queued frame to esp_wifi_internal_tx(), which
 * copies it. */

#include "radio_redirect.h"
#include "osi_impl.h"
#include "radio_main.h"

#include "esp_wifi.h"
#include "esp_private/wifi.h"

#define fence() __asm__ volatile("fence rw, rw" ::: "memory")

static radio_ctx_t *g_ctx;

/* IDF's esp_netif_receive path passes (buffer, len, eb); eb is the handle to give back. */
static int rx_cb(void *buffer, uint16_t len, void *eb) {
    rn_rings_t *r = g_ctx->rings;
    uint32_t head = r->rx.head;
    g_ctx->rx_calls++;
    if (len == 0 || len > 1514 || head - r->rx.tail >= RN_RX_SLOTS) g_ctx->rx_dropped++;
    if (len > 0 && len <= 1514 && head - r->rx.tail < RN_RX_SLOTS) {
        rn_slot_t *s = &r->rx_slot[head % RN_RX_SLOTS];
        const uint8_t *src = buffer;
        for (uint32_t i = 0; i < len; i++) s->data[i] = src[i];
        s->len = len;
        fence();
        r->rx.head = head + 1;
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return 0;
}

static void tx_thread(void *unused) {
    (void)unused;
    rn_rings_t *r = g_ctx->rings;
    for (;;) {
        if (!radio_osi_semphr_take((void *)(uintptr_t)g_ctx->tx_sem, 0xffffffffu)) {
            radio_osi_task_delay(10);          /* a refused take must never become a hot loop */
            continue;
        }
        while (r->tx.tail != r->tx.head) {
            rn_slot_t *s = &r->tx_slot[r->tx.tail % RN_TX_SLOTS];
            if (g_ctx->connected) {
                int e = esp_wifi_internal_tx(WIFI_IF_STA, s->data, (uint16_t)s->len);
                if (e == 0) g_ctx->tx_sent++; else g_ctx->tx_errors++;
                if (e == 0x101) radio_osi_task_delay(2);        /* ESP_ERR_NO_MEM: its buffers are busy; one retry */
                if (e == 0x101) esp_wifi_internal_tx(WIFI_IF_STA, s->data, (uint16_t)s->len);
            }
            fence();
            r->tx.tail++;
        }
    }
}

/* IDF's wifi_default.c registers these with the blob when the interface starts (esp_netif's reference
 * counting of a TCP/IP stack buffer that a zero-copy transmit may still hold). Our transmit copies the frame
 * into a ring slot, so there is never a stack buffer to count; registered anyway so that the blob sees the
 * same interface start as under IDF. */
static void netstack_buf_ref(void *b) { (void)b; }
static void netstack_buf_free(void *b) { (void)b; }

bool radio_netif_start(radio_ctx_t *ctx) {
    g_ctx = ctx;
    int nb = esp_wifi_internal_reg_netstack_buf_cb(netstack_buf_ref, netstack_buf_free);
    radio_osi_log_write(3, "radio", "reg_netstack_buf_cb -> 0x%x", nb);
    int rr = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, (wifi_rxcb_t)rx_cb);
    radio_osi_log_write(3, "radio", "reg_rxcb -> 0x%x", rr);
    if (rr != 0) return false;
    return radio_thread_create(tx_thread, 0, 2560, 21) >= 0;
}
