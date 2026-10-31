#ifndef NET_HTTPD_H
#define NET_HTTPD_H

/* The admin web server (plan/phase52_remote_admin.md 1.2, 52.11): plain HTTP on one port.
 *
 *   GET /, /index.html   the admin page: the image's own gzip copy (tools/sd_root/www/admin.html, built
 *                        in), so it always matches the firmware; a client that does not take gzip gets
 *                        /flash0/www/admin.html or /sd0/www/admin.html if there is one
 *   GET /9p + Upgrade    a WebSocket whose binary messages are 9P messages, served by the same
 *                        server as every other link, with auth_required: nothing is reachable
 *                        before a Tauth/Tattach with a granted key
 *   GET /board           the public facts of /proc/board, as text: which board this is, before sign-in
 *   anything else        404
 *
 * Its own task owns the connections (tcp_stream_accept()); the network pump never runs HTTP,
 * VFS or 9P code. At most HTTPD_MAX_SESSIONS connections at once. Buffers are taken from the page
 * allocator by httpd_start() and given back by httpd_stop(), so a board that never starts it
 * pays nothing.
 *
 * A WebSocket over plain HTTP has no integrity protection, so its 9P link is read-only
 * (p9_link_t.read_only) unless the client opens a protected session (52.11b) or the console has run
 * `httpd write on`, which says so. On a CONFIG_TLS_SERVER board with a key and certificate
 * (kernel/tls_cert.h) the same server also speaks HTTPS on port 443 (net/tls.c): there an admin peer
 * may write, plain HTTP only reads, and a request for the page on port 80 is redirected. */

#include <stdint.h>
#include <stdbool.h>

#define HTTPD_DEFAULT_PORT 80

int  httpd_start(uint16_t port);          /* 0, or -1: no network, no memory, no task */
void httpd_stop(void);
bool httpd_running(uint16_t *port_out);

void httpd_set_write(bool allow);
bool httpd_write_allowed(void);

/* The HTTPS key or certificate changed (kernel/tls_cert.c): the server picks them up when no HTTPS session
 * is open. Does nothing on a board without CONFIG_TLS_SERVER. */
void httpd_tls_changed(void);

/* One line per fact, for `httpd status`. */
uint32_t httpd_status(char *out, uint32_t cap);

#endif
