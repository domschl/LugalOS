#ifndef KERNEL_OTA_SIG_H
#define KERNEL_OTA_SIG_H

/* Checking an update image's signature (kernel/include/kernel/ota_image.h,
 * "Signatures"). Pure: no flash, no identity store, no policy -- kernel/ota.c
 * decides what a result means for this node, and tests/host/ota_host.c runs
 * this under the sanitizers. */

#include <stdbool.h>
#include <stdint.h>
#include "kernel/ota_image.h"

typedef enum {
    OTA_SIG_RESULT_OK = 0,      /* signed with `pubkey`, and the signature verifies */
    OTA_SIG_RESULT_UNSIGNED,    /* sig_alg is OTA_SIG_NONE and the signature fields are zero */
    OTA_SIG_RESULT_NO_KEY,      /* signed, but no key to check it against was given */
    OTA_SIG_RESULT_OTHER_KEY,   /* signed with a key other than `pubkey` */
    OTA_SIG_RESULT_BAD,         /* the right key id, and a signature that does not verify */
    OTA_SIG_RESULT_MALFORMED,   /* an unknown algorithm, or non-zero padding/reserved bytes */
} ota_sig_result_t;

/* The key id a signer's public key (64 bytes, X then Y) is named by. */
void ota_sig_key_id(const uint8_t pubkey[64], uint8_t id[OTA_KEY_ID_LEN]);

/* `pubkey` NULL: the node has no key. Takes about 0.2-0.4 s on the slower
 * chips (one P-256 verification). */
ota_sig_result_t ota_sig_check(const ota_image_header_t *h, const uint8_t *pubkey);

const char *ota_sig_result_str(ota_sig_result_t r);

#endif
