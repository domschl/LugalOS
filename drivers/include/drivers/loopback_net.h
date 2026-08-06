#ifndef DRIVERS_LOOPBACK_NET_H
#define DRIVERS_LOOPBACK_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void loopback_net_init(void);

// Transmits a raw 9P packet through loopback transport and fetches server response
int loopback_send_9p(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max);

// High-level 9P VFS loopback RPC (Write string payload over 9P Twrite, then read back over 9P Tread)
int loopback_9p_rpc(const char *write_payload, char *read_out_buf, uint32_t read_max);

#endif // DRIVERS_LOOPBACK_NET_H
