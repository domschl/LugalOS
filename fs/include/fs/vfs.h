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
void vfs_server_run(void);

int vfs_open(const char *path);
int vfs_read(const char *filename, void *buf, uint32_t max_len);
int vfs_write(const char *filename, const void *buf, uint32_t len);
int vfs_remove(const char *filename);
void vfs_ls(void);

#endif /* LUGALOS_FS_VFS_H */
