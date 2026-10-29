#include "net/websocket.h"
#include "kernel/crypto.h"
#include <string.h>

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool ws_accept_key(const char *client_key, char out[WS_ACCEPT_LEN + 1]) {
    if (!client_key) return false;
    uint32_t n = (uint32_t)strlen(client_key);
    if (n == 0 || n > 64u) return false;

    uint8_t cat[64 + sizeof WS_GUID];
    memcpy(cat, client_key, n);
    memcpy(cat + n, WS_GUID, sizeof WS_GUID - 1);
    uint8_t digest[SHA1_DIGEST_LEN];
    sha1(cat, n + (uint32_t)sizeof WS_GUID - 1, digest);

    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t o = 0;
    for (uint32_t i = 0; i < SHA1_DIGEST_LEN; i += 3) {
        uint32_t left = SHA1_DIGEST_LEN - i;
        uint32_t v = (uint32_t)digest[i] << 16 | (left > 1 ? (uint32_t)digest[i + 1] << 8 : 0u) | (left > 2 ? digest[i + 2] : 0u);
        out[o++] = b64[v >> 18 & 63u];
        out[o++] = b64[v >> 12 & 63u];
        out[o++] = left > 1 ? b64[v >> 6 & 63u] : '=';
        out[o++] = left > 2 ? b64[v & 63u] : '=';
    }
    out[o] = 0;
    return true;
}

int ws_parse_header(const uint8_t *buf, uint32_t n, uint32_t max_payload, ws_hdr_t *h) {
    if (n < 2) return 0;
    if (buf[0] & 0x70) return -1;                       /* RSV1-3: no extension negotiated */
    h->fin = (buf[0] & 0x80) != 0;
    h->opcode = buf[0] & 0x0F;
    if ((h->opcode > WS_OP_BINARY && h->opcode < WS_OP_CLOSE) || h->opcode > WS_OP_PONG) return -1;
    h->masked = (buf[1] & 0x80) != 0;

    uint32_t hdr = 2;
    uint64_t len = buf[1] & 0x7F;
    if (len == 126) {
        if (n < 4) return 0;
        len = (uint64_t)buf[2] << 8 | buf[3];
        hdr = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | buf[2 + i];
        hdr = 10;
    }
    if (h->opcode >= WS_OP_CLOSE && (!h->fin || len > 125)) return -1;
    if (len > max_payload) return -1;
    if (h->masked) {
        if (n < hdr + 4) return 0;
        memcpy(h->mask, buf + hdr, 4);
        hdr += 4;
    } else {
        memset(h->mask, 0, 4);
    }
    h->payload_len = (uint32_t)len;
    return (int)hdr;
}

void ws_unmask(uint8_t *buf, uint32_t len, const uint8_t mask[4], uint32_t offset) {
    for (uint32_t i = 0; i < len; i++) buf[i] ^= mask[(offset + i) & 3u];
}

uint32_t ws_build_header(uint8_t *out, uint8_t opcode, uint32_t len, const uint8_t mask[4]) {
    uint32_t n = 0;
    uint8_t m = mask ? 0x80 : 0;
    out[n++] = 0x80 | (opcode & 0x0F);
    if (len < 126) {
        out[n++] = m | (uint8_t)len;
    } else if (len <= 0xFFFF) {
        out[n++] = m | 126;
        out[n++] = (uint8_t)(len >> 8);
        out[n++] = (uint8_t)len;
    } else {
        out[n++] = m | 127;
        for (int i = 0; i < 4; i++) out[n++] = 0;
        out[n++] = (uint8_t)(len >> 24);
        out[n++] = (uint8_t)(len >> 16);
        out[n++] = (uint8_t)(len >> 8);
        out[n++] = (uint8_t)len;
    }
    if (mask) { memcpy(out + n, mask, 4); n += 4; }
    return n;
}
