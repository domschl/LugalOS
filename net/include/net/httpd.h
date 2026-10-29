#ifndef NET_HTTPD_H
#define NET_HTTPD_H

/* The admin web server (plan/phase52_remote_admin.md 1.2, 52.11): plain HTTP on one port.
 *
 *   GET /, /index.html   the admin page, from /flash0/www/admin.html (or /sd0/www/ if absent)
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
 * (p9_link_t.read_only) unless the console has run `httpd write on`, which says so. Writes over
 * HTTP proper arrive with TLS (52.15) or protected sessions (52.11b). */

#include <stdint.h>
#include <stdbool.h>

#define HTTPD_DEFAULT_PORT 80

int  httpd_start(uint16_t port);          /* 0, or -1: no network, no memory, no task */
void httpd_stop(void);
bool httpd_running(uint16_t *port_out);

void httpd_set_write(bool allow);
bool httpd_write_allowed(void);

/* One line per fact, for `httpd status`. */
uint32_t httpd_status(char *out, uint32_t cap);

#endif
