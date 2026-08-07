#ifndef FS_9P_H
#define FS_9P_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define P9_NOFID   ((uint32_t)~0U)
#define P9_NOTAG   ((uint16_t)~0U)
#define P9_MAX_MSIZE 4096

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
int p9_server_process(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max);

#endif // FS_9P_H
