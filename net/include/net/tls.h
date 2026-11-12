#ifndef NET_TLS_H
#define NET_TLS_H

#include <stdbool.h>
#include <stdint.h>
#include "kernel/sha256.h"

/* A TLS 1.3 server (RFC 8446), the part a node's HTTPS needs and no more, and a client for the node's
 * own connections out (MQTT to a broker, 66.3b):
 *
 *   cipher suite  TLS_CHACHA20_POLY1305_SHA256 (every TLS 1.3 client offers it)
 *   key exchange  X25519; a HelloRetryRequest when the client offered it but sent another share
 *   signature     ecdsa_secp256r1_sha256, with the node's own key and the certificate its owner's CA
 *                 issued (kernel/tls_cert.h)
 *
 * The client offers the same suite, group and signature, checks the server's certificate against one CA
 * public key and the name it dialled (kernel/tls_cert.h tls_cert_check()), asks for 4 KB records
 * (max_fragment_length, RFC 6066) so its receive buffer can be TLS_CLIENT_RX_MAX, and ignores session
 * tickets.
 *
 * No PSK, resumption, 0-RTT, client certificates, ALPN or TLS 1.2. A handshake message must arrive
 * within one record (every client's ClientHello does; one larger than 16 KB would be refused).
 * Records: up to 16 KB in (what a client may send), at most TLS_SEND_MAX out.
 *
 * Transport-independent: the caller supplies non-blocking recv/send functions (bytes moved, 0 when
 * nothing can move now, negative when the connection is gone) and the two buffers. net/httpd.c runs
 * it over a TCP stream, tests/host/tls_host.c over a socket against OpenSSL. Not thread-safe: one task
 * drives a session. */

#define TLS_RECORD_MAX   (5u + 16384u + 256u)   /* the largest record a client may send */
#define TLS_SEND_MAX     4096u                   /* plaintext per record we send */
#define TLS_TX_CAP       (5u + TLS_SEND_MAX + 1u + 16u)
#define TLS_CLIENT_RX_MAX (5u + 4096u + 256u)   /* a record from a server that took max_fragment_length 2^12 */

#define TLS_EOF   -1      /* the peer closed (close_notify, or the connection ended) */
#define TLS_FAIL  -2      /* a fatal error: an alert was sent (or received); the session is over */

typedef int (*tls_recv_fn)(void *ctx, uint8_t *buf, uint32_t len);
typedef int (*tls_send_fn)(void *ctx, const uint8_t *buf, uint32_t len);

typedef struct {
    /* Set by tls_server_init(). The certificate and key must outlive the session. */
    const uint8_t *cert;  uint32_t cert_len;
    const uint8_t *priv;                       /* 32 bytes, P-256 */
    tls_recv_fn recv; tls_send_fn send; void *io;
    uint8_t *rx; uint32_t rx_len, rx_cap;      /* a record being read: TLS_RECORD_MAX bytes (a client's may be less) */
    uint32_t app_off, app_end;                 /* decrypted application data still in rx */
    uint8_t *tx; uint32_t tx_len, tx_sent;     /* records owed to the peer: TLS_TX_CAP bytes */

    uint8_t  st;
    uint8_t  alert;                            /* the alert that ended the session, if any */
    bool     hrr_sent;
    bool     ccs_sent;
    uint8_t  sid[32]; uint8_t sid_len;         /* the client's legacy_session_id, echoed */
    sha256_ctx_t transcript;
    uint8_t  c_secret[32], s_secret[32];       /* current traffic secrets */
    uint8_t  c_key[32], c_iv[12], s_key[32], s_iv[12];
    uint64_t c_seq, s_seq;
    bool     c_enc, s_enc;
    uint8_t  c_ap[32];                         /* the client's application secret, after its Finished */
    uint8_t  expect_fin[32];                   /* the client Finished we expect */

    /* A client's (tls_client_init()). */
    bool     client;
    bool     mfl_ok;                           /* the server took max_fragment_length */
    bool     allow_unverified;                 /* explicit opt-in when ca_pub is NULL */
    const uint8_t *ca_pub;                     /* 64 bytes: the CA the server's certificate must come from */
    const char *host;                          /* the name or dotted address the certificate must carry */
    uint8_t  eph[32];                          /* our X25519 secret, until the ServerHello */
    uint8_t  peer_pub[64];                     /* the server's key, from its certificate */
    uint8_t  master[32];                       /* until the server's Finished */
} tls_t;

/* `rx` holds TLS_RECORD_MAX bytes, `tx` TLS_TX_CAP. */
void tls_server_init(tls_t *t, const uint8_t *cert, uint32_t cert_len, const uint8_t priv[32],
                     tls_recv_fn recv, tls_send_fn send, void *io, uint8_t *rx, uint8_t *tx);

/* A client session to `host` (kept by pointer, as `ca_pub` is). Queues the ClientHello; tls_read() and
 * tls_flush() send it and run the handshake. `rx_cap` is at least TLS_CLIENT_RX_MAX: a server that does
 * not take max_fragment_length is refused unless it is TLS_RECORD_MAX. False if entropy is missing. */
bool tls_client_init(tls_t *t, const char *host, const uint8_t ca_pub[64],
                     tls_recv_fn recv, tls_send_fn send, void *io, uint8_t *rx, uint32_t rx_cap, uint8_t *tx);
void tls_client_set_insecure(tls_t *t, bool allow);

/* Runs the handshake as bytes arrive, then returns application data: bytes copied into `buf`, 0 when
 * none is ready yet, TLS_EOF or TLS_FAIL. */
int tls_read(tls_t *t, uint8_t *buf, uint32_t cap);
/* Encrypts up to TLS_SEND_MAX bytes of `buf` into one record and starts sending it. Returns the bytes
 * taken, 0 while an earlier record is still going out (or the handshake is not done), or TLS_FAIL. */
int tls_write(tls_t *t, const uint8_t *buf, uint32_t len);
/* Sends what is owed. True when nothing is. */
bool tls_flush(tls_t *t);
bool tls_established(const tls_t *t);
/* Queues close_notify and tries to send it. The session is over either way. */
void tls_close(tls_t *t);
/* Wipes keys and secrets. */
void tls_wipe(tls_t *t);

#endif /* NET_TLS_H */
