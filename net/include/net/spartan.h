#ifndef NET_SPARTAN_H
#define NET_SPARTAN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SPARTAN_DEFAULT_PORT 300
#define SPARTAN_PUB_ROOT     "/sd0/pub"

typedef enum {
    SPARTAN_OK = 0,
    SPARTAN_REDIRECT = 1,
    SPARTAN_ERR_PARSE = -1,
    SPARTAN_ERR_DNS = -2,
    SPARTAN_ERR_CONNECT = -3,
    SPARTAN_ERR_TIMEOUT = -4,
    SPARTAN_ERR_INTERRUPTED = -5,
    SPARTAN_ERR_REFUSED = -6,
    SPARTAN_ERR_IO = -7,
    SPARTAN_ERR_HEADER = -8,
    SPARTAN_ERR_CLIENT = -9,
    SPARTAN_ERR_SERVER = -10,
    SPARTAN_ERR_FILE = -11,
    SPARTAN_ERR_PROTOCOL = -12,
} spartan_status_t;

/* --- Spartan Client --- */

/* Parses "spartan://<host>[:<port>][/<path>]".
 * Port defaults to 300 if omitted.
 * Path defaults to "/" if omitted. */
bool spartan_parse_url(const char *url, char *host, size_t host_sz,
                       uint16_t *port, char *path, size_t path_sz);

/* Fetches content from a Spartan server and writes the response body to local_dest
 * (e.g. "/ram0/spartan.gmi").
 * Returns SPARTAN_OK on success (status 2).
 * Returns SPARTAN_REDIRECT on status 3, with redirect destination in redirect_out.
 * Returns negative error code on client/server error, with description in err_out. */
int spartan_fetch(const char *url, const char *local_dest,
                  char *redirect_out, size_t redir_sz,
                  char *err_out, size_t err_sz);

/* --- Spartan Server (spartand) --- */

/* Validates and sanitizes a Spartan request path.
 * Enforces strict chroot jail to SPARTAN_PUB_ROOT ("/sd0/pub").
 * Rejects traversal attempts (".."), backslashes, and nulls.
 * Maps "/" to "/sd0/pub/index.gmi".
 * Returns true if valid and writes resolved safe path to out_safe_path. */
bool spartan_sandbox_validate_path(const char *req_path, char *out_safe_path, size_t out_sz);

/* Server lifecycle controls */
int  spartan_server_start(uint16_t port);
void spartan_server_stop(void);
bool spartan_server_running(uint16_t *port_out);
uint16_t spartan_server_port(void);

/* Status counters */
void spartan_server_stats(uint32_t *requests, uint32_t *served, uint32_t *errors);

/* Internal request processor called by tcp_service */
int spartan_process_request(const uint8_t *rx, uint32_t rx_len, uint32_t *consumed,
                           uint8_t *tx, uint32_t tx_max, uint32_t *tx_len,
                           bool *close_after_tx);

#endif /* NET_SPARTAN_H */
