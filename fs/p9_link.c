#include "fs/p9_link.h"
#include "kernel/sha256.h"
#include "kernel/identity.h"
#include "fs/9p.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include "kernel/irq.h"
#include "kernel/scratch.h"
#include <string.h>

/* Small fixed set, not one global pointer: RP2350 wants both its USB-CDC
 * net link (ACM1/EP4) and, once a user opts into `p9share`, the UART demux
 * link running as background servers at the same time; QEMU wants
 * virtio-console plus (for `p9share` testing) the UART demux. All of this
 * is still driven from a single call stack with no real concurrency (see
 * p9_link_cat()'s own comment below on why that's safe), so servicing N
 * links in one p9_link_background_poll() sweep carries no new race that
 * servicing 1 didn't already have. */
/* Four since R3 (plan/phase19_ip_stack_and_ethernet.md): a board can now
 * carry its own dedicated link plus up to two accepted TCP connections, each
 * of which registers here on establishment and unregisters on close. The
 * table is four pointers; the old limit of two was sized when links were only
 * ever soldered. */
#define P9_LINK_MAX_BACKGROUND 4

/* How long a client waits for a reply before calling it a failure. See the
 * wait loop in p9_client_rpc() for why this is not "forever". */
#define P9_CLIENT_TIMEOUT_MS 10000
static p9_link_t *g_background_links[P9_LINK_MAX_BACKGROUND];

/* --- B2: inbound frame demultiplexing (the D5 gate) ---
 *
 * A4's completion notes justified a node being both 9P client and server on
 * one link by arguing that nothing could run concurrently with a synchronous
 * C call, because there was no scheduler. B2 makes that false. A client
 * waiting for its reply now yields, so this node's own background pump can
 * run during the wait -- and would happily read the *reply* off the wire and
 * feed it to p9_server_process() as though it were a fresh request.
 *
 * The fix is that every inbound frame passes through one routing point,
 * used by both the background pump and by client waits. 9P makes the
 * classification trivial and exact: T-messages (requests) have even type
 * numbers and R-messages (replies) odd ones -- Tversion=100/Rversion=101,
 * Twalk=110/Rwalk=111, and so on for every pair. A reply is then matched to
 * a registered waiter by tag; a request goes to the server as before.
 *
 * Waiters live in a small fixed table rather than in p9_link_t, so backends
 * (drivers/virtio_console.c, usb_cdc.c, uart_net.c) need no changes and no
 * per-link 4 KB reply buffer is paid for on links that never act as client. */

/* --- Serialising access to the 9P layer's shared buffers (B6) ---
 *
 * p9_link_pump() and p9_link_roundtrip() each use static buffers, which was
 * safe while cooperative scheduling meant only one task could be inside them
 * at a time. Preemption removes that: the p9srv server task and a client task
 * (whose reply-wait loop also pumps) can now be inside the same buffer
 * simultaneously. The symptom was intermittent -- the two multi-node tests
 * failed in roughly one run in three -- which is exactly how this class of bug
 * presents and exactly why it is worth fixing rather than re-running.
 *
 * A yielding lock, not interrupt masking: these regions block (a roundtrip
 * waits for a peer's reply), and masking interrupts across a wait would stop
 * the very timer that lets the peer's reply be processed. Taking the flag is
 * itself done with interrupts masked so test-and-set is atomic.
 *
 * Separate locks for pump and client, deliberately: a client holding the
 * client lock while waiting for a reply must not block the pump that delivers
 * it. */
/* Re-entrant for the owning task, and that is not a convenience -- it is
 * required. A locally-mounted namespace can be walked into recursively
 * (/self/self/...), in which case the 9P server handler re-enters the client
 * path *on the same task*. A plain lock deadlocks there: the task waits
 * forever for something it already holds. The recursion is still bounded, by
 * chan_call()'s single-slot re-entrancy check, which rejects the inner call
 * exactly as it did before preemption existed. */
typedef struct {
    volatile bool held;
    volatile int  owner;
    volatile int  depth;
} p9_lock_t;

static void p9_lock(p9_lock_t *l) {
    int me = sched_current_pid();
    for (;;) {
        uintptr_t f = irq_save();
        if (!l->held) {
            l->held = true; l->owner = me; l->depth = 1;
            irq_restore(f);
            return;
        }
        if (l->owner == me) {
            l->depth++;
            irq_restore(f);
            return;
        }
        irq_restore(f);
        sched_yield();
    }
}

static void p9_unlock(p9_lock_t *l) {
    uintptr_t f = irq_save();
    if (l->depth > 0 && --l->depth == 0) {
        l->held = false;
        l->owner = -1;
    }
    irq_restore(f);
}

static p9_lock_t g_pump_lock;
static p9_lock_t g_client_lock;

#define P9_LINK_MAX_WAITERS 2

typedef struct {
    p9_link_t *link;          /* NULL = slot free */
    uint16_t   tag;
    bool       have_reply;
    uint32_t   reply_len;
    uint8_t    reply[P9_MAX_MSIZE];
} p9_waiter_t;

static p9_waiter_t g_waiters[P9_LINK_MAX_WAITERS];

static p9_waiter_t *waiter_begin(p9_link_t *link, uint16_t tag) {
    for (int i = 0; i < P9_LINK_MAX_WAITERS; i++) {
        if (!g_waiters[i].link) {
            g_waiters[i].link = link;
            g_waiters[i].tag = tag;
            g_waiters[i].have_reply = false;
            g_waiters[i].reply_len = 0;
            return &g_waiters[i];
        }
    }
    return NULL;
}

static void waiter_end(p9_waiter_t *w) {
    if (w) w->link = NULL;
}

/* Routes one already-received frame. Requests are answered here; replies are
 * parked for their waiter. Returns 1 if handled, -1 on a transport error. */
static int p9_route_frame(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    if (len < 7) return -1;

    uint8_t type = buf[4];              /* size[4] type[1] tag[2] */
    uint16_t tag = (uint16_t)(buf[5] | ((uint16_t)buf[6] << 8));

    if (type & 1u) {
        /* R-message: a reply to something this node asked for. */
        for (int i = 0; i < P9_LINK_MAX_WAITERS; i++) {
            p9_waiter_t *w = &g_waiters[i];
            if (w->link != link || w->tag != tag || w->have_reply) continue;
            if (len > sizeof(w->reply)) return -1;
            memcpy(w->reply, buf, len);
            w->reply_len = len;
            w->have_reply = true;
            return 1;
        }
        /* Nobody is waiting for this. Dropping it is correct and worth
         * saying out loud -- silently feeding a reply to the server would
         * produce an Rerror sent back to a peer that never asked anything,
         * which is precisely the confusion this routing exists to prevent. */
        printk("[9P Link] Dropped unexpected reply (type %d, tag %d) on '%s'\n",
               (int)type, (int)tag, link->name ? link->name : "?");
        return 1;
    }

    /* T-message: a genuine request for this node's server. */
    static uint8_t resp_buf[P9_MAX_MSIZE];
    int resp_len = p9_server_process(buf, len, resp_buf, sizeof(resp_buf),
                                     link->auth_required ? P9_AUTH_REQUIRED
                                                         : P9_AUTH_NOT_REQUIRED);
    if (resp_len < 7) return -1;
    if (link->send_frame(link, resp_buf, (uint32_t)resp_len) < 0) return -1;
    return 1;
}

/* Receives at most one frame from `link` and routes it. */
static int p9_link_pump(p9_link_t *link) {
    if (!link || !link->poll || !link->recv_frame || !link->send_frame) return -1;

    static uint8_t rx_buf[P9_MAX_MSIZE];

    p9_lock(&g_pump_lock);
    int ready = link->poll(link);
    if (ready <= 0) { p9_unlock(&g_pump_lock); return ready; }

    int rx_len = link->recv_frame(link, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) { p9_unlock(&g_pump_lock); return -1; }

    int r = p9_route_frame(link, rx_buf, (uint32_t)rx_len);
    p9_unlock(&g_pump_lock);
    return r;
}

int p9_link_service(p9_link_t *link) {
    return p9_link_pump(link);
}

void p9_link_register_background(p9_link_t *link) {
    if (!link) return;
    for (int i = 0; i < P9_LINK_MAX_BACKGROUND; i++) {
        if (g_background_links[i] == link) return; // already registered
    }
    for (int i = 0; i < P9_LINK_MAX_BACKGROUND; i++) {
        if (!g_background_links[i]) {
            g_background_links[i] = link;
            printk("[9P Link] '%s' registered as background transport.\n", link->name ? link->name : "?");
            return;
        }
    }
    printk("[9P Link] background transport slots full; '%s' not registered.\n", link->name ? link->name : "?");
}

void p9_link_unregister_background(p9_link_t *link) {
    for (int i = 0; i < P9_LINK_MAX_BACKGROUND; i++) {
        if (g_background_links[i] == link) g_background_links[i] = NULL;
    }
}

/* --- The 9P server as a task (B4, resolving D4) ---
 *
 * Until now the server ran opportunistically: p9_link_background_poll() was
 * called from inside uart_getc()'s busy-wait, so inbound 9P was serviced only
 * while the console happened to be blocked on a keystroke. That was the only
 * place such a pump *could* live before B2 -- there was no scheduler -- and
 * it means a node busy doing anything else silently stops answering its peers.
 *
 * As a task it is scheduled like anything else: it services every registered
 * background link and yields. A peer gets answered whenever this node yields,
 * not only when someone is waiting at the prompt.
 *
 * ## This is also the filesystem server
 *
 * There is no separate filesystem server to build, and that is a conclusion
 * rather than an omission. Rule 2 (§5.1) makes 9P the service protocol, and
 * the 9P server's handlers *are* VFS calls -- Tread is vfs_pread(), Twalk is
 * vfs_stat(), and so on. A distinct "fs server" behind its own channel would
 * be a second protocol saying the same things, reachable only locally, which
 * is exactly the duplication Rule 2 exists to avoid. Making this a task is
 * therefore what makes the filesystem a server: one component, reachable
 * identically by a local channel (/srv/p9, B1) or by a peer over a wire.
 */
static void p9_server_task_body(void *arg) {
    (void)arg;
    for (;;) {
        p9_link_background_poll();
        sched_yield();
    }
}

int p9_server_task_start(void) {
    /* Three pages, not task_create()'s default two.
     *
     * This task is a 9P server that is also, since N5, a 9P *client*: a
     * request that lands on a remote mount runs p9_link_cat()'s exchange --
     * its own request buffer, reply buffer and deserialised message -- nested
     * inside the server frame already holding the same three things for the
     * inbound request. Two kilobytes of msize appears twice on one stack.
     *
     * Measured, not guessed: 8 KB read 4288/8192 used before a mount existed
     * and **8192/8192 afterwards** -- filled to the last byte, which is not a
     * high-water mark but an overflow that `ps` had no way to distinguish
     * from a snug fit. It presented as the network dying a few minutes after
     * a test run with every W5500 register still reading correct, and it cost
     * a long detour through the hardware before the number was noticed. */
    int pid = task_create_sized("p9srv", p9_server_task_body, NULL, 3);
    if (pid < 0) {
        printk("[9P Link] Could not start the server task; falling back to "
               "opportunistic polling only.\n");
    }
    return pid;
}

void p9_link_background_poll(void) {
    for (int i = 0; i < P9_LINK_MAX_BACKGROUND; i++) {
        if (g_background_links[i]) p9_link_service(g_background_links[i]);
    }
}

/* --- p9_link_cat: a link-agnostic synchronous 9P client (A4) ---
 *
 * Safe to run on a link that also has a registered background server,
 * including on itself -- but the *reason* changed in B2, and the old reason
 * is worth recording because it was load-bearing and is now false.
 *
 * A4 argued this was safe because nothing could run concurrently with a
 * synchronous C call: there was no scheduler, so this link's own background
 * pump provably could not run mid-exchange and misread a reply as a request.
 * B2's scheduler destroys that argument -- the wait below yields, precisely
 * so it does not starve everything else, which is exactly the window A4
 * relied on not existing.
 *
 * What makes it safe now is p9_route_frame() above: every inbound frame is
 * classified as request or reply by its 9P type parity and, if a reply,
 * matched to a registered waiter by tag. Client and server roles can now
 * genuinely coexist on one link rather than by convention. */

/* Sends one request and waits for the reply carrying its tag.
 *
 * Registers a waiter *before* sending, so a reply that arrives while this
 * task is descheduled is parked rather than mistaken for a request. The wait
 * loop pumps the link itself and yields, so other tasks (and this link's own
 * server role) keep running instead of being starved by a busy-wait -- which
 * is also why the routing in p9_route_frame() is load-bearing rather than
 * belt-and-braces.
 *
 * Still no timeout, matching drivers/virtio_blk.c's busy-wait-until-done
 * precedent: an unresponsive peer blocks this task, but with a scheduler it
 * no longer blocks the whole system. */
static int p9_link_roundtrip(p9_link_t *link, const p9_msg_t *req, p9_msg_t *resp,
                              uint8_t *tx, uint8_t *rx, uint32_t cap) {
    /* Held across the whole exchange: `tx` and `rx` are shared static buffers,
     * so a second client task entering here mid-transaction would overwrite a
     * request still in flight. */
    p9_lock(&g_client_lock);

    int tx_len = p9_serialize(req, tx, cap);
    if (tx_len < 7) { p9_unlock(&g_client_lock); return -1; }

    p9_waiter_t *w = waiter_begin(link, req->tag);
    if (!w) {
        printk("[9P Link] No free reply-waiter slot for tag %d\n", (int)req->tag);
        p9_unlock(&g_client_lock);
        return -1;
    }

    if (link->send_frame(link, tx, (uint32_t)tx_len) < 0) {
        waiter_end(w);
        p9_unlock(&g_client_lock);
        return -1;
    }

    /* Bounded, since N5 (plan/phase18_networking_and_auth.md §9's own risk
     * entry, met on hardware the first time a downlink cable was tried).
     *
     * This used to wait forever, matching drivers/virtio_blk.c's
     * busy-wait-until-done convention -- defensible when the peer is a device
     * on the same board, and not when it is another board on the end of a
     * cable. A gateway whose downlink is unplugged, or whose peer is not
     * serving, would hang the task that asked: the shell, and with it any
     * hope of finding out why. Ten seconds is far longer than any real
     * round trip on any transport here (a 2 KB msize at 115200 baud is about
     * 350 ms) and far shorter than "never".
     *
     * The failure is reported as a failure, and the caller decides. */
    uint64_t deadline = time_get_ms() + P9_CLIENT_TIMEOUT_MS;
    for (;;) {
        if (w->have_reply) {
            uint32_t n = w->reply_len;
            if (n > cap) { waiter_end(w); p9_unlock(&g_client_lock); return -1; }
            memcpy(rx, w->reply, n);
            waiter_end(w);
            int r = p9_deserialize(rx, n, resp);
            p9_unlock(&g_client_lock);
            return r;
        }
        /* Pumping from inside the wait is what lets a reply arrive at all when
         * the server task has not been scheduled yet. It takes the *pump* lock,
         * not this one, so the two never deadlock against each other. */
        if (p9_link_pump(link) < 0) { waiter_end(w); p9_unlock(&g_client_lock); return -1; }
        if (time_get_ms() > deadline) {
            printk("[9P Link] No reply on '%s' within %d ms (tag %d) -- peer not "
                   "serving, or the wire is not connected\n",
                   link->name ? link->name : "?", (int)P9_CLIENT_TIMEOUT_MS,
                   (int)req->tag);
            waiter_end(w);
            p9_unlock(&g_client_lock);
            return -1;
        }
        sched_yield();
    }
}

int p9_link_cat(p9_link_t *link, const char *path, char *out_buf, uint32_t out_max) {
    if (!link || !link->poll || !link->send_frame || !link->recv_frame || !path) return -1;
    while (*path == '/') path++;

    /* §3.1 (plan/phase15_memory_reclamation.md): on-demand, not .bss.
     *
     * This is the *client* side -- one `p9cat` against a peer, start to
     * finish -- so it is exactly the "not live at boot, not on a hot path"
     * case the rule is for. The server-side buffers further up this file
     * (resp_buf, rx_buf, g_waiters) deliberately stay static: those are
     * touched once per inbound frame by a task whose whole job is serving
     * frames, and a page allocation per 9P message would be the wrong trade. */
    scratch_t sc;
    if (!scratch_acquire(&sc, 2u * P9_MAX_MSIZE)) { scratch_release(&sc); return -1; }
    uint8_t *tx = (uint8_t *)sc.base;
    uint8_t *rx = tx + P9_MAX_MSIZE;
    p9_msg_t req, resp;

    memset(&req, 0, sizeof(req));
    req.type = P9_TVERSION;
    req.tag = 1;
    req.msize = P9_MAX_MSIZE;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, P9_MAX_MSIZE) < 0 || resp.type != P9_RVERSION) { scratch_release(&sc); return -1; }

    /* What the peer agreed to, which may be less than we asked for -- an
     * RP2350 node answers 2048 (§2.3, plan/phase15_memory_reclamation.md).
     * The Tread chunk below is sized from this rather than from a constant,
     * so it cannot outgrow what the far side will accept. The reply is
     * clamped to our own maximum too: a peer is free to answer with more
     * than it was offered, and our receive buffer is P9_MAX_MSIZE. */
    uint32_t peer_msize = resp.msize;
    if (peer_msize == 0 || peer_msize > P9_MAX_MSIZE) peer_msize = P9_MAX_MSIZE;
    uint32_t chunk = (peer_msize > P9_IOHDRSZ) ? (peer_msize - P9_IOHDRSZ) : 0;
    if (chunk > 1024) chunk = 1024;   /* the long-standing read granularity */

    memset(&req, 0, sizeof(req));
    req.type = P9_TATTACH;
    req.tag = 2;
    req.fid = 1;
    strncpy(req.uname, node_name(), sizeof(req.uname) - 1);
    // aname left empty -> namespace root "/"
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, P9_MAX_MSIZE) < 0 || resp.type != P9_RATTACH) { scratch_release(&sc); return -1; }

    memset(&req, 0, sizeof(req));
    req.type = P9_TWALK;
    req.tag = 3;
    req.fid = 1;
    req.newfid = 2;
    char pathcopy[128];
    strncpy(pathcopy, path, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';
    uint16_t n = 0;
    char *tok = pathcopy;
    while (*tok && n < P9_MAX_WALK_ELEM) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok) {
            strncpy(req.wname[n], tok, P9_MAX_NAME_LEN - 1);
            n++;
        }
        if (!slash) break;
        tok = slash + 1;
    }
    req.nwname = n;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, P9_MAX_MSIZE) < 0 || resp.type != P9_RWALK || resp.nwqid != n) { scratch_release(&sc); return -1; }

    memset(&req, 0, sizeof(req));
    req.type = P9_TOPEN;
    req.tag = 4;
    req.fid = 2;
    req.mode = P9_OREAD;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, P9_MAX_MSIZE) < 0 || resp.type != P9_ROPEN) { scratch_release(&sc); return -1; }

    uint32_t total = 0;
    uint64_t offset = 0;
    uint16_t tag = 5;
    bool read_failed = false;
    for (;;) {
        uint32_t room = (out_buf && out_max > 0 && total + 1 < out_max) ? (out_max - 1 - total) : 0;
        uint32_t want = room < chunk ? room : chunk;
        if (want == 0) break;

        memset(&req, 0, sizeof(req));
        req.type = P9_TREAD;
        req.tag = tag++;
        req.fid = 2;
        req.offset = offset;
        req.count = want;
        if (p9_link_roundtrip(link, &req, &resp, tx, rx, P9_MAX_MSIZE) < 0 || resp.type != P9_RREAD) { read_failed = true; break; }
        if (resp.count == 0) break;

        if (out_buf) memcpy(out_buf + total, resp.data, resp.count);
        total += resp.count;
        offset += resp.count;
    }
    if (out_buf && out_max > 0) out_buf[(total < out_max) ? total : out_max - 1] = '\0';

    memset(&req, 0, sizeof(req));
    req.type = P9_TCLUNK;
    req.tag = tag;
    req.fid = 2;
    p9_link_roundtrip(link, &req, &resp, tx, rx, P9_MAX_MSIZE); // best-effort cleanup

    scratch_release(&sc);
    return read_failed ? -1 : (int)total;
}

/* --- Persistent remote 9P connection (A5) ---
 *
 * A fixed pool, not dynamic allocation (this kernel has none): one mount
 * per real remote peer is the expected usage (see p9_remote_mount_open()'s
 * doc comment in the header for why mounting the same underlying link
 * twice isn't meaningful -- the second Tversion would reset the first
 * mount's fids server-side). */
#define P9_REMOTE_MAX_MOUNTS 4

struct p9_remote_mount {
    bool in_use;
    p9_link_t *link;
    uint32_t root_fid;
    uint32_t next_fid;
    uint16_t next_tag;
    /* What this peer's Rversion agreed to. Every Tread against the mount is
     * chunked from it rather than from a constant, so a peer with a smaller
     * msize than ours -- an RP2350 node, §2.3 -- is never asked for more
     * than it can frame. */
    uint32_t msize;
};

static p9_remote_mount_t g_remote_mounts[P9_REMOTE_MAX_MOUNTS];
static uint8_t g_remote_tx[P9_MAX_MSIZE];
static uint8_t g_remote_rx[P9_MAX_MSIZE];

/* The largest Tread this mount may ask for: bounded by what the peer's
 * Rversion agreed, and by the 1024-byte granularity these directory scans
 * have always used. */
static uint32_t p9_remote_read_chunk(const p9_remote_mount_t *m) {
    uint32_t msize = (m && m->msize > 0) ? m->msize : P9_MAX_MSIZE;
    uint32_t chunk = (msize > P9_IOHDRSZ) ? (msize - P9_IOHDRSZ) : 0;
    return chunk > 1024 ? 1024 : chunk;
}

static uint16_t p9_remote_next_tag(p9_remote_mount_t *m) {
    uint16_t t = m->next_tag++;
    if (m->next_tag == 0) m->next_tag = 1; // skip 0 on wraparound, no protocol reason to avoid it beyond tidiness
    return t;
}

static int p9_remote_xchg(p9_remote_mount_t *m, const p9_msg_t *req, p9_msg_t *resp) {
    return p9_link_roundtrip(m->link, req, resp, g_remote_tx, g_remote_rx, sizeof(g_remote_tx));
}

/* --- The client half of the auth exchange (R3b,
 * plan/phase19_ip_stack_and_ethernet.md) ---
 *
 * Phase 18 built the gate and the host-side client for it; the *in-kernel*
 * client never had one, because until R3b no node could dial another over a
 * network. Now one can, and a node that cannot authenticate can only mount
 * peers that ask nothing -- which is to say, not the gateway, and not
 * anything else worth mounting.
 *
 * The exchange mirrors fs/9p.c's server side and host/p9lib's
 * Session.authenticate():
 *
 *     Tauth  afid, uname, aname
 *     Tread  afid  -> a 32-byte nonce the server chose
 *     Twrite afid  <- HMAC-SHA256(key, nonce | uname | aname)
 *
 * The key never crosses the wire. uname and aname are inside the MAC, which
 * is what stops a response captured for one identity being replayed as
 * another.
 *
 * Attempted whenever this node has a key at all. A server that wants no
 * authentication answers Tauth with an error, and that is not a failure --
 * it is the answer, and the attach then goes ahead with no afid. Doing it
 * this way means neither side has to be told what the other's policy is.
 *
 * Returns the afid to attach with, or P9_NOFID when no authentication
 * happened (either we have no key, or the peer wants none). Returns -1 as an
 * afid value never: a refusal that *should* have worked shows up as a failed
 * attach, which is where the operator will look. */
static uint32_t p9_remote_authenticate(p9_remote_mount_t *m, const char *uname) {
    uint8_t key[P9_AUTH_KEY_MAX];
    uint32_t key_len = 0;
    /* p9_auth_own_key(), not p9_auth_key_for(): this is always called with
     * `uname` == node_name() (this node's own name) -- see the one call
     * site below -- so it is "how do I prove myself", not "who may attach
     * to me". I5 split the two on purpose (fs/9p.h's own comment). */
    if (p9_auth_own_key(key, &key_len) != 0 || key_len == 0) return P9_NOFID;

    p9_msg_t req, resp;
    uint32_t afid = m->next_fid++;

    memset(&req, 0, sizeof(req));
    req.type = P9_TAUTH;
    req.tag = p9_remote_next_tag(m);
    req.afid = afid;
    strncpy(req.uname, uname, sizeof(req.uname) - 1);
    if (p9_remote_xchg(m, &req, &resp) < 0 || resp.type != P9_RAUTH) {
        /* The peer does not want authentication. Give the fid number back so
         * the attach's own fid does not skip a slot in a table of eight. */
        m->next_fid--;
        return P9_NOFID;
    }

    memset(&req, 0, sizeof(req));
    req.type = P9_TREAD;
    req.tag = p9_remote_next_tag(m);
    req.fid = afid;
    req.offset = 0;
    req.count = P9_AUTH_NONCE_LEN;
    if (p9_remote_xchg(m, &req, &resp) < 0 || resp.type != P9_RREAD ||
        resp.count != P9_AUTH_NONCE_LEN || !resp.data) {
        return P9_NOFID;
    }

    /* nonce | uname | aname, with aname empty: the same bytes fs/9p.c hashes,
     * in the same order. A mismatch here is invisible until an attach is
     * refused, so the two are worth reading side by side. */
    uint8_t msg[P9_AUTH_NONCE_LEN + 2u * P9_MAX_NAME_LEN];
    uint32_t n = 0;
    memcpy(msg, resp.data, P9_AUTH_NONCE_LEN);
    n = P9_AUTH_NONCE_LEN;
    for (const char *u = uname; *u; u++) msg[n++] = (uint8_t)*u;
    /* aname is empty -- the peer's namespace root -- so nothing more. */

    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256(key, key_len, msg, n, mac);

    memset(&req, 0, sizeof(req));
    req.type = P9_TWRITE;
    req.tag = p9_remote_next_tag(m);
    req.fid = afid;
    req.offset = 0;
    req.count = SHA256_DIGEST_LEN;
    req.data = mac;
    if (p9_remote_xchg(m, &req, &resp) < 0 || resp.type != P9_RWRITE ||
        resp.count != SHA256_DIGEST_LEN) {
        return P9_NOFID;
    }
    return afid;
}

p9_remote_mount_t *p9_remote_mount_open(p9_link_t *link) {
    if (!link) return NULL;

    p9_remote_mount_t *m = NULL;
    for (int i = 0; i < P9_REMOTE_MAX_MOUNTS; i++) {
        if (!g_remote_mounts[i].in_use) { m = &g_remote_mounts[i]; break; }
    }
    if (!m) return NULL;

    memset(m, 0, sizeof(*m));
    m->link = link;
    m->next_fid = 1;
    m->next_tag = 1;

    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TVERSION;
    req.tag = p9_remote_next_tag(m);
    req.msize = P9_MAX_MSIZE;
    if (p9_remote_xchg(m, &req, &resp) < 0 || resp.type != P9_RVERSION) return NULL;

    m->msize = (resp.msize > 0 && resp.msize <= P9_MAX_MSIZE) ? resp.msize : P9_MAX_MSIZE;

    /* This node's own name, not a constant. The uname is what the far end's
     * key store is indexed by (fs/9p.c's p9_auth_key_for()) and what the auth
     * MAC covers, so it is the difference between "some LugalOS board
     * attached" and "the clock attached" -- which is the whole point of phase
     * 18 §6's "multiple keys identify who". */
    uint32_t afid = p9_remote_authenticate(m, node_name());

    uint32_t root_fid = m->next_fid++;
    memset(&req, 0, sizeof(req));
    req.type = P9_TATTACH;
    req.tag = p9_remote_next_tag(m);
    req.fid = root_fid;
    /* Explicitly P9_NOFID when there was no auth. It used to be left as the
     * zero memset() put there, which is a *valid fid number*, and which an
     * auth-requiring server duly looked up and rejected as "not an auth fid"
     * -- a confusing way to fail for the right reason. */
    req.afid = afid;
    strncpy(req.uname, node_name(), sizeof(req.uname) - 1);
    // aname left empty -> the peer's namespace root "/"
    if (p9_remote_xchg(m, &req, &resp) < 0 || resp.type != P9_RATTACH) return NULL;

    m->root_fid = root_fid;
    m->in_use = true;
    return m;
}

void p9_remote_mount_close(p9_remote_mount_t *m) {
    if (!m || !m->in_use) return;
    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TCLUNK;
    req.tag = p9_remote_next_tag(m);
    req.fid = m->root_fid;
    p9_remote_xchg(m, &req, &resp); // best-effort
    memset(m, 0, sizeof(*m));
}

/* Splits `path` into up to P9_MAX_WALK_ELEM wname[] components in `out`
 * (used by open/remove/mkdir below, all of which need the same
 * component-splitting logic p9_link_cat() above already has inline --
 * pulled out here since this section needs it three times). */
static uint16_t p9_remote_split_path(const char *path, char out[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN]) {
    char pathcopy[128];
    strncpy(pathcopy, path, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';

    uint16_t n = 0;
    char *tok = pathcopy;
    while (*tok && n < P9_MAX_WALK_ELEM) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok) {
            strncpy(out[n], tok, P9_MAX_NAME_LEN - 1);
            n++;
        }
        if (!slash) break;
        tok = slash + 1;
    }
    return n;
}

int p9_remote_open(p9_remote_mount_t *mount, const char *path, uint8_t p9_mode, bool create, uint32_t *out_fid, bool *out_is_dir) {
    if (!mount || !mount->in_use || !path || !out_fid) return -1;
    while (*path == '/') path++;

    char wnames[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN];
    uint16_t n = p9_remote_split_path(path, wnames);

    uint32_t fid = mount->next_fid++;
    p9_msg_t twalk, resp;
    memset(&twalk, 0, sizeof(twalk));
    twalk.type = P9_TWALK;
    twalk.tag = p9_remote_next_tag(mount);
    twalk.fid = mount->root_fid;
    twalk.newfid = fid;
    twalk.nwname = n;
    memcpy(twalk.wname, wnames, sizeof(wnames));
    if (p9_remote_xchg(mount, &twalk, &resp) < 0) return -1;

    bool is_dir = true;
    if (resp.type == P9_RWALK && resp.nwqid == n) {
        // Full walk succeeded: the file/directory already exists.
        if (n > 0) is_dir = (resp.wqid[n - 1].type & P9_QTDIR) != 0;

        p9_msg_t topen;
        memset(&topen, 0, sizeof(topen));
        topen.type = P9_TOPEN;
        topen.tag = p9_remote_next_tag(mount);
        topen.fid = fid;
        topen.mode = p9_mode;
        if (p9_remote_xchg(mount, &topen, &resp) < 0 || resp.type != P9_ROPEN) {
            p9_remote_close(mount, fid);
            return -1;
        }
    } else if (resp.type == P9_RWALK && n > 0 && resp.nwqid == (uint16_t)(n - 1) && create) {
        // Walked exactly to the parent directory; only the final component
        // is missing -- create it there (Tcreate leaves `fid` open on the
        // new file, no separate Topen needed).
        p9_msg_t tcreate;
        memset(&tcreate, 0, sizeof(tcreate));
        tcreate.type = P9_TCREATE;
        tcreate.tag = p9_remote_next_tag(mount);
        tcreate.fid = fid;
        strncpy(tcreate.name, wnames[n - 1], sizeof(tcreate.name) - 1);
        tcreate.perm = 0644;
        tcreate.mode = p9_mode;
        if (p9_remote_xchg(mount, &tcreate, &resp) < 0 || resp.type != P9_RCREATE) {
            p9_remote_close(mount, fid);
            return -1;
        }
        is_dir = false;
    } else {
        // Walk failed outright, or stopped short of even the parent (a
        // genuinely missing intermediate directory -- this doesn't imply
        // `mkdir -p`, matching the local vfs_open()'s own semantics).
        if (resp.type == P9_RWALK) p9_remote_close(mount, fid);
        return -1;
    }

    *out_fid = fid;
    if (out_is_dir) *out_is_dir = is_dir;
    return 0;
}

int p9_remote_pread(p9_remote_mount_t *mount, uint32_t fid, void *buf, uint32_t count, uint64_t offset) {
    if (!mount || !mount->in_use || !buf) return -1;
    if (count > sizeof(g_remote_rx) - 32) count = sizeof(g_remote_rx) - 32; // framing headroom

    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TREAD;
    req.tag = p9_remote_next_tag(mount);
    req.fid = fid;
    req.offset = offset;
    req.count = count;
    if (p9_remote_xchg(mount, &req, &resp) < 0 || resp.type != P9_RREAD) return -1;
    if (resp.count > count) return -1; // defensive: peer claimed more than requested

    if (resp.count > 0) memcpy(buf, resp.data, resp.count);
    return (int)resp.count;
}

int p9_remote_pwrite(p9_remote_mount_t *mount, uint32_t fid, const void *buf, uint32_t count, uint64_t offset) {
    if (!mount || !mount->in_use || !buf) return -1;
    if (count > sizeof(g_remote_tx) - 64) count = sizeof(g_remote_tx) - 64; // framing headroom

    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TWRITE;
    req.tag = p9_remote_next_tag(mount);
    req.fid = fid;
    req.offset = offset;
    req.count = count;
    req.data = (const uint8_t *)buf;
    if (p9_remote_xchg(mount, &req, &resp) < 0 || resp.type != P9_RWRITE) return -1;
    return (int)resp.count;
}

/* Unpacks one 9P "stat" wire entry (the format fs/9p.c's p9_pack_stat()
 * produces, including its self-referential leading size field) starting at
 * src[0]. Returns the total bytes consumed (>= 2), or -1 if `src_len`
 * doesn't hold a complete, well-formed entry -- bounds-checked at every
 * step even though the only producer today is our own trusted server,
 * matching this codebase's general B11-era posture of not trusting wire
 * data just because nothing hostile is expected to send it yet. */
static int p9_unpack_stat_entry(const uint8_t *src, uint32_t src_len, char *name_out, uint32_t name_max, uint64_t *size_out, bool *is_dir_out) {
    if (src_len < 2) return -1;
    uint16_t inner_size = (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
    uint32_t total = 2u + inner_size;
    if (total > src_len || total < 2 + 39 + 2 + 2 + 2) return -1; // fixed fields + 3 empty strings + name len

    uint32_t off = 2;
    off += 2; // type (kernel-use only)
    off += 4; // dev (kernel-use only)
    uint8_t qid_type = src[off];
    off += 1 + 4 + 8; // qid.type already read; skip qid.vers + qid.path
    uint32_t mode = (uint32_t)src[off] | ((uint32_t)src[off + 1] << 8) | ((uint32_t)src[off + 2] << 16) | ((uint32_t)src[off + 3] << 24);
    off += 4;
    off += 4 + 4; // atime, mtime
    uint64_t length = 0;
    for (int i = 0; i < 8; i++) length |= ((uint64_t)src[off + i]) << (8 * i);
    off += 8;

    if (off + 2 > total) return -1;
    uint16_t namelen = (uint16_t)(src[off] | ((uint16_t)src[off + 1] << 8));
    off += 2;
    if (off + namelen > total) return -1;
    if (name_out && name_max > 0) {
        uint32_t copy = namelen < name_max - 1 ? namelen : name_max - 1;
        memcpy(name_out, src + off, copy);
        name_out[copy] = '\0';
    }
    off += namelen;

    for (int s = 0; s < 3; s++) { // uid, gid, muid -- not needed, just skip past them
        if (off + 2 > total) return -1;
        uint16_t slen = (uint16_t)(src[off] | ((uint16_t)src[off + 1] << 8));
        off += 2;
        if (off + slen > total) return -1;
        off += slen;
    }

    if (size_out) *size_out = length;
    if (is_dir_out) *is_dir_out = ((mode & P9_DMDIR) != 0) || ((qid_type & P9_QTDIR) != 0);
    return (int)total;
}

/* Re-walks the directory's Tread stream from offset 0 on every call rather
 * than caching decoded entries in the handle -- vfs_readdir()'s index-based
 * contract doesn't map cleanly onto 9P's stream-of-stats without either a
 * per-handle cache (real memory cost on RP2350's tight SRAM budget, times
 * VFS_MAX_HANDLES, for a feature only remote directory handles ever use)
 * or this: no retained state at all, at the cost of O(directory size) wire
 * traffic per call. Every directory in this system is small; matches A1's
 * own fat32_read_at() precedent of "walk from the start every call,
 * revisit if it's ever a hot path." */
int p9_remote_readdir(p9_remote_mount_t *mount, uint32_t fid, uint32_t index, char *name_out, uint32_t name_max, p9_remote_stat_t *stat_out) {
    if (!mount || !mount->in_use) return -1;

    uint64_t offset = 0;
    uint32_t found_index = 0;
    for (;;) {
        p9_msg_t req, resp;
        memset(&req, 0, sizeof(req));
        req.type = P9_TREAD;
        req.tag = p9_remote_next_tag(mount);
        req.fid = fid;
        req.offset = offset;
        req.count = p9_remote_read_chunk(mount);
        if (p9_remote_xchg(mount, &req, &resp) < 0 || resp.type != P9_RREAD) return -1;
        if (resp.count == 0) return -1; // EOF -- index out of range

        uint32_t p = 0;
        while (p < resp.count) {
            char name[P9_MAX_NAME_LEN];
            uint64_t size;
            bool is_dir;
            int consumed = p9_unpack_stat_entry(resp.data + p, resp.count - p, name, sizeof(name), &size, &is_dir);
            if (consumed < 0) break; // incomplete entry at the tail of this chunk; next Tread continues past it

            if (found_index == index) {
                if (name_out && name_max > 0) {
                    strncpy(name_out, name, name_max - 1);
                    name_out[name_max - 1] = '\0';
                }
                if (stat_out) { stat_out->size = size; stat_out->is_dir = is_dir; }
                return 0;
            }
            found_index++;
            p += (uint32_t)consumed;
        }
        offset += resp.count;
    }
}

int p9_remote_fstat(p9_remote_mount_t *mount, uint32_t fid, p9_remote_stat_t *out) {
    if (!mount || !mount->in_use || !out) return -1;

    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TSTAT;
    req.tag = p9_remote_next_tag(mount);
    req.fid = fid;
    if (p9_remote_xchg(mount, &req, &resp) < 0 || resp.type != P9_RSTAT) return -1;

    uint64_t size = 0;
    bool is_dir = false;
    if (p9_unpack_stat_entry(resp.data, resp.count, NULL, 0, &size, &is_dir) < 0) return -1;
    out->size = size;
    out->is_dir = is_dir;
    return 0;
}

int p9_remote_close(p9_remote_mount_t *mount, uint32_t fid) {
    if (!mount || !mount->in_use) return -1;
    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TCLUNK;
    req.tag = p9_remote_next_tag(mount);
    req.fid = fid;
    p9_remote_xchg(mount, &req, &resp); // best-effort
    return 0;
}

int p9_remote_remove(p9_remote_mount_t *mount, const char *path) {
    if (!mount || !mount->in_use || !path) return -1;
    while (*path == '/') path++;

    char wnames[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN];
    uint16_t n = p9_remote_split_path(path, wnames);
    if (n == 0) return -1; // refuse to remove the mount root

    uint32_t fid = mount->next_fid++;
    p9_msg_t twalk, resp;
    memset(&twalk, 0, sizeof(twalk));
    twalk.type = P9_TWALK;
    twalk.tag = p9_remote_next_tag(mount);
    twalk.fid = mount->root_fid;
    twalk.newfid = fid;
    twalk.nwname = n;
    memcpy(twalk.wname, wnames, sizeof(wnames));
    if (p9_remote_xchg(mount, &twalk, &resp) < 0 || resp.type != P9_RWALK || resp.nwqid != n) {
        if (resp.type == P9_RWALK) p9_remote_close(mount, fid);
        return -1;
    }

    p9_msg_t tremove;
    memset(&tremove, 0, sizeof(tremove));
    tremove.type = P9_TREMOVE;
    tremove.tag = p9_remote_next_tag(mount);
    tremove.fid = fid;
    if (p9_remote_xchg(mount, &tremove, &resp) < 0 || resp.type != P9_RREMOVE) return -1;
    return 0;
}

int p9_remote_mkdir(p9_remote_mount_t *mount, const char *path) {
    if (!mount || !mount->in_use || !path) return -1;
    while (*path == '/') path++;

    char wnames[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN];
    uint16_t n = p9_remote_split_path(path, wnames);
    if (n == 0) return -1;

    uint32_t fid = mount->next_fid++;
    p9_msg_t twalk, resp;
    memset(&twalk, 0, sizeof(twalk));
    twalk.type = P9_TWALK;
    twalk.tag = p9_remote_next_tag(mount);
    twalk.fid = mount->root_fid;
    twalk.newfid = fid;
    twalk.nwname = (uint16_t)(n - 1); // walk to the parent only
    memcpy(twalk.wname, wnames, sizeof(wnames));
    if (p9_remote_xchg(mount, &twalk, &resp) < 0 || resp.type != P9_RWALK || resp.nwqid != n - 1) {
        if (resp.type == P9_RWALK) p9_remote_close(mount, fid);
        return -1;
    }

    p9_msg_t tcreate;
    memset(&tcreate, 0, sizeof(tcreate));
    tcreate.type = P9_TCREATE;
    tcreate.tag = p9_remote_next_tag(mount);
    tcreate.fid = fid;
    strncpy(tcreate.name, wnames[n - 1], sizeof(tcreate.name) - 1);
    tcreate.perm = P9_DMDIR | 0755;
    tcreate.mode = P9_OREAD;
    if (p9_remote_xchg(mount, &tcreate, &resp) < 0 || resp.type != P9_RCREATE) return -1;
    p9_remote_close(mount, fid); // Tcreate leaves it open; mkdir doesn't need to keep it that way
    return 0;
}
