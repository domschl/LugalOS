/* The update image's and the boot record's logic (kernel/include/kernel/ota_image.h).
 * Freestanding and pure -- no kernel call, no library call -- because a board's
 * loader links it too, and tests/host/ota_host.c runs it under the sanitizers. */

#include "kernel/ota_image.h"

/* The zlib/Ethernet CRC-32, bitwise: a loader has no room to spare for a table. */
uint32_t ota_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xffffffffu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

bool ota_bootrec_valid(const ota_bootrec_t *r, uint32_t nslots) {
    if (!r || r->magic != OTA_BOOTREC_MAGIC) return false;
    if (r->crc != ota_crc32(r, (uint32_t)__builtin_offsetof(ota_bootrec_t, crc))) return false;
    if (r->active >= nslots) return false;
    return r->pending == OTA_SLOT_NONE || r->pending < nslots;
}

int ota_bootrec_current(const ota_bootrec_t *rec[2], uint32_t nslots) {
    bool v0 = ota_bootrec_valid(rec[0], nslots), v1 = ota_bootrec_valid(rec[1], nslots);
    if (v0 && v1) return (int32_t)(rec[1]->seq - rec[0]->seq) > 0 ? 1 : 0;   /* wraps safely */
    return v0 ? 0 : v1 ? 1 : -1;
}

ota_boot_choice_t ota_boot_choose(const ota_bootrec_t *rec[2], uint32_t nslots) {
    ota_boot_choice_t c = { .slot = 0, .trial = false, .fell_back = false,
                            .current = -1, .mark_sector = -1, .mark_off = 0 };
    int cur = ota_bootrec_current(rec, nslots);
    if (cur < 0) return c;                      /* a board fresh from the header pins: slot 0 */
    const ota_bootrec_t *r = rec[cur];
    c.current = cur;
    c.slot = r->active;
    if (r->pending == OTA_SLOT_NONE || r->pending == r->active) return c;
    if (r->tried == 0xffffffffu) {
        c.slot = r->pending;
        c.trial = true;
        c.mark_sector = cur;
        c.mark_off = OTA_BOOTREC_TRIED_OFF;
        return c;
    }
    c.fell_back = true;
    if (r->failed == 0xffffffffu) {
        c.mark_sector = cur;
        c.mark_off = OTA_BOOTREC_FAILED_OFF;
    }
    return c;
}

void ota_bootrec_make(ota_bootrec_t *r, uint32_t seq, uint8_t active, uint8_t pending) {
    r->magic = OTA_BOOTREC_MAGIC;
    r->seq = seq;
    r->active = active;
    r->pending = pending;
    r->pad[0] = r->pad[1] = 0;
    r->crc = ota_crc32(r, (uint32_t)__builtin_offsetof(ota_bootrec_t, crc));
    r->tried = 0xffffffffu;
    r->failed = 0xffffffffu;
}

bool ota_header_valid(const ota_image_header_t *h) {
    return h && h->magic == OTA_IMAGE_MAGIC && h->version == OTA_IMAGE_VERSION &&
           h->header_len == OTA_HEADER_LEN &&
           h->crc == ota_crc32(h, (uint32_t)__builtin_offsetof(ota_image_header_t, crc));
}
