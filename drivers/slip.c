#include "drivers/slip.h"

/* Moved from drivers/uart_net.c in 49.1:
 * LLP's serial binding needs SLIP and must build for the host harness without
 * a UART driver behind it. */

/* Incremental SLIP decode: one received byte at a time, straight into the
 * frame buffer.
 *
 * Both receive paths used to accumulate the *escaped* bytes in a raw[] buffer
 * and hand the whole thing to slip_decode() on SLIP_END. That was the right
 * first move -- it reused an already-tested decoder rather than writing a
 * second one -- but it cost a second buffer per context, sized at twice
 * P9_MAX_MSIZE because escaping can double a frame. Two contexts, 8 KB each,
 * on a board with 512 KB of SRAM.
 *
 * The state a SLIP decoder needs between bytes is one boolean, so the buffer
 * was never buying anything but the shape of the call. Returns 1 when a
 * complete frame is ready in `frame`, 0 otherwise.
 *
 * Two cases worth naming: a SLIP_END with nothing accumulated is a leading or
 * duplicate delimiter and is ignored, and an overflowing frame is dropped so
 * the link resynchronises on the next delimiter rather than emitting a
 * truncated message.
 *
 * 49.1 fixed the second one. The overflow used to reset *frame_len to 0 and
 * carry on accumulating, so the next SLIP_END returned the *tail* of the
 * oversized frame as a frame of its own -- the opposite of what this comment
 * said. 9P's parser rejected the tail; LLP's CRC would too, but a decoder that
 * hands up garbage it knows is garbage should not rely on the layer above.
 * Overflow now enters a discarding state (SLIP_DISCARDING in *frame_len),
 * left only at the next SLIP_END, which returns nothing. */
int slip_feed(uint8_t c, uint8_t *frame, uint32_t frame_cap,
              uint32_t *frame_len, bool *escaping) {
    if (c == SLIP_END) {
        *escaping = false;
        if (*frame_len == SLIP_DISCARDING) { *frame_len = 0; return 0; }
        if (*frame_len == 0) return 0;   /* leading/duplicate delimiter */
        return 1;
    }
    if (*frame_len == SLIP_DISCARDING) return 0;

    if (*escaping) {
        *escaping = false;
        if (c == SLIP_ESC_END) c = SLIP_END;
        else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
    } else if (c == SLIP_ESC) {
        *escaping = true;
        return 0;
    }

    if (*frame_len >= frame_cap) {
        *frame_len = SLIP_DISCARDING;    /* overflow: drop the whole frame */
        return 0;
    }
    frame[(*frame_len)++] = c;
    return 0;
}

int slip_append(uint8_t *dst, uint32_t *out, uint32_t dst_max, const uint8_t *src, uint32_t len) {
    uint32_t o = *out;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == SLIP_END || c == SLIP_ESC) {
            if (o + 2 >= dst_max) return -1;
            dst[o++] = SLIP_ESC;
            dst[o++] = (c == SLIP_END) ? SLIP_ESC_END : SLIP_ESC_ESC;
        } else {
            if (o + 1 >= dst_max) return -1;
            dst[o++] = c;
        }
        *out = o;
    }
    return 0;
}

int slip_encode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max) {
    if (!src || !dst || dst_max < 2) return -1;

    uint32_t out = 0;
    dst[out++] = SLIP_END;
    if (slip_append(dst, &out, dst_max, src, len) != 0) return -1;
    if (out + 1 > dst_max) return -1;
    dst[out++] = SLIP_END;
    return (int)out;
}

int slip_decode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max) {
    if (!src || !dst || len == 0) return -1;

    uint32_t out = 0;
    bool escaping = false;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == SLIP_END) {
            if (out > 0) break; // Frame completed
            continue;
        }

        if (escaping) {
            if (c == SLIP_ESC_END) c = SLIP_END;
            else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
            escaping = false;
        } else if (c == SLIP_ESC) {
            escaping = true;
            continue;
        }

        if (out >= dst_max) return -1;
        dst[out++] = c;
    }
    return (int)out;
}
