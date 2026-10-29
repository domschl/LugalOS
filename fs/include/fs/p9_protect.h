#ifndef FS_P9_PROTECT_H
#define FS_P9_PROTECT_H

/* Integrity-protected 9P sessions (plan/phase52_remote_admin.md, 52.11b; doc/dev/protocols).
 *
 * A network link without TLS is read by anyone on the LAN and written by anyone who can inject
 * a packet. The auth gate (Tauth, a HMAC over a nonce) proves who attached but protects nothing
 * after that: a hijacked or forged frame is served as if the owner sent it. A protected session
 * closes that, with the SHA-256 the tree already has:
 *
 *   Tversion offers "9P2000.lp"; a node that protects the link answers "9P2000.lp".
 *   After the auth response is accepted (the Rwrite is the last plain frame), both ends derive
 *       K = HKDF-SHA-256(salt = nonce, ikm = grant key, info = "LugalOS 9P v1" 0 uname 0 aname 0)
 *   (one output block: PRK = HMAC(nonce, key), K = HMAC(PRK, info || 0x01)).
 *   Every later frame, both ways, is followed by an 8-byte tag
 *       HMAC-SHA-256(K, dir || seq || frame)[0..8)
 *   dir is 0 for client-to-server and 1 for server-to-client, seq a 32-bit little-endian counter
 *   of the frames sent in that direction since activation: a reflected, replayed, reordered,
 *   dropped, altered or injected frame fails the check, and the first failure ends the session.
 *
 * This is integrity, not confidentiality: the frames are readable on the wire, so a secret
 * (a WLAN key, a grant) must not cross a link like this (p9_request_confidential()).
 *
 * A client that does not offer the extension gets a read-only session (P9_AUTH_REQUIRED_RO). */

#include <stdint.h>
#include <stdbool.h>

#define P9_TAG_LEN      8u
#define P9_DIR_C2S      0u
#define P9_DIR_S2C      1u
#define P9_SESSION_KEY_LEN 32u

typedef enum {
    P9_PROT_NONE = 0,       /* not offered */
    P9_PROT_OFFERED,        /* Tversion agreed the extension; no key yet */
    P9_PROT_ARMED,          /* the auth response was accepted; the Rwrite is yet to go out plain */
    P9_PROT_ACTIVE,         /* every frame in both directions carries a tag */
    P9_PROT_DEAD,           /* a frame failed its check: the link is to be closed */
} p9_prot_state_t;

typedef struct {
    uint8_t  key[P9_SESSION_KEY_LEN];
    uint32_t rx_seq, tx_seq;
    uint8_t  state;         /* p9_prot_state_t */
} p9_prot_t;

void p9_protect_derive(const uint8_t *key, uint32_t key_len, const uint8_t nonce[32],
                       const char *uname, const char *aname, uint8_t out[P9_SESSION_KEY_LEN]);

void p9_protect_tag(const uint8_t key[P9_SESSION_KEY_LEN], uint8_t dir, uint32_t seq,
                    const uint8_t *frame, uint32_t len, uint8_t out[P9_TAG_LEN]);

/* Constant time. */
bool p9_protect_verify(const uint8_t key[P9_SESSION_KEY_LEN], uint8_t dir, uint32_t seq,
                       const uint8_t *frame, uint32_t len, const uint8_t tag[P9_TAG_LEN]);

#endif
