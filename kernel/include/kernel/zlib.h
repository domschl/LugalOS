#ifndef LUGALOS_KERNEL_ZLIB_H
#define LUGALOS_KERNEL_ZLIB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Freestanding RFC 1950 (zlib) and RFC 1951 (DEFLATE) compression/decompression,
 * plus Adler-32 and CRC-32. Zero static writable storage and zero libc heap
 * dependencies; suitable for Git loose/pack objects and ZIP/EPUB archives.
 */

#define ZLIB_OK          0
#define ZLIB_ERR_DATA   -1   /* corrupt header, bad Huffman code, or Adler-32 mismatch */
#define ZLIB_ERR_SPACE  -2   /* output buffer too small */
#define ZLIB_ERR_INVAL  -3   /* invalid arguments */

uint32_t zlib_adler32(uint32_t adler, const void *buf, size_t len);
uint32_t zlib_crc32(uint32_t crc, const void *buf, size_t len);

/* Inflates raw RFC 1951 DEFLATE stream. `in_consumed` (if non-NULL) receives
 * the exact number of input bytes consumed (needed for packed git objects). */
int zlib_inflate_raw(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap,
                     size_t *out_len, size_t *in_consumed);

/* Inflates RFC 1950 zlib stream (2-byte header + DEFLATE + 4-byte Adler-32). */
int zlib_inflate(const uint8_t *src, size_t src_len,
                 uint8_t *dst, size_t dst_cap,
                 size_t *out_len, size_t *in_consumed);

/* Deflates `src` into raw RFC 1951 DEFLATE stream (LZ77 + fixed Huffman). */
int zlib_deflate_raw(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap,
                     size_t *out_len);

/* Deflates `src` into RFC 1950 zlib stream. */
int zlib_deflate(const uint8_t *src, size_t src_len,
                 uint8_t *dst, size_t dst_cap,
                 size_t *out_len);

#endif /* LUGALOS_KERNEL_ZLIB_H */
