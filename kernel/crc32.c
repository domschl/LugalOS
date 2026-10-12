#include "kernel/crc32.h"

/* Bitwise, no table: the identity record is at most 4 KB and read rarely, and
 * an LLP serial frame is at most ~4 KB on a link of a few Mbit/s, where 8
 * shift/xor steps per byte are far below the byte time. A 1 KB rodata table
 * buys nothing worth the space on RP2350. Moved here from kernel/idstore.c
 * (49.1) unchanged, so a second user did not mean a second implementation. */
uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t crc32_compute(const void *data, uint32_t len) {
    return ~crc32_update(0xFFFFFFFFu, data, len);
}
