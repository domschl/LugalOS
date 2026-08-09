#include "kernel/klog.h"
#include <string.h>

/* See kernel/include/kernel/klog.h for the rationale. */

static char     g_ring[KLOG_RING_SIZE];
static uint64_t g_total;   /* bytes ever written; g_ring holds the last min(g_total, SIZE) */

typedef struct {
    const char   *name;
    klog_putc_fn  putc;
    bool          in_use;
    bool          attached;
} klog_sink_t;

static klog_sink_t g_sinks[KLOG_MAX_SINKS];

/* Guards against a sink's putc() re-entering klog_write() (e.g. a future
 * sink that logs about its own failures). Without this, such a sink would
 * recurse until the stack died. Not a concurrency lock -- B2 needs a real
 * one, see the header. */
static bool g_in_fanout;

void klog_putc(char c) {
    g_ring[(uint32_t)(g_total % KLOG_RING_SIZE)] = c;
    g_total++;

    if (g_in_fanout) return;
    g_in_fanout = true;
    for (int i = 0; i < KLOG_MAX_SINKS; i++) {
        if (g_sinks[i].in_use && g_sinks[i].attached && g_sinks[i].putc) {
            g_sinks[i].putc(c);
        }
    }
    g_in_fanout = false;
}

void klog_write(const char *s, uint32_t len) {
    if (!s) return;
    for (uint32_t i = 0; i < len; i++) klog_putc(s[i]);
}

/* --- Sink registry --- */

static klog_sink_t *sink_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < KLOG_MAX_SINKS; i++) {
        if (g_sinks[i].in_use && g_sinks[i].name && strcmp(g_sinks[i].name, name) == 0) {
            return &g_sinks[i];
        }
    }
    return NULL;
}

int klog_sink_register(const char *name, klog_putc_fn putc) {
    if (!name || !putc) return -1;

    klog_sink_t *existing = sink_find(name);
    if (existing) {
        existing->putc = putc;
        existing->attached = true;
        return 0;
    }

    for (int i = 0; i < KLOG_MAX_SINKS; i++) {
        if (!g_sinks[i].in_use) {
            g_sinks[i].name = name;
            g_sinks[i].putc = putc;
            g_sinks[i].in_use = true;
            g_sinks[i].attached = true;
            return 0;
        }
    }
    return -1; /* table full; caller decides whether that's fatal */
}

int klog_sink_detach(const char *name) {
    klog_sink_t *s = sink_find(name);
    if (!s) return -1;
    s->attached = false;
    return 0;
}

int klog_sink_attach(const char *name) {
    klog_sink_t *s = sink_find(name);
    if (!s) return -1;
    s->attached = true;
    return 0;
}

bool klog_sink_info(uint32_t index, const char **name_out, bool *attached_out) {
    uint32_t seen = 0;
    for (int i = 0; i < KLOG_MAX_SINKS; i++) {
        if (!g_sinks[i].in_use) continue;
        if (seen == index) {
            if (name_out) *name_out = g_sinks[i].name;
            if (attached_out) *attached_out = g_sinks[i].attached;
            return true;
        }
        seen++;
    }
    return false;
}

/* --- Ring readback --- */

uint64_t klog_total(void) {
    return g_total;
}

uint64_t klog_oldest(void) {
    return g_total > KLOG_RING_SIZE ? g_total - KLOG_RING_SIZE : 0;
}

uint32_t klog_read(uint64_t abs_offset, char *buf, uint32_t count) {
    if (!buf || count == 0) return 0;

    uint64_t oldest = klog_oldest();
    /* Caller fell off the back of the ring (log wrapped past what it was
     * reading): resume at the oldest byte still held rather than returning
     * stale or garbage content. */
    if (abs_offset < oldest) abs_offset = oldest;
    if (abs_offset >= g_total) return 0;

    uint64_t avail = g_total - abs_offset;
    if (avail < count) count = (uint32_t)avail;

    for (uint32_t i = 0; i < count; i++) {
        buf[i] = g_ring[(uint32_t)((abs_offset + i) % KLOG_RING_SIZE)];
    }
    return count;
}
