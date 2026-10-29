#ifndef NET_WEBSOCKET_H
#define NET_WEBSOCKET_H

/* WebSocket (RFC 6455) framing and the opening handshake's key. Bytes in, bytes out: no
 * connection, no allocation, so a server (net/httpd.c) and a client (phase 62.12) share it.
 *
 * A frame is parsed in two steps, because a TCP stream delivers it in pieces: ws_parse_header()
 * says how long the header is and what follows, and the caller waits for that many payload
 * bytes before unmasking them in place. Fragmented messages are the caller's to join (the
 * `fin` bit and the continuation opcode say where they end); control frames (close, ping,
 * pong) are never fragmented and carry at most 125 bytes, which the parser enforces. */

#include <stdint.h>
#include <stdbool.h>

#define WS_OP_CONT    0x0
#define WS_OP_TEXT    0x1
#define WS_OP_BINARY  0x2
#define WS_OP_CLOSE   0x8
#define WS_OP_PING    0x9
#define WS_OP_PONG    0xA

#define WS_HDR_MAX    14u       /* 2 + 8 (64-bit length) + 4 (mask) */
#define WS_ACCEPT_LEN 28u       /* base64 of a SHA-1 digest, without the NUL */

typedef struct {
    uint8_t  opcode;
    bool     fin;
    bool     masked;
    uint8_t  mask[4];
    uint32_t payload_len;
} ws_hdr_t;

/* The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key (a NUL-terminated string as
 * it appears in the request): base64(SHA-1(key + the protocol's GUID)). Writes WS_ACCEPT_LEN
 * characters and a NUL into `out`. False if the key is empty or longer than 64 characters. */
bool ws_accept_key(const char *client_key, char out[WS_ACCEPT_LEN + 1]);

/* Parses the header at the start of `buf` (`n` bytes available).
 *   > 0  the header's length; `*h` is filled in. The payload follows.
 *   0    more bytes are needed.
 *   < 0  a protocol error: a reserved bit or an unknown opcode, a fragmented or oversized control
 *        frame, or a payload longer than `max_payload` (the caller's buffer, and the limit that
 *        keeps a 64-bit length from meaning anything). Close the connection. */
int ws_parse_header(const uint8_t *buf, uint32_t n, uint32_t max_payload, ws_hdr_t *h);

/* XORs `len` payload bytes with the mask. `offset` is the payload offset of buf[0], for a payload
 * that is unmasked as it arrives in pieces. */
void ws_unmask(uint8_t *buf, uint32_t len, const uint8_t mask[4], uint32_t offset);

/* Writes a frame header for a payload of `len` bytes into `out` (WS_HDR_MAX bytes) and returns its
 * length. `mask` NULL: an unmasked frame, which is what a server sends; a client passes its
 * mask and masks the payload itself with ws_unmask(). Always FIN. */
uint32_t ws_build_header(uint8_t *out, uint8_t opcode, uint32_t len, const uint8_t mask[4]);

#endif
