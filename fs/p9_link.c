#include "fs/p9_link.h"
#include "fs/9p.h"
#include "kernel/printk.h"

static p9_link_t *g_background_link = NULL;

int p9_link_service(p9_link_t *link) {
    if (!link || !link->poll || !link->recv_frame || !link->send_frame) return -1;

    int ready = link->poll(link);
    if (ready <= 0) return ready; // 0 = nothing pending, -1 = transport error

    static uint8_t req_buf[P9_MAX_MSIZE];
    static uint8_t resp_buf[P9_MAX_MSIZE];

    int req_len = link->recv_frame(link, req_buf, sizeof(req_buf));
    if (req_len < 7) return -1;

    int resp_len = p9_server_process(req_buf, (uint32_t)req_len, resp_buf, sizeof(resp_buf));
    if (resp_len < 7) return -1;

    if (link->send_frame(link, resp_buf, (uint32_t)resp_len) < 0) return -1;
    return 1;
}

void p9_link_register_background(p9_link_t *link) {
    g_background_link = link;
    if (link) {
        printk("[9P Link] '%s' registered as background transport.\n", link->name ? link->name : "?");
    }
}

void p9_link_background_poll(void) {
    if (!g_background_link) return;
    p9_link_service(g_background_link);
}
