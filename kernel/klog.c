#include "kernel/klog.h"
#include "kernel/lock.h"
#include "kernel/hart.h"
#include "kernel/sched.h"
#include "kernel/printk.h"
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

/* Bit 15 of the stored length word: this record's text already went to the
 * console, live, from printk_critical(). KLOG_REC_MAX is 320, so the top bits
 * of a 16-bit length are free and no separate flag byte is needed. */
#define KLOG_F_ON_CONSOLE 0x8000u
#define KLOG_LEN_MASK     0x7FFFu

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
    *len_out = len;      /* flag bit included; callers mask as needed */
}

static inline uint32_t rec_len_of(uint32_t raw)  { return raw & KLOG_LEN_MASK; }
static inline bool     rec_on_console(uint32_t raw) { return (raw & KLOG_F_ON_CONSOLE) != 0; }

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
        g_stored_oldest += KLOG_HDR_LEN + rec_len_of(len);
        g_render_oldest += rec_render_len(ms, rec_len_of(len));
    }
}

/* The ring half: append one record, under the lock, and nothing else.
 * Returns false if the message could not be stored at all. */
static bool ring_append(uint32_t ms, const char *text, uint32_t len, bool on_console) {
    uint32_t stored_len = len | (on_console ? KLOG_F_ON_CONSOLE : 0u);
    if (KLOG_HDR_LEN + len > KLOG_RING_SIZE) {
        g_drops++;
        return false;
    }

    uintptr_t flags = spin_lock_irqsave_bottom(&g_klog_lock);
    ring_make_room(KLOG_HDR_LEN + len);

    uint64_t pos = g_stored_total;
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)(ms & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)((ms >> 8) & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)((ms >> 16) & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)((ms >> 24) & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)(stored_len & 0xFFu);
    g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = (char)(uint8_t)((stored_len >> 8) & 0xFFu);
    for (uint32_t i = 0; i < len; i++) {
        g_ring[(uint32_t)(pos++ % KLOG_RING_SIZE)] = text[i];
    }

    g_stored_total = pos;
    g_render_total += rec_render_len(ms, len);
    spin_unlock_irqrestore(&g_klog_lock, flags);
    return true;
}

void klog_record_text(uint32_t ms, const char *text, uint32_t len,
                      bool already_on_console) {
    if (!text) return;
    if (len > KLOG_REC_MAX) { len = KLOG_REC_MAX; g_truncations++; }
    (void)ring_append(ms, text, len, already_on_console);
}

/* --- The consumer (Y5c) -------------------------------------------------- */

static int      g_consumer_pid = -1;
static uint64_t g_cursor;        /* rendered position the consumer has written to */
static uint64_t g_cursor_spos;   /* and the stored position of the next record */
static uint64_t g_gap_bytes;
static uint32_t g_gaps;

void klog_set_consumer(int pid) {
    /* From here, not from zero: everything before this point already went out
     * through the inline fan-out, and replaying it would print the whole boot
     * a second time. */
    g_cursor      = g_render_total;
    g_cursor_spos = g_stored_total;
    g_consumer_pid = pid;
}

static bool consumer_live(void) {
    if (g_consumer_pid < 0) return false;
    /* A plain read of the task table, no lock -- which is what lets this be
     * asked from a producer that holds a spinlock. */
    int st = sched_task_state(g_consumer_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

bool klog_has_consumer(void) { return consumer_live(); }

uint64_t klog_gap_bytes(void) { return g_gap_bytes; }
uint32_t klog_gaps(void)      { return g_gaps; }

/* Pushes bytes at every attached sink. Callers: the inline fan-out below
 * (before a consumer exists) and klog_drain() (after). */
static void sinks_write(const char *s, uint32_t len) {
    for (int i = 0; i < KLOG_MAX_SINKS; i++) {
        if (!(g_sinks[i].in_use && g_sinks[i].attached && g_sinks[i].putc)) continue;
        for (uint32_t j = 0; j < len; j++) g_sinks[i].putc(s[j]);
    }
}

static uint32_t klog_read_locked(uint64_t abs_offset, char *buf, uint32_t count);

/* Renders the next *record* for the consumer and advances past it, both under
 * the ring lock, returning how many bytes it produced. Zero means caught up.
 *
 * One record at a time, and the read and the advance are one operation. Both
 * were lessons rather than choices:
 *
 *  - Two drainers exist -- klogd, and cprintf()/readline_interactive(), which
 *    drain directly so the log and console streams stay in order where they
 *    meet. When the read and the cursor advance were separate, with a
 *    blocking sink write between them, two drainers straddled each other and
 *    emitted the same records twice.
 *  - Per record rather than per byte-range, because a record that
 *    printk_critical() already wrote to the console live has to be *skipped*,
 *    and skipping is a record-level operation. Its text stays in the ring for
 *    /proc/kmsg; it simply must not be echoed.
 *
 * `buf` needs room for the largest record plus its rendered timestamp; the
 * callers size it from KLOG_REC_MAX. A record too big for `buf` is truncated
 * rather than skipped -- losing the tail of one line beats losing the line. */
static uint32_t klog_take(char *buf, uint32_t cap) {
    uintptr_t flags = spin_lock_irqsave_bottom(&g_klog_lock);

    if (g_cursor_spos < g_stored_oldest) {
        g_cursor_spos = g_stored_oldest;
        g_cursor      = g_render_oldest;
    }

    while (g_cursor_spos < g_stored_total) {
        uint32_t ms, raw;
        rec_header(g_cursor_spos, &ms, &raw);
        uint32_t len = rec_len_of(raw);

        char ts[16];
        uint32_t ts_len = ts_render(ms, ts);

        if (rec_on_console(raw)) {
            /* Already on the wire, live, from the fault path. Step over it. */
            g_cursor_spos += KLOG_HDR_LEN + len;
            g_cursor      += ts_len + len;
            continue;
        }

        uint32_t n = 0;
        for (uint32_t i = 0; i < ts_len && n < cap; i++) buf[n++] = ts[i];
        for (uint32_t i = 0; i < len    && n < cap; i++) {
            buf[n++] = ring_at(g_cursor_spos + KLOG_HDR_LEN + i);
        }
        g_cursor_spos += KLOG_HDR_LEN + len;
        g_cursor      += ts_len + len;
        spin_unlock_irqrestore(&g_klog_lock, flags);
        return n;
    }

    spin_unlock_irqrestore(&g_klog_lock, flags);
    return 0;
}

void klog_drain(void) {
    /* Safe to call from anywhere, because it refuses everywhere it would not
     * be. That is what lets the console stream's own entry points call it
     * without each having to reason about its context.
     *
     * A serve callback: draining is console I/O, ending in the same
     * chan_call() to the uart task that makes cprintf() unsafe there. It is
     * also the reason this guard is here rather than in cprintf() -- without
     * it a single violation would be reported twice, once for the drain and
     * once for the write, and the write is the violation.
     *
     * A spinlock held, or no task: this call can block, and neither context
     * may. The records stay in the ring and klogd writes them out. */
    if (lock_noprintk_what() != NULL) return;
    if (lock_spin_held() != NULL) return;
    if (!sched_has_task()) return;

    /* Serialised against cprintf(), which still owns this lock, so a drained
     * burst cannot land in the middle of a shell line. Taken once per pass
     * rather than once per record: the console reads better and the lock is
     * taken far less often.
     *
     * This task may block here. Nothing waits on it, which is exactly why it
     * may -- see kernel/klogd.c. */
    if (klog_total() == g_cursor && g_gap_bytes == 0) return;

    printk_lock();

    /* Bytes that were evicted before we reached them. Reported in the stream,
     * at the position where they were lost, rather than stored as a record --
     * a record would have to go at the *end* of the ring, which is the one
     * place the gap did not happen. `klog` reports the totals afterwards. */
    if (g_cursor < klog_oldest()) {
        uint64_t lost = klog_oldest() - g_cursor;
        g_gap_bytes += lost;
        g_gaps++;
        g_cursor = klog_oldest();

        char note[80];
        uint32_t n = 0;
        const char *a = "\n[klogd] ";
        while (*a) note[n++] = *a++;
        char d[24]; uint32_t dn = 0;
        uint64_t v = lost;
        if (v == 0) d[dn++] = '0';
        while (v > 0) { d[dn++] = (char)('0' + (uint32_t)(v % 10u)); v /= 10u; }
        while (dn > 0) note[n++] = d[--dn];
        const char *b = " bytes of log were dropped (ring too small for the burst)\n";
        while (*b) note[n++] = *b++;
        sinks_write(note, n);
    }

    /* Whole records, in order, until caught up. The chunk is a stack buffer
     * rather than the whole remainder: a consumer that tried to take the
     * entire backlog at once would need a buffer the size of the ring. */
    char chunk[KLOG_REC_MAX + 16];
    for (;;) {
        uint32_t n = klog_take(chunk, sizeof(chunk));
        if (n == 0) break;
        sinks_write(chunk, n);
    }

    printk_unlock();
}

void klog_emit(uint32_t ms, const char *text, uint32_t len) {
    if (!text) return;
    if (len > KLOG_REC_MAX) { len = KLOG_REC_MAX; g_truncations++; }
    (void)ring_append(ms, text, len, false);

    if (consumer_live()) {
        /* The producer's whole remaining job: a non-blocking signal.
         *
         * Skipped when a spinlock is held, because task_unblock() takes
         * g_sched_lock and a nested spinlock is what Y2's leaf check refuses
         * -- correctly. The record is already in the ring either way; the
         * consumer's bounded sleep is what gets it out when no wake was safe
         * to send. Never a reason to block here, which is the invariant this
         * milestone exists to establish. */
        if (lock_spin_held() == NULL) (void)task_unblock(g_consumer_pid);
        return;
    }

    /* No consumer yet: drain inline, exactly as before Y5c -- including the
     * lock, which this path must keep.
     *
     * printk() itself no longer takes printk_lock(), and dropping it here too
     * spliced boot output mid-word:
     *
     *     [    0.005] [No[    0.007] [USB CDC] Not built for this target.
     *     de] rv32-nommu-fdb2 (name: derived; ...)
     *
     * The second writer is printk_debug(), which goes straight to the UART
     * registers by design -- "physical UART, always, no exceptions" -- and
     * bypasses klog entirely. It still takes printk_lock(); printk() no
     * longer did; so the two stopped excluding each other. §5.6 predicted
     * this hazard and named cprintf() as the other writer, and missed this
     * one. The ring was correct throughout: only the console was spliced.
     *
     * Taking it here is not a blocking printk() by the back door. This path
     * runs only while no consumer is registered, which is boot -- and once
     * klogd exists the fan-out (and the lock with it) belongs to klog_drain()
     * on a task that is allowed to block. The window where tasks exist and a
     * consumer does not is the two lines between uart_task_start() and
     * klogd_start(), and it behaves exactly as every release before this one.
     *
     * printk_unlock() also flushes the UART's TX batch, which is why boot
     * output appears as it is produced rather than in 256-byte lumps. */
    unsigned h = hart_id();
    if (g_in_fanout[h]) return;
    g_in_fanout[h] = true;

    printk_lock();
    char ts[16];
    uint32_t ts_len = ts_render(ms, ts);
    sinks_write(ts, ts_len);
    sinks_write(text, len);
    g_cursor      = g_render_total;   /* stays caught up while we are the drain */
    g_cursor_spos = g_stored_total;
    printk_unlock();

    g_in_fanout[h] = false;
}

void klog_truncated(void) { g_truncations++; }

uint32_t klog_truncations(void) { return g_truncations; }
uint32_t klog_drops(void)       { return g_drops; }

uint64_t klog_stored_bytes(void) { return g_stored_total - g_stored_oldest; }

uint32_t klog_records(void) {
    uintptr_t flags = spin_lock_irqsave_bottom(&g_klog_lock);
    uint32_t n = 0;
    uint64_t pos = g_stored_oldest;
    while (pos < g_stored_total) {
        uint32_t ms, raw;
        rec_header(pos, &ms, &raw);
        (void)ms;
        pos += KLOG_HDR_LEN + rec_len_of(raw);
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
    uintptr_t flags = spin_lock_irqsave_bottom(&g_klog_lock);
    uint32_t n = klog_read_locked(abs_offset, buf, count);
    spin_unlock_irqrestore(&g_klog_lock, flags);
    return n;
}

/* The body, with the ring lock already held -- klog_take() needs to read and
 * advance the cursor in one critical section, and could not do that if the
 * read took the lock itself. */
static uint32_t klog_read_locked(uint64_t abs_offset, char *buf, uint32_t count) {
    /* Caller fell off the back of the ring (the log wrapped past what it was
     * reading): resume at the oldest byte still held rather than returning
     * stale or garbage content. */
    if (abs_offset < g_render_oldest) abs_offset = g_render_oldest;
    if (abs_offset >= g_render_total) return 0;
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
        uint32_t ms, raw;
        rec_header(spos, &ms, &raw);
        uint32_t len = rec_len_of(raw);

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

    return written;
}
