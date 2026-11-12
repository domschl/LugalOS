#include "net/http.h"
#include "net/dns.h"
#include "net/tcp.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/console.h"
#include "libc/include/string.h"
#if defined(CONFIG_TLS_CLIENT)
#include "net/tls.h"
#include "kernel/crypto.h"
#include "kernel/identity.h"
#include "kernel/palloc.h"
#endif

static bool has_ci_prefix(const char *s, const char *prefix) {
    for (size_t i = 0; prefix[i]; i++) {
        char c1 = s[i];
        if (c1 == '\0') return false;
        if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
        if (c1 != prefix[i]) return false;
    }
    return true;
}

bool http_parse_url_ex(const char *url, bool *is_https, char *host, size_t host_sz,
                       uint16_t *port, char *path, size_t path_sz) {
    if (!url || !host || host_sz == 0 || !port || !path || path_sz == 0) return false;

    bool https = false;
    size_t plen = 0;
    if (has_ci_prefix(url, "https://")) {
        https = true;
        plen = 8;
    } else if (has_ci_prefix(url, "http://")) {
        https = false;
        plen = 7;
    } else {
        return false;
    }
    if (is_https) *is_https = https;

    const char *p = url + plen;
    if (*p == '\0') return false;

    const char *colon = NULL;
    const char *slash = NULL;
    for (const char *s = p; *s; s++) {
        if (*s == ':' && !colon && !slash) colon = s;
        if (*s == '/' && !slash) { slash = s; break; }
    }

    size_t host_len = 0;
    if (colon) {
        host_len = (size_t)(colon - p);
    } else if (slash) {
        host_len = (size_t)(slash - p);
    } else {
        host_len = strlen(p);
    }

    if (host_len == 0 || host_len >= host_sz) return false;
    memcpy(host, p, host_len);
    host[host_len] = '\0';

    if (colon) {
        const char *port_start = colon + 1;
        size_t port_len = slash ? (size_t)(slash - port_start) : strlen(port_start);
        if (port_len == 0 || port_len > 5) return false;
        long val = 0;
        for (size_t i = 0; i < port_len; i++) {
            if (port_start[i] < '0' || port_start[i] > '9') return false;
            val = val * 10 + (port_start[i] - '0');
        }
        if (val <= 0 || val > 65535) return false;
        *port = (uint16_t)val;
    } else {
        *port = https ? HTTPS_DEFAULT_PORT : HTTP_DEFAULT_PORT;
    }

    if (slash) {
        size_t path_len = strlen(slash);
        if (path_len >= path_sz) return false;
        memcpy(path, slash, path_len + 1);
    } else {
        if (path_sz < 2) return false;
        path[0] = '/';
        path[1] = '\0';
    }

    return true;
}

bool http_parse_url(const char *url, char *host, size_t host_sz,
                    uint16_t *port, char *path, size_t path_sz) {
    return http_parse_url_ex(url, NULL, host, host_sz, port, path, path_sz);
}

#if defined(CONFIG_TLS_CLIENT)
typedef struct {
    tls_t   tls;
    uint8_t rx[TLS_CLIENT_RX_MAX];
    uint8_t tx[TLS_TX_CAP];
    uint8_t ca[NODE_TLS_CA_LEN];
} http_tls_conn_t;

#define HTTP_TLS_PAGES ((sizeof(http_tls_conn_t) + 4095u) / 4096u)

static int http_tls_recv(void *ctx, uint8_t *buf, uint32_t len) {
    tcp_stream_t *s = (tcp_stream_t *)ctx;
    int n = tcp_stream_read(s, buf, len);
    if (n > 0) return n;
    if (n < 0 || tcp_stream_peer_closed(s)) return -1;
    return 0;
}

static int http_tls_send(void *ctx, const uint8_t *buf, uint32_t len) {
    return tcp_stream_write((tcp_stream_t *)ctx, buf, len);
}
#endif

typedef struct {
    tcp_stream_t stream;
    bool         tls_on;
#if defined(CONFIG_TLS_CLIENT)
    http_tls_conn_t *tls_mem;
#endif
} http_conn_t;

static int conn_write(http_conn_t *c, const uint8_t *buf, uint32_t len) {
#if defined(CONFIG_TLS_CLIENT)
    if (c->tls_on) {
        int n = tls_write(&c->tls_mem->tls, buf, len);
        return n < 0 ? -1 : n;
    }
#endif
    return tcp_stream_write(&c->stream, buf, len);
}

static int conn_read(http_conn_t *c, uint8_t *buf, uint32_t len) {
#if defined(CONFIG_TLS_CLIENT)
    if (c->tls_on) {
        tls_flush(&c->tls_mem->tls);
        int n = tls_read(&c->tls_mem->tls, buf, len);
        return n < 0 ? -1 : n;
    }
#endif
    return tcp_stream_read(&c->stream, buf, len);
}

static bool conn_peer_closed(http_conn_t *c) {
    return tcp_stream_peer_closed(&c->stream);
}

static void conn_close(http_conn_t *c) {
#if defined(CONFIG_TLS_CLIENT)
    if (c->tls_on && c->tls_mem) {
        tls_close(&c->tls_mem->tls);
        tls_wipe(&c->tls_mem->tls);
        c->tls_on = false;
    }
    if (c->tls_mem) {
        crypto_wipe_bytes(c->tls_mem, HTTP_TLS_PAGES * 4096u);
        palloc_free(c->tls_mem, HTTP_TLS_PAGES);
        c->tls_mem = NULL;
    }
#endif
    tcp_stream_close(&c->stream);
}

static int parse_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int http_request(const http_req_t *req, http_chunk_cb on_chunk, void *ctx,
                 char *err_out, size_t err_sz) {
    if (!req || !req->url) {
        if (err_out && err_sz > 0) strncpy(err_out, "Invalid request pointer", err_sz - 1);
        return -1;
    }

    char host[64];
    char path[128];
    uint16_t port = HTTP_DEFAULT_PORT;
    bool is_https = false;

    if (!http_parse_url_ex(req->url, &is_https, host, sizeof(host), &port, path, sizeof(path))) {
        if (err_out && err_sz > 0) strncpy(err_out, "Failed to parse HTTP URL", err_sz - 1);
        return -2;
    }

#if !defined(CONFIG_TLS_CLIENT)
    if (is_https) {
        if (err_out && err_sz > 0) strncpy(err_out, "HTTPS not enabled on this target", err_sz - 1);
        return -11;
    }
#endif

    uint8_t ip[IPV4_LEN];
    if (!dns_resolve(host, ip)) {
        if (err_out && err_sz > 0) {
            ksnprintf(err_out, err_sz, "Could not resolve host '%s'", host);
        }
        return -3;
    }

    http_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    if (tcp_stream_open(&conn.stream, ip, port) != 0) {
        if (err_out && err_sz > 0) strncpy(err_out, "Failed to open TCP stream", err_sz - 1);
        return -4;
    }

    uint32_t timeout = req->timeout_ms ? req->timeout_ms : 20000;
    uint64_t deadline = time_get_ms() + (timeout < 10000 ? 10000 : timeout);
    int ready = 0;
    while ((ready = tcp_stream_ready(&conn.stream)) == 0) {
        if (console_interrupt_requested()) {
            conn_close(&conn);
            if (err_out && err_sz > 0) strncpy(err_out, "Connection canceled by user", err_sz - 1);
            return -5;
        }
        if (time_get_ms() >= deadline) {
            conn_close(&conn);
            if (err_out && err_sz > 0) strncpy(err_out, "TCP connection timed out", err_sz - 1);
            return -6;
        }
        sched_yield();
    }
    if (ready < 0) {
        conn_close(&conn);
        if (err_out && err_sz > 0) strncpy(err_out, "Connection refused or reset", err_sz - 1);
        return -7;
    }

#if defined(CONFIG_TLS_CLIENT)
    if (is_https) {
        conn.tls_mem = (http_tls_conn_t *)palloc_pages_bulk(HTTP_TLS_PAGES);
        if (!conn.tls_mem) {
            conn_close(&conn);
            if (err_out && err_sz > 0) strncpy(err_out, "Out of memory for TLS session", err_sz - 1);
            return -11;
        }
        const uint8_t *ca_ptr = NULL;
        if (req->tls_ca_pub) {
            memcpy(conn.tls_mem->ca, req->tls_ca_pub, NODE_TLS_CA_LEN);
            ca_ptr = conn.tls_mem->ca;
        } else if (node_tls_ca(conn.tls_mem->ca)) {
            ca_ptr = conn.tls_mem->ca;
        } else if (!req->tls_insecure) {
            conn_close(&conn);
            if (err_out && err_sz > 0) strncpy(err_out, "No TLS CA installed (/dev/tlsca)", err_sz - 1);
            return -11;
        }
        if (!tls_client_init(&conn.tls_mem->tls, host, ca_ptr, http_tls_recv, http_tls_send,
                             &conn.stream, conn.tls_mem->rx, sizeof(conn.tls_mem->rx),
                             conn.tls_mem->tx)) {
            conn_close(&conn);
            if (err_out && err_sz > 0) strncpy(err_out, "TLS init failed", err_sz - 1);
            return -11;
        }
        if (!ca_ptr && req->tls_insecure) tls_client_set_insecure(&conn.tls_mem->tls, true);
        conn.tls_on = true;
        while (!tls_established(&conn.tls_mem->tls)) {
            uint8_t none[1];
            if (tls_read(&conn.tls_mem->tls, none, 0) < 0) {
                if (err_out && err_sz > 0) {
                    ksnprintf(err_out, err_sz, "TLS handshake failed (alert %u)",
                              conn.tls_mem->tls.alert);
                }
                conn_close(&conn);
                return -11;
            }
            if (console_interrupt_requested()) {
                conn_close(&conn);
                if (err_out && err_sz > 0) strncpy(err_out, "Connection canceled by user", err_sz - 1);
                return -5;
            }
            if (time_get_ms() >= deadline) {
                conn_close(&conn);
                if (err_out && err_sz > 0) strncpy(err_out, "TLS handshake timed out", err_sz - 1);
                return -6;
            }
            sched_yield();
        }
    }
#endif

    /* Format HTTP/1.1 request header */
    const char *method = req->method;
    if (!method) method = (req->body && req->body_len > 0) ? "POST" : "GET";

    char hdr_buf[512];
    uint16_t def_port = is_https ? HTTPS_DEFAULT_PORT : HTTP_DEFAULT_PORT;
    int hlen = (port == def_port)
        ? ksnprintf(hdr_buf, sizeof(hdr_buf),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: LugalOS/0.20\r\n"
                    "Accept: text/event-stream, application/json, */*\r\n"
                    "Connection: close\r\n",
                    method, path, host)
        : ksnprintf(hdr_buf, sizeof(hdr_buf),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s:%u\r\n"
                    "User-Agent: LugalOS/0.20\r\n"
                    "Accept: text/event-stream, application/json, */*\r\n"
                    "Connection: close\r\n",
                    method, path, host, (unsigned)port);

    if (req->content_type && hlen > 0 && (size_t)hlen < sizeof(hdr_buf) - 64) {
        hlen += ksnprintf(hdr_buf + hlen, sizeof(hdr_buf) - (size_t)hlen,
                          "Content-Type: %s\r\n", req->content_type);
    }
    if (req->auth_header && hlen > 0 && (size_t)hlen < sizeof(hdr_buf) - 128) {
        hlen += ksnprintf(hdr_buf + hlen, sizeof(hdr_buf) - (size_t)hlen,
                          "Authorization: %s\r\n", req->auth_header);
    }
    if (req->body && req->body_len > 0 && hlen > 0 && (size_t)hlen < sizeof(hdr_buf) - 32) {
        hlen += ksnprintf(hdr_buf + hlen, sizeof(hdr_buf) - (size_t)hlen,
                          "Content-Length: %u\r\n", (unsigned)req->body_len);
    }
    if (hlen > 0 && (size_t)hlen < sizeof(hdr_buf) - 4) {
        hlen += ksnprintf(hdr_buf + hlen, sizeof(hdr_buf) - (size_t)hlen, "\r\n");
    }

    /* Send request headers */
    size_t sent = 0;
    deadline = time_get_ms() + 5000;
    while (sent < (size_t)hlen) {
        int n = conn_write(&conn, (const uint8_t *)hdr_buf + sent, (uint32_t)((size_t)hlen - sent));
        if (n < 0) { conn_close(&conn); return -8; }
        if (n > 0) { sent += (size_t)n; continue; }
        if (time_get_ms() >= deadline || console_interrupt_requested()) {
            conn_close(&conn);
            return -9;
        }
        sched_yield();
    }

    /* Send request body */
    if (req->body && req->body_len > 0) {
        sent = 0;
        deadline = time_get_ms() + 10000;
        while (sent < (size_t)req->body_len) {
            int n = conn_write(&conn, (const uint8_t *)req->body + sent, (uint32_t)((size_t)req->body_len - sent));
            if (n < 0) { conn_close(&conn); return -8; }
            if (n > 0) { sent += (size_t)n; continue; }
            if (time_get_ms() >= deadline || console_interrupt_requested()) {
                conn_close(&conn);
                return -9;
            }
            sched_yield();
        }
    }

    /* Read response headers */
    char hdr_line[256];
    uint32_t hpos = 0;
    int status_code = 0;
    bool in_headers = true;
    bool chunked = false;
    deadline = time_get_ms() + timeout;

    while (in_headers && time_get_ms() < deadline) {
        if (console_interrupt_requested()) {
            conn_close(&conn);
            if (err_out && err_sz > 0) strncpy(err_out, "Interrupted by user", err_sz - 1);
            return -5;
        }

        uint8_t ch;
        int n = conn_read(&conn, &ch, 1);
        if (n < 0) break;
        if (n == 0) {
            if (conn_peer_closed(&conn)) break;
            sched_yield();
            continue;
        }

        if (ch == '\r') continue;
        if (ch == '\n') {
            hdr_line[hpos] = '\0';
            if (hpos == 0) {
                /* Empty line indicates end of headers */
                in_headers = false;
                break;
            }

            if (status_code == 0 && strncmp(hdr_line, "HTTP/", 5) == 0) {
                /* Parse "HTTP/1.x 200 OK" */
                const char *sp = strchr(hdr_line, ' ');
                if (sp) {
                    status_code = 0;
                    sp++;
                    while (*sp >= '0' && *sp <= '9') {
                        status_code = status_code * 10 + (*sp - '0');
                        sp++;
                    }
                }
            } else {
                /* Check headers */
                for (uint32_t i = 0; i < hpos; i++) {
                    if (hdr_line[i] >= 'A' && hdr_line[i] <= 'Z') {
                        hdr_line[i] = (char)(hdr_line[i] + 32);
                    }
                }
                if (strstr(hdr_line, "transfer-encoding:") && strstr(hdr_line, "chunked")) {
                    chunked = true;
                }
            }
            hpos = 0;
        } else if (hpos < sizeof(hdr_line) - 1) {
            hdr_line[hpos++] = (char)ch;
        }
    }

    if (in_headers || status_code == 0) {
        conn_close(&conn);
        if (err_out && err_sz > 0) strncpy(err_out, "Incomplete or missing HTTP response header", err_sz - 1);
        return -10;
    }

    /* Read response body */
    deadline = time_get_ms() + timeout;

    if (!chunked) {
        /* Plain stream / Connection: close */
        uint8_t buf[256];
        while (time_get_ms() < deadline) {
            if (console_interrupt_requested()) break;

            int n = conn_read(&conn, buf, sizeof(buf));
            if (n < 0) break;
            if (n > 0) {
                deadline = time_get_ms() + timeout; /* Reset timeout on progress */
                if (on_chunk) {
                    int rc = on_chunk(buf, (uint32_t)n, ctx);
                    if (rc != 0) break; /* Caller canceled */
                }
                continue;
            }
            if (conn_peer_closed(&conn)) break;
            sched_yield();
        }
    } else {
        /* Chunked transfer encoding */
        bool done = false;
        while (!done && time_get_ms() < deadline) {
            if (console_interrupt_requested()) break;

            /* 1. Read chunk size hex line */
            char hex_buf[16];
            uint32_t hlen_c = 0;
            bool got_line = false;
            while (time_get_ms() < deadline && !console_interrupt_requested()) {
                uint8_t c;
                int n = conn_read(&conn, &c, 1);
                if (n <= 0) {
                    if (n < 0 || conn_peer_closed(&conn)) break;
                    sched_yield();
                    continue;
                }
                if (c == '\r') continue;
                if (c == '\n') { got_line = true; break; }
                if (hlen_c < sizeof(hex_buf) - 1) hex_buf[hlen_c++] = (char)c;
            }
            if (!got_line || hlen_c == 0) break;
            hex_buf[hlen_c] = '\0';

            uint32_t chunk_sz = 0;
            for (uint32_t i = 0; i < hlen_c; i++) {
                int d = parse_hex_digit(hex_buf[i]);
                if (d < 0) break;
                chunk_sz = (chunk_sz << 4) | (uint32_t)d;
            }

            if (chunk_sz == 0) {
                done = true;
                break;
            }

            /* 2. Read chunk_sz bytes */
            uint32_t chunk_rem = chunk_sz;
            uint8_t buf[256];
            while (chunk_rem > 0 && time_get_ms() < deadline) {
                if (console_interrupt_requested()) break;
                uint32_t want = chunk_rem < sizeof(buf) ? chunk_rem : (uint32_t)sizeof(buf);
                int n = conn_read(&conn, buf, want);
                if (n <= 0) {
                    if (n < 0 || conn_peer_closed(&conn)) break;
                    sched_yield();
                    continue;
                }
                deadline = time_get_ms() + timeout;
                chunk_rem -= (uint32_t)n;
                if (on_chunk) {
                    int rc = on_chunk(buf, (uint32_t)n, ctx);
                    if (rc != 0) { done = true; break; }
                }
            }

            /* 3. Consume trailing \r\n after chunk data */
            int crlf_count = 0;
            while (crlf_count < 2 && time_get_ms() < deadline) {
                uint8_t c;
                int n = conn_read(&conn, &c, 1);
                if (n > 0) crlf_count++;
                else sched_yield();
            }
        }
    }

    conn_close(&conn);
    return status_code;
}
