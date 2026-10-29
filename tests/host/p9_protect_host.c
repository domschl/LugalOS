/*
 * Protected-session keys and frame tags (fs/p9_protect.c) on the host, under ASan/UBSan: the values
 * are those of an independent implementation (Python's hmac), and the same vectors are checked by
 * host/p9lib and by the admin page's JavaScript -- three implementations that must agree on the wire.
 */
#include "fs/p9_protect.h"
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); g_bad = 1; } } while (0)

static int hexeq(const uint8_t *b, size_t n, const char *h) {
    char s[160];
    for (size_t i = 0; i < n; i++) sprintf(s + 2 * i, "%02x", b[i]);
    return strcmp(s, h) == 0;
}

int main(void) {
    uint8_t key[16], nonce[32], K[32], frame[11] = { 0x0b, 0, 0, 0, 100, 0xff, 0xff, 0, 0x10, 0, 0 }, tag[P9_TAG_LEN];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(1 + i);
    for (int i = 0; i < 32; i++) nonce[i] = (uint8_t)(100 + i);

    p9_protect_derive(key, sizeof key, nonce, "lugal", "/", K);
    CHECK(hexeq(K, 32, "efd2818fdd1968a223551d03afd03e7dfc82e74335e776e5fbfe0426f39acb39"), "session key");
    p9_protect_tag(K, P9_DIR_C2S, 0, frame, sizeof frame, tag);
    CHECK(hexeq(tag, 8, "0d6abc4a07e0f2ff"), "tag c2s seq 0");
    p9_protect_tag(K, P9_DIR_S2C, 0, frame, sizeof frame, tag);
    CHECK(hexeq(tag, 8, "0c08bacd3c090cf0"), "tag s2c seq 0");
    p9_protect_tag(K, P9_DIR_C2S, 7, frame, sizeof frame, tag);
    CHECK(hexeq(tag, 8, "212ed7169c0581e4"), "tag c2s seq 7");

    /* A different uname, aname, nonce or key gives a different session key. */
    uint8_t K2[32];
    p9_protect_derive(key, sizeof key, nonce, "other", "/", K2);  CHECK(memcmp(K, K2, 32), "uname is in the key");
    p9_protect_derive(key, sizeof key, nonce, "lugal", "/x", K2); CHECK(memcmp(K, K2, 32), "aname is in the key");
    nonce[0]++;
    p9_protect_derive(key, sizeof key, nonce, "lugal", "/", K2);  CHECK(memcmp(K, K2, 32), "nonce is in the key");
    nonce[0]--;
    key[0]++;
    p9_protect_derive(key, sizeof key, nonce, "lugal", "/", K2);  CHECK(memcmp(K, K2, 32), "grant key is in the key");

    /* Verification accepts the right tag and refuses every alteration of what it covers. */
    p9_protect_tag(K, P9_DIR_C2S, 3, frame, sizeof frame, tag);
    CHECK(p9_protect_verify(K, P9_DIR_C2S, 3, frame, sizeof frame, tag), "good tag refused");
    CHECK(!p9_protect_verify(K, P9_DIR_S2C, 3, frame, sizeof frame, tag), "a reflected frame was accepted");
    CHECK(!p9_protect_verify(K, P9_DIR_C2S, 4, frame, sizeof frame, tag), "a replayed/reordered frame was accepted");
    CHECK(!p9_protect_verify(K, P9_DIR_C2S, 2, frame, sizeof frame, tag), "an old sequence number was accepted");
    for (size_t i = 0; i < sizeof frame; i++) {
        frame[i] ^= 1;
        CHECK(!p9_protect_verify(K, P9_DIR_C2S, 3, frame, sizeof frame, tag), "byte %zu altered, accepted", i);
        frame[i] ^= 1;
    }
    for (size_t i = 0; i < P9_TAG_LEN; i++) {
        tag[i] ^= 0x80;
        CHECK(!p9_protect_verify(K, P9_DIR_C2S, 3, frame, sizeof frame, tag), "tag byte %zu altered, accepted", i);
        tag[i] ^= 0x80;
    }
    CHECK(!p9_protect_verify(K, P9_DIR_C2S, 3, frame, sizeof frame - 1, tag), "a truncated frame was accepted");

    /* Frames of every length tag and verify (a frame longer than one SHA-256 block, and msize). */
    static uint8_t big[4096];
    for (unsigned i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 31u);
    for (unsigned n = 0; n <= sizeof big; n += (n < 200 ? 1 : 97)) {
        p9_protect_tag(K, P9_DIR_S2C, n, big, n, tag);
        CHECK(p9_protect_verify(K, P9_DIR_S2C, n, big, n, tag), "length %u", n);
    }
    if (g_bad) return 1;
    printf("p9_protect_host: session key and tags match the reference; every alteration refused\n");
    return 0;
}
