#include "kernel/klog.h"
#include "kernel/lock.h"
#include "kernel/hart.h"
#include <string.h>

/* See kernel/include/kernel/klog.h for the rationale. */

/* Y5b, plan/phase31_concurrency_hierarchy.md §5.7: the ring stores *records*,
 * not a byte stream.
 *
 * Each record is a 6-byte header -- 4 bytes of milliseconds (or KLOG_NO_TS)
 * and 2 bytes of payload length -- followed by that many payload bytes. The
 * header is written byte at a time rather than as a struct, because a record
 * straddles the wrap and a struct store would not.
 *
 * Two coordinate spaces exist and both are needed:
 *
 *  - *stored* bytes, which is what the ring's capacity is measured in;
 *  - *rendered* bytes, which is what a reader sees once the timestamp is
 *    expanded back to "[    0.010] ".
 *
 * klog_total()/klog_oldest()/klog_read() keep their existing meaning on the
 * rendered stream, which is why /proc/kmsg needed no change at all: the
 * window it snapshots at open() time is still [oldest, total) in bytes it
 * will actually receive. */
static char     g_ring[KLOG_RING_SIZE];

static uint64_t g_stored_total;    /* bytes ever appended to the ring */
static uint64_t g_stored_oldest;   /* stored position of the oldest intact record */
static uint64_t g_render_total;    /* rendered bytes ever produced */
static uint64_t g_render_oldest;   /* rendered position of that same record */

static uint32_t g_truncations;
static uint32_t g_drops;

#define KLOG_HDR_LEN 6u

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
 * warns about -- and, since phase 31 Y2, refuses: the blocking primitives
 * check for a held spinlock and say so. So the two halves of this function are protected by
 * different things for different reasons -- the ring by a lock because two
 * harts writing g_ring[g_total % SIZE] would interleave characters and tear
 * the counter, the fanout by a per-hart flag because its hazard is
 * recursion rather than concurrency. */
static spinlock_t g_klog_lock;

/* The ring, and nothing else. See kernel/printk.h's printk_critical().
 *
 * klog_putc() below fans out to every attached sink, and the console sink's
 * putc is uart_putc(), which batches and blocks once the batch fills. That
 * is correct for ordinary logging and unusable from a context that must not
 * block -- so a critical message records here and reaches the terminal by a
 * separate, bounded route, instead of going through a sink that can wait. */
/* Renders a record's timestamp into `out` exactly as printk used to emit it,
 * and returns its length. `out` needs 16 bytes.
 *
 * The one place the "[    0.010] " form is produced, so that what a sink
 * receives and what klog_read() returns cannot drift apart -- they were the
 * same characters when the producer wrote them and they have to stay so. */
static uint32_t ts_render(uint32_t ms, char *out) {
    if (ms == KLOG_NO_TS) return 0;

    uint32_t sec = ms / 1000u;
    uint32_t msec = ms % 1000u;
    uint32_t n = 0;

    out[n++] = '[';
    if (sec < 10u)        { out[n++]=' '; out[n++]=' '; out[n++]=' '; out[n++]=' '; }
    else if (sec < 100u)  { out[n++]=' '; out[n++]=' '; out[n++]=' '; }
    else if (sec < 1000u) { out[n++]=' '; out[n++]=' '; }
    else if (sec < 10000u){ out[n++]=' '; }

    char digits[10];
    uint32_t d = 0;
    if (sec == 0) digits[d++] = '0';
    while (sec > 0) { digits[d++] = (char)('0' + (sec % 10u)); sec /= 10u; }
    while (d > 0) out[n++] = digits[--d];

    out[n++] = '.';
    out[n++] = (char)('0' + ((msec / 100u) % 10u));
    out[n++] = (char)('0' + ((msec / 10u) % 10u));
    out[n++] = (char)('0' + (msec % 10u));
    out[n++] = ']';
    out[n++] = ' ';
    return n;
}

static uint32_t rec_render_len(uint32_t ms, uint32_t payload_len) {
    char scratch[16];
    return ts_render(ms, scratch) + payload_len;
}

/* Reads one stored byte. */
static inline char ring_at(uint64_t pos) {
    return g_ring[(uint32_t)(pos % KLOG_RING_SIZE)];
}

/* Header of the record starting at `pos`. */
static void rec_header(uint64_t pos, uint32_t *ms_out, uint32_t *len_out) {
    uint32_t ms = (uint32_t)(uint8_t)ring_at(pos)
                | ((uint32_t)(uint8_t)ring_at(pos + 1) << 8)
                | ((uint32_t)(uint8_t)ring_at(pos + 2) << 16)
                | ((uint32_t)(uint8_t)ring_at(pos + 3) << 24);
    uint32_t len = (uint32_t)(uint8_t)ring_at(pos + 4)
                 | ((uint32_t)(uint8_t)ring_at(pos + 5) << 8);
    *ms_out = ms;
    *len_out = len;
}

/* Evicts whole records from the back until `need` stored bytes are free.
 * Called with g_klog_lock held.
 *
 * Whole records, never partial ones: a half-overwritten record has a header
 * made of message text, and walking into it would produce a length of
 * whatever two characters happened to land there. The old byte-stream ring
 * could tolerate losing its first half-line; a framed one cannot. */
static void ring_make_room(uint32_t need) {
    while ((g_stored_total + need) - g_stored_oldest > KLOG_RING_SIZE) {
        uint32_t ms, len;
        rec_header(g_stored_oldest, &ms, &len);
        g_stored_oldest += KLOG_HDR_LEN + len;
        g_render_oldest += rec_render_len(ms, len);
    }
}

/* The ring half: append one record, under the lock, and nothing else.
 * Returns false if the message could not be stored at all. */
static bool ring_append(uint32_t ms, const char *text, uint32_t len) {
    if (KLOG_HDR_LEN + len > KLOG_RING_SIZE) {
        g_drops++;
        return false;
    }

    uintptr_t flags = spin_lock_irqsave(&g_klog_lock);
    ring_make_room(KLOG_HDR_LEN + len);

    uint64_t pos = g_stored_total;
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)(ms & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)((ms >> 8) & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)((ms >> 16) & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)((ms >> 24) & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)(len & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)((len >> 8) & 0xFFu);
    for (uint32_t i = 0; i < len; i++) {
        g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = text[i];
    }

    g_stored_total = pos;
    g_render_total += rec_render_len(ms, len);
    spin_unlock_irqrestore(&g_klog_lock, flags);
    return true;
}

void klog_record_text(uint32_t ms, const char *text, uint32_t len) {
    if (!text) return;
    if (len > KLOG_REC_MAX) { len = KLOG_REC_MAX; g_truncations++; }
    (void)ring_append(ms, text, len);
}

void klog_emit(uint32_t ms, const char *text, uint32_t len) {
    if (!text) return;
    if (len > KLOG_REC_MAX) { len = KLOG_REC_MAX; g_truncations++; }
    (void)ring_append(ms, text, len);

    /* The fan-out, outside the lock and unchanged in what it produces: a
     * sink's putc() is a UART write that can block, and a spinlock_t held
     * across a block is what kernel/lock.h forbids and phase 31's Y2 now
     * refuses outright. Y5c is what stops the producer doing this at all. */
    unsigned h = hart_id();
    if (g_in_fanout[h]) return;
    g_in_fanout[h] = true;

    char ts[16];
    uint32_t ts_len = ts_render(ms, ts);
    for (int i = 0; i < KLOG_MAX_SINKS; i++) {
        if (!(g_sinks[i].in_use && g_sinks[i].attached && g_sinks[i].putc)) continue;
        for (uint32_t j = 0; j < ts_len; j++) g_sinks[i].putc(ts[j]);
        for (uint32_t j = 0; j < len;    j++) g_sinks[i].putc(text[j]);
    }
    g_in_fanout[h] = false;
}

void klog_truncated(void) { g_truncations++; }

uint32_t klog_truncations(void) { return g_truncations; }
uint32_t klog_drops(void)       { return g_drops; }

uint64_t klog_stored_bytes(void) { return g_stored_total - g_stored_oldest; }

uint32_t klog_records(void) {
    uintptr_t flags = spin_lock_irqsave(&g_klog_lock);
    uint32_t n = 0;
    uint64_t pos = g_stored_oldest;
    while (pos < g_stored_total) {
        uint32_t ms, len;
        rec_header(pos, &ms, &len);
        pos += KLOG_HDR_LEN + len;
        n++;
    }
    spin_unlock_irqrestore(&g_klog_lock, flags);
    return n;
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

/* Both of these are in *rendered* bytes -- what a reader receives, not what
 * the ring stores (Y5b). That is the coordinate space /proc/kmsg has always
 * used, so its snapshot-at-open() logic is unchanged. */
uint64_t klog_total(void) {
    return g_render_total;
}

uint64_t klog_oldest(void) {
    return g_render_oldest;
}

uint32_t klog_read(uint64_t abs_offset, char *buf, uint32_t count) {
    if (!buf || count == 0) return 0;

    uintptr_t flags = spin_lock_irqsave(&g_klog_lock);

    /* Caller fell off the back of the ring (the log wrapped past what it was
     * reading): resume at the oldest byte still held rather than returning
     * stale or garbage content. */
    if (abs_offset < g_render_oldest) abs_offset = g_render_oldest;
    if (abs_offset >= g_render_total) {
        spin_unlock_irqrestore(&g_klog_lock, flags);
        return 0;
    }
    uint64_t avail = g_render_total - abs_offset;
    if (avail < count) count = (uint32_t)avail;

    /* Walk records from the oldest intact one, rendering as we go, and start
     * copying once we reach abs_offset. Linear in the ring rather than in the
     * request, which is affordable because the ring is 8 KB and a read of it
     * is a human typing `cat /proc/kmsg`. The alternative -- an index of
     * record starts -- is state to keep correct through eviction for no gain
     * at this size. */
    uint64_t spos = g_stored_oldest;
    uint64_t rpos = g_render_oldest;
    uint32_t written = 0;

    while (spos < g_stored_total && written < count) {
        uint32_t ms, len;
        rec_header(spos, &ms, &len);

        char ts[16];
        uint32_t ts_len = ts_render(ms, ts);
        uint64_t rec_len = ts_len + len;

        if (rpos + rec_len <= abs_offset) {      /* entirely before the window */
            spos += KLOG_HDR_LEN + len;
            rpos += rec_len;
            continue;
        }

        for (uint64_t i = 0; i < rec_len && written < count; i++) {
            if (rpos + i < abs_offset) continue;  /* partial record at the front */
            buf[written++] = (i < ts_len) ? ts[i]
                                          : ring_at(spos + KLOG_HDR_LEN + (i - ts_len));
        }
        spos += KLOG_HDR_LEN + len;
        rpos += rec_len;
    }

    spin_unlock_irqrestore(&g_klog_lock, flags);
    return written;
}
