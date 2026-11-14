#include "drivers/hal_chan.h"
#include "drivers/i2c_bus.h"
#include "kernel/chan.h"
#include "kernel/balloc.h"
#include <stddef.h>

static int hal_chan_handler(void *ctx, const uint8_t *req, uint32_t req_len,
                            uint8_t *resp, uint32_t resp_max) {
    (void)ctx;
    if (req_len == 0 || resp_max == 0) return -1;
    uint8_t op = req[0];

    if (op == 'N') {
        resp[0] = i2c_bus_count();
        return 1;
    }

    if (op == 'I' && req_len >= 5) {
        uint8_t bus = req[1];
        uint8_t addr = req[2];
        uint8_t wlen = req[3];
        uint8_t rlen = req[4];
        if (wlen > I2C_XFER_WMAX || rlen > I2C_XFER_RMAX) return -1;
        if (5u + (uint32_t)wlen > req_len || 1u + (uint32_t)rlen > resp_max) return -1;
        const uint8_t *wbuf = wlen ? (req + 5) : NULL;
        uint8_t *rbuf = rlen ? (resp + 1) : NULL;
        bool ok = i2c_xfer_bus(bus, addr, wbuf, wlen, rbuf, rlen);
        resp[0] = ok ? 1u : 0u;
        return ok ? (int)(1u + rlen) : 1;
    }

    if (op == 'S' && req_len >= 2) {
        uint8_t bus = req[1];
        uint32_t pos = 1u;
        resp[0] = 1u;
        for (uint8_t addr = 0x08u; addr <= 0x77u; addr++) {
            if (pos >= resp_max) break;
            if (i2c_xfer_bus(bus, addr, NULL, 0, NULL, 0)) {
                resp[pos++] = addr;
            }
        }
        return (int)pos;
    }

    if (op == 'P') {
        resp[0] = 0u;
        return 1;
    }

    return -1;
}

void hal_chan_init(void) {
    uint8_t *req = (uint8_t *)balloc_alloc(64u);
    uint8_t *resp = (uint8_t *)balloc_alloc(128u);
    if (!req || !resp) return;
    (void)chan_register("hal", hal_chan_handler, NULL, req, 64u, resp, 128u);
}
