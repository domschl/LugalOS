#ifndef KERNEL_CRC32_H
#define KERNEL_CRC32_H

#include <stdint.h>

/* CRC-32 (the IEEE 802.3 polynomial, init/final 0xFFFFFFFF -- the same
 * construction as zip and Ethernet, so any host tool can check it without a
 * bespoke implementation: Python's zlib.crc32() gives the same value).
 *
 * Used by the identity record (kernel/idstore.c, tools/provision.py) and by
 * the LLP serial binding (kernel/llp.c, plan/phase49_c6_network_nodes.md).
 *
 * crc32_update() continues a CRC over discontiguous pieces: start from
 * 0xFFFFFFFF, feed each piece, invert at the end -- which is exactly what
 * crc32_compute() does for one piece. */
uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len);
uint32_t crc32_compute(const void *data, uint32_t len);

#endif
