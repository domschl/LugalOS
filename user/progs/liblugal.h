#ifndef LUGALOS_USER_LIBLUGAL_H
#define LUGALOS_USER_LIBLUGAL_H

#include "usys.h"
#include <stdint.h>
#include <stdbool.h>

/* Shared U-mode OS client wrappers over VFS syscalls and Plan 9 channels.
 *
 * Used by both U-mode C programs and MicroPython C modules so both
 * language runtimes share identical kernel IPC semantics. */

#define LUGAL_SCREEN_REPLY_LEN 32u

static inline void lugal_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static inline uint16_t lugal_get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline int32_t lugal_get_le32(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)u;
}

static inline bool lugal_canvas_call(const uint8_t *req, uint32_t req_len,
                                     uint8_t reply[LUGAL_SCREEN_REPLY_LEN]) {
    uint8_t tmp[LUGAL_SCREEN_REPLY_LEN];
    uint8_t *out = reply ? reply : tmp;
    long n = uchan_call("canvas", req, (long)req_len, out, (long)LUGAL_SCREEN_REPLY_LEN);
    return n >= 1 && out[0] == 0;
}

static inline bool lugal_canvas_open(void) {
    uint8_t req[2] = { 'k', 1 };
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline bool lugal_canvas_close(void) {
    uint8_t req[2] = { 'k', 0 };
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline bool lugal_canvas_size(uint16_t *w, uint16_t *h) {
    uint8_t req[1] = { 'y' };
    uint8_t reply[LUGAL_SCREEN_REPLY_LEN];
    if (!lugal_canvas_call(req, sizeof(req), reply)) return false;
    if (w) *w = lugal_get16(reply + 2);
    if (h) *h = lugal_get16(reply + 4);
    return true;
}

static inline bool lugal_canvas_fill(uint16_t colour) {
    uint8_t req[3] = { 'f', 0, 0 };
    lugal_put16(req + 1, colour);
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline bool lugal_canvas_pixel(uint16_t x, uint16_t y, uint16_t colour) {
    uint8_t req[7];
    req[0] = 'p';
    lugal_put16(req + 1, x);
    lugal_put16(req + 3, y);
    lugal_put16(req + 5, colour);
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline bool lugal_canvas_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                     uint16_t colour) {
    uint8_t req[11];
    req[0] = 'r';
    lugal_put16(req + 1, x);
    lugal_put16(req + 3, y);
    lugal_put16(req + 5, w);
    lugal_put16(req + 7, h);
    lugal_put16(req + 9, colour);
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline bool lugal_canvas_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      uint16_t colour) {
    uint8_t req[11];
    req[0] = 'o';
    lugal_put16(req + 1, x);
    lugal_put16(req + 3, y);
    lugal_put16(req + 5, w);
    lugal_put16(req + 7, h);
    lugal_put16(req + 9, colour);
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline bool lugal_canvas_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                                     uint16_t colour) {
    uint8_t req[11];
    req[0] = 'l';
    lugal_put16(req + 1, x0);
    lugal_put16(req + 3, y0);
    lugal_put16(req + 5, x1);
    lugal_put16(req + 7, y1);
    lugal_put16(req + 9, colour);
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline bool lugal_canvas_circle(uint16_t x, uint16_t y, uint16_t r, uint16_t colour) {
    uint8_t req[9];
    req[0] = 'c';
    lugal_put16(req + 1, x);
    lugal_put16(req + 3, y);
    lugal_put16(req + 5, r);
    lugal_put16(req + 7, colour);
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline bool lugal_canvas_text(uint16_t x, uint16_t y, const char *s,
                                     uint16_t colour, uint16_t scale) {
    uint8_t req[144];
    req[0] = 't';
    lugal_put16(req + 1, x);
    lugal_put16(req + 3, y);
    lugal_put16(req + 5, colour);
    lugal_put16(req + 7, scale);
    uint32_t i = 0;
    while (s && s[i] && i < sizeof(req) - 10u) {
        req[9u + i] = (uint8_t)s[i];
        i++;
    }
    req[9u + i] = '\0';
    return lugal_canvas_call(req, 10u + i, 0);
}

static inline bool lugal_canvas_swap(void) {
    uint8_t req[1] = { 'W' };
    return lugal_canvas_call(req, sizeof(req), 0);
}

static inline long lugal_sensor_list(char *buf, long max_len) {
    uint8_t req[1] = { 'L' };
    long n = uchan_call("sensor", req, 1, buf, max_len > 0 ? max_len - 1 : 0);
    if (n >= 0 && buf && max_len > 0) buf[n] = '\0';
    return n;
}

static inline bool lugal_sensor_read(const char *chan, int32_t *val_out, uint32_t *age_out) {
    uint8_t req[33];
    req[0] = 'R';
    uint32_t i = 0;
    while (chan && chan[i] && i < 31u) {
        req[1u + i] = (uint8_t)chan[i];
        i++;
    }
    req[1u + i] = '\0';
    uint8_t resp[9];
    long n = uchan_call("sensor", req, (long)(2u + i), resp, sizeof(resp));
    if (n < 9 || resp[0] != 0) return false;
    if (val_out) *val_out = lugal_get_le32(resp + 1);
    if (age_out) *age_out = (uint32_t)lugal_get_le32(resp + 5);
    return true;
}

#endif /* LUGALOS_USER_LIBLUGAL_H */
