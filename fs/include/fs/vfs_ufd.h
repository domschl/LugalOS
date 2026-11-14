#ifndef FS_VFS_UFD_H
#define FS_VFS_UFD_H

#include <stdint.h>
#include "fs/vfs.h"

/* Per-task U-mode file descriptor table over VFS handles.
 *
 * Every U-mode fd tracks its owning task PID and current byte offset, and is
 * automatically closed when the task exits (SYS_UEXIT) or is terminated by a
 * U-mode fault so crashing programs cannot exhaust VFS_MAX_HANDLES. */

int     vfs_ufd_open(int pid, const char *path, int flags);
int     vfs_ufd_close(int pid, int ufd);
int     vfs_ufd_read(int pid, int ufd, void *buf, uint32_t count);
int     vfs_ufd_write(int pid, int ufd, const void *buf, uint32_t count);
int64_t vfs_ufd_lseek(int pid, int ufd, int64_t offset, int whence);
int     vfs_ufd_fstat(int pid, int ufd, vfs_stat_t *out);
int     vfs_ufd_readdir(int pid, int ufd, uint32_t index, char *name_out,
                        uint32_t name_max, vfs_stat_t *stat_out);
void    vfs_ufd_cleanup_task(int pid);

#endif /* FS_VFS_UFD_H */
