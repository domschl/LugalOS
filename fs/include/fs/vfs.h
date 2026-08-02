#ifndef LUGALOS_FS_VFS_H
#define LUGALOS_FS_VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_PID 3

#define VFS_TAG_OPEN    0x10
#define VFS_TAG_READ    0x11
#define VFS_TAG_WRITE   0x12
#define VFS_TAG_CLOSE   0x13
#define VFS_TAG_LS      0x14
#define VFS_TAG_RM      0x15

void vfs_server_init(void);
int vfs_register_service(const char *service_name, int target_pid);

int vfs_open(const char *path);
int vfs_read(const char *path, void *buf, uint32_t max_len);
int vfs_write(const char *path, const void *buf, uint32_t len);
int vfs_remove(const char *path);
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
int vfs_cp(const char *src_path, const char *dst_path);
void vfs_ls(const char *path);

#endif /* LUGALOS_FS_VFS_H */


