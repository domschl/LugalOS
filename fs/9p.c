#include "fs/9p.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include <string.h>

static uint8_t g_p9_storage_buf[2048];
static uint32_t g_p9_storage_len = 0;

static void write_u8(uint8_t **p, uint8_t val) {
    **p = val;
    (*p)++;
}

static void write_u16(uint8_t **p, uint16_t val) {
    (*p)[0] = (uint8_t)(val & 0xFF);
    (*p)[1] = (uint8_t)((val >> 8) & 0xFF);
    *p += 2;
}

static void write_u32(uint8_t **p, uint32_t val) {
    (*p)[0] = (uint8_t)(val & 0xFF);
    (*p)[1] = (uint8_t)((val >> 8) & 0xFF);
    (*p)[2] = (uint8_t)((val >> 16) & 0xFF);
    (*p)[3] = (uint8_t)((val >> 24) & 0xFF);
    *p += 4;
}

static void write_str(uint8_t **p, const char *str) {
    uint16_t len = str ? (uint16_t)strlen(str) : 0;
    write_u16(p, len);
    if (len > 0) {
        memcpy(*p, str, len);
        *p += len;
    }
}

static uint8_t read_u8(const uint8_t **p) {
    uint8_t val = **p;
    (*p)++;
    return val;
}

static uint16_t read_u16(const uint8_t **p) {
    uint16_t val = (uint16_t)((*p)[0] | ((*p)[1] << 8));
    *p += 2;
    return val;
}

static uint32_t read_u32(const uint8_t **p) {
    uint32_t val = (uint32_t)((*p)[0] | ((*p)[1] << 8) | ((*p)[2] << 16) | ((*p)[3] << 24));
    *p += 4;
    return val;
}

void p9_init(void) {
    memset(g_p9_storage_buf, 0, sizeof(g_p9_storage_buf));
    g_p9_storage_len = 0;
    printk("[9P2000] Protocol Serialization Engine Initialized.\n");
}

int p9_serialize(const p9_msg_t *msg, uint8_t *buf, uint32_t buf_size) {
    if (!msg || !buf || buf_size < 7) return -1;

    uint8_t *p = buf + 4; // Reserve 4 bytes for total size
    write_u8(&p, msg->type);
    write_u16(&p, msg->tag);

    switch (msg->type) {
        case P9_TVERSION:
            write_u32(&p, msg->msize);
            write_str(&p, "9P2000");
            break;
        case P9_RVERSION:
            write_u32(&p, msg->msize);
            write_str(&p, "9P2000");
            break;
        case P9_TATTACH:
            write_u32(&p, msg->fid);
            write_u32(&p, P9_NOFID); // AFID
            write_str(&p, msg->uname ? msg->uname : "lugal");
            write_str(&p, msg->aname ? msg->aname : "");
            break;
        case P9_RATTACH:
            write_u8(&p, msg->qid.type);
            write_u32(&p, msg->qid.vers);
            write_u32(&p, (uint32_t)msg->qid.path);
            write_u32(&p, (uint32_t)(msg->qid.path >> 32));
            break;
        case P9_RERROR:
            write_str(&p, msg->ename ? msg->ename : "Unknown error");
            break;
        case P9_TWALK:
            write_u32(&p, msg->fid);
            write_u32(&p, msg->newfid);
            write_u16(&p, msg->path ? 1 : 0);
            if (msg->path) write_str(&p, msg->path);
            break;
        case P9_RWALK:
            write_u16(&p, 1); // nqid = 1
            write_u8(&p, 0x00); // file type
            write_u32(&p, 1);   // version
            write_u32(&p, 2);   // path low
            write_u32(&p, 0);   // path high
            break;
        case P9_TOPEN:
            write_u32(&p, msg->fid);
            write_u8(&p, 0); // mode = READ/WRITE
            break;
        case P9_ROPEN:
            write_u8(&p, 0x00); // qid type
            write_u32(&p, 1);   // vers
            write_u32(&p, 2);   // path low
            write_u32(&p, 0);   // path high
            write_u32(&p, 4096); // iounit
            break;
        case P9_TWRITE:
            write_u32(&p, msg->fid);
            write_u32(&p, msg->offset);
            write_u32(&p, (uint32_t)((uint64_t)msg->offset >> 32));
            write_u32(&p, msg->count);
            if (msg->data && msg->count > 0) {
                memcpy(p, msg->data, msg->count);
                p += msg->count;
            }
            break;
        case P9_RWRITE:
            write_u32(&p, msg->count);
            break;
        case P9_TREAD:
            write_u32(&p, msg->fid);
            write_u32(&p, msg->offset);
            write_u32(&p, (uint32_t)((uint64_t)msg->offset >> 32));
            write_u32(&p, msg->count);
            break;
        case P9_RREAD:
            write_u32(&p, msg->count);
            if (msg->data && msg->count > 0) {
                memcpy(p, msg->data, msg->count);
                p += msg->count;
            }
            break;
        case P9_TCLUNK:
        case P9_RCLUNK:
            write_u32(&p, msg->fid);
            break;
        default:
            break;
    }

    uint32_t total_size = (uint32_t)(p - buf);
    uint8_t *hdr = buf;
    write_u32(&hdr, total_size);

    return (int)total_size;
}

int p9_deserialize(const uint8_t *buf, uint32_t len, p9_msg_t *msg) {
    if (!buf || len < 7 || !msg) return -1;
    memset(msg, 0, sizeof(*msg));

    const uint8_t *p = buf;
    uint32_t size = read_u32(&p);
    if (size > len) return -1;

    msg->type = read_u8(&p);
    msg->tag = read_u16(&p);

    switch (msg->type) {
        case P9_TVERSION:
        case P9_RVERSION:
            msg->msize = read_u32(&p);
            break;
        case P9_TATTACH:
            msg->fid = read_u32(&p);
            break;
        case P9_TWALK:
            msg->fid = read_u32(&p);
            msg->newfid = read_u32(&p);
            break;
        case P9_TOPEN:
            msg->fid = read_u32(&p);
            break;
        case P9_TWRITE:
            msg->fid = read_u32(&p);
            msg->offset = read_u32(&p);
            (void)read_u32(&p); // high offset
            msg->count = read_u32(&p);
            msg->data = p;
            break;
        case P9_RWRITE:
            msg->count = read_u32(&p);
            break;
        case P9_TREAD:
            msg->fid = read_u32(&p);
            msg->offset = read_u32(&p);
            (void)read_u32(&p); // high offset
            msg->count = read_u32(&p);
            break;
        case P9_RREAD:
            msg->count = read_u32(&p);
            msg->data = p;
            break;
        case P9_TCLUNK:
            msg->fid = read_u32(&p);
            break;
        default:
            break;
    }
    return 0;
}

int p9_server_process(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max) {
    p9_msg_t req;
    if (p9_deserialize(req_buf, req_len, &req) < 0) {
        p9_msg_t err = { .type = P9_RERROR, .tag = 1, .ename = "Invalid 9P frame" };
        return p9_serialize(&err, resp_buf, resp_max);
    }

    p9_msg_t resp = { .tag = req.tag };

    switch (req.type) {
        case P9_TVERSION:
            resp.type = P9_RVERSION;
            resp.msize = (req.msize < 4096) ? req.msize : 4096;
            break;
        case P9_TATTACH:
            resp.type = P9_RATTACH;
            resp.qid.type = 0x80; // directory
            resp.qid.vers = 1;
            resp.qid.path = 1;
            break;
        case P9_TWALK:
            resp.type = P9_RWALK;
            resp.fid = req.newfid;
            break;
        case P9_TOPEN:
            resp.type = P9_ROPEN;
            resp.fid = req.fid;
            break;
        case P9_TWRITE:
            resp.type = P9_RWRITE;
            if (req.data && req.count > 0) {
                uint32_t to_write = req.count;
                if (to_write > sizeof(g_p9_storage_buf) - 1) {
                    to_write = sizeof(g_p9_storage_buf) - 1;
                }
                memcpy(g_p9_storage_buf, req.data, to_write);
                g_p9_storage_buf[to_write] = '\0';
                g_p9_storage_len = to_write;
                resp.count = to_write;
            } else {
                resp.count = 0;
            }
            break;
        case P9_TREAD:
            resp.type = P9_RREAD;
            if (g_p9_storage_len > 0) {
                uint32_t to_read = req.count;
                if (to_read > g_p9_storage_len) to_read = g_p9_storage_len;
                resp.count = to_read;
                resp.data = g_p9_storage_buf;
            } else {
                resp.count = 0;
                resp.data = NULL;
            }
            break;
        case P9_TCLUNK:
            resp.type = P9_RCLUNK;
            break;
        default:
            resp.type = P9_RERROR;
            resp.ename = "Unsupported 9P request type";
            break;
    }
    return p9_serialize(&resp, resp_buf, resp_max);
}
