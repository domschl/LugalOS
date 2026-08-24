#ifndef FS_P9_LINK_H
#define FS_P9_LINK_H

#include <stdint.h>
#include <stdbool.h>

/* Transport-agnostic 9P link (A3, plan/phase5_distributed_design.md) -- the
 * 9P server (fs/9p.c, A2) never knows what wire it's running over. A
 * backend (drivers/virtio_console.c, drivers/uart_net.c's SLIP link, ...)
 * fills in a p9_link_t and hands it to p9_link_service()/the background
 * pump; framing (SLIP-escaped vs. plain length-prefixed) is entirely the
 * backend's own business.
 *
 * `poll()` pumps the backend's RX path and returns 1 once a *complete*
 * frame is buffered and ready for recv_frame(), 0 if nothing's ready yet,
 * -1 on a transport error. It must never block -- callers may invoke it
 * from a tight busy-wait loop (see p9_link_background_poll()). */
typedef struct p9_link {
    const char *name;
    int (*poll)(struct p9_link *link);
    int (*send_frame)(struct p9_link *link, const uint8_t *buf, uint32_t len);
    int (*recv_frame)(struct p9_link *link, uint8_t *buf, uint32_t max_len);
    void *ctx;

    /* N2, plan/phase18_networking_and_auth.md: must an attach arriving here
     * authenticate first?
     *
     * A property of the wire, which is why it lives on the link rather than
     * in the server: a local channel and a cable between two boards on one
     * desk are trusted by the same argument that trusts the boards, and a
     * network link is not. Default false, so an existing transport keeps
     * behaving exactly as it did; the W5500 link sets it, and `p9auth` can
     * set it on any link for testing or for a cable someone does not trust.
     *
     * A link that requires auth but has no keys configured refuses every
     * attach -- see p9_auth_have_keys(). */
    bool auth_required;
} p9_link_t;

/* Services exactly one pending request on `link`, if poll() reports one
 * ready: recv_frame() -> p9_server_process() -> send_frame(). Returns 1 if
 * a request was serviced, 0 if nothing was pending, -1 on a transport
 * error (a malformed *frame* still yields 1 with an Rerror reply --
 * p9_server_process() itself never fails to produce a response for a frame
 * it actually received). Never blocks. */
int p9_link_service(p9_link_t *link);

/* Registers `link` to be serviced opportunistically from
 * p9_link_background_poll() -- the hook used by drivers/uart_16550.c's and
 * drivers/uart_rp2350.c's uart_getc() busy-wait loops (the same spot
 * usb_cdc_task() is already pumped from) so a link like virtio-console, the
 * USB-CDC net link (ACM1/EP4, A3b), or the UART demux link (A3b, once a
 * user opts in via the `p9share` shell command) can carry live 9P traffic
 * *while the interactive shell is blocked on keyboard input*. Up to
 * P9_LINK_MAX_BACKGROUND (2) links may be registered at once -- e.g. RP2350
 * running its USB-CDC net link and the UART demux link simultaneously.
 * Re-registering the same link is a no-op; registering past the slot limit
 * is logged and dropped. Use p9_link_unregister_background() to remove one
 * (passing NULL to this function used to mean "unregister" when there was
 * only one slot; with multiple slots that's ambiguous, hence the separate
 * call). */
void p9_link_register_background(p9_link_t *link);
void p9_link_unregister_background(p9_link_t *link);
void p9_link_background_poll(void);

/* Starts the 9P server as a scheduled task (B4, resolving D4). Before this
 * the server was pumped from uart_getc()'s busy-wait, so a node answered its
 * peers only while sitting at the prompt. Returns the pid, or -1.
 *
 * This is also the filesystem server: the 9P handlers are VFS calls, so there
 * is no second local-only protocol to build. See fs/p9_link.c. */
int p9_server_task_start(void);

/* Link-agnostic synchronous 9P client (A4, plan/phase5_distributed_design.md):
 * attaches at "/", walks to `path` (split on '/'), opens it for read, reads
 * the whole file into `out_buf`, clunks. Works over any p9_link_t, not just
 * loopback -- e.g. a node fetching a file from whatever peer is bridged
 * onto its virtio-console link. See fs/p9_link.c for why this is safe to
 * use even on a link that also has a registered background server.
 * Returns the byte count read, or -1. A non-responding peer blocks forever
 * (matches drivers/virtio_blk.c's own busy-wait-until-done convention). */
int p9_link_cat(p9_link_t *link, const char *path, char *out_buf, uint32_t out_max);

/* --- Persistent remote 9P connection (A5, plan/phase5_distributed_design.md) ---
 * Unlike p9_link_cat() above (which does its own fresh Tversion + Tattach on
 * every call -- p9_server_process()'s Tversion handler resets *all*
 * server-side fid state, by design, as a connection reset), a mounted
 * filesystem needs one Tversion + Tattach establishing a connection ONCE,
 * kept alive for the mount's lifetime, so file handles opened against it
 * survive across separate, later vfs_open()/vfs_pread()/... calls. That's
 * the actual distinction between "a single command-line round trip" and
 * "a mounted filesystem". Backed by a small fixed pool (no dynamic
 * allocation in this kernel) -- see fs/p9_link.c for the pool size. */

typedef struct p9_remote_mount p9_remote_mount_t; // opaque

typedef struct {
    uint64_t size;
    bool is_dir;
} p9_remote_stat_t;

/* Tversion + Tattach("/") over `link`. Returns NULL if the pool is full or
 * `link` is NULL. Like every other wait in this file, a reply that never
 * arrives hangs the caller rather than timing out (matches
 * drivers/virtio_blk.c's own busy-wait-until-done convention) -- a
 * genuinely unresponsive peer hangs mount_open() itself, not just later
 * operations against an already-open mount. */
p9_remote_mount_t *p9_remote_mount_open(p9_link_t *link);
void p9_remote_mount_close(p9_remote_mount_t *mount);

/* File-handle operations against an established mount, mirroring
 * fs/include/fs/vfs.h's vfs_open()/vfs_pread()/vfs_pwrite()/vfs_readdir()/
 * vfs_fstat()/vfs_close() contracts closely so fs/vfs_server.c's dispatch
 * is a thin pass-through. `path` is relative to the mount's root.
 * `p9_mode` is a P9_OREAD/OWRITE/ORDWR mode byte, optionally OR'd with
 * P9_OTRUNC; `create` requests Tcreate if the final path component doesn't
 * exist yet (its immediate parent must already exist -- matches the local
 * vfs_open()'s own VFS_O_CREATE semantics, no implied `mkdir -p`).
 * p9_remote_readdir() re-walks the directory stream from the start on
 * every call rather than caching it -- see fs/p9_link.c for why that's a
 * deliberate simplicity/memory tradeoff, not an oversight. */
int p9_remote_open(p9_remote_mount_t *mount, const char *path, uint8_t p9_mode, bool create, uint32_t *out_fid, bool *out_is_dir);
int p9_remote_pread(p9_remote_mount_t *mount, uint32_t fid, void *buf, uint32_t count, uint64_t offset);
int p9_remote_pwrite(p9_remote_mount_t *mount, uint32_t fid, const void *buf, uint32_t count, uint64_t offset);
int p9_remote_readdir(p9_remote_mount_t *mount, uint32_t fid, uint32_t index, char *name_out, uint32_t name_max, p9_remote_stat_t *stat_out);
int p9_remote_fstat(p9_remote_mount_t *mount, uint32_t fid, p9_remote_stat_t *out);
int p9_remote_close(p9_remote_mount_t *mount, uint32_t fid);

/* Removes a file or (empty) directory at `path` on the mount -- Tremove.
 * Returns 0 on success, -1 otherwise. */
int p9_remote_remove(p9_remote_mount_t *mount, const char *path);

/* Creates a directory at `path` on the mount (parent must already exist) --
 * Tcreate with the DMDIR permission bit. Returns 0 on success, -1
 * otherwise. */
int p9_remote_mkdir(p9_remote_mount_t *mount, const char *path);

#endif // FS_P9_LINK_H
