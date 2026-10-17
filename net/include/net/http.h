#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HTTP_DEFAULT_PORT 80

typedef struct http_req {
    const char *url;          /* "http://<host>[:<port>][/<path>]" */
    const char *method;       /* "GET", "POST", etc. NULL defaults to GET (or POST if body!=NULL) */
    const char *content_type; /* e.g. "application/json", NULL for none */
    const char *auth_header;  /* optional "Authorization" header value (e.g. "Bearer ...") */
    const char *body;         /* optional request body payload */
    uint32_t    body_len;     /* payload length in bytes */
    uint32_t    timeout_ms;   /* request timeout in milliseconds (0 for 20000 ms default) */
} http_req_t;

/* Callback invoked when response body chunks arrive.
 * Return 0 to continue, non-zero to cancel download early. */
typedef int (*http_chunk_cb)(const uint8_t *data, uint32_t len, void *ctx);

/* Parses an HTTP URL into its constituent host, port and path components.
 * Returns true on success, false if format is invalid. */
bool http_parse_url(const char *url, char *host, size_t host_sz,
                    uint16_t *port, char *path, size_t path_sz);

/* Performs an HTTP/1.1 request over a non-blocking TCP stream.
 * Response body bytes are streamed to `on_chunk`.
 * Returns HTTP status code (e.g. 200, 404), or negative error code on transport failure. */
int http_request(const http_req_t *req, http_chunk_cb on_chunk, void *ctx,
                 char *err_out, size_t err_sz);

#endif /* NET_HTTP_H */
