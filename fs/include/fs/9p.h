#ifndef FS_9P_H
#define FS_9P_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define P9_NOFID   ((uint32_t)~0U)
#define P9_NOTAG   ((uint16_t)~0U)
#define P9_MAX_MSIZE 4096

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

// 9P QID Structure (13 bytes)
typedef struct {
    uint8_t  type;  // QID type (e.g. 0x80 = directory, 0x00 = file)
    uint32_t vers;  // Version
    uint64_t path;  // Unique file identifier
} p9_qid_t;

// 9P Generic In-Memory Message Descriptor
typedef struct {
    uint8_t     type;
    uint16_t    tag;
    uint32_t    fid;
    uint32_t    newfid;
    uint32_t    msize;
    uint32_t    offset;
    uint32_t    count;
    p9_qid_t    qid;
    const char *uname;
    const char *aname;
    const char *ename; // Error string for Rerror
    const char *path;  // Target path string
    const uint8_t *data; // Write payload
} p9_msg_t;

void p9_init(void);

// Serialization & Deserialization
int p9_serialize(const p9_msg_t *msg, uint8_t *buf, uint32_t buf_size);
int p9_deserialize(const uint8_t *buf, uint32_t len, p9_msg_t *msg);

// Microkernel 9P File Server Entrypoint
int p9_server_process(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max);

#endif // FS_9P_H
