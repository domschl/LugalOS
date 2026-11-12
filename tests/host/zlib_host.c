#include "kernel/zlib.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pre-compressed RFC 1950 zlib stream (dynamic Huffman) produced by Python zlib.compress(..., 9). */
static const uint8_t k_dyn_zlib[] = {
    0x78, 0xda, 0x0b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56,
    0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a,
    0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28, 0x01, 0x4a,
    0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0xeb, 0x29, 0x84, 0x0c,
    0x0e, 0xc5, 0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96,
    0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa,
    0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa, 0x08, 0x79,
    0x2b, 0x6b, 0x1b, 0x5b, 0x3b, 0x7b, 0x07, 0x88, 0x32, 0x77, 0x0f, 0x4f,
    0x2f, 0x6f, 0x1f, 0x5f, 0x3f, 0xff, 0x80, 0xc0, 0xa0, 0xe0, 0x90, 0xd0,
    0xb0, 0xf0, 0x88, 0xc8, 0xa8, 0xe8, 0x98, 0xd8, 0xb8, 0xf8, 0x84, 0xc4,
    0xa4, 0xe4, 0x94, 0xd4, 0xb4, 0xf4, 0x8c, 0xcc, 0xac, 0xec, 0x9c, 0xdc,
    0xbc, 0xfc, 0x82, 0xc2, 0xa2, 0xe2, 0x92, 0xd2, 0xb2, 0xf2, 0x8a, 0xca,
    0xaa, 0xea, 0x9a, 0xda, 0x3a, 0x00, 0xde, 0x6a, 0x61, 0x90
};

static void test_checksums(void) {
    assert(zlib_adler32(1u, "Wikipedia", 9u) == 0x11e60398u);
    assert(zlib_crc32(0u, "123456789", 9u) == 0xcbf43926u);
}

static void test_stored_block(void) {
    /* Raw DEFLATE stored block: BFINAL=1, BTYPE=00, len=5, nlen=~5, "ABCDE" */
    static const uint8_t stored[] = {
        0x01, 0x05, 0x00, 0xfa, 0xff, 'A', 'B', 'C', 'D', 'E', 0x99
    };
    uint8_t out[16];
    size_t out_len = 0, consumed = 0;
    int rc = zlib_inflate_raw(stored, sizeof(stored), out, sizeof(out), &out_len, &consumed);
    assert(rc == ZLIB_OK);
    assert(out_len == 5u);
    assert(consumed == 10u);
    assert(memcmp(out, "ABCDE", 5u) == 0);
}

static void test_fixed_and_roundtrip(void) {
    const char *inputs[] = {
        "",
        "a",
        "hello world",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog!",
    };
    uint8_t zbuf[1024];
    uint8_t out[1024];
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        size_t slen = strlen(inputs[i]);
        size_t zlen = 0, olen = 0, consumed = 0;
        assert(zlib_deflate((const uint8_t *)inputs[i], slen, zbuf, sizeof(zbuf), &zlen) == ZLIB_OK);
        assert(zlib_inflate(zbuf, zlen, out, sizeof(out), &olen, &consumed) == ZLIB_OK);
        assert(olen == slen);
        assert(consumed == zlen);
        assert(slen == 0 || memcmp(out, inputs[i], slen) == 0);

        /* Also test trailing bytes after zlib stream (as in git packfiles). */
        zbuf[zlen] = 0xaa;
        zbuf[zlen + 1] = 0xbb;
        consumed = 0;
        assert(zlib_inflate(zbuf, zlen + 2u, out, sizeof(out), &olen, &consumed) == ZLIB_OK);
        assert(consumed == zlen);
    }
}

static void test_dynamic_and_errors(void) {
    uint8_t out[64];
    size_t olen = 0, consumed = 0;
    /* Empty/short/corrupt inputs must return errors cleanly without OOB. */
    assert(zlib_inflate(NULL, 0, out, sizeof(out), &olen, NULL) != ZLIB_OK);
    static const uint8_t bad_hdr[] = { 0x78, 0x00, 0x00, 0x00, 0x00, 0x00 };
    assert(zlib_inflate(bad_hdr, sizeof(bad_hdr), out, sizeof(out), &olen, NULL) == ZLIB_ERR_DATA);

    /* Inflate real dynamic-Huffman zlib stream. */
    uint8_t dyn_out[512];
    assert(zlib_inflate(k_dyn_zlib, sizeof(k_dyn_zlib), dyn_out, sizeof(dyn_out), &olen, &consumed) == ZLIB_OK);
    assert(consumed == sizeof(k_dyn_zlib));
    assert(olen == 45u * 4u + 16u + 95u);
    assert(memcmp(dyn_out, "The quick brown fox jumps over the lazy dog. ", 45u) == 0);
    assert(memcmp(dyn_out + 45u * 4u, "0123456789ABCDEF", 16u) == 0);

    uint8_t zbuf[128];
    size_t zlen = 0;
    const char *msg = "blob 12\0hello world\n";
    assert(zlib_deflate((const uint8_t *)msg, 20u, zbuf, sizeof(zbuf), &zlen) == ZLIB_OK);
    assert(zlib_inflate(zbuf, zlen, out, 5u, &olen, &consumed) == ZLIB_ERR_SPACE);
    /* Corrupt Adler-32 trailer */
    zbuf[zlen - 1u] ^= 0x01u;
    assert(zlib_inflate(zbuf, zlen, out, sizeof(out), &olen, &consumed) == ZLIB_ERR_DATA);
    (void)k_dyn_zlib;
}

int main(void) {
    test_checksums();
    test_stored_block();
    test_fixed_and_roundtrip();
    test_dynamic_and_errors();
    printf("zlib_host: all tests passed\n");
    return 0;
}
