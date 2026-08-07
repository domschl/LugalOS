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

// Reads any absolute VFS path (e.g. "/sd0/system/init.lisp") over a full
// real 9P session -- Tattach "/", multi-component Twalk, Topen, Tread,
// Tclunk -- proving the server's fid table (A2) actually resolves arbitrary
// namespace paths through the VFS handle API, not just a fixed scratch
// file. Returns the byte count read, or -1.
int loopback_9p_cat(const char *path, char *out_buf, uint32_t out_max);

#endif // DRIVERS_LOOPBACK_NET_H
