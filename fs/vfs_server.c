#include "fs/vfs.h"
#include "fs/fat32.h"
#include "drivers/block.h"
#include "kernel/printk.h"
#include "kernel/ipc.h"

static fat32_fs_t g_fat32;

void vfs_server_init(void) {
    block_dev_t *ramdisk = ramdisk_get_device();
    fat32_init(&g_fat32, ramdisk);
    printk("[VFS Server] Isolated VFS Service Task initialized (PID %d).\n", VFS_PID);
}

int vfs_read(const char *filename, void *buf, uint32_t max_len) {
    fat32_dir_entry_t entry;
    if (fat32_find_file(&g_fat32, filename, &entry) < 0) {
        return -1;
    }
    return fat32_read_file(&g_fat32, &entry, buf, max_len);
}

int vfs_write(const char *filename, const void *buf, uint32_t len) {
    return fat32_write_file(&g_fat32, filename, buf, len);
}

int vfs_remove(const char *filename) {
    return fat32_remove_file(&g_fat32, filename);
}

void vfs_ls(void) {
    fat32_list_dir(&g_fat32);
}
