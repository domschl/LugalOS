#include "drivers/loopback_net.h"
#include "fs/9p.h"
#include "kernel/printk.h"
#include <string.h>

void loopback_net_init(void) {
    p9_init();
    printk("[Loopback 9P] In-Memory Transport Gateway Online.\n");
}

int loopback_send_9p(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max) {
    if (!req_buf || req_len < 7 || !resp_buf) return -1;
    return p9_server_process(req_buf, req_len, resp_buf, resp_max);
}

int loopback_9p_rpc(const char *write_payload, char *read_out_buf, uint32_t read_max) {
    static uint8_t tx_buf[1024];
    static uint8_t rx_buf[1024];

    // 1. Tversion
    p9_msg_t tv = { .type = P9_TVERSION, .tag = 1, .msize = 1024 };
    int tx_len = p9_serialize(&tv, tx_buf, sizeof(tx_buf));
    int rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    // 2. Tattach
    p9_msg_t ta = { .type = P9_TATTACH, .tag = 2, .fid = 1, .uname = "lugal", .aname = "sd0" };
    tx_len = p9_serialize(&ta, tx_buf, sizeof(tx_buf));
    rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    // 3. Twrite payload (if provided)
    if (write_payload && strlen(write_payload) > 0) {
        uint32_t plen = (uint32_t)strlen(write_payload);
        p9_msg_t tw = {
            .type = P9_TWRITE,
            .tag = 3,
            .fid = 1,
            .offset = 0,
            .count = plen,
            .data = (const uint8_t *)write_payload
        };
        tx_len = p9_serialize(&tw, tx_buf, sizeof(tx_buf));
        rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;
    }

    // 4. Tread payload
    p9_msg_t tr = {
        .type = P9_TREAD,
        .tag = 4,
        .fid = 1,
        .offset = 0,
        .count = (read_max > 0) ? (read_max - 1) : 0
    };
    tx_len = p9_serialize(&tr, tx_buf, sizeof(tx_buf));
    rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    p9_msg_t rr;
    if (p9_deserialize(rx_buf, (uint32_t)rx_len, &rr) < 0) return -1;

    if (rr.type == P9_RREAD && read_out_buf && read_max > 0) {
        uint32_t copy_cnt = rr.count;
        if (copy_cnt >= read_max) copy_cnt = read_max - 1;
        if (rr.data && copy_cnt > 0) {
            memcpy(read_out_buf, rr.data, copy_cnt);
        }
        read_out_buf[copy_cnt] = '\0';
        return (int)copy_cnt;
    }
    return -1;
}
