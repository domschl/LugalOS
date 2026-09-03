#include "kernel/klog.h"
#include "kernel/lock.h"
#include "kernel/hart.h"
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
 * recurse until the stack died. Not a concurrency lock -- see below.
 *
 * S5 (plan/phase22_smp_locking_foundation.md) makes it per-hart, which is
 * what it always meant. The thing it guards against is a sink's putc()
 * reaching printk() and coming back here *on the same call stack*, and a
 * call stack belongs to a hart. A single flag shared by two harts would
 * have made one hart's fanout suppress the other's -- silently dropping
 * that hart's console output rather than merely nesting it. */
static bool g_in_fanout[MAX_HARTS];

/* Guards the ring and its counter, and nothing else (S5).
 *
 * Not the fanout below: a sink's putc() is a UART write that can block
 * (M2), and a spinlock_t held across a block is the deadlock its own header
 * warns about. So the two halves of this function are protected by
 * different things for different reasons -- the ring by a lock because two
 * harts writing g_ring[g_total % SIZE] would interleave characters and tear
 * the counter, the fanout by a per-hart flag because its hazard is
 * recursion rather than concurrency. */
static spinlock_t g_klog_lock;

void klog_putc(char c) {
    uintptr_t flags = spin_lock_irqsave(&g_klog_lock);
    g_ring[(uint32_t)(g_total % KLOG_RING_SIZE)] = c;
    g_total++;
    spin_unlock_irqrestore(&g_klog_lock, flags);

    unsigned h = hart_id();
    if (g_in_fanout[h]) return;
    g_in_fanout[h] = true;
    for (int i = 0; i < KLOG_MAX_SINKS; i++) {
        if (g_sinks[i].in_use && g_sinks[i].attached && g_sinks[i].putc) {
            g_sinks[i].putc(c);
        }
    }
    g_in_fanout[h] = false;
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
