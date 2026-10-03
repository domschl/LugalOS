/*
 * The 9P server (fs/9p.c) on the host, under ASan/UBSan or valgrind
 * (phase 40 review).
 *
 * Every byte a 9P peer sends arrives here, over USB, a UART or the network,
 * so the codec and the server behind it must survive any frame. This runs a
 * real client session -- version, attach, walk, open, read, write, stat,
 * create, remove, clunk, and the auth exchange -- against the server over a
 * small fake namespace, then the same frames with bytes changed, then
 * frames of random bytes, under both auth policies. A reply may be an
 * error; nothing may touch memory it does not own.
 *
 * Usage: p9_host [iterations [seed]].
 */

#include "fs/9p.h"
#include "fs/vfs.h"
#include "kernel/identity.h"
#include "shim.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- A fake namespace behind the VFS calls the server makes --- */

#define NODES 24
static struct { char path[96]; int is_dir; uint8_t data[1024]; uint32_t size; int used; } g_ns[NODES];
static struct { int node; int open; } g_fd[VFS_MAX_HANDLES];

static int ns_find(const char *path) {
    if (!path) return -1;
    while (path[0] == '/' && path[1] == '/') path++;
    for (int i = 0; i < NODES; i++) if (g_ns[i].used && strcmp(g_ns[i].path, path) == 0) return i;
    return -1;
}

static int ns_add(const char *path, int is_dir, uint32_t size) {
    for (int i = 0; i < NODES; i++) if (!g_ns[i].used) {
        snprintf(g_ns[i].path, sizeof(g_ns[i].path), "%s", path);
        g_ns[i].is_dir = is_dir;
        g_ns[i].size = size > sizeof(g_ns[i].data) ? sizeof(g_ns[i].data) : size;
        for (uint32_t k = 0; k < g_ns[i].size; k++) g_ns[i].data[k] = (uint8_t)('a' + k % 26);
        g_ns[i].used = 1;
        return i;
    }
    return -1;
}

static void ns_reset(void) {
    memset(g_ns, 0, sizeof(g_ns));
    memset(g_fd, 0, sizeof(g_fd));
    ns_add("/", 1, 0);
    ns_add("/sd0", 1, 0);
    ns_add("/sd0/a.txt", 0, 300);
    ns_add("/sd0/sub", 1, 0);
    ns_add("/sd0/sub/b.bin", 0, 1000);
}

int vfs_open(const char *path, int flags) {
    int n = ns_find(path);
    if (n < 0 && (flags & VFS_O_CREATE)) n = ns_add(path, 0, 0);
    if (n < 0) return -1;
    if (flags & VFS_O_TRUNC) g_ns[n].size = 0;
    for (int f = 0; f < VFS_MAX_HANDLES; f++)
        if (!g_fd[f].open) { g_fd[f].open = 1; g_fd[f].node = n; return f; }
    return -1;
}

static int fd_ok(int fd) { return fd >= 0 && fd < VFS_MAX_HANDLES && g_fd[fd].open; }

int vfs_pread(int fd, void *buf, uint32_t count, uint64_t offset) {
    if (!fd_ok(fd)) return -1;
    int n = g_fd[fd].node;
    if (offset >= g_ns[n].size) return 0;
    uint32_t k = g_ns[n].size - (uint32_t)offset;
    if (k > count) k = count;
    memcpy(buf, g_ns[n].data + offset, k);
    return (int)k;
}

int vfs_pwrite(int fd, const void *buf, uint32_t count, uint64_t offset) {
    if (!fd_ok(fd)) return -1;
    int n = g_fd[fd].node;
    if (g_ns[n].is_dir || offset > sizeof(g_ns[n].data)) return -1;
    uint32_t room = (uint32_t)(sizeof(g_ns[n].data) - offset);
    uint32_t k = count < room ? count : room;
    memcpy(g_ns[n].data + offset, buf, k);
    if (offset + k > g_ns[n].size) g_ns[n].size = (uint32_t)(offset + k);
    return (int)k;
}

int vfs_readdir(int fd, uint32_t index, char *name_out, uint32_t name_max, vfs_stat_t *st) {
    if (!fd_ok(fd)) return -1;
    const char *dir = g_ns[g_fd[fd].node].path;
    size_t dl = strlen(dir);
    uint32_t seen = 0;
    for (int i = 0; i < NODES; i++) {
        if (!g_ns[i].used || i == g_fd[fd].node) continue;
        const char *p = g_ns[i].path;
        if (strncmp(p, dir, dl) != 0) continue;
        const char *rest = p + dl;
        if (dl > 1) { if (*rest != '/') continue; rest++; }
        else if (*rest == '/') rest++;
        if (!*rest || strchr(rest, '/')) continue;
        if (seen++ == index) {
            snprintf(name_out, name_max, "%s", rest);
            if (st) { st->size = g_ns[i].size; st->is_dir = (uint8_t)g_ns[i].is_dir; }
            return 0;
        }
    }
    return -1;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    int n = ns_find(path);
    if (n < 0) return -1;
    if (st) { st->size = g_ns[n].size; st->is_dir = (uint8_t)g_ns[n].is_dir; }
    return 0;
}

int vfs_fstat(int fd, vfs_stat_t *st) {
    if (!fd_ok(fd)) return -1;
    return vfs_stat(g_ns[g_fd[fd].node].path, st);
}

int vfs_close(int fd) {
    if (!fd_ok(fd)) return -1;
    g_fd[fd].open = 0;
    return 0;
}

int vfs_mkdir(const char *path) { return ns_find(path) >= 0 || ns_add(path, 1, 0) < 0 ? -1 : 0; }
int vfs_remove(const char *path) {
    int n = ns_find(path);
    if (n < 0 || g_ns[n].is_dir) return -1;
    g_ns[n].used = 0;
    return 0;
}
int vfs_rmdir(const char *path) {
    int n = ns_find(path);
    if (n <= 1 || !g_ns[n].is_dir) return -1;
    g_ns[n].used = 0;
    return 0;
}

/* --- Identity: a node with one key, so the auth gate has work to do --- */

block_dev_t *identity_store_device(void) { return NULL; }
bool idstore_path_is_secret(const char *path) { return path && strstr(path, "identity") != NULL; }
bool node_devkey(uint8_t *out, uint32_t cap, uint32_t *len_out) {
    static const uint8_t key[32] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    if (cap < sizeof(key)) return false;
    memcpy(out, key, sizeof(key));
    *len_out = sizeof(key);
    return true;
}
bool node_grants(char *out, uint32_t cap, uint32_t *len_out) {
    const char *g = "peer /sd0\n";
    uint32_t n = (uint32_t)strlen(g);
    if (cap < n) return false;
    memcpy(out, g, n);
    *len_out = n;
    return true;
}
node_id_result_t node_identity_set_grants(const char *text, uint32_t len) {
    (void)text; (void)len;
    return NODE_ID_ERR_NO_BACKEND;
}
static uint64_t g_rand_state = 1;
void random_bytes(void *out, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) ((uint8_t *)out)[i] = (uint8_t)host_rand(&g_rand_state);
}

/* --- The session --- */

static uint8_t g_frames[64][P9_MAX_MSIZE];  /* the session, serialised */
static uint32_t g_flen[64];
static int g_nframes;

static void add(p9_msg_t *m) {
    int n = p9_serialize(m, g_frames[g_nframes], P9_MAX_MSIZE);
    if (n <= 0) { fprintf(stderr, "FAIL: could not serialise message type %u\n", m->type); exit(1); }
    g_flen[g_nframes++] = (uint32_t)n;
}

static void build_session(void) {
    p9_msg_t m;
    uint16_t tag = 1;
#define MSG(t) do { memset(&m, 0, sizeof(m)); m.type = (t); m.tag = tag++; } while (0)
    MSG(P9_TVERSION); m.tag = P9_NOTAG; m.msize = P9_MAX_MSIZE; add(&m);
    MSG(P9_TAUTH); m.afid = 9; snprintf(m.uname, sizeof(m.uname), "peer"); snprintf(m.aname, sizeof(m.aname), "/sd0"); add(&m);
    MSG(P9_TREAD); m.fid = 9; m.count = 32; add(&m);
    static uint8_t mac[32];
    MSG(P9_TWRITE); m.fid = 9; m.count = 32; m.data = mac; add(&m);
    MSG(P9_TATTACH); m.fid = 1; m.afid = P9_NOFID; snprintf(m.uname, sizeof(m.uname), "peer"); add(&m);
    MSG(P9_TATTACH); m.fid = 1; m.afid = 9; snprintf(m.uname, sizeof(m.uname), "peer"); snprintf(m.aname, sizeof(m.aname), "/sd0"); add(&m);
    MSG(P9_TWALK); m.fid = 1; m.newfid = 2; m.nwname = 2;
    snprintf(m.wname[0], P9_MAX_NAME_LEN, "sd0"); snprintf(m.wname[1], P9_MAX_NAME_LEN, "a.txt"); add(&m);
    MSG(P9_TOPEN); m.fid = 2; m.mode = P9_OREAD; add(&m);
    MSG(P9_TREAD); m.fid = 2; m.offset = 0; m.count = 200; add(&m);
    MSG(P9_TREAD); m.fid = 2; m.offset = 250; m.count = 4000; add(&m);
    MSG(P9_TSTAT); m.fid = 2; add(&m);
    MSG(P9_TCLUNK); m.fid = 2; add(&m);
    MSG(P9_TWALK); m.fid = 1; m.newfid = 3; m.nwname = 1; snprintf(m.wname[0], P9_MAX_NAME_LEN, "sd0"); add(&m);
    MSG(P9_TOPEN); m.fid = 3; m.mode = P9_OREAD; add(&m);
    MSG(P9_TREAD); m.fid = 3; m.offset = 0; m.count = 1000; add(&m);      /* a directory read */
    MSG(P9_TCLUNK); m.fid = 3; add(&m);
    MSG(P9_TWALK); m.fid = 1; m.newfid = 3; m.nwname = 1; snprintf(m.wname[0], P9_MAX_NAME_LEN, "sd0"); add(&m);
    MSG(P9_TCREATE); m.fid = 3; snprintf(m.name, sizeof(m.name), "new.txt"); m.perm = 0644; m.mode = P9_OWRITE; add(&m);
    static uint8_t payload[512];
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    MSG(P9_TWRITE); m.fid = 3; m.offset = 0; m.count = sizeof(payload); m.data = payload; add(&m);
    MSG(P9_TWRITE); m.fid = 3; m.offset = 0x7FFFFFFFFFull; m.count = 10; m.data = payload; add(&m);
    MSG(P9_TCLUNK); m.fid = 3; add(&m);
    MSG(P9_TWALK); m.fid = 1; m.newfid = 4; m.nwname = 3;
    snprintf(m.wname[0], P9_MAX_NAME_LEN, "sd0"); snprintf(m.wname[1], P9_MAX_NAME_LEN, ".."); snprintf(m.wname[2], P9_MAX_NAME_LEN, ".."); add(&m);
    MSG(P9_TWALK); m.fid = 1; m.newfid = 5; m.nwname = P9_MAX_WALK_ELEM;
    for (int i = 0; i < P9_MAX_WALK_ELEM; i++) snprintf(m.wname[i], P9_MAX_NAME_LEN, "sub");
    add(&m);
    MSG(P9_TREMOVE); m.fid = 4; add(&m);
    MSG(P9_TFLUSH); m.oldtag = 3; add(&m);
    MSG(P9_TWSTAT); m.fid = 1; add(&m);
    MSG(P9_TCLUNK); m.fid = 1; add(&m);
#undef MSG
}

static void run_frames(const uint8_t (*frames)[P9_MAX_MSIZE], const uint32_t *lens, int n,
                       p9_auth_policy_t policy) {
    static uint8_t resp[P9_MAX_MSIZE];
    p9_init();
    for (int i = 0; i < n; i++) {
        int r = p9_server_process(frames[i], lens[i], resp, sizeof(resp), policy);
        if (r > (int)sizeof(resp)) { fprintf(stderr, "FAIL: reply of %d bytes into %zu\n", r, sizeof(resp)); exit(1); }
        if (r > 0) {
            p9_msg_t back;
            int ok = p9_deserialize(resp, (uint32_t)r, &back);
            if (getenv("P9_TRACE"))   /* which replies the session gets: is it reaching the server? */
                printf("  T%u -> R%u%s%s\n", frames[i][4], ok == 0 ? back.type : 0,
                       ok == 0 && back.type == P9_RERROR ? " " : "", ok == 0 && back.ename ? back.ename : "");
        }
    }
}

static volatile sig_atomic_t g_iter = -1;
static uint64_t g_seed0;
static void on_alarm(int sig) {
    (void)sig;
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "FAIL: server did not return, input %d (seed %llu)\n",
                     (int)g_iter, (unsigned long long)g_seed0);
    (void)!write(2, msg, (size_t)n);
    _exit(3);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    signal(SIGALRM, on_alarm);
    g_seed0 = seed;

    build_session();

    /* The codec agrees with itself on every frame of the session. */
    for (int i = 0; i < g_nframes; i++) {
        p9_msg_t m, again;
        static uint8_t buf[P9_MAX_MSIZE];
        if (p9_deserialize(g_frames[i], g_flen[i], &m) != 0) { fprintf(stderr, "FAIL: frame %d does not parse\n", i); return 1; }
        int n = p9_serialize(&m, buf, sizeof(buf));
        if (n != (int)g_flen[i] || memcmp(buf, g_frames[i], (size_t)n) != 0) {
            fprintf(stderr, "FAIL: frame %d (type %u) does not round-trip\n", i, m.type);
            return 1;
        }
        (void)again;
    }

    ns_reset();
    run_frames((const uint8_t (*)[P9_MAX_MSIZE])g_frames, g_flen, g_nframes, P9_AUTH_NOT_REQUIRED);
    ns_reset();
    run_frames((const uint8_t (*)[P9_MAX_MSIZE])g_frames, g_flen, g_nframes, P9_AUTH_REQUIRED);
    printf("p9_host: %d-frame session round-trips and runs under both policies\n", g_nframes);

    static uint8_t mut[64][P9_MAX_MSIZE];
    static uint32_t mlen[64];
    uint64_t s = seed | 1;
    for (int it = 0; it < iterations; it++) {
        g_iter = it;
        memcpy(mut, g_frames, sizeof(g_frames));
        memcpy(mlen, g_flen, sizeof(g_flen));
        int nm = 1 + (int)(host_rand(&s) % 12u);
        for (int k = 0; k < nm; k++) {
            int f = (int)(host_rand(&s) % (uint32_t)g_nframes);
            uint32_t r = host_rand(&s);
            switch (r % 5u) {
                case 0: if (mlen[f]) mut[f][host_rand(&s) % mlen[f]] = (uint8_t)host_rand(&s); break;
                case 1: if (mlen[f] > 7) mut[f][7 + host_rand(&s) % 8u % (mlen[f] - 7)] = 0xFF; break;  /* fids, counts */
                case 2: mlen[f] = host_rand(&s) % (mlen[f] + 1); break;                               /* cut short */
                case 3: { uint32_t v = host_rand(&s); memcpy(mut[f], &v, 4); break; }                  /* size field */
                default: mlen[f] = host_rand(&s) % P9_MAX_MSIZE;                                     /* random frame */
                         for (uint32_t b = 0; b < mlen[f]; b++) mut[f][b] = (uint8_t)host_rand(&s);
                         if (mlen[f] >= 5) { memcpy(mut[f], &mlen[f], 4); mut[f][4] = (uint8_t)(100 + host_rand(&s) % 28u); }
                         break;
            }
        }
        ns_reset();
        alarm(5);
        run_frames((const uint8_t (*)[P9_MAX_MSIZE])mut, mlen, g_nframes,
                   (it & 1) ? P9_AUTH_REQUIRED : P9_AUTH_NOT_REQUIRED);
        alarm(0);
    }
    printf("p9_host: %d mutated sessions (seed %llu), no fault\n", iterations, (unsigned long long)seed);
    return 0;
}
