#ifndef DRIVERS_SLIP_H
#define DRIVERS_SLIP_H

#include <stdint.h>
#include <stdbool.h>

/* SLIP framing (RFC 1055), shared by every byte-stream link in the tree: the
 * console's 9P link and its demux (drivers/uart_net.c), the RP2350's UART1
 * downlink, and LLP's serial binding (kernel/llp.c). Its own file since 49.1,
 * so a protocol that needs SLIP need not link a UART driver to get it.
 * Target-independent and host-tested (tests/host/llp_host.c). */

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

/* One frame, END-delimited at both ends. Returns the encoded length, or -1
 * if it does not fit in dst_max. */
int slip_encode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max);

/* The first complete frame in `src` (leading ENDs skipped). Returns its
 * decoded length, or -1. */
int slip_decode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max);

/* The piece slip_encode() is made of: escapes `len` bytes of `src` into
 * dst[*out..], always leaving one byte free for a closing SLIP_END. Returns 0,
 * or -1 (and *out unchanged past the last whole byte) if they do not fit. A
 * frame built from several pieces is: dst[0] = SLIP_END, *out = 1,
 * slip_append() per piece, then dst[(*out)++] = SLIP_END. */
int slip_append(uint8_t *dst, uint32_t *out, uint32_t dst_max, const uint8_t *src, uint32_t len);

/* One received byte into a SLIP frame accumulator. Returns 1 when `frame`
 * holds a complete frame of `*frame_len` bytes, 0 otherwise; `escaping`
 * carries the escape state across calls. The caller sets *frame_len = 0
 * after taking a frame.
 *
 * A frame longer than `frame_cap` is discarded whole: nothing is returned for
 * it, and the accumulator resynchronises on the next SLIP_END. While
 * discarding, *frame_len holds SLIP_DISCARDING. */
#define SLIP_DISCARDING 0xFFFFFFFFu
int slip_feed(uint8_t c, uint8_t *frame, uint32_t frame_cap,
              uint32_t *frame_len, bool *escaping);

#endif
