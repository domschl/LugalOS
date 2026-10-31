#include "kernel/der.h"
#include <string.h>

void der_writer_init(der_writer_t *w, uint8_t *buf, uint32_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->depth = 0;
    w->err = false;
}

static void put_byte(der_writer_t *w, uint8_t b) {
    if (w->err) return;
    if (w->len >= w->cap) { w->err = true; return; }
    w->buf[w->len++] = b;
}

/* Tag and length, then (for a primitive) the content follows. */
static void put_header(der_writer_t *w, uint8_t tag, uint32_t len) {
    put_byte(w, tag);
    if (len < 0x80u) {
        put_byte(w, (uint8_t)len);
    } else if (len < 0x100u) {
        put_byte(w, 0x81u);
        put_byte(w, (uint8_t)len);
    } else if (len < 0x10000u) {
        put_byte(w, 0x82u);
        put_byte(w, (uint8_t)(len >> 8));
        put_byte(w, (uint8_t)len);
    } else {
        w->err = true;
    }
}

void der_begin(der_writer_t *w, uint8_t tag) {
    if (w->err) return;
    if (w->depth >= DER_MAX_DEPTH) { w->err = true; return; }
    put_byte(w, tag);
    w->open[w->depth++] = w->len;
    put_byte(w, 0);                               /* the length, fixed by der_end() */
}

void der_end(der_writer_t *w) {
    if (w->err) return;
    if (w->depth == 0) { w->err = true; return; }
    uint32_t at = w->open[--w->depth];
    uint32_t start = at + 1u;
    uint32_t len = w->len - start;
    if (len < 0x80u) { w->buf[at] = (uint8_t)len; return; }
    uint32_t extra = len < 0x100u ? 1u : len < 0x10000u ? 2u : 0u;
    if (extra == 0 || w->len + extra > w->cap) { w->err = true; return; }
    memmove(&w->buf[start + extra], &w->buf[start], len);
    w->buf[at] = (uint8_t)(0x80u | extra);
    if (extra == 2) { w->buf[start] = (uint8_t)(len >> 8); w->buf[start + 1] = (uint8_t)len; }
    else w->buf[start] = (uint8_t)len;
    w->len += extra;
}

void der_put(der_writer_t *w, uint8_t tag, const void *val, uint32_t len) {
    put_header(w, tag, len);
    if (w->err) return;
    if (w->len + len > w->cap) { w->err = true; return; }
    if (len) memcpy(&w->buf[w->len], val, len);
    w->len += len;
}

void der_put_uint(der_writer_t *w, const uint8_t *be, uint32_t len) {
    while (len > 1 && be[0] == 0) { be++; len--; }
    bool pad = len == 0 || (be[0] & 0x80u);
    put_header(w, DER_INTEGER, len + (pad ? 1u : 0u));
    if (pad) put_byte(w, 0);
    if (w->err) return;
    if (w->len + len > w->cap) { w->err = true; return; }
    if (len) memcpy(&w->buf[w->len], be, len);
    w->len += len;
}

void der_put_small(der_writer_t *w, uint32_t v) {
    uint8_t be[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    der_put_uint(w, be, 4);
}

void der_put_raw(der_writer_t *w, const void *der, uint32_t len) {
    if (w->err) return;
    if (w->len + len > w->cap) { w->err = true; return; }
    memcpy(&w->buf[w->len], der, len);
    w->len += len;
}

/* --- reader --- */

static bool read_header(const der_span_t *in, uint8_t *tag, uint32_t *hdr, uint32_t *len) {
    if (in->len < 2) return false;
    *tag = in->p[0];
    if ((in->p[0] & 0x1fu) == 0x1fu) return false;     /* high tag numbers: none in what we read */
    uint8_t l0 = in->p[1];
    if (l0 < 0x80u) { *hdr = 2; *len = l0; }
    else if (l0 == 0x81u && in->len >= 3 && in->p[2] >= 0x80u) { *hdr = 3; *len = in->p[2]; }
    else if (l0 == 0x82u && in->len >= 4 && in->p[2] != 0) { *hdr = 4; *len = (uint32_t)in->p[2] << 8 | in->p[3]; }
    else return false;                                   /* indefinite, non-minimal or too long */
    return *len <= in->len - *hdr;
}

bool der_next(der_span_t *in, uint8_t *tag, der_span_t *val) {
    uint32_t hdr, len;
    uint8_t t;
    if (!read_header(in, &t, &hdr, &len)) return false;
    if (tag) *tag = t;
    if (val) { val->p = in->p + hdr; val->len = len; }
    in->p += hdr + len;
    in->len -= hdr + len;
    return true;
}

bool der_expect(der_span_t *in, uint8_t tag, der_span_t *val) {
    der_span_t save = *in;
    uint8_t t;
    if (!der_next(in, &t, val) || t != tag) { *in = save; return false; }
    return true;
}

bool der_next_raw(der_span_t *in, der_span_t *whole) {
    uint32_t hdr, len;
    uint8_t t;
    if (!read_header(in, &t, &hdr, &len)) return false;
    whole->p = in->p;
    whole->len = hdr + len;
    in->p += hdr + len;
    in->len -= hdr + len;
    return true;
}

/* --- base64 and PEM --- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t base64_encode(const uint8_t *in, uint32_t n, char *out, uint32_t cap) {
    uint32_t need = (n + 2u) / 3u * 4u;
    if (need > cap) return 0;
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 3) {
        uint32_t left = n - i;
        uint32_t v = (uint32_t)in[i] << 16 | (left > 1 ? (uint32_t)in[i + 1] << 8 : 0) | (left > 2 ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63u];
        out[o++] = B64[v >> 12 & 63u];
        out[o++] = left > 1 ? B64[v >> 6 & 63u] : '=';
        out[o++] = left > 2 ? B64[v & 63u] : '=';
    }
    return o;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *in, uint32_t n, uint8_t *out, uint32_t cap) {
    uint32_t acc = 0, bits = 0, o = 0;
    bool ended = false;
    for (uint32_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') { ended = true; continue; }
        int v = b64_value(c);
        if (v < 0 || ended) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

static uint32_t put_str(char *out, uint32_t o, uint32_t cap, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    if (o == UINT32_MAX || o + n > cap) return UINT32_MAX;
    memcpy(out + o, s, n);
    return o + n;
}

uint32_t pem_encode(const char *label, const uint8_t *der, uint32_t n, char *out, uint32_t cap) {
    uint32_t o = put_str(out, 0, cap, "-----BEGIN ");
    o = put_str(out, o, cap, label);
    o = put_str(out, o, cap, "-----\n");
    for (uint32_t i = 0; i < n && o != UINT32_MAX; i += 48) {
        uint32_t take = n - i < 48u ? n - i : 48u;
        uint32_t k = base64_encode(der + i, take, out + o, cap - o);
        if (k == 0) return 0;
        o = put_str(out, o + k, cap, "\n");
    }
    o = put_str(out, o, cap, "-----END ");
    o = put_str(out, o, cap, label);
    o = put_str(out, o, cap, "-----\n");
    return o == UINT32_MAX ? 0 : o;
}

/* The offset of `needle` in hay[from..n), or -1. */
static int32_t find(const char *hay, uint32_t n, uint32_t from, const char *needle) {
    uint32_t k = (uint32_t)strlen(needle);
    for (uint32_t i = from; i + k <= n; i++) {
        if (memcmp(hay + i, needle, k) == 0) return (int32_t)i;
    }
    return -1;
}

int pem_decode(const char *text, uint32_t n, const char *label, uint8_t *der, uint32_t cap, uint32_t *end) {
    char begin[48], fin[48];
    uint32_t ll = (uint32_t)strlen(label);
    if (ll + 17u > sizeof begin) return -1;
    memcpy(begin, "-----BEGIN ", 11); memcpy(begin + 11, label, ll); memcpy(begin + 11 + ll, "-----", 6);
    memcpy(fin, "-----END ", 9); memcpy(fin + 9, label, ll); memcpy(fin + 9 + ll, "-----", 6);
    int32_t b = find(text, n, 0, begin);
    if (b < 0) return -1;
    uint32_t body = (uint32_t)b + (uint32_t)strlen(begin);
    int32_t e = find(text, n, body, fin);
    if (e < 0) return -1;
    int got = base64_decode(text + body, (uint32_t)e - body, der, cap);
    if (end) *end = (uint32_t)e + (uint32_t)strlen(fin);
    return got;
}
