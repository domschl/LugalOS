#ifndef LUGALOS_RADIO_MAIN_H
#define LUGALOS_RADIO_MAIN_H

#include <stdint.h>

/* What the kernel hands the radio's main thread, and what comes back: one
 * page, in the radio's own domain, written by U-mode and read by the kernel.
 * The kernel never follows a pointer in it. */
#define RADIO_STAGE_NONE   0
#define RADIO_STAGE_FAILED 1
#define RADIO_STAGE_OSI    2   /* heap and tables up */
#define RADIO_STAGE_COEX   3
#define RADIO_STAGE_INIT   4   /* esp_wifi_init_internal returned 0 */
#define RADIO_STAGE_STARTED 5  /* esp_wifi_start returned 0 */
#define RADIO_STAGE_JOINED  7  /* associated and the 4-way handshake done */
#define RADIO_STAGE_SCANNED 6  /* a scan found at least one access point */

/* Frames between the kernel's netif and the radio. Two single-producer, single-consumer rings in
 * the first bytes of the radio's arena (so inside its domain, where U-mode writes the RX ring and
 * the kernel writes the TX ring, and each side only ever reads what the other has published):
 * head is advanced by the producer after the slot is complete, tail by the consumer after it has
 * taken the slot. No locks, no pointers followed across the boundary. */
#define RN_FRAME     1536u          /* a frame (<= 1514) rounded to a word multiple */
#define RN_RX_SLOTS  4u
#define RN_TX_SLOTS  3u

typedef struct { uint32_t len; uint8_t data[RN_FRAME]; } rn_slot_t;
typedef struct { volatile uint32_t head, tail; } rn_idx_t;
typedef struct {
    rn_idx_t  rx;                    /* radio -> kernel */
    rn_idx_t  tx;                    /* kernel -> radio */
    rn_slot_t rx_slot[RN_RX_SLOTS];
    rn_slot_t tx_slot[RN_TX_SLOTS];
} rn_rings_t;

typedef struct {
    void    *arena;            /* the radio heap, in the radio's domain */
    uint32_t arena_bytes;
    volatile uint32_t stage;
    volatile int32_t  rc_init;
    volatile int32_t  rc_start;
    volatile uint32_t ap_count;
    uint32_t join;                      /* kernel -> radio: 1 = associate after the scan */
    char     ssid[33];
    char     pass[65];                  /* a 64-hex derived PSK, never a passphrase */
    volatile uint32_t connected;        /* link state: set on STA_CONNECTED, cleared on STA_DISCONNECTED */
    volatile uint32_t done;             /* the initial phase (scan/join) is over; radio_main keeps running */
    uint32_t tx_sem;                    /* a kernel semaphore (the kernel gives it per queued TX frame) */
    rn_rings_t *rings;                  /* in the arena */
    uint8_t  mac[6];                    /* the station's MAC, set once the radio started */
    volatile uint32_t rx_calls, rx_dropped, tx_sent, tx_errors;   /* frame-path counters, for `radio stats` */
    volatile uint32_t disc_reason;
} radio_ctx_t;

void radio_main(uintptr_t arg);
/* Registers the receive callback and starts the transmit thread (radio_netif.c). */
#include <stdbool.h>
bool radio_netif_start(radio_ctx_t *ctx);

#endif
