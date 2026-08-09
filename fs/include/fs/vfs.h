#ifndef LUGALOS_FS_VFS_H
#define LUGALOS_FS_VFS_H

#include <stdint.h>
#include <stddef.h>
#include "fs/p9_link.h"

#define VFS_PID 3

#define VFS_TAG_OPEN    0x10
#define VFS_TAG_READ    0x11
#define VFS_TAG_WRITE   0x12
#define VFS_TAG_CLOSE   0x13
#define VFS_TAG_LS      0x14
#define VFS_TAG_RM      0x15

void vfs_server_init(void);
int vfs_mount_ramdisk(int size_kb);
int vfs_register_service(const char *service_name, int target_pid);

/* Mounts this node's own 9P server at /<name>/ over a local channel (B1).
 * Implemented as vfs_mount_remote() with a channel-backed link -- see
 * fs/vfs_server.c for why there is deliberately no separate local path. */
int vfs_mount_local(const char *name);

/* --- Handle-based file API (A1, plan/phase5_distributed_design.md) ---
 *
 * Real, offset-addressed file handles across the whole namespace (/flash0,
 * /sd0, /ram0, /proc, /dev, and the root "/" mount table), so a caller (and
 * eventually a remote 9P client) can open a path once and issue multiple
 * reads/writes/seeks against it instead of the whole-file-only vfs_read()/
 * vfs_write() below. /srv/ is deliberately NOT part of this API: it's a
 * message-oriented IPC channel, not a byte-addressable file, and stays on
 * its own direct-dispatch path inside vfs_read()/vfs_write().
 *
 * /proc files are now real byte streams generated fresh into the handle at
 * open() time (see vfs_generate_proc_content() in vfs_server.c), not a
 * printk() side effect -- so they can be read in pieces, and a remote 9P
 * client can read them at all.
 */

#define VFS_O_READ   0x01
#define VFS_O_WRITE  0x02
#define VFS_O_CREATE 0x04 /* create if it doesn't exist (write-only paths) */
#define VFS_O_TRUNC  0x08 /* truncate to empty if it already exists */

#define VFS_MAX_HANDLES 8

typedef struct {
    uint32_t size;
    uint8_t is_dir;
} vfs_stat_t;

int vfs_open(const char *path, int flags);
int vfs_pread(int fd, void *buf, uint32_t count, uint64_t offset);
int vfs_pwrite(int fd, const void *buf, uint32_t count, uint64_t offset);
int vfs_readdir(int fd, uint32_t index, char *name_out, uint32_t name_max, vfs_stat_t *stat_out);
int vfs_fstat(int fd, vfs_stat_t *out);
int vfs_stat(const char *path, vfs_stat_t *out);
int vfs_close(int fd);

/* --- Legacy whole-file API ---
 * Kept for the ~35 existing call sites across the shell/Lisp/9P layers;
 * implemented as thin compat wrappers over the handle API above (except
 * vfs_read()/vfs_write()'s /srv/ branch, which bypasses it -- see above). */
int vfs_read(const char *path, void *buf, uint32_t max_len);
int vfs_write(const char *path, const void *buf, uint32_t len);
int vfs_append(const char *path, const void *buf, uint32_t len);
int vfs_remove(const char *path);
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
int vfs_cp(const char *src_path, const char *dst_path);
int vfs_format(const char *path);
void vfs_ls(const char *path);

/* --- Mount table (A5, plan/phase5_distributed_design.md) ---
 * Attaches a remote 9P namespace at /<name>/, turning "9P works" into
 * "distributed namespace works" -- once mounted, /<name>/ behaves like any
 * other mount for every function above (vfs_open/vfs_read/vfs_write/
 * vfs_ls/vfs_cp/...), not just a special-purpose primitive. `link` must
 * already be a usable p9_link_t (e.g. virtio_console_get_link()).
 * vfs_unmount() only ever removes a mount added this way -- the built-in
 * mounts (flash0/sd0/ram0/proc/dev/srv) aren't unmountable through this
 * path. Both return 0 on success, -1 otherwise (name collision, mount
 * table full, or -- for mount -- the remote handshake itself failing). */
int vfs_mount_remote(const char *name, p9_link_t *link);
int vfs_unmount(const char *name);

#endif /* LUGALOS_FS_VFS_H */


