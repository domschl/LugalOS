#include "fs/vfs_ufd.h"
#include "kernel/ipc.h"
#include "kernel/lock.h"
#include <stdbool.h>

typedef struct {
    bool     in_use;
    bool     busy;      /* true while a blocking vfs_* call is in flight */
    int      owner_pid;
    int      vfs_fd;
    int      flags;
    uint64_t offset;
} vfs_ufd_slot_t;

/* guarded by: g_ufd_lock */
static vfs_ufd_slot_t g_ufds[VFS_MAX_HANDLES];
static spinlock_t     g_ufd_lock = SPINLOCK_INIT;

static int ufd_acquire(int pid, int ufd, int *vfs_fd_out, uint64_t *off_out,
                       int *flags_out) {
    if (ufd < 0 || ufd >= VFS_MAX_HANDLES) return -1;
    uintptr_t f = spin_lock_irqsave(&g_ufd_lock);
    vfs_ufd_slot_t *s = &g_ufds[ufd];
    if (!s->in_use || s->busy || s->owner_pid != pid) {
        spin_unlock_irqrestore(&g_ufd_lock, f);
        return -1;
    }
    s->busy = true;
    if (vfs_fd_out) *vfs_fd_out = s->vfs_fd;
    if (off_out) *off_out = s->offset;
    if (flags_out) *flags_out = s->flags;
    spin_unlock_irqrestore(&g_ufd_lock, f);
    return 0;
}

static void ufd_release(int ufd, uint64_t new_offset, bool update_offset) {
    uintptr_t f = spin_lock_irqsave(&g_ufd_lock);
    if (update_offset) g_ufds[ufd].offset = new_offset;
    g_ufds[ufd].busy = false;
    spin_unlock_irqrestore(&g_ufd_lock, f);
}

int vfs_ufd_open(int pid, const char *path, int flags) {
    if (!path || pid < 0) return -1;
    int vfs_flags = flags & (VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    int vfd = vfs_open(path, vfs_flags);
    if (vfd < 0) return -1;

    uint64_t start_off = 0;
    if (flags & LUGAL_O_APPEND) {
        vfs_stat_t st;
        if (vfs_fstat(vfd, &st) == 0 && !st.is_dir) {
            start_off = st.size;
        }
    }

    uintptr_t f = spin_lock_irqsave(&g_ufd_lock);
    int slot = -1;
    for (int i = 0; i < VFS_MAX_HANDLES; i++) {
        if (!g_ufds[i].in_use) {
            g_ufds[i].in_use    = true;
            g_ufds[i].busy      = false;
            g_ufds[i].owner_pid = pid;
            g_ufds[i].vfs_fd    = vfd;
            g_ufds[i].flags     = flags;
            g_ufds[i].offset    = start_off;
            slot = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_ufd_lock, f);

    if (slot < 0) {
        vfs_close(vfd);
        return -1;
    }
    return slot;
}

int vfs_ufd_close(int pid, int ufd) {
    int vfd = -1;
    if (ufd < 0 || ufd >= VFS_MAX_HANDLES) return -1;
    uintptr_t f = spin_lock_irqsave(&g_ufd_lock);
    vfs_ufd_slot_t *s = &g_ufds[ufd];
    if (!s->in_use || s->busy || s->owner_pid != pid) {
        spin_unlock_irqrestore(&g_ufd_lock, f);
        return -1;
    }
    vfd = s->vfs_fd;
    s->in_use    = false;
    s->busy      = false;
    s->owner_pid = -1;
    s->vfs_fd    = -1;
    s->flags     = 0;
    s->offset    = 0;
    spin_unlock_irqrestore(&g_ufd_lock, f);

    return vfs_close(vfd);
}

int vfs_ufd_read(int pid, int ufd, void *buf, uint32_t count) {
    int vfd = 0;
    uint64_t off = 0;
    if (ufd_acquire(pid, ufd, &vfd, &off, NULL) < 0) return -1;

    int n = vfs_pread(vfd, buf, count, off);
    uint64_t next_off = (n > 0) ? (off + (uint64_t)n) : off;
    ufd_release(ufd, next_off, n > 0);
    return n;
}

int vfs_ufd_write(int pid, int ufd, const void *buf, uint32_t count) {
    int vfd = 0, flags = 0;
    uint64_t off = 0;
    if (ufd_acquire(pid, ufd, &vfd, &off, &flags) < 0) return -1;

    if (flags & LUGAL_O_APPEND) {
        vfs_stat_t st;
        if (vfs_fstat(vfd, &st) == 0 && !st.is_dir) {
            off = st.size;
        }
    }

    int n = vfs_pwrite(vfd, buf, count, off);
    uint64_t next_off = (n > 0) ? (off + (uint64_t)n) : off;
    ufd_release(ufd, next_off, n > 0);
    return n;
}

int64_t vfs_ufd_lseek(int pid, int ufd, int64_t offset, int whence) {
    int vfd = 0;
    uint64_t cur = 0;
    if (ufd_acquire(pid, ufd, &vfd, &cur, NULL) < 0) return -1;

    int64_t base = 0;
    if (whence == LUGAL_SEEK_SET) {
        base = 0;
    } else if (whence == LUGAL_SEEK_CUR) {
        base = (int64_t)cur;
    } else if (whence == LUGAL_SEEK_END) {
        vfs_stat_t st;
        if (vfs_fstat(vfd, &st) < 0) {
            ufd_release(ufd, cur, false);
            return -1;
        }
        base = (int64_t)st.size;
    } else {
        ufd_release(ufd, cur, false);
        return -1;
    }

    int64_t target = base + offset;
    if (target < 0) {
        ufd_release(ufd, cur, false);
        return -1;
    }

    ufd_release(ufd, (uint64_t)target, true);
    return target;
}

int vfs_ufd_fstat(int pid, int ufd, vfs_stat_t *out) {
    int vfd = 0;
    uint64_t cur = 0;
    if (!out || ufd_acquire(pid, ufd, &vfd, &cur, NULL) < 0) return -1;
    int rc = vfs_fstat(vfd, out);
    ufd_release(ufd, cur, false);
    return rc;
}

int vfs_ufd_readdir(int pid, int ufd, uint32_t index, char *name_out,
                    uint32_t name_max, vfs_stat_t *stat_out) {
    int vfd = 0;
    uint64_t cur = 0;
    if (ufd_acquire(pid, ufd, &vfd, &cur, NULL) < 0) return -1;
    int rc = vfs_readdir(vfd, index, name_out, name_max, stat_out);
    ufd_release(ufd, cur, false);
    return rc;
}

void vfs_ufd_cleanup_task(int pid) {
    if (pid < 0) return;
    int to_close[VFS_MAX_HANDLES];
    int count = 0;

    uintptr_t f = spin_lock_irqsave(&g_ufd_lock);
    for (int i = 0; i < VFS_MAX_HANDLES; i++) {
        if (g_ufds[i].in_use && g_ufds[i].owner_pid == pid) {
            to_close[count++] = g_ufds[i].vfs_fd;
            g_ufds[i].in_use    = false;
            g_ufds[i].busy      = false;
            g_ufds[i].owner_pid = -1;
            g_ufds[i].vfs_fd    = -1;
            g_ufds[i].flags     = 0;
            g_ufds[i].offset    = 0;
        }
    }
    spin_unlock_irqrestore(&g_ufd_lock, f);

    for (int i = 0; i < count; i++) {
        if (to_close[i] >= 0) (void)vfs_close(to_close[i]);
    }
}
