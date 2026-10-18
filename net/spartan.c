#include "net/spartan.h"
#include "net/tcp.h"
#include "net/ip.h"
#include "fs/vfs.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "libc/include/string.h"

/* accepted: `spartand start|stop` from a shell; one flag and one port. */
static bool     g_spartan_running = false;
static uint16_t g_spartan_port = SPARTAN_DEFAULT_PORT;
static uint32_t g_spartan_requests = 0;
static uint32_t g_spartan_served = 0;
static uint32_t g_spartan_errors = 0;

/* --- URL Parsing & Validation --- */

bool spartan_parse_url(const char *url, char *host, size_t host_sz,
                       uint16_t *port, char *path, size_t path_sz) {
    if (!url || !host || host_sz == 0 || !port || !path || path_sz == 0) return false;

    /* Case-insensitive prefix check for "spartan://" */
    const char *prefix = "spartan://";
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < plen; i++) {
        char c1 = url[i];
        char c2 = prefix[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
        if (c1 != c2) return false;
    }

    const char *p = url + plen;
    if (*p == '\0') return false;

    /* Find end of host or colon for port */
    const char *colon = NULL;
    const char *slash = NULL;
    for (const char *s = p; *s; s++) {
        if (*s == ':' && !colon && !slash) colon = s;
        if (*s == '/' && !slash) { slash = s; break; }
    }

    /* Extract host */
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

    /* Extract port */
    if (colon) {
        const char *port_start = colon + 1;
        size_t port_len = slash ? (size_t)(slash - port_start) : strlen(port_start);
        if (port_len == 0 || port_len > 5) return false;
        char pbuf[6];
        memcpy(pbuf, port_start, port_len);
        pbuf[port_len] = '\0';
        long val = 0;
        for (size_t i = 0; i < port_len; i++) {
            if (pbuf[i] < '0' || pbuf[i] > '9') return false;
            val = val * 10 + (pbuf[i] - '0');
        }
        if (val <= 0 || val > 65535) return false;
        *port = (uint16_t)val;
    } else {
        *port = SPARTAN_DEFAULT_PORT;
    }

    /* Extract path */
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

bool spartan_sandbox_validate_path(const char *req_path, char *out_safe_path, size_t out_sz) {
    if (!req_path || !out_safe_path || out_sz == 0) return false;
    out_safe_path[0] = '\0';

    /* Request path must begin with '/' */
    if (req_path[0] != '/') return false;

    /* Rejection of dangerous patterns */
    for (const char *s = req_path; *s; s++) {
        if (*s == '\\') return false;
        /* Path traversal check: any ".." */
        if (s[0] == '.' && s[1] == '.') {
            if ((s == req_path || *(s - 1) == '/') &&
                (*(s + 2) == '/' || *(s + 2) == '\0')) {
                return false;
            }
        }
    }

    /* If request path is "/" or empty after slash, map to "/sd0/pub/index.gmi" */
    if (strcmp(req_path, "/") == 0 || req_path[0] == '\0') {
        const char *idx = SPARTAN_PUB_ROOT "/index.gmi";
        if (strlen(idx) >= out_sz) return false;
        strcpy(out_safe_path, idx);
        return true;
    }

    /* Concatenate SPARTAN_PUB_ROOT + req_path */
    size_t root_len = strlen(SPARTAN_PUB_ROOT);
    size_t req_len = strlen(req_path);
    if (root_len + req_len >= out_sz) return false;

    memcpy(out_safe_path, SPARTAN_PUB_ROOT, root_len);
    memcpy(out_safe_path + root_len, req_path, req_len + 1);
    return true;
}

/* --- Spartan Network Client --- */

int spartan_fetch(const char *url, const char *local_dest,
                  char *redirect_out, size_t redir_sz,
                  char *err_out, size_t err_sz) {
    if (!url || !local_dest) return SPARTAN_ERR_PARSE;

    char host[64];
    char path[128];
    uint16_t port = SPARTAN_DEFAULT_PORT;

    if (!spartan_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        if (err_out && err_sz > 0) {
            strncpy(err_out, "Invalid Spartan URL format", err_sz - 1);
            err_out[err_sz - 1] = '\0';
        }
        return SPARTAN_ERR_PARSE;
    }

    uint8_t ip[IPV4_LEN];
    if (!ipv4_parse(host, ip)) {
        if (err_out && err_sz > 0) {
            strncpy(err_out, "DNS not supported; host must be an IPv4 dotted quad", err_sz - 1);
            err_out[err_sz - 1] = '\0';
        }
        return SPARTAN_ERR_DNS;
    }

    tcp_stream_t stream;
    if (tcp_stream_open(&stream, ip, port) != 0) {
        if (err_out && err_sz > 0) {
            strncpy(err_out, "Cannot open TCP stream (no buffers or slot busy)", err_sz - 1);
            err_out[err_sz - 1] = '\0';
        }
        return SPARTAN_ERR_CONNECT;
    }

    /* Wait for handshake to complete */
    uint64_t deadline = time_get_ms() + 5000;
    int ready = 0;
    while ((ready = tcp_stream_ready(&stream)) == 0) {
        if (console_interrupt_requested()) {
            tcp_stream_close(&stream);
            return SPARTAN_ERR_INTERRUPTED;
        }
        if (time_get_ms() >= deadline) {
            tcp_stream_close(&stream);
            if (err_out && err_sz > 0) {
                strncpy(err_out, "Connection timeout to server", err_sz - 1);
                err_out[err_sz - 1] = '\0';
            }
            return SPARTAN_ERR_TIMEOUT;
        }
        sched_yield();
    }
    if (ready < 0) {
        tcp_stream_close(&stream);
        if (err_out && err_sz > 0) {
            strncpy(err_out, "Connection refused or reset by peer", err_sz - 1);
            err_out[err_sz - 1] = '\0';
        }
        return SPARTAN_ERR_REFUSED;
    }

    /* Send request line: "<host> <path> 0\r\n" */
    char req[256];
    ksnprintf(req, sizeof(req), "%s %s 0\r\n", host, path);
    size_t req_len = strlen(req);
    size_t sent = 0;
    deadline = time_get_ms() + 3000;
    while (sent < req_len) {
        int n = tcp_stream_write(&stream, (const uint8_t *)req + sent, (uint32_t)(req_len - sent));
        if (n < 0) {
            tcp_stream_close(&stream);
            return SPARTAN_ERR_IO;
        }
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (time_get_ms() >= deadline || console_interrupt_requested()) {
            tcp_stream_close(&stream);
            return SPARTAN_ERR_TIMEOUT;
        }
        sched_yield();
    }

    /* Read header line: "<status> <meta>\r\n" */
    char header[256];
    size_t hpos = 0;
    bool got_header = false;
    deadline = time_get_ms() + 6000;

    while (time_get_ms() < deadline && !console_interrupt_requested()) {
        uint8_t ch;
        int n = tcp_stream_read(&stream, &ch, 1);
        if (n < 0) break;
        if (n == 0) {
            if (tcp_stream_peer_closed(&stream)) break;
            sched_yield();
            continue;
        }
        if (ch == '\r') continue;
        if (ch == '\n') {
            header[hpos] = '\0';
            got_header = true;
            break;
        }
        if (hpos < sizeof(header) - 1) {
            header[hpos++] = (char)ch;
        }
    }

    if (!got_header || hpos == 0) {
        tcp_stream_close(&stream);
        if (err_out && err_sz > 0) {
            strncpy(err_out, "Empty or missing response header from Spartan server", err_sz - 1);
            err_out[err_sz - 1] = '\0';
        }
        return SPARTAN_ERR_HEADER;
    }

    char status = header[0];
    const char *meta = &header[1];
    while (*meta == ' ') meta++;

    if (status == '3') {
        /* Redirect */
        if (redirect_out && redir_sz > 0) {
            strncpy(redirect_out, meta, redir_sz - 1);
            redirect_out[redir_sz - 1] = '\0';
        }
        tcp_stream_close(&stream);
        return SPARTAN_REDIRECT;
    }

    if (status == '4' || status == '5') {
        /* Client or Server Error */
        if (err_out && err_sz > 0) {
            strncpy(err_out, meta, err_sz - 1);
            err_out[err_sz - 1] = '\0';
        }
        tcp_stream_close(&stream);
        return (status == '4') ? SPARTAN_ERR_CLIENT : SPARTAN_ERR_SERVER;
    }

    if (status != '2') {
        tcp_stream_close(&stream);
        if (err_out && err_sz > 0) {
            ksnprintf(err_out, err_sz, "Unknown status code '%c' from server", status);
        }
        return SPARTAN_ERR_PROTOCOL;
    }

    /* Status 2: Success! Stream body into local_dest */
    int fd = vfs_open(local_dest, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) {
        tcp_stream_close(&stream);
        if (err_out && err_sz > 0) {
            ksnprintf(err_out, err_sz, "Cannot create local destination '%s'", local_dest);
        }
        return SPARTAN_ERR_FILE;
    }

    uint8_t r_buf[512];
    uint64_t file_off = 0;
    deadline = time_get_ms() + 10000;
    while (time_get_ms() < deadline && !console_interrupt_requested()) {
        int n = tcp_stream_read(&stream, r_buf, sizeof(r_buf));
        if (n < 0) break;
        if (n > 0) {
            vfs_pwrite(fd, r_buf, (uint32_t)n, file_off);
            file_off += (uint64_t)n;
            deadline = time_get_ms() + 5000;
            continue;
        }
        if (tcp_stream_peer_closed(&stream)) break;
        sched_yield();
    }

    vfs_close(fd);
    tcp_stream_close(&stream);
    return SPARTAN_OK;
}

/* --- Spartan Server (spartand) --- */

int spartan_process_request(const uint8_t *rx, uint32_t rx_len, uint32_t *consumed,
                           uint8_t *tx, uint32_t tx_max, uint32_t *tx_len,
                           bool *close_after_tx) {
    if (!rx || rx_len == 0 || !consumed || !tx || tx_max == 0 || !tx_len || !close_after_tx) {
        return -1;
    }

    /* Find newline in rx */
    const uint8_t *nl = NULL;
    for (uint32_t i = 0; i < rx_len; i++) {
        if (rx[i] == '\n') { nl = &rx[i]; break; }
    }
    if (!nl) {
        /* If line exceeds 256 bytes without newline, reject */
        if (rx_len >= 256) {
            *consumed = rx_len;
            int n = ksnprintf((char *)tx, tx_max, "40 Bad Request: line too long\r\n");
            *tx_len = (n > 0) ? (uint32_t)n : 0;
            *close_after_tx = true;
            g_spartan_errors++;
            return 0;
        }
        return -1; /* Wait for full line */
    }

    uint32_t line_len = (uint32_t)(nl - rx + 1);
    *consumed = line_len;
    g_spartan_requests++;

    /* Copy line into null-terminated string */
    char line[256];
    uint32_t copy_len = line_len;
    if (copy_len >= sizeof(line)) copy_len = sizeof(line) - 1;
    memcpy(line, rx, copy_len);
    line[copy_len] = '\0';

    /* Strip trailing \r\n */
    while (copy_len > 0 && (line[copy_len - 1] == '\r' || line[copy_len - 1] == '\n')) {
        line[--copy_len] = '\0';
    }

    /* Spartan request format: "<host> <path> [content-length]" */
    char host[128] = {0};
    char path[128] = {0};

    const char *p = line;
    while (*p == ' ') p++;
    const char *sp1 = strchr(p, ' ');
    if (!sp1) {
        int n = ksnprintf((char *)tx, tx_max, "40 Bad Request: missing path\r\n");
        *tx_len = (n > 0) ? (uint32_t)n : 0;
        *close_after_tx = true;
        g_spartan_errors++;
        return 0;
    }
    size_t hlen = (size_t)(sp1 - p);
    if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    p = sp1 + 1;
    while (*p == ' ') p++;
    const char *sp2 = strchr(p, ' ');
    if (sp2) {
        size_t plen = (size_t)(sp2 - p);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, p, plen);
        path[plen] = '\0';
    } else {
        strncpy(path, p, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    /* Path validation and sandboxing */
    char safe_path[160];
    if (!spartan_sandbox_validate_path(path, safe_path, sizeof(safe_path))) {
        int n = ksnprintf((char *)tx, tx_max, "40 Access Denied: invalid path\r\n");
        *tx_len = (n > 0) ? (uint32_t)n : 0;
        *close_after_tx = true;
        g_spartan_errors++;
        return 0;
    }

    /* Open requested file */
    int fd = vfs_open(safe_path, VFS_O_READ);
    if (fd < 0 && strcmp(path, "/") == 0) {
        /* If index.gmi in /sd0/pub is absent, fallback to help manual */
        fd = vfs_open("/sd0/help/shell.gmi", VFS_O_READ);
    }

    if (fd < 0) {
        int n = ksnprintf((char *)tx, tx_max, "40 File Not Found\r\n");
        *tx_len = (n > 0) ? (uint32_t)n : 0;
        *close_after_tx = true;
        g_spartan_errors++;
        return 0;
    }

    /* Success: write "2 text/gemini; charset=utf-8\r\n" */
    int hdr_len = ksnprintf((char *)tx, tx_max, "2 text/gemini; charset=utf-8\r\n");
    if (hdr_len < 0 || (uint32_t)hdr_len >= tx_max) {
        vfs_close(fd);
        *tx_len = 0;
        *close_after_tx = true;
        g_spartan_errors++;
        return 0;
    }

    /* Read file body into remaining tx buffer */
    uint32_t room = tx_max - (uint32_t)hdr_len;
    int rd = vfs_pread(fd, tx + hdr_len, room, 0);
    vfs_close(fd);

    *tx_len = (uint32_t)hdr_len + (rd > 0 ? (uint32_t)rd : 0);
    *close_after_tx = true;
    g_spartan_served++;
    return 0;
}

int spartan_server_start(uint16_t port) {
    if (port == 0) port = SPARTAN_DEFAULT_PORT;
    if (g_spartan_running && g_spartan_port == port) return 0;

    int rc = tcp_listen(port);
    if (rc != 0) return rc;

    g_spartan_port = port;
    g_spartan_running = true;
    printk("[Spartand] Spartan server listening on port %u (sandbox: %s)\n",
           g_spartan_port, SPARTAN_PUB_ROOT);
    return 0;
}

void spartan_server_stop(void) {
    if (!g_spartan_running) return;
    g_spartan_running = false;
    printk("[Spartand] Spartan server stopped.\n");
}

bool spartan_server_running(uint16_t *port_out) {
    if (port_out) *port_out = g_spartan_port;
    return g_spartan_running;
}

uint16_t spartan_server_port(void) {
    return g_spartan_port;
}

void spartan_server_stats(uint32_t *requests, uint32_t *served, uint32_t *errors) {
    if (requests) *requests = g_spartan_requests;
    if (served)   *served = g_spartan_served;
    if (errors)   *errors = g_spartan_errors;
}
