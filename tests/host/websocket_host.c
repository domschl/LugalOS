/*
 * WebSocket framing (net/websocket.c) on the host, under ASan/UBSan: the RFC 6455 handshake
 * example, frames built and parsed in every length class, and the parser against hostile headers.
 */
#include "net/websocket.h"
#include "shim.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* kernel/crypto.c asks where its entropy comes from; framing never draws any. */
bool random_is_hardware(void) { return false; }

static int g_bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); g_bad = 1; } } while (0)

int main(void) {
    /* RFC 6455 section 1.3's own example. */
    char acc[WS_ACCEPT_LEN + 1];
    CHECK(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc), "accept key refused");
    CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "accept key %s", acc);
    CHECK(!ws_accept_key("", acc), "empty key accepted");

    /* Every length class round-trips, masked and not, and the payload unmasks to itself. */
    static const uint32_t lens[] = { 0, 1, 125, 126, 127, 4095, 65535, 65536, 70000 };
    static uint8_t frame[WS_HDR_MAX + 70000], payload[70000];
    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 7u);
    for (unsigned k = 0; k < sizeof lens / sizeof lens[0]; k++) {
        for (int masked = 0; masked < 2; masked++) {
            const uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
            uint32_t h = ws_build_header(frame, WS_OP_BINARY, lens[k], masked ? mask : NULL);
            memcpy(frame + h, payload, lens[k]);
            if (masked) ws_unmask(frame + h, lens[k], mask, 0);
            ws_hdr_t hd;
            int r = ws_parse_header(frame, h + lens[k], 100000, &hd);
            CHECK(r == (int)h, "len %u masked %d: header %d want %u", lens[k], masked, r, h);
            CHECK(hd.opcode == WS_OP_BINARY && hd.fin && hd.masked == (bool)masked && hd.payload_len == lens[k], "fields");
            if (masked) ws_unmask(frame + h, lens[k], hd.mask, 0);
            CHECK(memcmp(frame + h, payload, lens[k]) == 0, "payload len %u", lens[k]);
            /* Every strict prefix of the header asks for more bytes. */
            for (uint32_t cut = 0; cut < h; cut++)
                CHECK(ws_parse_header(frame, cut, 100000, &hd) == 0, "prefix %u of %u", cut, h);
        }
    }

    /* Unmasking in pieces equals unmasking whole. */
    uint8_t a[100], b[100];
    const uint8_t mk[4] = { 1, 2, 3, 4 };
    for (int i = 0; i < 100; i++) a[i] = b[i] = (uint8_t)i;
    ws_unmask(a, 100, mk, 0);
    ws_unmask(b, 33, mk, 0);
    ws_unmask(b + 33, 67, mk, 33);
    CHECK(memcmp(a, b, 100) == 0, "unmask in pieces");

    /* Hostile headers. */
    ws_hdr_t hd;
    static const uint8_t rsv[]     = { 0xC2, 0x00 };
    static const uint8_t badop[]   = { 0x83, 0x00 };
    static const uint8_t fragctl[] = { 0x09, 0x00 };                 /* ping without FIN */
    static const uint8_t bigctl[]  = { 0x89, 126, 0, 200 };          /* ping of 200 bytes */
    static const uint8_t huge[]    = { 0x82, 127, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t over[]    = { 0x82, 127, 0, 0, 0, 1, 0, 0, 0, 0 };   /* 4 GiB */
    CHECK(ws_parse_header(rsv, sizeof rsv, 1000, &hd) < 0, "reserved bit");
    CHECK(ws_parse_header(badop, sizeof badop, 1000, &hd) < 0, "reserved opcode");
    CHECK(ws_parse_header(fragctl, sizeof fragctl, 1000, &hd) < 0, "fragmented control frame");
    CHECK(ws_parse_header(bigctl, sizeof bigctl, 1000, &hd) < 0, "oversized control frame");
    CHECK(ws_parse_header(huge, sizeof huge, 1000, &hd) < 0, "64-bit length");
    CHECK(ws_parse_header(over, sizeof over, 1000, &hd) < 0, "4 GiB length");

    /* Random bytes never crash the parser and never report a header longer than they have. */
    uint64_t s = 0x5eed;
    for (int it = 0; it < 200000; it++) {
        uint8_t buf[16];
        uint32_t n = host_rand(&s) % 17u;
        for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)host_rand(&s);
        int r = ws_parse_header(buf, n, 4096, &hd);
        CHECK(r <= (int)n, "header longer than input");
        if (r > 0) CHECK(hd.payload_len <= 4096, "payload over the cap");
    }
    if (g_bad) return 1;
    printf("websocket_host: handshake key, framing in every length class, hostile headers\n");
    return 0;
}
