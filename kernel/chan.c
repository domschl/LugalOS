#include "kernel/chan.h"
#include "kernel/printk.h"
#include <string.h>

/* See kernel/include/kernel/chan.h for the rationale, especially why the
 * two copies below are deliberate rather than wasteful. */

struct chan_endpoint {
    const char     *name;
    chan_handler_fn handler;
    void           *ctx;
    uint8_t        *req_buf;
    uint32_t        req_cap;
    uint8_t        *resp_buf;
    uint32_t        resp_cap;
    bool            in_use;
    bool            busy;
};

static chan_endpoint_t g_endpoints[CHAN_MAX_ENDPOINTS];
static uint32_t        g_num_endpoints;

int chan_register(const char *name, chan_handler_fn handler, void *ctx,
                  uint8_t *req_buf, uint32_t req_cap,
                  uint8_t *resp_buf, uint32_t resp_cap) {
    if (!name || !handler || !req_buf || !resp_buf || req_cap == 0 || resp_cap == 0) {
        return -1;
    }
    if (chan_lookup(name)) {
        printk("[Chan] Duplicate endpoint name '%s' ignored\n", name);
        return -1;
    }
    if (g_num_endpoints >= CHAN_MAX_ENDPOINTS) {
        printk("[Chan] Endpoint table full, dropping '%s'\n", name);
        return -1;
    }

    chan_endpoint_t *ep = &g_endpoints[g_num_endpoints++];
    ep->name     = name;
    ep->handler  = handler;
    ep->ctx      = ctx;
    ep->req_buf  = req_buf;
    ep->req_cap  = req_cap;
    ep->resp_buf = resp_buf;
    ep->resp_cap = resp_cap;
    ep->in_use   = true;
    ep->busy     = false;
    return 0;
}

chan_endpoint_t *chan_lookup(const char *name) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_num_endpoints; i++) {
        if (g_endpoints[i].in_use && strcmp(g_endpoints[i].name, name) == 0) {
            return &g_endpoints[i];
        }
    }
    return NULL;
}

int chan_call(chan_endpoint_t *ep, const uint8_t *req, uint32_t req_len,
              uint8_t *resp, uint32_t resp_max) {
    if (!ep || !ep->in_use || !req || req_len == 0) return -1;
    if (req_len > ep->req_cap) return -1;

    /* Re-entrancy: the endpoint's buffers are single-slot, so an inner call
     * would overwrite the outer call's request mid-flight. See the header --
     * a recursive local 9P mount is the concrete way to reach this. */
    if (ep->busy) return -1;
    ep->busy = true;

    /* Copy IN. The handler must never see the caller's pointer -- that is
     * Rule 1, and it is what keeps this identical to the MMU case (where the
     * caller's address is meaningless here) and to the remote case (where it
     * is on another machine entirely). */
    memcpy(ep->req_buf, req, req_len);

    int resp_len = ep->handler(ep->ctx, ep->req_buf, req_len,
                               ep->resp_buf, ep->resp_cap);

    ep->busy = false;

    if (resp_len < 0) return -1;
    if ((uint32_t)resp_len > ep->resp_cap) return -1; /* handler overran its contract */

    /* Copy OUT, bounded by what the caller can actually accept. A truncated
     * response is a failure, not a short read: every protocol carried over a
     * channel so far is message-oriented (9P frames), where half a message is
     * not a partial success. */
    if ((uint32_t)resp_len > resp_max) return -1;
    if (resp && resp_len > 0) memcpy(resp, ep->resp_buf, (uint32_t)resp_len);
    return resp_len;
}

bool chan_info(uint32_t index, const char **name_out, bool *busy_out) {
    if (index >= g_num_endpoints || !g_endpoints[index].in_use) return false;
    if (name_out) *name_out = g_endpoints[index].name;
    if (busy_out) *busy_out = g_endpoints[index].busy;
    return true;
}
