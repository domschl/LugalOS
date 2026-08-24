#ifndef FS_9P_H
#define FS_9P_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define P9_NOFID   ((uint32_t)~0U)
#define P9_NOTAG   ((uint16_t)~0U)
/* The largest 9P message this node will send or accept, and therefore the
 * size of every frame buffer in fs/. There are ten of them across p9_chan.c,
 * p9_link.c and 9p.c, so this constant is multiplied by ten in .bss -- 48 KB
 * at 4096, which on RP2350 is 12 of the heap's 62 pages, since .bss and the
 * heap are the same budget there (§2.3,
 * plan/phase15_memory_reclamation.md).
 *
 * Board-scoped for that reason, the same way user/lisp/lisp.c's pools and
 * search.c's MAX_SEARCH_PLYS are. It costs round trips, not correctness:
 * msize is negotiated per connection (see p9_server_process()'s P9_TVERSION
 * case), so a peer asking for more is simply answered with this and must
 * respect it. */
#if defined(CONFIG_BOARD_RP2350)
#define P9_MAX_MSIZE 2048
#else
#define P9_MAX_MSIZE 4096
#endif

/* Bytes of framing that must fit alongside a read's or write's payload
 * inside one msize-bounded message.
 *
 * 24 is Plan 9's own IOHDRSZ, and it is the *write* direction that sets it:
 * Twrite is size[4] type[1] tag[2] fid[4] offset[8] count[4] = 23 bytes of
 * header before a single payload byte, rounded up. Rread's own framing is
 * smaller (11), so one conservative number covers both directions.
 *
 * This is what an iounit must be computed from. Advertising an iounit equal
 * to the msize -- which this server did, hardcoded, until §2.3 -- promises a
 * client a transfer that provably cannot fit: a 4096-byte Tread against a
 * 4096 msize needs 4107 bytes on the wire for its reply. */
#define P9_IOHDRSZ 24

/* The largest payload a single Tread/Twrite can carry at this node's own
 * msize. Derived, never written down twice. */
#define P9_MAX_IOUNIT (P9_MAX_MSIZE - P9_IOHDRSZ)

/* The iounit for the connection as currently negotiated -- at most
 * P9_MAX_IOUNIT, less if the peer asked for a smaller msize. */
uint32_t p9_negotiated_iounit(void);

/* Whether an attach on a given transport must authenticate first. */
typedef enum {
    P9_AUTH_NOT_REQUIRED = 0,   /* a local channel, or an attached cable */
    P9_AUTH_REQUIRED     = 1,   /* anything reachable over a network */
} p9_auth_policy_t;

/* Bounds for the in-kernel fid table and Twalk's name list -- both are
 * per-connection resource limits, not protocol limits (9P itself allows up
 * to 65535 walk elements per message). Small values keep the fid table's
 * memory footprint predictable on RP2350; a real client (our own RPC
 * helpers, or a future Python peer) never needs more than a couple of fids
 * or a couple of path components at once. */
#define P9_MAX_FIDS       8
#define P9_MAX_WALK_ELEM  16
#define P9_MAX_NAME_LEN   32

/* 9P open/create mode bits (the low 2 bits select the access mode; the rest
 * are independent flags), per the 9P2000 spec. */
#define P9_OREAD   0x00
#define P9_OWRITE  0x01
#define P9_ORDWR   0x02
#define P9_OEXEC   0x03
#define P9_OTRUNC  0x10
#define P9_ORCLOSE 0x40

/* Tcreate's `perm` DMDIR bit -- the top bit of the 32-bit permission word
 * marks the new file as a directory. */
#define P9_DMDIR   0x80000000U

// 9P2000 Message Types
typedef enum {
    P9_TVERSION = 100, P9_RVERSION = 101,
    P9_TAUTH    = 102, P9_RAUTH    = 103,
    P9_TATTACH  = 104, P9_RATTACH  = 105,
    P9_RERROR   = 107,
    P9_TFLUSH   = 108, P9_RFLUSH   = 109,
    P9_TWALK    = 110, P9_RWALK    = 111,
    P9_TOPEN    = 112, P9_ROPEN    = 113,
    P9_TCREATE  = 114, P9_RCREATE  = 115,
    P9_TREAD    = 116, P9_RREAD    = 117,
    P9_TWRITE   = 118, P9_RWRITE   = 119,
    P9_TCLUNK   = 120, P9_RCLUNK   = 121,
    P9_TREMOVE  = 122, P9_RREMOVE  = 123,
    P9_TSTAT    = 124, P9_RSTAT    = 125,
    P9_TWSTAT   = 126, P9_RWSTAT   = 127
} p9_msg_type_t;

// QID types (top bits of p9_qid_t.type)
#define P9_QTDIR  0x80
#define P9_QTAUTH 0x08   // the file behind an afid, not a file on any disk
#define P9_QTFILE 0x00

// 9P QID Structure (13 bytes on the wire)
typedef struct {
    uint8_t  type;  // QID type (P9_QTDIR / P9_QTFILE)
    uint32_t vers;  // Version
    uint64_t path;  // Unique file identifier
} p9_qid_t;

// 9P Generic In-Memory Message Descriptor
typedef struct {
    uint8_t     type;
    uint16_t    tag;
    uint16_t    oldtag;  // Tflush's target tag
    uint32_t    fid;
    uint32_t    afid;    // Tauth/Tattach: the auth fid, or P9_NOFID for none
    uint32_t    newfid;
    uint32_t    msize;
    uint64_t    offset;
    uint32_t    count;
    uint8_t     mode;    // Topen/Tcreate mode byte (P9_OREAD/OWRITE/ORDWR + flags)
    uint32_t    perm;    // Tcreate permission word (P9_DMDIR bit selects a directory)
    p9_qid_t    qid;
    char        uname[P9_MAX_NAME_LEN];
    char        aname[P9_MAX_NAME_LEN];
    const char *ename;   // Error string for Rerror (server constructs from string literals only)
    char        name[P9_MAX_NAME_LEN];              // Tcreate's new file/dir name
    uint16_t    nwname;
    char        wname[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN];
    uint16_t    nwqid;
    p9_qid_t    wqid[P9_MAX_WALK_ELEM];
    const uint8_t *data;  // Read/write/stat payload (points into the caller's buffer)
} p9_msg_t;

void p9_init(void);

// Serialization & Deserialization -- both fully bounds-checked against
// buf_size/len (see B11 in plan/completed/2026-08-07_review_and_remediation.md
// and the A2 completion notes in plan/phase5_distributed_design.md): every
// field read is checked against the remaining frame before it's consumed,
// and every field written is checked against the remaining output space
// before it's produced. Malformed input yields -1, never an out-of-bounds
// access.
int p9_serialize(const p9_msg_t *msg, uint8_t *buf, uint32_t buf_size);
int p9_deserialize(const uint8_t *buf, uint32_t len, p9_msg_t *msg);

// Microkernel 9P File Server Entrypoint -- stateful across calls via an
// internal, bounded fid table (see P9_MAX_FIDS) wired to the real VFS
// handle API (fs/include/fs/vfs.h, A1), not a single shared buffer.
//
// `policy` says whether an attach arriving on THIS transport has to prove it
// knows a key (N2, plan/phase18_networking_and_auth.md). It is a parameter
// rather than a global because it is a property of the wire the request came
// off, and every caller knows which wire that is: a local channel and an
// attached cable do not require it, an Ethernet link does. Passing it
// explicitly at all four call sites is deliberate -- the failure mode of a
// default would be a link that silently does not authenticate.
int p9_server_process(const uint8_t *req_buf, uint32_t req_len,
                      uint8_t *resp_buf, uint32_t resp_max,
                      p9_auth_policy_t policy);

/* --- The auth gate (N2, plan/phase18_networking_and_auth.md §6) ---
 *
 * 9P-native, through Tauth and an afid, because that is the extension point
 * the protocol already has for exactly this. The exchange:
 *
 *     C -> Tauth  afid, uname, aname
 *     S -> Rauth  qid                    (afid is now an auth file)
 *     C -> Tread  afid, 0, 32            <- 32-byte server nonce
 *     C -> Twrite afid, 0, 32            -> HMAC-SHA256(key, nonce|uname|aname)
 *     S            verifies, marks the afid authenticated
 *     C -> Tattach fid, afid, uname, aname
 *
 * What it proves: the client holds the key for `uname`. What it does not
 * prove, and must not be read as proving: anything about confidentiality (the
 * frames are cleartext), and anything about authorization -- every
 * authenticated identity gets the same namespace. See the plan's §1.
 */

/* Where the keys live, searched in this order. The first file is a list --
 * one "uname hexkey" line per identity, so a key can be revoked by deleting a
 * line -- and the second is a single key for a board with no card.
 *
 * The 9P server refuses to serve anything under P9_AUTH_KEY_DIR (see
 * p9_path_is_secret() in fs/9p.c). That is not belt-and-braces: the gateway
 * exports the very volume its keys live on, so without it any authenticated
 * client could simply read everyone else's secret. */
#define P9_AUTH_KEY_DIR   "/sd0/system/etc/auth"
#define P9_AUTH_KEYS_FILE P9_AUTH_KEY_DIR "/keys"
#define P9_AUTH_FALLBACK_KEY_FILE "/flash0/system/etc/p9key"

/* True if any key is configured at all. A link whose policy is
 * P9_AUTH_REQUIRED and for which this is false refuses every attach: the
 * failure mode of a misconfigured gateway must be "nobody gets in", never
 * "everybody does". */
bool p9_auth_have_keys(void);

/* Installs a key for this boot only, from the local console (`p9key`). It
 * overrides the key files, is never reachable over 9P, and does not survive a
 * reboot -- see the definition in fs/9p.c for why it exists at all rather
 * than a test key being baked into a shipped image. Pass NULL/0 to clear. */
void p9_auth_set_console_key(const uint8_t *key, uint32_t len);

/* True for a path the server refuses to serve to anyone over any transport --
 * the key store. Exposed for the self-test. */
bool p9_auth_path_is_secret(const char *path);

/* Known-answer tests for the parts of the gate that are pure logic: the
 * secret-path guard, and the fact that the response MAC binds uname and aname
 * (so a response captured for one identity cannot be replayed as another on
 * the same nonce). No network, no keys, no peer. Returns failures. */
int p9_auth_selftest(void);

#endif // FS_9P_H
