#ifndef KERNEL_LLP_H
#define KERNEL_LLP_H

#include <stdint.h>
#include <stdbool.h>

/* LLP -- the LugalOS link protocol (49.1, plan/phase49_c6_network_nodes.md;
 * the specification is doc/llp.md, and that document wins where they differ).
 *
 * The link between two LugalOS nodes on one board -- an ESP32-P4 and its
 * ESP32-C6 over SDIO, or an RP2350 and a C6-Zero over a UART. It carries two
 * payloads, Ethernet frames (the P4's IP stack, through the C6's radio) and
 * 9P messages (the two nodes' namespaces), plus the few things neither can do
 * for itself: version check, session start, flow control, the radio's link
 * state.
 *
 * This file is the whole protocol and nothing else: no allocation, no clock,
 * no locks, no I/O. A binding (a UART driver, the SDIO pair) owns the wire
 * and the buffers, passes time in, and gets frames out through a callback.
 * That is what lets tests/host/llp_host.c and the Python reference
 * (host/llp/) run the same state machine against the same vectors. */

#define LLP_VERSION_MAJOR 1
#define LLP_VERSION_MINOR 0

#define LLP_HDR_LEN      4u      /* type, seq, len (u16 LE) */
#define LLP_CRC_LEN      4u      /* serial binding only */
#define LLP_MAX_PAYLOAD  4096u   /* the protocol's ceiling; HELLO says each side's own */
#define LLP_MIN_PAYLOAD  1514u   /* every node takes one whole Ethernet frame */
#define LLP_MAX_WINDOW   64u     /* credits; < 128 keeps 8-bit sequence arithmetic exact */

/* Frame types. */
#define LLP_T_LINK  0x01   /* link management, never flow-controlled, seq 0 */
#define LLP_T_ETH   0x02   /* one Ethernet frame, 14..1514 bytes, no FCS */
#define LLP_T_P9    0x03   /* one 9P message, size[4] == len */

/* LINK operations: the payload's first byte. */
#define LLP_OP_HELLO   0x01
#define LLP_OP_CREDIT  0x02
#define LLP_OP_PROBE   0x03
#define LLP_OP_STATE   0x04

#define LLP_HELLO_LEN  22u
#define LLP_CREDIT_LEN 3u
#define LLP_PROBE_LEN  2u
#define LLP_STATE_LEN  5u

#define LLP_HELLO_F_UP    0x01   /* the sender considers the link up */
#define LLP_HELLO_F_FLOW  0x02   /* the sender's receive side uses LLP credits */

#define LLP_ROLE_HOST   1   /* the node that owns the IP address (P4, RP2350) */
#define LLP_ROLE_RADIO  2   /* the node with the radio (C6) */

/* Timers, in milliseconds (doc/llp.md §6). */
#define LLP_HELLO_RETRY_MS  500u
#define LLP_DEAD_MS         3000u
#define LLP_KEEPALIVE_MS    1000u
#define LLP_PROBE_MS        100u

typedef enum { LLP_DOWN = 0, LLP_UP = 1, LLP_MISMATCH = 2 } llp_state_t;

typedef enum {
    LLP_EV_UP = 1,          /* handshake complete; peer_* are valid */
    LLP_EV_DOWN = 2,        /* the peer restarted, or went silent for LLP_DEAD_MS */
    LLP_EV_MISMATCH = 3,    /* the peer speaks another major version, or is too small */
    LLP_EV_PEER_STATE = 4,  /* a STATE arrived: peer_link / peer_rssi changed */
} llp_event_t;

/* A parsed frame. `payload` points into the buffer it was parsed from. */
typedef struct {
    uint8_t        type;
    uint8_t        seq;
    uint16_t       len;
    const uint8_t *payload;
} llp_frame_t;

/* --- Framing (every binding) --------------------------------------------- */

void llp_hdr_put(uint8_t hdr[LLP_HDR_LEN], uint8_t type, uint8_t seq, uint16_t len);

/* Header + payload in `buf` (exactly `n` bytes, no CRC). Returns 0, or -1 if
 * shorter than a header, -2 if the header's length disagrees with `n`. */
int llp_frame_parse(const uint8_t *buf, uint32_t n, llp_frame_t *out);

/* --- Serial binding: SLIP(header | payload | CRC-32 LE) ------------------- */

/* Worst case on the wire for a payload of `n`: every byte escaped, two ENDs. */
#define LLP_SERIAL_WIRE_MAX(n) (2u + 2u * (LLP_HDR_LEN + (n) + LLP_CRC_LEN))

/* Encodes one frame for a byte stream. Returns its wire length, or -1 if it
 * does not fit in `cap`. */
int llp_serial_encode(const uint8_t hdr[LLP_HDR_LEN], const uint8_t *payload, uint16_t len,
                      uint8_t *dst, uint32_t cap);

/* The same bytes as llp_serial_encode(), handed to `put` one at a time
 * instead of written to a buffer: for a channel that takes bytes, on a board
 * that cannot spare a frame-sized buffer twice over (the C6, 49.3). */
typedef void (*llp_put_fn)(void *ctx, uint8_t byte);
void llp_serial_emit(const uint8_t hdr[LLP_HDR_LEN], const uint8_t *payload, uint16_t len,
                     llp_put_fn put, void *ctx);

typedef struct {
    uint8_t *buf;       /* the binding's: LLP_HDR_LEN + max_payload + LLP_CRC_LEN */
    uint32_t cap;
    uint32_t len;
    bool     esc;
    uint32_t frames;    /* good frames out */
    uint32_t crc_err;   /* CRC mismatch */
    uint32_t runt;      /* shorter than header + CRC */
    uint32_t bad_len;   /* header length disagrees with the frame */
    uint32_t overflow;  /* longer than the buffer: dropped whole by SLIP */
} llp_serial_rx_t;

void llp_serial_rx_init(llp_serial_rx_t *rx, uint8_t *buf, uint32_t cap);

/* One received byte. Returns 1 with `out` filled (pointing into rx->buf, valid
 * until the next call) when a frame with a good CRC is complete, else 0.
 * Oversized frames are dropped by the SLIP layer; damaged ones are counted. */
int llp_serial_feed(llp_serial_rx_t *rx, uint8_t c, llp_frame_t *out);

/* --- The link: handshake, credits, liveness ------------------------------- */

typedef struct llp_link llp_link_t;

/* Transmit one frame (header, then `len` payload bytes). 0, or -1 if the
 * channel refused; a refused LINK frame is retried by the timers, a refused
 * data frame is the caller's to retry. */
typedef int (*llp_send_fn)(void *ctx, const uint8_t hdr[LLP_HDR_LEN],
                           const uint8_t *payload, uint16_t len);
/* How many more data frames the receiving side can hold right now. Only
 * called when cfg.rx_window > 0. */
typedef uint32_t (*llp_rx_free_fn)(void *ctx);
typedef void (*llp_event_fn)(void *ctx, llp_event_t ev, const llp_link_t *link);

typedef struct {
    uint8_t  role;          /* LLP_ROLE_* */
    uint8_t  major, minor;  /* 0, 0 = this build's LLP_VERSION_*; tests set others */
    uint16_t max_payload;   /* largest payload this side accepts, LLP_MIN..LLP_MAX */
    uint8_t  rx_window;     /* 0 = the channel does flow control; else <= LLP_MAX_WINDOW */
    uint8_t  mac[6];        /* the radio's station MAC; zeros on a host */
    llp_send_fn    send;
    llp_rx_free_fn rx_free;
    llp_event_fn   event;   /* may be NULL */
    void          *ctx;
} llp_config_t;

typedef struct {
    uint32_t tx_frames, rx_frames;      /* data frames */
    uint32_t tx_link, rx_link;          /* LINK frames */
    uint32_t tx_refused;                /* send callback said no */
    uint32_t tx_no_credit;              /* times sending was held back for credit (episodes) */
    uint32_t rx_lost;                   /* data frames missing from the sequence */
    uint32_t rx_stale;                  /* data frames behind the sequence (dropped) */
    uint32_t rx_not_up;                 /* data while not up (dropped) */
    uint32_t rx_bad;                    /* unknown type/op, short LINK body, bad ETH/P9 shape */
    uint32_t rx_too_big;                /* longer than our max_payload */
    uint32_t hellos, probes, credits;   /* LINK frames sent, by kind */
    uint32_t ups, downs;
} llp_stats_t;

struct llp_link {
    llp_config_t cfg;
    llp_state_t  state;
    uint32_t     session;           /* ours, nonzero, new every start */
    uint32_t     peer_session;      /* 0 until a HELLO arrives */
    /* What the peer's last HELLO said. */
    uint8_t      peer_major, peer_minor, peer_role, peer_flags;
    uint16_t     peer_max_payload;
    uint8_t      peer_mac[6];
    /* What the peer's last STATE said (radio side: Wi-Fi link, RSSI dBm). */
    uint8_t      peer_link;
    int8_t       peer_rssi;
    /* Sending. */
    uint8_t      tx_seq;            /* next data frame's sequence number */
    uint8_t      tx_ack;            /* peer's last CREDIT: frames before this are done */
    uint8_t      tx_window;         /* ... and how many beyond it it can take */
    bool         blocked;           /* a send was held back for credit */
    uint32_t     last_probe;
    /* Receiving. */
    uint8_t      rx_next;           /* next sequence number expected */
    uint8_t      adv_limit;         /* rx_next + window as last advertised */
    /* Timers. */
    uint32_t     last_rx, last_tx, last_hello;
    llp_stats_t  st;
};

/* Starts a link: state DOWN, a HELLO sent at once. `session` must be nonzero
 * and new for every start (a random number); a peer that sees it change
 * treats everything before as gone. */
void llp_link_init(llp_link_t *l, const llp_config_t *cfg, uint32_t session, uint32_t now);

/* A frame from the channel. Returns 1 when it is data for the caller (ETH or
 * P9, in `out`, to be stored or dropped now -- `out` points into the frame the
 * caller passed in), 0 when the link consumed or dropped it. */
int llp_link_input(llp_link_t *l, const llp_frame_t *f, uint32_t now, llp_frame_t *out);

/* Sends one data frame. 0 on success; -1 not up; -2 no credit (the link
 * starts probing; try again after a tick); -3 too large for the peer or a bad
 * type; -4 the channel refused. */
int llp_link_send(llp_link_t *l, uint8_t type, const uint8_t *payload, uint16_t len, uint32_t now);

/* True when llp_link_send() would not be held back for credit. */
bool llp_link_can_send(const llp_link_t *l);

/* The radio side reports its Wi-Fi link (1 up, 0 down) and RSSI to the peer. */
void llp_link_send_state(llp_link_t *l, uint8_t link_up, int8_t rssi, uint32_t now);

/* Call regularly (every few ms to ~50 ms) and after consuming received data:
 * HELLO retries, credits, probes, keepalive, the dead-peer timeout. */
void llp_link_tick(llp_link_t *l, uint32_t now);

#endif
