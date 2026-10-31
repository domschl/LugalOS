/* The admin web server (net/include/net/httpd.h). One task, cooperative: each pass gives every
 * session a little work -- drain what it owes the peer, read what the peer sent, serve one 9P
 * request -- and nothing blocks, so a slow browser does not hold up another. */

#include "lugalos_config.h"
#include "net/httpd.h"
#include "net/tcp.h"
#include "net/websocket.h"
#include "fs/vfs.h"
#include "fs/9p.h"
#include "fs/p9_link.h"
#include "kernel/boardinfo.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/setup.h"
#include "kernel/time.h"
#if defined(CONFIG_TLS_SERVER)
#include "net/tls.h"
#include "kernel/crypto.h"
#include "kernel/identity.h"
#include "kernel/tls_cert.h"
#endif
#include <stddef.h>
#include <string.h>

#if defined(CONFIG_BOARD_RP2350)
#define HTTPD_MAX_SESSIONS 1u       /* SRAM: a session is two message buffers */
#define HTTPD_STACK_PAGES  4u       /* 6.6 KB high-water on a read (msize 2048) */
#elif defined(CONFIG_BOARD_ESP32C6)
#define HTTPD_MAX_SESSIONS 1u       /* the whole heap is 63 pages, and the radio, MQTT and sensors share it */
#define HTTPD_STACK_PAGES  4u       /* 7.1 KB high-water measured on a C6-Zero at msize 4096 */
#else
#define HTTPD_MAX_SESSIONS 2u
#define HTTPD_STACK_PAGES  6u       /* 11 KB high-water on a read (msize 4096) */
#endif

#define HTTPD_REQ_MAX       1536u   /* request line + headers; the message buffer is at least this */
#define HTTPD_REQ_TIMEOUT_MS  10000u
#define HTTPD_PING_MS         15000u
#define HTTPD_DEAD_MS         45000u
#define HTTPD_OBUF  (P9_MAX_MSIZE + WS_HDR_MAX)
#define HTTPD_PAGE_A  "/flash0/www/admin.html"
#define HTTPD_PAGE_B  "/sd0/www/admin.html"

typedef enum { H_FREE, H_REQUEST, H_FILE, H_WS, H_CLOSING } hstate_t;

typedef struct {
    hstate_t    st;
    tcp_stream_t s;
    uint8_t    *ibuf;                 /* the request, then an assembled WebSocket message */
    uint32_t    ilen;
    uint8_t    *obuf;                 /* what is owed to the peer, drained as the stream takes it */
    uint32_t    olen, osent;
    uint64_t    started_ms, last_rx_ms, ping_sent_ms;
    bool        ping_out;
    bool        close_after_drain;
    int         fd;                   /* H_FILE */
    const char *mem;                  /* H_FILE from memory (the setup page, in the image), else NULL */
    uint32_t    file_left, file_off;
    /* H_WS: one frame at a time. */
    uint8_t     hbuf[WS_HDR_MAX];
    uint32_t    hlen;
    ws_hdr_t    hdr;
    bool        have_hdr;
    uint32_t    got;                  /* payload bytes of the current frame read so far */
    uint8_t     ctl[128];             /* a control frame's payload */
    uint32_t    msg_len;              /* bytes of the message being assembled / ready in ibuf */
    bool        msg_ready;
    uint8_t     peer[4];
    p9_link_t   link;
    p9_prot_t   prot;                 /* the protected session's state (fs/p9_protect.h) */
#if defined(CONFIG_TLS_SERVER)
    bool        tls_on;               /* accepted on the HTTPS port: every byte goes through `tls` */
    tls_t       tls;
#endif
} hsess_t;

/* single task: httpd (everything below, except the flags it reads from the console). */
static hsess_t  *g_sess;              /* HTTPD_MAX_SESSIONS of them, at the start of the pool */
static uint8_t  *g_pool;              /* the sessions, then each one's ibuf and obuf: one palloc run, taken by httpd_start() */
static uint32_t  g_pool_pages;
static uint16_t  g_port;
static int       g_pid = -1;
static uint32_t  g_requests, g_pages, g_ws_total, g_not_found, g_bad;
/* accepted: written by the console task, read by the httpd task; a stale read costs one pass. */
static volatile bool g_run, g_want_stop, g_write_ok;

#if defined(CONFIG_TLS_SERVER)
#define HTTPS_PORT 443
#define TLS_SESS_BYTES ((TLS_RECORD_MAX + TLS_TX_CAP + 15u) & ~15u)
/* single task: httpd. The key (32 bytes), the certificate (NODE_TLS_CERT_MAX), then each session's
 * record buffers, in bulk pages (PSRAM on a P4): taken when HTTPS comes up, wiped and returned when it
 * goes. */
static uint8_t  *g_tls_mem;
static uint32_t  g_tls_pages, g_tls_cert_len, g_tls_sessions;
static bool      g_tls_on;
#endif
/* accepted: set by whoever changed the key or the certificate (kernel/tls_cert.c), read by httpd, which
 * reloads them when no HTTPS session is open. */
static volatile bool g_tls_dirty;

void httpd_tls_changed(void) { g_tls_dirty = true; }

void httpd_set_write(bool allow) { g_write_ok = allow; }
bool httpd_write_allowed(void) { return g_write_ok; }

bool httpd_running(uint16_t *port_out) {
    if (port_out) *port_out = g_port;
    return g_run;
}

/* --- the session's half of the 9P link ---------------------------------------------- */

static hsess_t *link_sess(p9_link_t *l) { return (hsess_t *)l->ctx; }

/* Ready only when nothing is owed to the peer: a reply always has somewhere to go, so
 * send_frame() never has to wait. */
static int ws_link_poll(p9_link_t *l) {
    hsess_t *h = link_sess(l);
    return h->st == H_WS && h->msg_ready && h->olen == 0 ? 1 : 0;
}

static p9_prot_t *ws_link_prot(p9_link_t *l) { return &link_sess(l)->prot; }

static int ws_link_recv(p9_link_t *l, uint8_t *buf, uint32_t max) {
    hsess_t *h = link_sess(l);
    if (!h->msg_ready || h->msg_len > max) return -1;
    memcpy(buf, h->ibuf, h->msg_len);
    int n = (int)h->msg_len;
    h->msg_ready = false;
    h->msg_len = 0;
    return n;
}

static int ws_link_send(p9_link_t *l, const uint8_t *buf, uint32_t len) {
    hsess_t *h = link_sess(l);
    if (h->olen != 0 || len > P9_MAX_MSIZE) return -1;
    uint32_t n = ws_build_header(h->obuf, WS_OP_BINARY, len, NULL);
    memcpy(h->obuf + n, buf, len);
    h->olen = n + len;
    h->osent = 0;
    return (int)len;
}

/* --- the bytes of a session: TCP, or TLS over it ------------------------------------- */

#if defined(CONFIG_TLS_SERVER)
static int tls_tcp_recv(void *io, uint8_t *buf, uint32_t len) {
    tcp_stream_t *st = (tcp_stream_t *)io;
    int n = tcp_stream_read(st, buf, len);
    return n == 0 && tcp_stream_peer_closed(st) ? -1 : n;
}
static int tls_tcp_send(void *io, const uint8_t *buf, uint32_t len) {
    return tcp_stream_write((tcp_stream_t *)io, buf, len);
}
#endif

/* Bytes from the peer: 0 when none are ready, negative when the connection is over. */
static int sread(hsess_t *h, uint8_t *buf, uint32_t len) {
#if defined(CONFIG_TLS_SERVER)
    if (h->tls_on) { int r = tls_read(&h->tls, buf, len); return r < 0 ? -1 : r; }
#endif
    return tcp_stream_read(&h->s, buf, len);
}

/* The peer closed (with no bytes left). Over TLS the end comes out of sread() instead. */
static bool speer_closed(hsess_t *h) {
#if defined(CONFIG_TLS_SERVER)
    if (h->tls_on) return false;
#endif
    return tcp_stream_peer_closed(&h->s);
}

static int swrite(hsess_t *h, const uint8_t *buf, uint32_t len) {
#if defined(CONFIG_TLS_SERVER)
    if (h->tls_on) { int r = tls_write(&h->tls, buf, len); return r < 0 ? -1 : r; }
#endif
    return tcp_stream_write(&h->s, buf, len);
}

/* --- sessions ----------------------------------------------------------------------- */

static void sess_close(hsess_t *h) {
    if (h->st == H_WS) p9_link_closed(&h->link);
    if (h->fd >= 0) vfs_close(h->fd);
#if defined(CONFIG_TLS_SERVER)
    if (h->tls_on) { tls_close(&h->tls); tls_wipe(&h->tls); }
#endif
    tcp_stream_close(&h->s);
    uint8_t *ib = h->ibuf, *ob = h->obuf;
    memset(h, 0, sizeof *h);
    h->ibuf = ib;
    h->obuf = ob;
    h->fd = -1;
    h->st = H_FREE;
}

/* Queues a whole small reply (headers and body) for the peer, then closes after it drains. */
static void reply_and_close(hsess_t *h, const char *status, const char *ctype, const char *body) {
    uint32_t blen = (uint32_t)strlen(body);
    int n = ksnprintf((char *)h->obuf, HTTPD_OBUF,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n%s", status, ctype, (unsigned)blen, body);
    h->olen = n > 0 ? (uint32_t)n : 0;
    h->osent = 0;
    h->close_after_drain = true;
    h->st = H_CLOSING;
}

/* A captive portal's answer to every request that is not for the setup page: phones and laptops probe a
 * known address on joining and open a sign-in sheet when the answer is not the one they expect. */
static void reply_redirect(hsess_t *h, const char *location) {
    int n = ksnprintf((char *)h->obuf, HTTPD_OBUF,
                      "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n"
                      "Cache-Control: no-store\r\n\r\n", location);
    h->olen = n > 0 ? (uint32_t)n : 0;
    h->osent = 0;
    h->close_after_drain = true;
    h->st = H_CLOSING;
}

/* Case-insensitive search of a header's value in the request block; copies it, trimmed. */
static bool header_value(const char *req, const char *name, char *out, uint32_t cap) {
    uint32_t nl = (uint32_t)strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && *p; p = strstr(p + 2, "\r\n")) {
        const char *line = p + 2;
        if (line[0] == '\r') return false;           /* the blank line: end of headers */
        uint32_t i = 0;
        while (i < nl && line[i] && ((line[i] | 0x20) == (name[i] | 0x20))) i++;
        if (i != nl || line[nl] != ':') continue;
        const char *v = line + nl + 1;
        while (*v == ' ' || *v == '\t') v++;
        uint32_t k = 0;
        while (v[k] && v[k] != '\r' && k < cap - 1) { out[k] = v[k]; k++; }
        while (k && (out[k - 1] == ' ' || out[k - 1] == '\t')) k--;
        out[k] = 0;
        return true;
    }
    return false;
}

static bool token_in(const char *list, const char *tok) {
    uint32_t tl = (uint32_t)strlen(tok);
    for (const char *p = list; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        uint32_t i = 0;
        while (i < tl && p[i] && ((p[i] | 0x20) == (tok[i] | 0x20))) i++;
        if (i == tl && (p[i] == 0 || p[i] == ',' || p[i] == ' ' || p[i] == ';')) return true;
        while (*p && *p != ',') p++;
    }
    return false;
}

static void start_ws(hsess_t *h, const char *key, bool want_9p) {
    char acc[WS_ACCEPT_LEN + 1];
    if (!ws_accept_key(key, acc)) { reply_and_close(h, "400 Bad Request", "text/plain", "bad key\n"); return; }
    int n = ksnprintf((char *)h->obuf, HTTPD_OBUF,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n%s\r\n", acc, want_9p ? "Sec-WebSocket-Protocol: 9p\r\n" : "");
    h->olen = (uint32_t)n;
    h->osent = 0;
    h->ilen = 0;
    h->hlen = 0;
    h->have_hdr = false;
    h->msg_len = 0;
    h->msg_ready = false;
    h->last_rx_ms = h->ping_sent_ms = time_get_ms();
    h->link.name = "httpws";
    h->link.poll = ws_link_poll;
    h->link.recv_frame = ws_link_recv;
    h->link.send_frame = ws_link_send;
    h->link.ctx = h;
    h->link.auth_required = true;
    /* Plain HTTP: a client that offers the protected-session extension (9P2000.lp, after Tauth) may
     * write; one that does not is read-only unless the console allowed plain writes. */
    h->link.prot_get = ws_link_prot;
    h->link.setup_net = setup_net_contains(h->peer);
    h->link.plain_writes_ok = g_write_ok;
#if defined(CONFIG_TLS_SERVER)
    if (h->tls_on) {
        /* TLS protects the bytes: not a plaintext link, so an admin peer may write, secrets included. */
        h->link.prot_get = NULL;
        h->link.plain_writes_ok = false;
    } else if (g_tls_on && !h->link.setup_net) {
        h->link.read_only = !g_write_ok;            /* HTTPS is up: plain HTTP only reads */
    }
#endif
    h->st = H_WS;
    g_ws_total++;
}

/* The admin page in the image, gzip-compressed (tools/embed_page.py --gzip): it is updated with the
 * firmware, so the page and the node it talks to always match. A file on /flash0 or /sd0 is not:
 * an update replaces the image and leaves the file as it was. */
extern const char g_admin_page_gz[];
extern const unsigned g_admin_page_gz_len;

static void start_embedded_admin(hsess_t *h) {
    int n = ksnprintf((char *)h->obuf, HTTPD_OBUF,
                      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\n"
                      "Content-Length: %u\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
                      g_admin_page_gz_len);
    h->olen = (uint32_t)n;
    h->osent = 0;
    h->mem = g_admin_page_gz;
    h->file_left = g_admin_page_gz_len;
    h->file_off = 0;
    h->st = H_FILE;
    g_pages++;
}

/* The page for a client that takes gzip is the image's own; one that does not (a script) gets a copy
 * from a filesystem if there is one. */
static void start_page(hsess_t *h, bool gzip_ok) {
    if (gzip_ok) { start_embedded_admin(h); return; }
    h->fd = vfs_open(HTTPD_PAGE_A, VFS_O_READ);
    if (h->fd < 0) h->fd = vfs_open(HTTPD_PAGE_B, VFS_O_READ);
    vfs_stat_t st;
    if (h->fd < 0 || vfs_fstat(h->fd, &st) != 0) {
        if (h->fd >= 0) { vfs_close(h->fd); h->fd = -1; }
        reply_and_close(h, "406 Not Acceptable", "text/plain", "this node keeps its page gzip-compressed\n");
        return;
    }
    int n = ksnprintf((char *)h->obuf, HTTPD_OBUF,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                     "Connection: close\r\nCache-Control: no-cache\r\n\r\n", (unsigned)st.size);
    h->olen = (uint32_t)n;
    h->osent = 0;
    h->file_left = st.size;
    h->file_off = 0;
    h->st = H_FILE;
    g_pages++;
}

#if defined(CONFIG_NETWORK_CLAIM)
/* The setup page is part of the firmware image (tools/embed_page.py), not a file: an unclaimed node serves it
 * whatever its filesystems hold. */
extern const char g_setup_page[];
extern const unsigned g_setup_page_len;

static void start_setup_page(hsess_t *h) {
    int n = ksnprintf((char *)h->obuf, HTTPD_OBUF,
                      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                      "Connection: close\r\nCache-Control: no-store\r\n\r\n", g_setup_page_len);
    h->olen = (uint32_t)n;
    h->osent = 0;
    h->mem = g_setup_page;
    h->file_left = g_setup_page_len;
    h->file_off = 0;
    h->st = H_FILE;
    g_pages++;
}
#endif

/* A complete request is in ibuf (NUL-terminated at ilen). */
static void handle_request(hsess_t *h) {
    char *req = (char *)h->ibuf;
    g_requests++;
    char path[64];
    if (strncmp(req, "GET ", 4) != 0) { reply_and_close(h, "405 Method Not Allowed", "text/plain", "GET only\n"); g_bad++; return; }
    uint32_t i = 0;
    const char *p = req + 4;
    while (p[i] && p[i] != ' ' && p[i] != '?' && i < sizeof path - 1) { path[i] = p[i]; i++; }
    path[i] = 0;

    char v[96];
    if (setup_net_contains(h->peer) && strcmp(path, "/9p") != 0 && strcmp(path, "/board") != 0) {
        /* On the setup network the only page is the setup page, at the node's own address; everything
         * else -- a probe for a known host, a favicon, a page the user typed -- goes there. */
        char host[64] = "";
        (void)header_value(req, "Host", host, sizeof host);
        bool ours = strcmp(host, SETUP_AP_IP_STR) == 0;
        if (ours && strcmp(path, "/setup-done") == 0) {
            setup_ap_release();
            reply_and_close(h, "200 OK", "text/plain", "ok\n");
            return;
        }
        if (ours && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0 || strcmp(path, "/setup.html") == 0)) {
#if defined(CONFIG_NETWORK_CLAIM)
            start_setup_page(h);
#else
            reply_redirect(h, "http://" SETUP_AP_IP_STR "/");
#endif
        } else {
            reply_redirect(h, "http://" SETUP_AP_IP_STR "/");
        }
        return;
    }
    if (strcmp(path, "/9p") == 0) {
        char key[80], proto[64] = "";
        bool up = header_value(req, "Upgrade", v, sizeof v) && token_in(v, "websocket");
        bool conn = header_value(req, "Connection", v, sizeof v) && token_in(v, "upgrade");
        bool ver = header_value(req, "Sec-WebSocket-Version", v, sizeof v) && strcmp(v, "13") == 0;
        if (!up || !conn || !ver || !header_value(req, "Sec-WebSocket-Key", key, sizeof key)) {
            reply_and_close(h, "400 Bad Request", "text/plain", "a WebSocket upgrade is expected here\n");
            g_bad++;
            return;
        }
        (void)header_value(req, "Sec-WebSocket-Protocol", proto, sizeof proto);
        start_ws(h, key, token_in(proto, "9p"));
        return;
    }
    if (strcmp(path, "/board") == 0) {          /* the same public facts as /proc/board */
        char text[320];
        board_info_text(text, sizeof text);
        reply_and_close(h, "200 OK", "text/plain; charset=utf-8", text);
        return;
    }
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0 || strcmp(path, "/admin.html") == 0) {
#if defined(CONFIG_TLS_SERVER)
        if (!h->tls_on && g_tls_on) {               /* the page over HTTPS, where it may write */
            char host[64] = "", loc[96];
            (void)header_value(req, "Host", host, sizeof host);
            for (char *c = host; *c; c++) if (*c == ':') { *c = 0; break; }
            if (!host[0]) ksnprintf(host, sizeof host, "%s.local", node_name());
            ksnprintf(loc, sizeof loc, "https://%s/", host);
            reply_redirect(h, loc);
            return;
        }
#endif
        bool gz = header_value(req, "Accept-Encoding", v, sizeof v) && token_in(v, "gzip");
        start_page(h, gz);
        return;
    }
    g_not_found++;
    reply_and_close(h, "404 Not Found", "text/plain", "not found\n");
}

/* Hands the owed bytes to the stream; true if everything went. */
static bool drain(hsess_t *h) {
    while (h->osent < h->olen) {
        int n = swrite(h, h->obuf + h->osent, h->olen - h->osent);
        if (n < 0) { h->close_after_drain = true; h->olen = h->osent; return true; }
        if (n == 0) return false;
        h->osent += (uint32_t)n;
    }
    h->olen = h->osent = 0;
#if defined(CONFIG_TLS_SERVER)
    if (h->tls_on && !tls_flush(&h->tls)) return false;   /* the last record is still going out */
#endif
    return true;
}

static void queue_control(hsess_t *h, uint8_t op, const uint8_t *payload, uint32_t len) {
    if (h->olen != 0) return;                       /* one thing owed at a time; a ping can wait */
    uint32_t n = ws_build_header(h->obuf, op, len, NULL);
    if (len) memcpy(h->obuf + n, payload, len);
    h->olen = n + len;
    h->osent = 0;
}

static void ws_close_now(hsess_t *h, uint16_t code) {
    uint8_t p[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    h->olen = h->osent = 0;
    queue_control(h, WS_OP_CLOSE, p, 2);
    h->close_after_drain = true;
    h->msg_ready = false;
    h->st = H_CLOSING;
}

/* One frame's worth of progress: header bytes one at a time (so nothing past the header is
 * consumed), then the payload straight to where it belongs. */
static void ws_read(hsess_t *h) {
    for (int guard = 0; guard < 8; guard++) {
        if (h->msg_ready) return;                   /* the 9P request goes first */
        if (!h->have_hdr) {
            uint8_t b;
            int n = sread(h, &b, 1);
            if (n < 0) { h->st = H_CLOSING; h->close_after_drain = true; return; }
            if (n == 0) {
                if (speer_closed(h)) { h->st = H_CLOSING; h->close_after_drain = true; }
                return;
            }
            h->last_rx_ms = time_get_ms();
            h->hbuf[h->hlen++] = b;
            int r = ws_parse_header(h->hbuf, h->hlen, P9_MAX_MSIZE, &h->hdr);
            if (r < 0 || (r == 0 && h->hlen >= WS_HDR_MAX)) { ws_close_now(h, 1002); return; }
            if (r == 0) continue;
            if (!h->hdr.masked) { ws_close_now(h, 1002); return; }   /* a client must mask */
            if (h->hdr.opcode == WS_OP_TEXT) { ws_close_now(h, 1003); return; }
            if (h->hdr.opcode <= WS_OP_BINARY && h->msg_len + h->hdr.payload_len > P9_MAX_MSIZE) {
                ws_close_now(h, 1009);
                return;
            }
            h->have_hdr = true;
            h->got = 0;
        }
        bool ctl = h->hdr.opcode >= WS_OP_CLOSE;
        uint8_t *dst = (ctl ? h->ctl : h->ibuf + h->msg_len) + h->got;
        uint32_t want = h->hdr.payload_len - h->got;
        if (want) {
            int n = sread(h, dst, want);
            if (n < 0) { h->st = H_CLOSING; h->close_after_drain = true; return; }
            if (n == 0) return;
            h->last_rx_ms = time_get_ms();
            ws_unmask(dst, (uint32_t)n, h->hdr.mask, h->got);
            h->got += (uint32_t)n;
            if (h->got < h->hdr.payload_len) return;
        }
        /* A whole frame. */
        h->have_hdr = false;
        h->hlen = 0;
        switch (h->hdr.opcode) {
        case WS_OP_PING:  queue_control(h, WS_OP_PONG, h->ctl, h->hdr.payload_len); break;
        case WS_OP_PONG:  h->ping_out = false; break;
        case WS_OP_CLOSE: ws_close_now(h, 1000); return;
        default:
            h->msg_len += h->hdr.payload_len;
            if (h->hdr.fin) h->msg_ready = true;
            break;
        }
        if (h->olen) return;                         /* let the reply go before reading on */
    }
}

static void sess_step(hsess_t *h) {
    uint64_t now = time_get_ms();
    switch (h->st) {
    case H_FREE:
        return;
    case H_REQUEST: {
        int n = sread(h, h->ibuf + h->ilen, HTTPD_REQ_MAX - 1 - h->ilen);
        if (n < 0) { sess_close(h); return; }
        if (n > 0) {
            h->ilen += (uint32_t)n;
            h->ibuf[h->ilen] = 0;
            if (strstr((char *)h->ibuf, "\r\n\r\n")) { handle_request(h); return; }
            if (h->ilen >= HTTPD_REQ_MAX - 1) { reply_and_close(h, "431 Request Header Fields Too Large", "text/plain", "too big\n"); g_bad++; return; }
        } else if (speer_closed(h) || now - h->started_ms > HTTPD_REQ_TIMEOUT_MS) {
            sess_close(h);
        }
        return;
    }
    case H_FILE: {
        if (!drain(h)) return;
        if (h->file_left == 0) { sess_close(h); return; }
        uint32_t n = h->file_left < HTTPD_OBUF ? h->file_left : HTTPD_OBUF;
        int r;
        if (h->mem) { memcpy(h->obuf, h->mem + h->file_off, n); r = (int)n; }
        else r = vfs_pread(h->fd, h->obuf, n, h->file_off);
        if (r <= 0) { sess_close(h); return; }       /* short: the connection ends early, which the browser notices */
        h->olen = (uint32_t)r;
        h->osent = 0;
        h->file_off += (uint32_t)r;
        h->file_left -= (uint32_t)r;
        return;
    }
    case H_CLOSING:
        if (drain(h) && h->close_after_drain) sess_close(h);
        else if (now - h->started_ms > 3u * HTTPD_REQ_TIMEOUT_MS) sess_close(h);
        return;
    case H_WS:
        break;
    }

    /* H_WS */
    if (!drain(h)) return;
    if (h->st == H_WS) ws_read(h);
    if (h->st == H_WS && h->msg_ready && h->olen == 0) (void)p9_link_service(&h->link);
    if (h->st == H_WS && h->prot.state == P9_PROT_DEAD) { ws_close_now(h, 1008); return; }   /* a frame failed its tag */
    if (h->st == H_WS && h->olen) (void)drain(h);
    if (h->st != H_WS) return;
    /* A peer that has gone quiet is asked once; a reset follows if the segment is never
     * acknowledged (the TCP retransmission limit), a silent one is dropped here. */
    if (now - h->last_rx_ms > HTTPD_PING_MS && !h->ping_out && h->olen == 0) {
        queue_control(h, WS_OP_PING, NULL, 0);
        h->ping_out = true;
        h->ping_sent_ms = now;
    } else if (h->ping_out && now - h->ping_sent_ms > HTTPD_DEAD_MS - HTTPD_PING_MS) {
        sess_close(h);
    }
}

#if defined(CONFIG_TLS_SERVER)
static void https_down(void) {
    if (g_tls_on) tcp_stream_unlisten(HTTPS_PORT);
    if (g_tls_mem) {
        crypto_wipe_bytes(g_tls_mem, 32);
        palloc_free(g_tls_mem, g_tls_pages);
    }
    g_tls_mem = NULL;
    g_tls_on = false;
}

/* HTTPS on port 443 when the identity holds a key and a certificate for it; off otherwise. */
static void https_up(void) {
    https_down();
    uint8_t key[NODE_TLS_KEY_LEN];
    if (!node_tls_key(key)) return;
    g_tls_pages = (32u + NODE_TLS_CERT_MAX + HTTPD_MAX_SESSIONS * TLS_SESS_BYTES + 4095u) / 4096u;
    uint8_t *mem = (uint8_t *)palloc_pages_bulk(g_tls_pages);
    tls_cert_info_t ci;
    uint32_t len = 0;
    const char *why = NULL;
    if (!mem) why = "no memory for its buffers";
    else if (!node_tls_cert(mem + 32, NODE_TLS_CERT_MAX, &len)) why = "no certificate (lugalos-ca issue)";
    else if (tls_cert_parse(mem + 32, len, &ci) != TLS_OK || !crypto_equal(ci.pub, key + 32, 64)) why = "the certificate is not for this key";
    else if (tcp_stream_listen(HTTPS_PORT) != 0) why = "cannot listen on port 443";
    if (why) {
        crypto_wipe_bytes(key, sizeof key);
        if (mem) palloc_free(mem, g_tls_pages);
        printk("[Httpd] no HTTPS: %s\n", why);
        return;
    }
    memcpy(mem, key, 32);
    crypto_wipe_bytes(key, sizeof key);
    g_tls_mem = mem;
    g_tls_cert_len = len;
    g_tls_on = true;
    printk("[Httpd] HTTPS on port %u for %s, until %s\n", (unsigned)HTTPS_PORT, ci.subject_cn, ci.not_after);
}

static uint8_t *tls_rx_of(const hsess_t *h) {
    return g_tls_mem + 32u + NODE_TLS_CERT_MAX + (uint32_t)(h - g_sess) * TLS_SESS_BYTES;
}
#endif

/* A connection waiting on either port, into `h`. */
static bool accept_into(hsess_t *h) {
    tcp_stream_t s;
    uint8_t peer[4];
    bool tls = false;
    int a = tcp_stream_accept(g_port, &s, peer);
#if defined(CONFIG_TLS_SERVER)
    if (a != 1 && g_tls_on) { a = tcp_stream_accept(HTTPS_PORT, &s, peer); tls = a == 1; }
#endif
    if (a != 1) return false;
    h->s = s;
    memcpy(h->peer, peer, 4);
    h->st = H_REQUEST;
    h->ilen = 0;
    h->started_ms = time_get_ms();
#if defined(CONFIG_TLS_SERVER)
    h->tls_on = tls;
    if (tls) {
        uint8_t *rx = tls_rx_of(h);
        tls_server_init(&h->tls, g_tls_mem + 32, g_tls_cert_len, g_tls_mem, tls_tcp_recv, tls_tcp_send, &h->s,
                        rx, rx + TLS_RECORD_MAX);
        g_tls_sessions++;
    }
#else
    (void)tls;
#endif
    return true;
}

static void httpd_task(void *arg) {
    (void)arg;
#if defined(CONFIG_TLS_SERVER)
    https_up();
    g_tls_dirty = false;
#endif
    while (!g_want_stop) {
#if defined(CONFIG_TLS_SERVER)
        if (g_tls_dirty) {
            bool busy = false;
            for (uint32_t i = 0; i < HTTPD_MAX_SESSIONS; i++) if (g_sess[i].st != H_FREE && g_sess[i].tls_on) busy = true;
            if (!busy) { g_tls_dirty = false; https_up(); }
        }
#endif
        for (;;) {
            hsess_t *free_slot = NULL;
            for (uint32_t i = 0; i < HTTPD_MAX_SESSIONS; i++) if (g_sess[i].st == H_FREE) { free_slot = &g_sess[i]; break; }
            if (!free_slot || !accept_into(free_slot)) break;
        }
        for (uint32_t i = 0; i < HTTPD_MAX_SESSIONS; i++) sess_step(&g_sess[i]);
        task_sleep_ms(3);
    }
    for (uint32_t i = 0; i < HTTPD_MAX_SESSIONS; i++) if (g_sess[i].st != H_FREE) sess_close(&g_sess[i]);
#if defined(CONFIG_TLS_SERVER)
    https_down();
#endif
    tcp_stream_unlisten(g_port);
    palloc_free(g_pool, g_pool_pages);
    g_pool = NULL;
    g_sess = NULL;
    g_run = false;
    g_pid = -1;
}

int httpd_start(uint16_t port) {
    if (g_run) return 0;
    if (port == 0) port = HTTPD_DEFAULT_PORT;
    const uint32_t table = (HTTPD_MAX_SESSIONS * (uint32_t)sizeof(hsess_t) + 15u) & ~15u;
    g_pool_pages = (table + HTTPD_MAX_SESSIONS * (P9_MAX_MSIZE + HTTPD_OBUF) + 4095u) / 4096u;
    g_pool = (uint8_t *)palloc_pages(g_pool_pages);
    if (!g_pool) { printk("[Httpd] no memory for %u session pages\n", (unsigned)g_pool_pages); return -1; }
    g_sess = (hsess_t *)g_pool;
    memset(g_sess, 0, table);
    for (uint32_t i = 0; i < HTTPD_MAX_SESSIONS; i++) {
        g_sess[i].ibuf = g_pool + table + i * (P9_MAX_MSIZE + HTTPD_OBUF);
        g_sess[i].obuf = g_sess[i].ibuf + P9_MAX_MSIZE;
        g_sess[i].fd = -1;
    }
    if (tcp_stream_listen(port) != 0) {
        palloc_free(g_pool, g_pool_pages);
        g_pool = NULL;
        printk("[Httpd] cannot listen on port %u\n", (unsigned)port);
        return -1;
    }
    g_port = port;
    g_want_stop = false;
    g_pid = task_create_sized("httpd", httpd_task, NULL, HTTPD_STACK_PAGES);
    if (g_pid < 0) {
        tcp_stream_unlisten(port);
        palloc_free(g_pool, g_pool_pages);
        g_pool = NULL;
        return -1;
    }
    g_run = true;
    printk("[Httpd] listening on port %u (%u sessions)\n", (unsigned)port, (unsigned)HTTPD_MAX_SESSIONS);
    return 0;
}

void httpd_stop(void) {
    if (!g_run) return;
    g_want_stop = true;
    for (int i = 0; i < 200 && g_run; i++) task_sleep_ms(10);
}

uint32_t httpd_status(char *out, uint32_t cap) {
    uint32_t live = 0, ws = 0;
    for (uint32_t i = 0; g_run && g_sess && i < HTTPD_MAX_SESSIONS; i++) {
        if (g_sess[i].st != H_FREE) live++;
        if (g_sess[i].st == H_WS) ws++;
    }
    int n = ksnprintf(out, cap,
                     "httpd: %s\nport: %u\nsessions: %u of %u (%u websocket)\nrequests: %u\npages: %u\nwebsockets: %u\n"
                     "not_found: %u\nbad_requests: %u\nwrites: %s\n",
                     g_run ? "running" : "stopped", (unsigned)g_port, (unsigned)live, (unsigned)HTTPD_MAX_SESSIONS,
                     (unsigned)ws, (unsigned)g_requests, (unsigned)g_pages, (unsigned)g_ws_total, (unsigned)g_not_found,
                     (unsigned)g_bad, g_write_ok ? "ALLOWED over plain HTTP" : "refused (read-only links)");
    if (n < 0) return 0;
    if ((uint32_t)n > cap) n = (int)cap;
#if defined(CONFIG_TLS_SERVER)
    int m = ksnprintf(out + n, cap - (uint32_t)n, "https: %s\nhttps_sessions: %u\n",
                      g_tls_on ? "on (port 443; plain HTTP reads only)" : "off (no key and certificate: tls, lugalos-ca issue)",
                      (unsigned)g_tls_sessions);
    if (m > 0) n += m;
#endif
    return (uint32_t)(n < (int)cap ? n : (int)cap);
}
