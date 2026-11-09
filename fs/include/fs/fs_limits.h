#ifndef LUGALOS_FS_LIMITS_H
#define LUGALOS_FS_LIMITS_H

#include <stdint.h>

/* One name (a path component), UTF-8, with its NUL: what a buffer for
 * vfs_readdir() needs to receive any FAT long name (255 UTF-16 units of
 * ASCII or Latin-1) in full. A smaller buffer gets the 8.3 alias of a name
 * that does not fit, which opens the same file. */
#define VFS_NAME_MAX 256

/* A whole path, with its NUL, as the layers that keep paths (9P fids, the
 * launcher's document list) hold one. Larger where memory is not scarce: a
 * board with PSRAM, or a 64-bit one. A longer path is refused, never cut
 * short -- a truncated path names some other file. */
#if defined(CONFIG_PSRAM_BYTES) || UINTPTR_MAX > 0xFFFFFFFFu || defined(LUGALOS_HOST_TEST)
#define VFS_PATH_MAX 256
#else
#define VFS_PATH_MAX 128
#endif

#endif /* LUGALOS_FS_LIMITS_H */
