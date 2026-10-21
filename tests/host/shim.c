/*
 * The kernel services the host harnesses' modules call, implemented on the
 * host C library. Just enough for the modules under test to
 * link: output, locks that do nothing (the harnesses are single-threaded),
 * pages from the C heap, and a tiny in-memory file table standing in for the
 * VFS. A module that needs more gets a stub in its own harness, so this file
 * stays the part every harness shares.
 *
 * Output is discarded unless HOST_VERBOSE is set: a fuzzer drives modules
 * through thousands of failing paths, each of which says so.
 */

#include "shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/lock.h"

static int verbose(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOST_VERBOSE") != NULL;
    return v;
}

static int vout(const char *fmt, va_list ap) {
    if (!verbose()) return 0;
    return vprintf(fmt, ap);
}

int printk(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vout(fmt, ap); va_end(ap); return r;
}
int printk_critical(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vout(fmt, ap); va_end(ap); return r;
}
int cprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vout(fmt, ap); va_end(ap); return r;
}
int ksnprintf(char *buf, uint32_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    /* The kernel's returns what it wrote, not what it would have. */
    if (cap == 0) return 0;
    return r < 0 ? 0 : (r >= (int)cap ? (int)cap - 1 : r);
}

/* Re-entrant like the real one, and checked: a release without an acquire
 * is a bug in the module, not something to paper over. */
void ylock_acquire_at(ylock_t *l, const char *name, const char *site) {
    (void)name; (void)site;
    l->depth++;
}
void ylock_release(ylock_t *l) {
    if (l->depth <= 0) { fprintf(stderr, "ylock_release without acquire\n"); abort(); }
    l->depth--;
}

/* Spinlocks that do nothing, and one execution context: the harnesses are
 * single-threaded. */
__attribute__((weak)) uintptr_t spin_lock_irqsave_at(spinlock_t *l, const char *name, const char *site) {
    (void)l; (void)name; (void)site; return 0;
}
__attribute__((weak)) void spin_unlock_irqrestore_at(spinlock_t *l, uintptr_t flags) { (void)l; (void)flags; }
__attribute__((weak)) int sched_context_id(void) { return 0; }
__attribute__((weak)) void net_lock(void) {}
__attribute__((weak)) void net_unlock(void) {}

/* Pages from the C heap, page-aligned and zeroed like palloc's, and sized
 * exactly -- so ASan sees a write one byte past the last page. */
#define HOST_PAGE 4096u
void *palloc_pages(uint32_t n) {
    if (n == 0) return NULL;
    void *p = aligned_alloc(HOST_PAGE, (size_t)n * HOST_PAGE);
    if (p) memset(p, 0, (size_t)n * HOST_PAGE);
    return p;
}
void *palloc_pages_bulk(uint32_t n) { return palloc_pages(n); }
void palloc_free(void *p, uint32_t n) { (void)n; free(p); }

/* --- A flat in-memory file table for vfs_read()/vfs_write() --- */

#define HOST_FILES 32
static struct { char path[128]; uint8_t *data; uint32_t len; } g_files[HOST_FILES];

void host_file_put(const char *path, const void *data, uint32_t len) {
    int slot = -1;
    for (int i = 0; i < HOST_FILES; i++) {
        if (g_files[i].data && strcmp(g_files[i].path, path) == 0) { slot = i; break; }
        if (!g_files[i].data && slot < 0) slot = i;
    }
    if (slot < 0) { fprintf(stderr, "host file table full\n"); abort(); }
    free(g_files[slot].data);
    g_files[slot].data = malloc(len ? len : 1);
    memcpy(g_files[slot].data, data, len);
    g_files[slot].len = len;
    snprintf(g_files[slot].path, sizeof(g_files[slot].path), "%s", path);
}

void host_files_clear(void) {
    for (int i = 0; i < HOST_FILES; i++) { free(g_files[i].data); g_files[i].data = NULL; }
}

const uint8_t *host_file_get(const char *path, uint32_t *len) {
    for (int i = 0; i < HOST_FILES; i++)
        if (g_files[i].data && strcmp(g_files[i].path, path) == 0) {
            *len = g_files[i].len;
            return g_files[i].data;
        }
    return NULL;
}

/* The kernel's contract: at most max_len - 1 bytes, NUL-terminated. */
int vfs_read(const char *path, void *buf, uint32_t max_len) {
    uint32_t len;
    const uint8_t *d = host_file_get(path, &len);
    if (!d || !buf || max_len == 0) return -1;
    uint32_t n = len < max_len - 1 ? len : max_len - 1;
    memcpy(buf, d, n);
    ((char *)buf)[n] = '\0';
    return (int)n;
}

int vfs_write(const char *path, const void *buf, uint32_t len) {
    host_file_put(path, buf, len);
    return 0;
}

__attribute__((weak)) int vfs_open(const char *path, int flags) {
    (void)flags;
    for (int i = 0; i < HOST_FILES; i++) {
        if (g_files[i].data && strcmp(g_files[i].path, path) == 0) {
            return i + 1;
        }
    }
    if (flags & 0x02) { /* VFS_O_WRITE / VFS_O_CREATE */
        host_file_put(path, "", 0);
        for (int i = 0; i < HOST_FILES; i++) {
            if (g_files[i].data && strcmp(g_files[i].path, path) == 0) {
                return i + 1;
            }
        }
    }
    return -1;
}

__attribute__((weak)) int vfs_close(int fd) {
    (void)fd;
    return 0;
}

__attribute__((weak)) int vfs_pread(int fd, void *buf, uint32_t count, uint64_t offset) {
    if (fd < 1 || fd > HOST_FILES) return -1;
    int slot = fd - 1;
    if (!g_files[slot].data) return -1;
    if (offset >= g_files[slot].len) return 0;
    uint32_t avail = g_files[slot].len - (uint32_t)offset;
    uint32_t n = count < avail ? count : avail;
    memcpy(buf, g_files[slot].data + offset, n);
    return (int)n;
}

__attribute__((weak)) int vfs_pwrite(int fd, const void *buf, uint32_t count, uint64_t offset) {
    if (fd < 1 || fd > HOST_FILES) return -1;
    int slot = fd - 1;
    if (!g_files[slot].data) return -1;
    uint32_t req_len = (uint32_t)offset + count;
    if (req_len > g_files[slot].len) {
        g_files[slot].data = realloc(g_files[slot].data, req_len ? req_len : 1);
        g_files[slot].len = req_len;
    }
    memcpy(g_files[slot].data + offset, buf, count);
    return (int)count;
}

/* Weak: a harness whose module runs timers supplies a clock it can move. */
__attribute__((weak)) uint64_t time_get_ms(void) { return 0; }
__attribute__((weak)) uint64_t time_get_us(void) { return 0; }
__attribute__((weak)) void sched_yield(void) {}
__attribute__((weak)) bool console_interrupt_requested(void) { return false; }
__attribute__((weak)) int tcp_listen(uint16_t port) { (void)port; return 0; }
__attribute__((weak)) void tcp_unlisten(void) {}
__attribute__((weak)) void tcp_unlisten_port(uint16_t port) { (void)port; }
__attribute__((weak)) int tcp_stream_open(void *s, const uint8_t *ip, uint16_t port) {
    (void)s; (void)ip; (void)port; return -1;
}
__attribute__((weak)) int tcp_stream_ready(void *s) { (void)s; return -1; }
__attribute__((weak)) int tcp_stream_read(void *s, uint8_t *buf, uint32_t max) {
    (void)s; (void)buf; (void)max; return -1;
}
__attribute__((weak)) int tcp_stream_write(void *s, const uint8_t *buf, uint32_t len) {
    (void)s; (void)buf; (void)len; return -1;
}
__attribute__((weak)) bool tcp_stream_peer_closed(void *s) { (void)s; return true; }
__attribute__((weak)) void tcp_stream_close(void *s) { (void)s; }
__attribute__((weak)) bool ipv4_parse(const char *s, uint8_t out[4]) {
    if (!s || !out) return false;
    unsigned part = 0, val = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10u + (unsigned)(*s - '0');
            if (++digits > 3 || val > 255) return false;
        } else if (*s == '.' || *s == '\0') {
            if (digits == 0 || part > 3) return false;
            out[part++] = (uint8_t)val;
            val = 0; digits = 0;
            if (*s == '\0') break;
        } else {
            return false;
        }
    }
    return part == 4;
}

__attribute__((weak)) bool dhcp_dns_server(uint8_t out[4]) { (void)out; return false; }
__attribute__((weak)) const void *net_state(void) { return NULL; }
__attribute__((weak)) int udp_bind(uint16_t port, void *cb, void *ctx) { (void)port; (void)cb; (void)ctx; return -1; }
__attribute__((weak)) int udp_unbind(uint16_t port) { (void)port; return 0; }
__attribute__((weak)) int udp_send(const uint8_t dst[4], uint16_t dport, uint16_t sport, const void *d, uint32_t l) {
    (void)dst; (void)dport; (void)sport; (void)d; (void)l; return -1;
}
__attribute__((weak)) void random_bytes(void *buf, uint32_t len) {
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) p[i] = (uint8_t)rand();
}

