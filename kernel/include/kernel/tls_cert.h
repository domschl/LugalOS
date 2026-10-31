#ifndef KERNEL_TLS_CERT_H
#define KERNEL_TLS_CERT_H

#include <stdbool.h>
#include <stdint.h>

/* The node's HTTPS identity (plan/phase52 52.14): a P-256 key the node makes itself and never gives
 * out, a certificate request for it, and the certificate the owner's CA issues (host/ca). Three
 * files under /dev:
 *
 *   /dev/tls      read: the key's fingerprint, the certificate's names and dates, whether they
 *                 match. write (admin): `keygen` (only when there is no key), `rekey` (a new key; the
 *                 certificate goes with the old one), `clear` (both).
 *   /dev/tlscsr   read: a PKCS#10 request, PEM, for CN=<name>.local with that name and the node's
 *                 current IPv4 address as subjectAltNames, signed with the key. Built on the first read
 *                 and kept until the key or the certificate changes, so a client reading it in parts
 *                 gets one request.
 *   /dev/tlscert  read: the installed certificate, PEM. write (admin): a PEM certificate (a chain may
 *                 follow; only the first block is kept), from offset 0, in as many writes as it
 *                 takes. Installed as soon as its block is complete, if it names this node's key;
 *                 refused otherwise.
 *
 * The private key lives in the identity record's owner part (IDSTORE_FIELD_TLS_KEY): an owner reset
 * wipes it, nothing serves it. */

#define TLS_OK             0
#define TLS_ERR_NO_KEY    -1   /* no key yet: `keygen` first */
#define TLS_ERR_HAVE_KEY  -2   /* `keygen` when there is one: `rekey` replaces it */
#define TLS_ERR_ENTROPY   -3   /* no hardware entropy: no key, no signature */
#define TLS_ERR_STORE     -4   /* the identity record could not be written */
#define TLS_ERR_CERT      -5   /* not a P-256 certificate this code can read */
#define TLS_ERR_WRONG_KEY -6   /* a certificate for some other key */
#define TLS_ERR_MEMORY    -7

const char *tls_result_str(int rc);

int tls_keygen(bool replace);
int tls_clear(void);

/* The certificate's fields, read from its DER (no signature check: the browser does that). */
typedef struct {
    uint8_t pub[64];              /* subjectPublicKeyInfo's point, X then Y */
    char    subject_cn[64];
    char    issuer_cn[64];
    char    not_before[20];       /* as encoded: YYMMDDHHMMSSZ or YYYYMMDDHHMMSSZ */
    char    not_after[20];
} tls_cert_info_t;
int tls_cert_parse(const uint8_t *der, uint32_t len, tls_cert_info_t *out);

/* A PKCS#10 request for `pub`, signed with `priv`, CN = `cn`, SAN dNSName `cn` and, if `ip` is not
 * NULL, iPAddress `ip`. Returns its length in `out`, or a TLS_ERR_*. Target-independent (host test). */
int tls_csr_build(const uint8_t priv[32], const uint8_t pub[64], const char *cn, const uint8_t ip[4],
                  uint8_t *out, uint32_t cap);

/* The /dev files. */
uint32_t tls_ctl_read(char *out, uint32_t cap);
int tls_ctl_write(const char *buf, uint32_t len);
int tls_csr_read(void *buf, uint32_t count, uint64_t off);
int tls_certfile_read(void *buf, uint32_t count, uint64_t off);
int tls_certfile_write(const void *buf, uint32_t count, uint64_t off);

#endif /* KERNEL_TLS_CERT_H */
